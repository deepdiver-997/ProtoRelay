// Native backend TLS adapter (OpenSSL). Implements the tls_hook seam from
// native_tls_hook.h: SSL_CTX construction for the server/client sides and a
// loop-side TlsSession state machine that tunnels the TLS record layer over a
// plain StreamState through a memory BIO pair. OpenSSL types stay in this
// translation unit; the native backend module itself remains OpenSSL-free.
//
// Threading mirrors StreamState: all state below lives on the owning loop
// thread, foreign initiations are routed through loop->post, and completions
// are always queued (never inline, never at the initiation site).
#include "framework/net/native_tls.h"
#include "framework/net/tls_error.h"
#include "native/native_tls_hook.h"

#include <openssl/bio.h>
#include <openssl/dh.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pr::net::native {
namespace {

std::error_code tls_error(TlsErrc value) { return make_error_code(value); }

// Drains the per-thread OpenSSL error stack; empty when nothing queued.
std::string drain_error_stack() {
    std::string trace;
    unsigned long code = 0;
    while ((code = ERR_get_error()) != 0) {
        char text[256] = {0};
        ERR_error_string_n(code, text, sizeof(text));
        if (!trace.empty()) trace += "; ";
        trace += text;
    }
    return trace;
}

int hostname_ex_index() {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

// Mandatory hostname check for the leaf certificate. Chain validation already
// happened (preverify); intermediates pass on that alone. DNS names go through
// X509_check_host, IP literals through X509_check_ip_asc (IP SANs).
int hostname_verify_callback(int preverified, X509_STORE_CTX* store) {
    if (preverified != 1) return 0;
    SSL* ssl = static_cast<SSL*>(
        X509_STORE_CTX_get_ex_data(store, SSL_get_ex_data_X509_STORE_CTX_idx()));
    const char* expected =
        static_cast<const char*>(SSL_get_ex_data(ssl, hostname_ex_index()));
    if (!expected || !*expected) return 0;
    if (X509_STORE_CTX_get_error_depth(store) != 0) return 1;
    X509* cert = X509_STORE_CTX_get_current_cert(store);
    if (!cert) return 0;
    if (X509_check_host(cert, expected, std::strlen(expected),
                        X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, nullptr) == 1)
        return 1;
    return X509_check_ip_asc(cert, expected, 0) == 1;
}

struct CtxDeleter {
    void operator()(SSL_CTX* context) const { SSL_CTX_free(context); }
};
using CtxPtr = std::shared_ptr<SSL_CTX>;

// Adapter-owned context bundle; type-erased to shared_ptr<void> at the seam.
struct NativeTlsContext {
    CtxPtr ctx;
    std::string server_name; // client side: SNI and mandatory hostname target
};

void apply_common_options(SSL_CTX* ctx) {
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    // The write buffer copy in TlsSession has a stable address, but a canceled
    // app write legally abandons the buffer and a later one resumes the record:
    // allow resumption with fresh storage, and let SSL_write report partial
    // progress so cancellation can account the bytes actually moved.
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
    // A vanished peer (no close_notify) must surface as an error, never as a
    // clean EOF — truncation protection is a contract requirement.
    SSL_CTX_clear_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif
    // Session tickets/resumption add state the backend has no use for.
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
#if OPENSSL_VERSION_NUMBER >= 0x10101000L && !defined(LIBRESSL_VERSION_NUMBER)
    SSL_CTX_set_num_tickets(ctx, 0); // TLS 1.3 server-side tickets
#endif
}

CtxPtr build_server_context(const net::TlsConfig& config) {
    CtxPtr ctx(SSL_CTX_new(TLS_server_method()), CtxDeleter{});
    if (!ctx) throw std::runtime_error("native TLS: SSL_CTX_new failed: " + drain_error_stack());
    apply_common_options(ctx.get());
    if (!config.enable_tls1_3) SSL_CTX_set_max_proto_version(ctx.get(), TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_chain_file(ctx.get(), config.certificate_file.c_str()) != 1)
        throw std::runtime_error("native TLS: cannot load certificate " + config.certificate_file +
                                 ": " + drain_error_stack());
    if (SSL_CTX_use_PrivateKey_file(ctx.get(), config.private_key_file.c_str(), SSL_FILETYPE_PEM) != 1)
        throw std::runtime_error("native TLS: cannot load private key " + config.private_key_file +
                                 ": " + drain_error_stack());
    if (SSL_CTX_check_private_key(ctx.get()) != 1)
        throw std::runtime_error("native TLS: private key does not match the certificate: " +
                                 drain_error_stack());
    if (!config.dh_file.empty() && std::filesystem::exists(config.dh_file)) {
        BIO* bio = BIO_new_file(config.dh_file.c_str(), "r");
        DH* dh = bio ? PEM_read_bio_DHparams(bio, nullptr, nullptr, nullptr) : nullptr;
        if (!dh) {
            if (bio) BIO_free(bio);
            throw std::runtime_error("native TLS: cannot load DH parameters " + config.dh_file +
                                     ": " + drain_error_stack());
        }
        SSL_CTX_set_tmp_dh(ctx.get(), dh);
        DH_free(dh);
        BIO_free(bio);
    }
    return ctx;
}

CtxPtr build_client_context(const net::ClientTlsConfig& config) {
    if (config.server_name.empty())
        throw std::invalid_argument("native TLS client requires server_name");
    if (!config.use_system_ca && config.ca_file.empty() && config.ca_pem.empty())
        throw std::invalid_argument(
            "native TLS client requires a trust source (system CA, ca_file or ca_pem)");
    CtxPtr ctx(SSL_CTX_new(TLS_client_method()), CtxDeleter{});
    if (!ctx) throw std::runtime_error("native TLS: SSL_CTX_new failed: " + drain_error_stack());
    apply_common_options(ctx.get());
    // Verification is mandatory and cannot be disabled (interface contract).
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);
    if (config.use_system_ca) SSL_CTX_set_default_verify_paths(ctx.get());
    if (!config.ca_file.empty() &&
        SSL_CTX_load_verify_locations(ctx.get(), config.ca_file.c_str(), nullptr) != 1)
        throw std::runtime_error("native TLS: cannot load CA file " + config.ca_file + ": " +
                                 drain_error_stack());
    if (!config.ca_pem.empty()) {
        BIO* bio = BIO_new_mem_buf(config.ca_pem.data(), static_cast<int>(config.ca_pem.size()));
        STACK_OF(X509_INFO)* certs = bio ? PEM_X509_INFO_read_bio(bio, nullptr, nullptr, nullptr)
                                         : nullptr;
        if (!certs) {
            if (bio) BIO_free(bio);
            throw std::runtime_error("native TLS: cannot parse CA PEM: " + drain_error_stack());
        }
        X509_STORE* store = SSL_CTX_get_cert_store(ctx.get());
        for (int i = 0; i < sk_X509_INFO_num(certs); ++i) {
            X509_INFO* info = sk_X509_INFO_value(certs, i);
            if (info->x509) X509_STORE_add_cert(store, info->x509);
        }
        sk_X509_INFO_pop_free(certs, X509_INFO_free);
        BIO_free(bio);
    }
    return ctx;
}

constexpr std::size_t kNetworkChunk = 16384;

// Loop-side TLS state machine over one plain StreamState. A memory BIO pair
// carries ciphertext between the SSL object and the socket: network reads feed
// the read BIO, writes drain the write BIO, and SSL_read/SSL_write drive the
// protocol. WANT_READ/WANT_WRITE translate into plain-level async IO, which
// arms the poller through StreamState's ordinary interest tracking.
class TlsSession final : public std::enable_shared_from_this<TlsSession> {
public:
    TlsSession(const NativeLoopPtr& loop, std::shared_ptr<StreamState> stream, const CtxPtr& ctx,
               net::HandshakeRole role, std::string server_name)
        : loop_(loop), stream_(std::move(stream)), role_(role),
          server_name_(std::move(server_name)) {
        ssl_ = SSL_new(ctx.get());
        if (!ssl_) throw std::runtime_error("native TLS: SSL_new failed: " + drain_error_stack());
        // One BIO pair, the classic memory-pipe shape: the SSL owns BOTH
        // directions of the internal end, this session drives the network end
        // (BIO_write ciphertext in, BIO_read ciphertext out). Giving the SSL
        // the two different halves would let it read back its own records.
        BIO* internal_bio = nullptr;
        if (BIO_new_bio_pair(&internal_bio, 0, &network_bio_, 0) != 1) {
            SSL_free(ssl_);
            ssl_ = nullptr;
            network_bio_ = nullptr;
            throw std::runtime_error("native TLS: BIO_new_bio_pair failed: " + drain_error_stack());
        }
        BIO_up_ref(internal_bio); // SSL_set_bio consumes one ref per slot
        SSL_set_bio(ssl_, internal_bio, internal_bio);
        if (role_ == net::HandshakeRole::server) {
            SSL_set_accept_state(ssl_);
        } else {
            SSL_set_connect_state(ssl_);
            SSL_set_verify(ssl_, SSL_VERIFY_PEER, hostname_verify_callback);
            SSL_set_ex_data(ssl_, hostname_ex_index(),
                            const_cast<char*>(server_name_.c_str()));
            if (SSL_set_tlsext_host_name(ssl_, server_name_.c_str()) != 1) {
                SSL_free(ssl_);
                ssl_ = nullptr;
                throw std::runtime_error("native TLS: cannot set SNI name: " + drain_error_stack());
            }
        }
    }

