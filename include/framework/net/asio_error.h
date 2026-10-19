#ifndef PR_FRAMEWORK_NET_ASIO_ERROR_H
#define PR_FRAMEWORK_NET_ASIO_ERROR_H
// Adapter-only translation. Core callbacks continue to use std::error_code;
// only framework semantics (EOF/cancellation) are normalized. Opaque backend
// categories and values remain available for diagnostics.
#include "framework/net/io_types.h"
#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>
#include <exception>
#include <utility>

namespace pr::net {
inline std::error_code asio_error(const boost::system::error_code& error) {
    if (!error) return {};
    if (error == boost::asio::error::operation_aborted) return std::make_error_code(std::errc::operation_canceled);
    if (error == boost::asio::error::eof) return make_error_code(IoErrc::end_of_stream);
    return static_cast<std::error_code>(error);
}

// A synchronous backend system error leaves the adapter as std::system_error.
// Retain the complete original exception as a nested cause. Do not translate
// user exceptions or programming errors into a misleading IO error.
template <typename Function>
decltype(auto) asio_call(const char* operation, Function&& function) {
    try {
        return std::forward<Function>(function)();
    } catch (const boost::system::system_error& error) {
        std::throw_with_nested(std::system_error(asio_error(error.code()), operation));
    }
}
} // namespace pr::net
#endif
