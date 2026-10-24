#ifndef PR_FRAMEWORK_NET_NATIVE_TLS_H
#define PR_FRAMEWORK_NET_NATIVE_TLS_H
// Native backend TLS extension. The OpenSSL-bearing adapter lives in the
// protorelay_native_tls module; this header carries no TLS library types so
// contracts-only builds can include the framework surface without OpenSSL.
//
// Without this module (or before the call below) the native backend keeps its
// fail-closed behavior: SSL listeners and TLS configurations are rejected at
// construction, and configure_tls / upgrade_tls / async_start_tls throw
// operation_not_supported.
namespace pr::net::native {
// Installs the OpenSSL-backed TLS hooks used by the native transports. Call
// once at startup, before creating native transports that need TLS.
// Idempotent; returns true when native TLS is available afterwards (false
// when this binary was built without the protorelay_native_tls module).
bool enable_native_tls();
} // namespace pr::net::native
#endif