    // Loop-thread teardown (scheduled by ~TlsConnection / the client
    // transport). No close_notify here: destruction is silent by design —
    // sessions that want the peer to see a clean EOF call close().
    void dispose() {
        loop_->assert_loop_thread("tls dispose");
        disposed_ = true;
        dead_ = true;
        cancel_pending(std::make_error_code(std::errc::operation_canceled));
        if (ssl_) {
            SSL_free(ssl_); // also drops the internal BIO pair end
            ssl_ = nullptr;
        }
        if (network_bio_) {
            BIO_free(network_bio_);
            network_bio_ = nullptr;
        }
        if (stream_) stream_->dispose();
    }

    const NativeLoopPtr& loop() const { return loop_; }

    // ---- loop-thread initiations ----

    void start_handshake(net::HandshakeRole role, IConnection::HandshakeHandler handler) {
        loop_->assert_loop_thread("tls async_handshake");
        if (disposed_ || close_requested_) {
            post_handshake(std::make_error_code(std::errc::bad_file_descriptor), std::move(handler));
            return;
        }
        if (handshake_done_) {
            post_handshake(std::error_code{}, std::move(handler));
            return;
        }
        if (handshake_pending_) throw std::logic_error("TLS handshake already pending");
        if (role != role_) {
            post_handshake(std::make_error_code(std::errc::invalid_argument), std::move(handler));
            return;
        }
        handshake_pending_ = true;
        handshake_handler_ = std::move(handler);
        pump();
    }

