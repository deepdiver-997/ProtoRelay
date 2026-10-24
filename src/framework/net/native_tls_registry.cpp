// Registry for the native backend TLS seam (see native_tls_hook.h). Lives in
// protorelay_native_net so binaries linking only the plain backend resolve
// these symbols; the definitions carry no TLS library dependency. The
// protorelay_native_tls module installs its hook set via enable_native_tls().
#include "native/native_tls_hook.h"

namespace pr::net::native::tls_hook {
namespace {
Hooks* g_hooks = nullptr;
} // namespace

void set_hooks(Hooks* hooks) { g_hooks = hooks; }
Hooks* get_hooks() { return g_hooks; }
} // namespace pr::net::native::tls_hook
