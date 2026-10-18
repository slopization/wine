/*
 * Minimal KMDF 1.15 binding boundary for compatibility testing.
 * This is not a KMDF implementation. No framework objects or callbacks are
 * provided; every unsupported table operation raises a named Wine stub exception.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
 * License for more details.
 */

#ifndef WDFLDR_HOST_TEST
#include <stdarg.h>
#include <string.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "rtlsupportapi.h"
#include "ddk/wdm.h"
#include "wine/debug.h"
#endif

WINE_DEFAULT_DEBUG_CHANNEL(wdfldr);

#ifndef WDFLDR_HOST_TEST
static const char *debugstr_us(const UNICODE_STRING *str)
{
    return wine_dbgstr_wn(str ? str->Buffer : NULL, str ? str->Length / sizeof(WCHAR) : 0);
}
#endif

#define WDF_FUNCTION_COUNT 444
#define WDF_POOL_TAG 0x724c6457

typedef void (WINAPI *wdf_func)(void);

/* ABI layout from the MIT-licensed Microsoft WDF headers, see README.md. */
struct wdf_bind_info
{
    ULONG size;
    WCHAR *component;
    struct { ULONG major, minor, build; } version;
    ULONG func_count;
    wdf_func **func_table; /* KMDF 1.15: address of the client's table-pointer slot */
    void *module;
};

struct wdf_driver_globals
{
    void *driver;  /* opaque WDFDRIVER, not a DRIVER_OBJECT */
    ULONG driver_flags;
    ULONG driver_tag;
    char driver_name[32];
    BOOLEAN displace_driver_unload;
};

struct binding
{
    struct binding *next;
    struct wdf_bind_info *info;
    wdf_func **table_slot;
    struct wdf_driver_globals globals;
};

#ifdef _WIN64
C_ASSERT(sizeof(struct wdf_bind_info) == 48);
C_ASSERT(offsetof(struct wdf_bind_info, func_table) == 32);
C_ASSERT(sizeof(struct wdf_driver_globals) == 56);
C_ASSERT(offsetof(struct wdf_driver_globals, displace_driver_unload) == 48);
#endif

static SRWLOCK binding_lock = SRWLOCK_INIT;
static struct binding *bindings;

static DECLSPEC_NORETURN void unsupported_function(unsigned int index, const char *name, void *caller)
{
    EXCEPTION_RECORD rec = {0};

    ERR("unsupported KMDF 1.15 slot %u (%s), caller %p\n", index, name, caller);
    rec.ExceptionCode = EXCEPTION_WINE_STUB;
    rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    rec.ExceptionAddress = caller;
    rec.NumberParameters = 2;
    rec.ExceptionInformation[0] = (ULONG_PTR)"wdfldr.sys";
    rec.ExceptionInformation[1] = (ULONG_PTR)name;
    for (;;) RtlRaiseException(&rec);
}