    void start_read(net::MutableBuffer buffer, IConnection::ReadHandler handler) {
        loop_->assert_loop_thread("tls async_read");
        if (disposed_ || close_requested_) {
            post_read(std::make_error_code(std::errc::bad_file_descriptor), 0, std::move(handler));
            return;
        }
        if (!handshake_done_) {
            post_read(std::make_error_code(std::errc::not_connected), 0, std::move(handler));
            return;
        }
        if (read_pending_) throw std::logic_error("TLS connection already has a pending read");
        read_pending_ = true;
        read_buffer_ = buffer;
        read_handler_ = std::move(handler);
        pump();
    }

    void start_write(const void* data, std::size_t size, IConnection::WriteHandler handler) {
        loop_->assert_loop_thread("tls async_write");
        if (disposed_ || close_requested_) {
            post_write(std::make_error_code(std::errc::bad_file_descriptor), 0, std::move(handler));
            return;
        }
        if (!handshake_done_) {
            post_write(std::make_error_code(std::errc::not_connected), 0, std::move(handler));
            return;
        }
        if (write_pending_) throw std::logic_error("TLS connection already has a pending write");
        write_pending_ = true;
        write_offset_ = 0;
        write_data_.assign(static_cast<const char*>(data), size);
        write_handler_ = std::move(handler);
        pump();
    }

