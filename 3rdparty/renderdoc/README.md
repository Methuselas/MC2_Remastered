# renderdoc_app.h vendored header

Single-file C API header for the RenderDoc in-application API. Used by
Tier 5 (`docs/testing-strategy.md`) to trigger frame captures from inside
`mc2.exe` when `MC2_RDC_CAPTURE_FRAME` is set.

- **Version:** v1.x branch HEAD (header declares `eRENDERDOC_API_Version_1_5_0`
  through `eRENDERDOC_API_Version_1_7_0`; we request 1.5.0 for max compat).
- **Source URL:** https://raw.githubusercontent.com/baldurk/renderdoc/v1.x/renderdoc/api/app/renderdoc_app.h
- **Fetched:** 2026-05-20
- **License:** MIT (Copyright 2015-2026 Baldur Karlsson)

No link-time dependency. The engine resolves `renderdoc.dll` at runtime via
`GetModuleHandleA` (the DLL is injected when launched under
`renderdoccmd capture`) and falls back to `LoadLibraryA("renderdoc.dll")`.
When neither resolves the hook silently no-ops -- the engine continues to
run normally.

To refresh: re-download from the URL above and update the date. Do not
hand-edit; the file is upstream-owned.
