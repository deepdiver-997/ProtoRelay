# Storage objects and MIME metadata

`framework/storage/i_storage_provider.h` exposes generic read/write stream interfaces.
The default read stream buffers bytes; the default write stream adapts append operations.
Neither needs local files, mmap, nor the buffered upload implementation. Local bounded
downloads now live in `local_file_download_stream.h`. The legacy mail storage shim keeps
explicit aliases for older application code.

`entities/mime_part.h` contains the MIME data model. `common/mime_tree.h` parses MIME and
encodes/decodes existing sidecar JSON without file IO. `IMimeMetadataStore` controls cache
lookup and publication; cache misses, old metadata versions, and cache exceptions fall back
to parsing the supplied source. Cache failures do not reject a readable message.

`IObjectMetadataCache` is a separate optional provider capability. Local and distributed
file providers explicitly implement it with the existing `.mime` format and atomic
same-directory rename. The asynchronous provider decorator forwards the capability to its
inner provider. Other providers can omit it: SMTP, IMAP, submission, and Webmail parse
source bytes without reading or creating a local file named after an object key. Remote
providers do not gain synchronous network metadata requests as a side effect of this change.

The path-only overloads in `mime_parser.h` preserve legacy local sidecar callers. New
provider-aware callers use `mime_metadata_store_for(provider)` and pass the resulting store
to `load_mime_tree`, `save_mime_tree`, or `ensure_mime_tree`. A null provider explicitly
selects the legacy local adapter. A real provider without the capability never selects it.
IMAP SEARCH also keeps local-body fallback only when no provider exists; a provider read
error cannot redirect the lookup into the local filesystem.

Mail/attachment key builder methods remain for source compatibility. They can be moved
behind an application key policy later; changing all storage drivers is not required to
make metadata IO explicit.

`storage_boundary_test` covers an opaque provider key colliding with a real local sidecar,
cache version upgrades, exception isolation, decorator capability forwarding, and concurrent
local cache publication. Existing MIME parser tests exercise legacy JSON compatibility.