    void start_cancel() {
        loop_->assert_loop_thread("tls cancel");
        if (disposed_) return;
        if (stream_) stream_->initiate_cancel();
        // cancel() must not close: the connection stays usable afterwards.
        cancel_pending(std::make_error_code(std::errc::operation_canceled));
    }

    // close() = cancel pending ops, then best-effort close_notify: SSL_shutdown
    // phase 1 queues it behind any unsent ciphertext; the flush goes out over
    // the plain stream and only then the socket closes. The second half
    // (waiting for the peer's close_notify) is skipped — teardown wins.
    // Note the plain stream's `open` flag must stay true until teardown: the
    // close_notify flush is an ordinary stream write.
    void start_close() {
        loop_->assert_loop_thread("tls close");
        if (close_started_ || disposed_) return;
        close_started_ = true;
        cancel_pending(std::make_error_code(std::errc::operation_canceled));
        if (stream_->fd < 0) return;
        SSL_shutdown(ssl_);
        drain_write_bio();
        if (outbox_.empty()) {
            teardown_plain();
            return;
        }
        net_write_armed_ = true;
        auto self = shared_from_this();
        stream_->initiate_write(outbox_.data(), outbox_.size(),
                                [self](const std::error_code& error, std::size_t bytes) {
                                    self->on_network_flush(error, bytes);
                                });
    }

    // close() from any thread: is_open() flips immediately; teardown (and the
    // close_notify flush) still happens on the loop.
    void request_close() { close_requested_.store(true, std::memory_order_release); }

    bool is_open() const {
        return stream_ && stream_->is_open() && !close_requested_.load(std::memory_order_acquire);
    }
    uint16_t local_port() const { return stream_ ? stream_->local_port : 0; }
    std::string remote_ip() const { return stream_ ? stream_->remote_ip : std::string(); }

    std::string diagnostic() const {
        std::lock_guard<std::mutex> lock(diagnostic_mutex_);
        return diagnostic_;
    }

private:
    // ---- pump: drive the SSL state machine until it blocks ----

    void pump() {
        if (dead_ || close_started_ || disposed_) return;
        if (handshake_pending_ && !handshake_done_) pump_handshake();
        if (dead_ || close_started_ || disposed_) return;
        if (handshake_done_) {
            pump_write();
            if (dead_ || close_started_ || disposed_) return;
            pump_read();
            if (dead_ || close_started_ || disposed_) return;
        }
        if (BIO_pending(network_bio_) > 0) arm_network_flush();
    }

    void pump_handshake() {
        const int rc = SSL_do_handshake(ssl_);
        if (rc == 1) {
            handshake_done_ = true;
            post_handshake(std::error_code{}, std::move(handshake_handler_));
            handshake_pending_ = false;
            return;
        }
        if (rc == 0) { // peer close_notify mid-handshake: premature close
            record_diagnostic("handshake");
            post_handshake(tls_error(TlsErrc::stream_truncated), std::move(handshake_handler_));
            handshake_pending_ = false;
            dead_ = true;
            teardown_plain();
            return;
        }
        const int error = SSL_get_error(ssl_, rc);
        switch (error) {
        case SSL_ERROR_WANT_READ:
            if (eof_seen_) fail_handshake(tls_error(TlsErrc::stream_truncated));
            else arm_network_read();
            return;
        case SSL_ERROR_WANT_WRITE:
            arm_network_flush();
            return;
        case SSL_ERROR_SSL:
        case SSL_ERROR_SYSCALL:
            record_diagnostic("handshake");
            fail_handshake(map_ssl_error(error));
            return;
        default:
            record_diagnostic("handshake");
            fail_handshake(std::make_error_code(std::errc::protocol_error));
        }
    }