#define WDF_SLOT(index, name) \
    static DECLSPEC_NORETURN void WINAPI unsupported_##name(void) \
    { unsupported_function(index, #name, __builtin_return_address(0)); }
#define WDF_CLEANUP_SLOT(index, name)
#include "wdf_slots.h"
#undef WDF_SLOT
#undef WDF_CLEANUP_SLOT

static void WINAPI miniport_unload(struct wdf_driver_globals *globals, void *driver)
{
    /* This shim creates no WDF objects. Binding ownership is released by Unbind. */
    TRACE("binding-only miniport cleanup %p, driver %p\n", globals, driver);
}

static wdf_func function_table[WDF_FUNCTION_COUNT] =
{
#define WDF_SLOT(index, name) [index] = unsupported_##name,
#define WDF_CLEANUP_SLOT(index, name) [index] = (wdf_func)miniport_unload,
#include "wdf_slots.h"
#undef WDF_SLOT
#undef WDF_CLEANUP_SLOT
};

static BOOL is_kmdf_library(const WCHAR *name)
{
    static const WCHAR expected[] = {'K','m','d','f','L','i','b','r','a','r','y',0};
    unsigned int i;

    if (!name) return FALSE;
    for (i = 0; i < ARRAY_SIZE(expected); ++i)
        if (name[i] != expected[i]) return FALSE;
    return TRUE;
}

NTSTATUS WINAPI WdfVersionBind(DRIVER_OBJECT *driver, UNICODE_STRING *path,
                              struct wdf_bind_info *info, struct wdf_driver_globals **globals)
{
    struct binding *binding, *iter;
    unsigned int start = 0, i, length;

    if (!driver || !globals || !info || info->size != sizeof(*info) || !info->func_table)
        return STATUS_INVALID_PARAMETER;
    if (!is_kmdf_library(info->component) || info->version.major != 1 ||
        info->version.minor != 15 || info->version.build != 0 || info->func_count != WDF_FUNCTION_COUNT)
    {
        FIXME("unsupported WDF binding %lu.%lu.%lu, count %lu\n",
              info->version.major, info->version.minor, info->version.build, info->func_count);
        return STATUS_NOT_SUPPORTED;
    }
    if (path && (path->Length % sizeof(WCHAR) || (path->Length && !path->Buffer)))
        return STATUS_INVALID_PARAMETER;

    binding = ExAllocatePoolWithTag(NonPagedPool, sizeof(*binding), WDF_POOL_TAG);
    if (!binding) return STATUS_NO_MEMORY;
    memset(binding, 0, sizeof(*binding));
    binding->info = info;
    binding->table_slot = info->func_table;
    /* Driver stays NULL: no WDFDRIVER object is created by the binding boundary. */
    binding->globals.driver_tag = WDF_POOL_TAG;
    if (path && path->Buffer)
    {
        length = path->Length / sizeof(WCHAR);
        for (i = 0; i < length; ++i)
            if (path->Buffer[i] == '\\' || path->Buffer[i] == '/') start = i + 1;
        for (i = 0; i < sizeof(binding->globals.driver_name) - 1 && start + i < length; ++i)
            binding->globals.driver_name[i] = path->Buffer[start+i] < 128 ? path->Buffer[start+i] : '?';
    }

    RtlAcquireSRWLockExclusive(&binding_lock);
    for (iter = bindings; iter; iter = iter->next)
        if (iter->info == info || iter->table_slot == info->func_table) break;
    if (iter || *info->func_table || info->module)
    {
        RtlReleaseSRWLockExclusive(&binding_lock);
        ExFreePoolWithTag(binding, WDF_POOL_TAG);
        return STATUS_INVALID_DEVICE_STATE;
    }
    binding->next = bindings;
    bindings = binding;
    *info->func_table = function_table;
    info->module = binding;
    *globals = &binding->globals;
    /* Report while locked: no binding access is allowed after releasing the lock. */
    FIXME("WDF boundary shim: bound KMDF 1.15 %s, globals %p, table %p; framework APIs remain unsupported\n",
          debugstr_us(path), &binding->globals, function_table);
    RtlReleaseSRWLockExclusive(&binding_lock);
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI WdfVersionUnbind(UNICODE_STRING *path, struct wdf_bind_info *info,
                                struct wdf_driver_globals *globals)
{
    struct binding **link, *binding;

    if (!info || !globals) return STATUS_INVALID_PARAMETER;
    RtlAcquireSRWLockExclusive(&binding_lock);
    for (link = &bindings; *link; link = &(*link)->next)
        if ((*link)->info == info && &(*link)->globals == globals) break;
    if (!(binding = *link))
    {
        RtlReleaseSRWLockExclusive(&binding_lock);
        return STATUS_INVALID_PARAMETER;
    }
    *link = binding->next;
    if (*binding->table_slot == function_table) *binding->table_slot = NULL;
    if (info->module == binding) info->module = NULL;
    TRACE("unbound %s, globals %p\n", debugstr_us(path), globals);
    RtlReleaseSRWLockExclusive(&binding_lock);
    ExFreePoolWithTag(binding, WDF_POOL_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI WdfVersionBindClass(struct wdf_bind_info *info, struct wdf_driver_globals *globals, void *class_info)
{
    FIXME("class extensions unsupported: bind %p, globals %p, class %p\n", info, globals, class_info);
    return STATUS_NOT_SUPPORTED;
}

void WINAPI WdfVersionUnbindClass(struct wdf_bind_info *info, struct wdf_driver_globals *globals, void *class_info)
{
    /* BindClass never succeeds; there is no class-extension state to release. */
    TRACE("no class state: bind %p, globals %p, class %p\n", info, globals, class_info);
}

NTSTATUS WINAPI DllInitialize(UNICODE_STRING *path)
{
    TRACE("%s\n", debugstr_us(path));
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI DllUnload(void)
{
    NTSTATUS status;
    RtlAcquireSRWLockExclusive(&binding_lock);
    status = bindings ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    RtlReleaseSRWLockExclusive(&binding_lock);
    return status;
}

NTSTATUS WINAPI DriverEntry(DRIVER_OBJECT *driver, UNICODE_STRING *path)
{
    TRACE("%p, %s\n", driver, debugstr_us(path));
    return STATUS_SUCCESS;
}
