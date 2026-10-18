# Experimental Wine WDF binding boundary

This module is **not a KMDF implementation** and does not provide Windows kernel monitoring, WDF objects, devices, queues or callback registration. It is a controlled compatibility test for clients whose visible native entry wrapper only needs the framework binding boundary.

## Supported boundary

- `WdfVersionBind` accepts only the observed `KmdfLibrary` v1.15.0, v1 bind-info layout and 444-entry ABI.
- On 64-bit targets, bind-info is 48 bytes; the table field at offset 32 addresses the client's function-table pointer slot.
- Each binding owns a zero-initialized 56-byte public driver-globals object on 64-bit targets. Its opaque WDFDRIVER handle remains NULL: this shim creates no WDF driver object.
- Slot 201 (`WdfDriverMiniportUnload`) performs no object cleanup because this module creates no WDF objects. Binding memory is freed by `WdfVersionUnbind`.
- All other 443 slots raise a noncontinuable `EXCEPTION_WINE_STUB`, with the exact WDF API name and caller address. They never return a fabricated success/pointer.
- Class extensions are unsupported: `WdfVersionBindClass` returns `STATUS_NOT_SUPPORTED`. The two inspected client images have empty visible class-binding descriptor ranges.
- Binding ownership is locked, independent clients get independent globals, duplicate bindings are rejected, and unbind resets this module's table/module pointers. The caller must stop using globals before unbinding, as with driver unload generally.

After a successful bind, the log includes:

```text
WDF boundary shim: bound KMDF 1.15 ...; framework APIs remain unsupported
```

An additional framework operation produces a named error such as:

```text
unsupported KMDF 1.15 slot 116 (WdfDriverCreate), caller ...
```

## Coverage and limits

The supplied ARM64 clients' normal entry wrappers were inspected statically. They immediately read table slot 201 after bind and later inspect public DriverFlags/DisplaceDriverUnload fields. Their protected `.tvm0` regions were not unpacked or proven free of additional framework calls. This module deliberately reports such additional calls rather than claim full support.

Existing Wine kernel stubs are unchanged. A client driver may still fail later, and the game may or may not tolerate that failure. This test distinguishes missing-library failure from binding/entry progress; it is not a promise that the game will launch.

## Validation

The actual C logic is exercised by host tests under ASan/UBSan: ABI layout, invalid/version-mismatched binds, allocation failure, independent/duplicate bindings, cleanup, every unsupported slot and 1000 concurrent bind/unbind cycles. The actual compiled ARM64 PE is also executed under Unicorn with mocked OS imports to check the native ABI, table/globals setup, cleanup, a named unsupported exception and unbinding. Neither replaces tablet integration testing.

## ABI provenance and licenses

Interface layouts and the index/name mapping follow Microsoft's MIT-licensed WDF 1.15 headers:

- https://github.com/microsoft/Windows-Driver-Frameworks/blob/main/src/framework/shared/inc/private/common/fxldr.h
- https://github.com/microsoft/Windows-Driver-Frameworks/blob/main/src/publicinc/wdf/kmdf/1.15/wdfglobals.h
- https://github.com/microsoft/Windows-Driver-Frameworks/blob/main/src/publicinc/wdf/kmdf/1.15/wdffuncenum.h

The generated slot header records the upstream header SHA256. Microsoft's license is retained in LICENSE.microsoft-wdf. This compatibility module is LGPL-2.1-or-later, like Wine. It is not an upstream WDF or Wine implementation.
