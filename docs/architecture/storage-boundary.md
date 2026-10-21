# Storage objects and MIME metadata

`framework/storage/i_storage_provider.h` exposes generic read/write stream interfaces.
The default read stream buffers bytes; the default write stream adapts append operations.
Neither needs local files, mmap, nor the buffered upload implementation. Local bounded
downloads now live in `local_file_download_stream.h`. The mail storage application adapters layer mail object layouts over these generic
providers; the provider interface itself has no mail identifiers or key builders.

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

`mail_system::storage::IMailObjectKeyLayout` is a separate application capability.
Application provider adapters preserve each backend's legacy body/attachment layout,
filename spelling and per-instance sequence. Application helpers fail explicitly if the
capability is absent. The asynchronous application adapter forwards the same inner policy,
so wrapping a provider does not reset its counter. Distributed object placement resolves
the complete relative key with the existing generic hash operation.

Storage configuration and its JSON parsing live under `mail_system/back/storage/`.
Existing JSON names and defaults are unchanged. `protorelay_mail_storage` contains the
application layout code and links generic storage without the database or mail runtime.

`storage_boundary_test` covers an opaque provider key colliding with a real local sidecar,
cache version upgrades, exception isolation, decorator capability forwarding, and concurrent
local cache publication. Existing MIME parser tests exercise legacy JSON compatibility.

`mail_object_key_layout_test` checks legacy key vectors, generic placement, configuration parsing, decorator lifetime and metadata forwarding. Remote provider checks construct layouts without performing HTTP requests.