    void fail_handshake(const std::error_code& error) {
        post_handshake(error, std::move(handshake_handler_));
        handshake_pending_ = false;
        dead_ = true;
        teardown_plain();
    }

    void pump_write() {
        if (!write_pending_) return;
        if (write_offset_ >= write_data_.size()) {
            post_write(std::error_code{}, write_data_.size(), std::move(write_handler_));
            write_pending_ = false;
            return;
        }
        if (shutdown_received_) {
            post_write(make_error_code(net::IoErrc::end_of_stream), write_offset_,
                       std::move(write_handler_));
            write_pending_ = false;
            return;
        }
        const int rc = SSL_write(ssl_, write_data_.data() + write_offset_,
                                 write_data_.size() - write_offset_);
        if (rc > 0) {
            write_offset_ += static_cast<std::size_t>(rc);
            if (write_offset_ >= write_data_.size()) {
                post_write(std::error_code{}, write_data_.size(), std::move(write_handler_));
                write_pending_ = false;
            }
            return;
        }
        const int error = SSL_get_error(ssl_, rc);
        switch (error) {
        case SSL_ERROR_WANT_READ:
            if (eof_seen_) {
                post_write(tls_error(TlsErrc::stream_truncated), write_offset_,
                           std::move(write_handler_));
                write_pending_ = false;
                dead_ = true;
                teardown_plain();
            } else {
                arm_network_read();
            }
            return;
        case SSL_ERROR_WANT_WRITE:
            return; // ciphertext flush armed at the end of pump()
        case SSL_ERROR_ZERO_RETURN: // peer close_notify: the write cannot finish
            shutdown_received_ = true;
            post_write(make_error_code(net::IoErrc::end_of_stream), write_offset_,
                       std::move(write_handler_));
            write_pending_ = false;
            return;
        case SSL_ERROR_SSL:
        case SSL_ERROR_SYSCALL: {
            record_diagnostic("write");
            post_write(map_ssl_error(error), write_offset_, std::move(write_handler_));
            write_pending_ = false;
            dead_ = true;
            teardown_plain();
            return;
        }
        default:
            post_write(std::make_error_code(std::errc::protocol_error), write_offset_,
                       std::move(write_handler_));
            write_pending_ = false;
            dead_ = true;
            teardown_plain();
        }
    }

    void pump_read() {
        if (!read_pending_) return;
        if (read_buffer_.size() == 0) { // zero-byte read is a success with 0
            post_read(std::error_code{}, 0, std::move(read_handler_));
            read_pending_ = false;
            return;
        }
        if (shutdown_received_) {
            post_read(make_error_code(net::IoErrc::end_of_stream), 0, std::move(read_handler_));
            read_pending_ = false;
            return;
        }
        const int rc = SSL_read(ssl_, read_buffer_.data(), read_buffer_.size());
        if (rc > 0) {
            post_read(std::error_code{}, static_cast<std::size_t>(rc), std::move(read_handler_));
            read_pending_ = false;
            return;
        }
        const int error = SSL_get_error(ssl_, rc);
        switch (error) {
        case SSL_ERROR_WANT_READ:
            if (eof_seen_) {
                // The anti-truncation rule: ciphertext that just stops, without
                // a close_notify, must never pass as EOF.
                record_diagnostic("read");
                post_read(tls_error(TlsErrc::stream_truncated), 0, std::move(read_handler_));
                read_pending_ = false;
                dead_ = true;
                teardown_plain();
            } else {
                arm_network_read();
            }
            return;
        case SSL_ERROR_WANT_WRITE:
            arm_network_flush(); // e.g. a KeyUpdate response must go out first
            return;
        case SSL_ERROR_ZERO_RETURN: // close_notify: the one clean-EOF shape
            shutdown_received_ = true;
            post_read(make_error_code(net::IoErrc::end_of_stream), 0, std::move(read_handler_));
            read_pending_ = false;
            return;
        case SSL_ERROR_SSL:
        case SSL_ERROR_SYSCALL: {
            record_diagnostic("read");
            const std::error_code mapped = map_ssl_error(error);
            post_read(mapped, 0, std::move(read_handler_));
            read_pending_ = false;
            dead_ = true;
            teardown_plain();
            return;
        }
        default:
            post_read(std::make_error_code(std::errc::protocol_error), 0, std::move(read_handler_));
            read_pending_ = false;
            dead_ = true;
            teardown_plain();
        }
    }

