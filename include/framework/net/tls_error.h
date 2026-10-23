#ifndef PR_FRAMEWORK_NET_TLS_ERROR_H
#define PR_FRAMEWORK_NET_TLS_ERROR_H
#include <string>
#include <system_error>

namespace pr::net {
// Backend-neutral TLS failure classes. The contract carries no TLS library
// objects; these codes let callers tell a rejected peer identity (worth an
// audit log) from ordinary IO trouble, and a truncated stream from a clean
// EOF. Both TLS adapters (Asio / native) report these for the same conditions.
enum class TlsErrc {
    certificate_verification = 1, // peer certificate chain or hostname rejected
    stream_truncated = 2,         // connection ended without close_notify
};

class TlsErrorCategory final : public std::error_category {
public:
    const char* name() const noexcept override { return "protorelay.tls"; }
    std::string message(int value) const override {
        switch (value) {
        case static_cast<int>(TlsErrc::certificate_verification):
            return "TLS peer certificate or hostname verification failed";
        case static_cast<int>(TlsErrc::stream_truncated):
            return "TLS stream truncated (connection closed without close_notify)";
        default:
            return "Unknown TLS error";
        }
    }
};
inline std::error_code make_error_code(TlsErrc value) {
    static const TlsErrorCategory category;
    return {static_cast<int>(value), category};
}
} // namespace pr::net
namespace std {
template <> struct is_error_code_enum<pr::net::TlsErrc> : true_type {};
} // namespace std
#endif
