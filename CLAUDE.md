# Firestom viewer

## Compiling
DO NOT compile things, this project uses a non-standard build system, ask the user to build it

## Merging upstream: mesh importer
`LLModelPreview` (llmodelpreview.cpp) also runs without its floater, for the MCP `upload.mesh` job. If an upstream change in llmodelpreview.cpp fails to compile on `mFMP`, `sInstance` or `addStringToLog`, port it like this:
- Reading an upload option from a widget: add a field to `IDModelPreviewSettings` (idmodelpreviewhost.h), fill it in `LLFloaterModelPreview::getPreviewSettings()`, and read it through `mHost->getPreviewSettings()`. Also set it in idmcp_meshupload.cpp if the job should control it.
- Writing to a widget: go through `mUI`, inside `if (mUI)`. It is null without a floater.
- Logging: `mHost->addPreviewLog(...)`.
- Anything that changes the model itself must still run when `mUI` is null.