    // SSL_ERROR_SSL / SSL_ERROR_SYSCALL after a failed SSL_* call.
    std::error_code map_ssl_error(int error) const {
        if (SSL_get_verify_result(ssl_) != X509_V_OK)
            return tls_error(TlsErrc::certificate_verification);
        if (error == SSL_ERROR_SSL) {
            const unsigned long code = ERR_peek_last_error();
#ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
            if (ERR_GET_LIB(code) == ERR_LIB_SSL &&
                ERR_GET_REASON(code) == SSL_R_UNEXPECTED_EOF_WHILE_READING)
                return tls_error(TlsErrc::stream_truncated);
#endif
            if (ERR_GET_LIB(code) == ERR_LIB_SSL &&
                ERR_GET_REASON(code) == SSL_R_CERTIFICATE_VERIFY_FAILED)
                return tls_error(TlsErrc::certificate_verification);
            return std::make_error_code(std::errc::protocol_error);
        }
        // SYSCALL: errno 0 means the socket hit EOF mid-record.
        return errno == 0 ? tls_error(TlsErrc::stream_truncated)
                          : std::error_code(errno, std::generic_category());
    }

    // ---- network (plain-stream) side ----

    void arm_network_read() {
        if (net_read_armed_ || eof_seen_ || dead_ || close_started_ || disposed_) return;
        if (!stream_->is_open()) return;
        net_read_armed_ = true;
        auto self = shared_from_this();
        stream_->initiate_read(
            net::buffer(net_read_buffer_.data(), net_read_buffer_.size()),
            [self](const std::error_code& error, std::size_t bytes) {
                self->on_network_read(error, bytes);
            });
    }

    void on_network_read(const std::error_code& error, std::size_t bytes) {
        net_read_armed_ = false;
        if (dead_ || close_started_ || disposed_) return;
        // cancel()/loop shutdown surface here too; the pending app op must
        // still complete exactly once with operation_canceled.
        if (error == std::make_error_code(std::errc::operation_canceled)) {
            cancel_pending(error);
            return;
        }
        if (error == make_error_code(net::IoErrc::end_of_stream)) {
            eof_seen_ = true; // FIN without close_notify: pump() maps the outcome
            pump();
            return;
        }
        if (error) {
            fail_all(error);
            return;
        }
        if (bytes > 0) {
            const int wrote = BIO_write(network_bio_, net_read_buffer_.data(),
                                        static_cast<int>(bytes));
            if (wrote != static_cast<int>(bytes)) {
                record_diagnostic("network read");
                fail_all(std::make_error_code(std::errc::protocol_error));
                return;
            }
        }
        pump();
    }

    void drain_write_bio() {
        char chunk[kNetworkChunk];
        for (;;) {
            const int got = BIO_read(network_bio_, chunk, sizeof(chunk));
            if (got <= 0) break;
            outbox_.append(chunk, static_cast<std::size_t>(got));
        }
    }

    void arm_network_flush() {
        if (net_write_armed_ || dead_ || close_started_ || disposed_) return;
        if (!stream_->is_open()) return;
        drain_write_bio();
        if (outbox_.empty()) return;
        net_write_armed_ = true;
        auto self = shared_from_this();
        stream_->initiate_write(outbox_.data(), outbox_.size(),
                                [self](const std::error_code& error, std::size_t bytes) {
                                    self->on_network_flush(error, bytes);
                                });
    }

