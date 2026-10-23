// Error domains are identified by category AND value. Backend codes that have
// the same integer as a framework EOF/cancellation must remain distinguishable.
// The synchronous exception bridge keeps its original cause without changing
// allocation, programming, or user exception types.
#undef NDEBUG
#include "framework/net/asio_error.h"
#include <cassert>
#include <stdexcept>
#include <string>

class DriverCategory final : public boost::system::error_category {
public:
    const char* name() const noexcept override { return "test.driver"; }
    std::string message(int) const override { return "opaque backend diagnostic"; }
};

int main() {
    using pr::net::asio_error;
    using pr::net::asio_call;
    assert(!asio_error({}));
    assert(asio_error(boost::asio::error::eof) == pr::net::IoErrc::end_of_stream);
    assert(asio_error(boost::asio::error::operation_aborted) == std::errc::operation_canceled);
    assert(asio_error(boost::asio::error::connection_refused) == std::errc::connection_refused);
    assert(asio_error(boost::asio::error::timed_out) == std::errc::timed_out);
#ifdef USE_SSL
    assert(asio_error(boost::asio::ssl::error::stream_truncated) ==
           pr::net::TlsErrc::stream_truncated);
    const boost::system::error_code verify_failed(
        ERR_PACK(ERR_LIB_SSL, 0, SSL_R_CERTIFICATE_VERIFY_FAILED),
        boost::asio::error::get_ssl_category());
    assert(asio_error(verify_failed) == pr::net::TlsErrc::certificate_verification);
    const boost::system::error_code other_ssl(
        ERR_PACK(ERR_LIB_SSL, 0, SSL_R_BAD_RECORD_TYPE),
        boost::asio::error::get_ssl_category());
    assert(asio_error(other_ssl) == static_cast<std::error_code>(other_ssl));
#endif

    static DriverCategory category;
    for (auto value : {static_cast<int>(boost::asio::error::eof), static_cast<int>(boost::asio::error::operation_aborted)}) {
        const boost::system::error_code backend(value, category);
        const auto translated = asio_error(backend);
        assert(translated.value() == value && translated == static_cast<std::error_code>(backend));
        assert(translated != pr::net::IoErrc::end_of_stream);
        assert(translated != std::errc::operation_canceled);
        assert(translated.message() == "opaque backend diagnostic");
    }
    const boost::system::error_code backend(17, category);
    bool caught = false, original_cause = false;
    try {
        asio_call("start test operation", [&]() -> void {
            throw boost::system::system_error(backend, "original driver context");
        });
    } catch (const std::system_error& error) {
        caught = true;
        assert(error.code() == asio_error(backend));
        assert(std::string(error.what()).find("start test operation") != std::string::npos);
        try { std::rethrow_if_nested(error); }
        catch (const boost::system::system_error& cause) {
            original_cause = true;
            assert(cause.code() == backend);
            assert(std::string(cause.what()).find("original driver context") != std::string::npos);
        }
    }
    assert(caught && original_cause);
    assert(asio_call("return value", [] { return 42; }) == 42);
    bool unchanged = false;
    try { asio_call("not an IO error", [] { throw std::invalid_argument("bad argument"); }); }
    catch (const std::invalid_argument& error) { unchanged = std::string(error.what()) == "bad argument"; }
    assert(unchanged);
}