    void on_network_flush(const std::error_code& error, std::size_t bytes) {
        net_write_armed_ = false;
        if (dead_ || disposed_) return;
        if (close_started_) {
            // Closing: this pass may have carried only the ciphertext that was
            // in flight when close() ran; one more pass sends the close_notify
            // that SSL_shutdown appended afterwards. An error ends it early.
            drain_write_bio();
            if (!error && !outbox_.empty() && stream_->fd >= 0) {
                net_write_armed_ = true;
                auto self = shared_from_this();
                stream_->initiate_write(outbox_.data(), outbox_.size(),
                                        [self](const std::error_code& flush_error,
                                               std::size_t flush_bytes) {
                                            self->on_network_flush(flush_error, flush_bytes);
                                        });
                return;
            }
            teardown_plain();
            return;
        }
        if (error == std::make_error_code(std::errc::operation_canceled)) {
            // An aborted ciphertext flush may leave an app write pending; it
            // must complete canceled exactly once as well.
            cancel_pending(error);
            return;
        }
        if (error) {
            fail_all(error);
            return;
        }
        if (bytes > 0 && bytes <= outbox_.size()) outbox_.erase(0, bytes);
        pump();
    }

    // ---- completion / failure helpers ----

    // Socket-level failure: everything pending fails with the same error and
    // the stream closes. Single completion per pending op.
    void fail_all(const std::error_code& error) {
        dead_ = true;
        cancel_pending(error);
        teardown_plain();
    }

    void cancel_pending(const std::error_code& error) {
        if (handshake_pending_) {
            handshake_pending_ = false;
            post_handshake(error, std::move(handshake_handler_));
        }
        if (read_pending_) {
            read_pending_ = false;
            post_read(error, 0, std::move(read_handler_));
        }
        if (write_pending_) {
            write_pending_ = false;
            post_write(error, write_offset_, std::move(write_handler_));
        }
    }

    void post_handshake(const std::error_code& error, IConnection::HandshakeHandler handler) {
        loop_->post([handler = std::move(handler), error] { handler(error); });
    }

    void post_read(const std::error_code& error, std::size_t bytes,
                   IConnection::ReadHandler handler) {
        read_buffer_ = net::MutableBuffer{};
        loop_->post([handler = std::move(handler), error, bytes] { handler(error, bytes); });
    }

    void post_write(const std::error_code& error, std::size_t bytes,
                    IConnection::WriteHandler handler) {
        loop_->post([handler = std::move(handler), error, bytes] { handler(error, bytes); });
    }

    void teardown_plain() {
        if (stream_) stream_->initiate_close();
    }

    void record_diagnostic(const char* where) {
        std::string trace = drain_error_stack();
        std::lock_guard<std::mutex> lock(diagnostic_mutex_);
        diagnostic_ = where;
        if (!trace.empty()) diagnostic_ += ": " + trace;
    }

    NativeLoopPtr loop_;
    std::shared_ptr<StreamState> stream_;
    SSL* ssl_ = nullptr;              // owns the internal end of the BIO pair
    BIO* network_bio_ = nullptr;      // this session's end: ciphertext in/out
    net::HandshakeRole role_;
    std::string server_name_;

    bool handshake_done_ = false;
    bool shutdown_received_ = false; // peer's close_notify seen
    bool eof_seen_ = false;          // plain FIN seen, close_notify may still come
    bool close_started_ = false;     // loop thread: close teardown begun
    bool disposed_ = false;
    bool dead_ = false; // broken session: no further TLS progress possible
    std::atomic<bool> close_requested_{false}; // set synchronously by close()

    bool handshake_pending_ = false;
    IConnection::HandshakeHandler handshake_handler_;

    bool read_pending_ = false;
    net::MutableBuffer read_buffer_{};
    IConnection::ReadHandler read_handler_;

    bool write_pending_ = false;
    std::string write_data_; // copy: completion never depends on caller storage
    std::size_t write_offset_ = 0;
    IConnection::WriteHandler write_handler_;

    std::vector<char> net_read_buffer_ = std::vector<char>(kNetworkChunk);
    bool net_read_armed_ = false;
    std::string outbox_; // ciphertext drained from the write BIO, awaiting send
    bool net_write_armed_ = false;

    mutable std::mutex diagnostic_mutex_;
    std::string diagnostic_;
};

// IConnection face over a TlsSession; returned by upgrade_tls, SSL listeners
// and the client STARTTLS path. The executor is the wrapped stream's loop.
class TlsConnection final : public IConnection {
public:
    TlsConnection(NativeLoopPtr loop, std::shared_ptr<TlsSession> session)
        : loop_(std::move(loop)), session_(std::move(session)) {}

    ~TlsConnection() override {
        auto session = std::move(session_);
        if (!session) return;
        if (session->loop()->is_loop_thread()) {
            // Already on the loop (e.g. a grave closure destroying us while
            // drain_graves holds its mutex): enqueueing would self-deadlock.
            session->dispose();
            return;
        }
        session->loop()->defer_destroy([session] { session->dispose(); });
    }

    std::shared_ptr<net::IExecutor> executor() override { return loop_; }

    void async_read(net::MutableBuffer buffer, ReadHandler handler) override {
        run_on_loop([session = session_, buffer, handler = std::move(handler)]() mutable {
            session->start_read(buffer, std::move(handler));
        });
    }

    void async_write(net::ConstBuffer buffer, WriteHandler handler) override {
        run_on_loop([session = session_, buffer, handler = std::move(handler)]() mutable {
            session->start_write(buffer.data(), buffer.size(), std::move(handler));
        });
    }

    void async_handshake(net::HandshakeRole role, HandshakeHandler handler) override {
        run_on_loop([session = session_, role, handler = std::move(handler)]() mutable {
            session->start_handshake(role, std::move(handler));
        });
    }

    void close() override {
        session_->request_close();
        run_on_loop([session = session_] { session->start_close(); });
    }

    void cancel() override {
        run_on_loop([session = session_] { session->start_cancel(); });
    }

    bool is_open() const override { return session_->is_open(); }
    uint16_t get_local_port() const override { return session_->local_port(); }
    std::string get_remote_ip() const override { return session_->remote_ip(); }
    // The raw fd bypasses the TLS layer; hide it (IConnection contract note).
    int native_handle() const override { return -1; }
    std::string get_handshake_diagnostic() const override { return session_->diagnostic(); }

private:
    void run_on_loop(std::function<void()> action) {
        if (loop_->is_loop_thread()) action();
        else loop_->post(std::move(action));
    }

    NativeLoopPtr loop_;
    std::shared_ptr<TlsSession> session_;
};

class NativeTlsHooks final : public tls_hook::Hooks {
public:
    Context make_server_context(const net::TlsConfig& config) override {
        auto bundle = std::make_shared<NativeTlsContext>();
        bundle->ctx = build_server_context(config);
        return bundle;
    }

    Context make_client_context(const net::ClientTlsConfig& config) override {
        auto bundle = std::make_shared<NativeTlsContext>();
        bundle->ctx = build_client_context(config);
        bundle->server_name = config.server_name;
        return bundle;
    }

    std::unique_ptr<IConnection> wrap(const NativeLoopPtr& loop,
                                      const std::shared_ptr<StreamState>& stream,
                                      const Context& context,
                                      net::HandshakeRole role) override {
        const auto bundle = std::static_pointer_cast<NativeTlsContext>(context);
        auto session = std::make_shared<TlsSession>(loop, stream, bundle->ctx, role,
                                                    bundle->server_name);
        return std::make_unique<TlsConnection>(loop, std::move(session));
    }
};

NativeTlsHooks g_native_tls_hooks;

} // namespace

bool enable_native_tls() {
    static std::once_flag once;
    std::call_once(once, [] { tls_hook::set_hooks(&g_native_tls_hooks); });
    return true;
}

} // namespace pr::net::native
