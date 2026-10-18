#include "mail_system/back/smtp_client/client_session.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/common/smtp_wire.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <atomic>
#include <cctype>
#include <istream>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <sstream>

namespace mail_system::smtp {
namespace asio = boost::asio;
using asio::ip::tcp;
using Error = boost::system::error_code;

namespace {
bool atom(std::string_view value) {
    if (value.empty() || value.size() > 998) return false;
    for (unsigned char c : value)
        if (c <= 32 || c >= 127 || c == '<' || c == '>') return false;
    return true;
}
std::string upper(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}
} // namespace

struct ClientSession::Impl : std::enable_shared_from_this<Impl> {
    struct Connection {
        explicit Connection(asio::any_io_executor ex) : socket(ex), resolver(ex), replies(64 * 1024) {}
        std::string endpoint;
        tcp::socket socket;
        tcp::resolver resolver;
        std::shared_ptr<asio::ssl::context> context;
        std::unique_ptr<asio::ssl::stream<tcp::socket>> tls;
        asio::streambuf replies;
        tcp::socket& lowest() { return tls ? tls->next_layer() : socket; }
        void close() {
            resolver.cancel();
            Error ignored;
            lowest().close(ignored);
        }
    };
    struct Operation {
        Operation(asio::any_io_executor ex, std::shared_ptr<Connection> conn, Complete cb)
            : timer(ex), connection(std::move(conn)), complete(std::move(cb)) {}
        ~Operation() { cleanse(); }
        void cleanse() {
            if (secret_write && !wire.empty()) OPENSSL_cleanse(wire.data(), wire.size());
        }
        asio::steady_timer timer;
        std::shared_ptr<Connection> connection; // cancelled I/O 结束前持活 socket 和 buffer
        Complete complete;
        ConnectOptions options;
        Message message;
        Result result;
        std::string stage, wire;
        std::vector<std::string> reply;
        size_t reply_bytes = 0, recipient = 0;
        int reply_code = 0;
        bool done = false, body_started = false, secret_write = false;
    };
    explicit Impl(asio::any_io_executor ex) : executor(asio::make_strand(ex)) {}
    asio::any_io_executor executor;
    std::shared_ptr<Connection> connection;
    std::shared_ptr<Operation> current;
    std::atomic<bool> open{false};

    void disconnect() {
        open.store(false);
        if (connection) connection->close();
        connection.reset();
    }
    void finish(const std::shared_ptr<Operation>& op, Outcome outcome, int code = 0,
                std::string error = {}, bool reusable = false) {
        if (op->done) return;
        op->done = true;
        op->timer.cancel();
        op->options.password.reset();
        op->result.outcome = outcome;
        op->result.code = code;
        op->result.stage = op->stage;
        op->result.error = std::move(error);
        op->result.reusable = reusable;
        if (!reusable) disconnect();
        if (outcome != Outcome::Ready && outcome != Outcome::Accepted)
            LOG_AT("SMTPCLIENT", warn, "operation failed: endpoint={} stage={} code={} outcome={} reason={}",
                   op->connection ? op->connection->endpoint : "none", op->stage, code, static_cast<int>(outcome), op->result.error);
        current.reset();
        auto cb = std::move(op->complete);
        if (cb) cb(std::move(op->result));
    }
    void io_failed(const std::shared_ptr<Operation>& op, std::string reason) {
        finish(op, op->body_started ? Outcome::OutcomeUnknown : Outcome::TemporaryFailure,
               0, std::move(reason));
    }
    void protocol_failed(const std::shared_ptr<Operation>& op, std::string reason) {
        finish(op, op->body_started ? Outcome::OutcomeUnknown : Outcome::ProtocolError,
               op->reply_code, std::move(reason));
    }
    bool available(Complete& cb) {
        if (!current) return true;
        LOG_AT("SMTPCLIENT", warn, "operation rejected: connection busy stage={}", current->stage);
        if (cb) cb({Outcome::ProtocolError, 0, "busy", "operation already in progress", {}, false});
        return false;
    }
    void deadline(const std::shared_ptr<Operation>& op, std::chrono::milliseconds timeout) {
        op->timer.expires_after(timeout);
        auto self = shared_from_this();
        op->timer.async_wait([self, op](Error ec) {
            if (!ec && !op->done) self->io_failed(op, "deadline exceeded");
        });
    }
    void write(const std::shared_ptr<Operation>& op, std::string stage, std::string wire,
               bool secret = false) {
        op->stage = std::move(stage);
        op->wire = std::move(wire);
        op->secret_write = secret;
        auto self = shared_from_this();
        auto cb = [self, op](Error ec, size_t) {
            op->cleanse();
            op->wire.clear();
            op->secret_write = false;
            if (op->done) return;
            if (ec) { self->io_failed(op, ec.message()); return; }
            self->read_reply(op, true);
        };
        if (op->connection->tls) asio::async_write(*op->connection->tls, asio::buffer(op->wire), cb);
        else asio::async_write(op->connection->socket, asio::buffer(op->wire), cb);
    }
    void read_reply(const std::shared_ptr<Operation>& op, bool first = false) {
        if (first) { op->reply.clear(); op->reply_code = 0; op->reply_bytes = 0; }
        auto self = shared_from_this();
        auto cb = [self, op](Error ec, size_t count) {
            if (op->done) return;
            if (ec) { self->io_failed(op, ec.message()); return; }
            std::string line(count, '\0');
            std::istream input(&op->connection->replies);
            input.read(line.data(), static_cast<std::streamsize>(count));
            op->reply_bytes += count;
            const bool bare = count == 5; // RFC reply-code CRLF（无文本）
            if (count < 5 || op->reply_bytes > 64 * 1024 || line[0] < '2' || line[0] > '5' ||
                !std::isdigit(static_cast<unsigned char>(line[1])) ||
                !std::isdigit(static_cast<unsigned char>(line[2])) ||
                (!bare && line[3] != ' ' && line[3] != '-')) {
                self->protocol_failed(op, "malformed reply"); return;
            }
            const int code = (line[0] - '0') * 100 + (line[1] - '0') * 10 + line[2] - '0';
            if (op->reply_code && op->reply_code != code) {
                self->protocol_failed(op, "inconsistent multiline reply"); return;
            }
            op->reply_code = code;
            op->reply.push_back(bare ? "" : line.substr(4, count - 6));
            if (!bare && line[3] == '-') { self->read_reply(op); return; }
            self->on_reply(op, code);
        };
        if (op->connection->tls) asio::async_read_until(*op->connection->tls, op->connection->replies, "\r\n", cb);
        else asio::async_read_until(op->connection->socket, op->connection->replies, "\r\n", cb);
    }
    bool capability(const std::shared_ptr<Operation>& op, const std::string& name) {
        for (const auto& line : op->reply) if (upper(line) == name) return true;
        return false;
    }
    bool auth_plain(const std::shared_ptr<Operation>& op) {
        for (const auto& line : op->reply) {
            auto cap = upper(line);
            if (cap.rfind("AUTH ", 0) != 0 && cap.rfind("AUTH=", 0) != 0) continue;
            std::istringstream input(cap.substr(5));
            std::string mechanism;
            while (input >> mechanism) if (mechanism == "PLAIN") return true;
        }
        return false;
    }
    void ehlo(const std::shared_ptr<Operation>& op) { write(op, "ehlo", "EHLO " + op->options.helo + "\r\n"); }
    void handshake(const std::shared_ptr<Operation>& op, bool implicit) {
        op->stage = "tls";
        auto conn = op->connection;
        try {
            conn->context = op->options.tls_context;
            if (!conn->context) {
                conn->context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
                conn->context->set_default_verify_paths();
            }
            conn->tls = std::make_unique<asio::ssl::stream<tcp::socket>>(std::move(conn->socket), *conn->context);
            if (!SSL_set_min_proto_version(conn->tls->native_handle(), TLS1_2_VERSION)) {
                protocol_failed(op, "cannot set minimum TLS version"); return;
            }
            conn->tls->set_verify_mode(asio::ssl::verify_peer);
            const auto name = op->options.tls_server_name.empty() ? op->options.host : op->options.tls_server_name;
            conn->tls->set_verify_callback(asio::ssl::host_name_verification(name));
            if (!SSL_set_tlsext_host_name(conn->tls->native_handle(), name.c_str())) {
                protocol_failed(op, "cannot set TLS server name"); return;
            }
            auto self = shared_from_this();
            conn->tls->async_handshake(asio::ssl::stream_base::client, [self, op, implicit](Error ec) {
                if (op->done) return;
                if (ec) { self->io_failed(op, "TLS: " + ec.message()); return; }
                if (implicit) { op->stage = "connect"; self->read_reply(op, true); }
                else self->ehlo(op);
            });
        } catch (const std::exception& e) { io_failed(op, e.what()); }
    }
    void on_reply(const std::shared_ptr<Operation>& op, int code) {
        const auto stage = op->stage;
        if (stage == "recipient") op->result.recipients.push_back({op->message.recipients[op->recipient], code});
        if (code >= 400) {
            // 明确收到 DATA 拒绝时结果已知；仅丢失/损坏最终确认才 Unknown。
            auto outcome = ((stage == "auth" || stage == "credentials") && code == 535) ? Outcome::AuthFailed :
                           code < 500 ? Outcome::TemporaryFailure : Outcome::Rejected;
            finish(op, outcome, code, "SMTP peer rejected operation"); return;
        }
        const int expected = stage == "connect" || stage == "starttls" ? 220 :
                             stage == "auth" ? 334 :
                             stage == "credentials" ? 235 : stage == "data" ? 354 :
                             stage == "quit" ? 221 : 250;
        if (code != expected && !(stage == "recipient" && code == 251)) {
            protocol_failed(op, "unexpected reply code"); return;
        }
        if (stage == "connect") ehlo(op);
        else if (stage == "ehlo") {
            if (op->options.tls == TlsMode::StartTlsRequired && !op->connection->tls) {
                if (!capability(op, "STARTTLS")) { protocol_failed(op, "STARTTLS not advertised"); return; }
                write(op, "starttls", "STARTTLS\r\n");
            } else if (op->options.password) {
                if (!auth_plain(op)) { protocol_failed(op, "AUTH PLAIN not advertised"); return; }
                write(op, "auth", "AUTH PLAIN\r\n");
            } else { open.store(true); finish(op, Outcome::Ready, code, {}, true); }
        } else if (stage == "starttls") {
            // 不允许把升级前的明文缓冲当 TLS 后的响应。
            if (op->connection->replies.size()) { protocol_failed(op, "buffered bytes before TLS upgrade"); return; }
            handshake(op, false);
        } else if (stage == "auth") {
            auto password = op->options.password->view();
            std::string plain = std::string(1, '\0') + op->options.auth_user + '\0';
            plain.append(password.data(), password.size());
            const size_t encoded_size = 4 * ((plain.size() + 2) / 3);
            std::string encoded(encoded_size + 1, '\0');
            EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                            reinterpret_cast<const unsigned char*>(plain.data()), static_cast<int>(plain.size()));
            OPENSSL_cleanse(plain.data(), plain.size());
            encoded.resize(encoded_size);
            encoded += "\r\n";
            write(op, "credentials", std::move(encoded), true);
        } else if (stage == "credentials") { open.store(true); finish(op, Outcome::Ready, code, {}, true); }
        else if (stage == "mail") recipient(op);
        else if (stage == "recipient") {
            if (++op->recipient < op->message.recipients.size()) recipient(op);
            else write(op, "data", "DATA\r\n");
        } else if (stage == "data") {
            op->body_started = true;
            write(op, "body", data_frame(*op->message.raw));
        } else if (stage == "body") finish(op, Outcome::Accepted, code, {}, true);
        else if (stage == "quit") finish(op, Outcome::Ready, code);
    }
    void recipient(const std::shared_ptr<Operation>& op) {
        write(op, "recipient", "RCPT TO:<" + op->message.recipients[op->recipient] + ">\r\n");
    }
    void connect(ConnectOptions options, Complete cb) {
        if (!available(cb)) return;
        disconnect();
        connection = std::make_shared<Connection>(executor);
        auto op = std::make_shared<Operation>(executor, connection, std::move(cb));
        current = op;
        op->options = std::move(options);
        op->stage = "connect";
        auto& opts = op->options;
        connection->endpoint = opts.host + ":" + std::to_string(opts.port);
        if (!atom(opts.host) || !atom(opts.helo) || !opts.port || opts.timeout.count() <= 0 ||
            (opts.password && (!atom(opts.auth_user) || opts.password->view().empty() ||
             opts.password->view().size() > 4096 || opts.password->view().find('\0') != std::string_view::npos))) {
            finish(op, Outcome::ProtocolError, 0, "invalid connection options"); return;
        }
        deadline(op, opts.timeout);
        auto self = shared_from_this();
        op->connection->resolver.async_resolve(opts.host, std::to_string(opts.port),
            [self, op](Error ec, tcp::resolver::results_type endpoints) {
                if (op->done) return;
                if (ec) { self->io_failed(op, ec.message()); return; }
                asio::async_connect(op->connection->socket, endpoints, [self, op](Error ec, tcp::endpoint) {
                    if (op->done) return;
                    if (ec) { self->io_failed(op, ec.message()); return; }
                    if (op->options.tls == TlsMode::Implicit) self->handshake(op, true);
                    else self->read_reply(op, true);
                });
            });
    }
    void send(Message message, Complete cb) {
        if (!available(cb)) return;
        auto op = std::make_shared<Operation>(executor, connection, std::move(cb));
        current = op;
        op->message = std::move(message);
        op->stage = "mail";
        if (!open.load() || !connection) { finish(op, Outcome::TemporaryFailure, 0, "connection not ready"); return; }
        const auto& msg = op->message;
        bool valid = (msg.sender.empty() || atom(msg.sender)) && !msg.recipients.empty() &&
                     msg.recipients.size() <= 100 && msg.raw && !msg.raw->empty() && msg.timeout.count() > 0;
        for (const auto& rcpt : msg.recipients) valid = valid && atom(rcpt);
        if (!valid) { finish(op, Outcome::ProtocolError, 0, "invalid message envelope or payload"); return; }
        deadline(op, msg.timeout);
        write(op, "mail", "MAIL FROM:<" + msg.sender + ">\r\n");
    }
    void quit(Complete cb) {
        if (!available(cb)) return;
        auto op = std::make_shared<Operation>(executor, connection, std::move(cb));
        current = op;
        op->stage = "quit";
        if (!connection || !open.load()) { finish(op, Outcome::Ready); return; }
        deadline(op, std::chrono::seconds(5));
        write(op, "quit", "QUIT\r\n");
    }
    void close() {
        auto op = current;
        if (op) finish(op, op->body_started ? Outcome::OutcomeUnknown : Outcome::Cancelled,
                       0, "connection closed by caller");
        else disconnect();
    }
};

ClientSession::ClientSession(asio::any_io_executor executor) : impl_(std::make_shared<Impl>(executor)) {}
ClientSession::~ClientSession() { close(); }
void ClientSession::async_connect(ConnectOptions options, Complete cb) {
    auto impl = impl_;
    asio::post(impl->executor, [impl, options = std::move(options), cb = std::move(cb)]() mutable {
        impl->connect(std::move(options), std::move(cb));
    });
}
void ClientSession::async_send(Message message, Complete cb) {
    auto impl = impl_;
    asio::post(impl->executor, [impl, message = std::move(message), cb = std::move(cb)]() mutable {
        impl->send(std::move(message), std::move(cb));
    });
}
void ClientSession::async_quit(Complete cb) {
    auto impl = impl_;
    asio::post(impl->executor, [impl, cb = std::move(cb)]() mutable { impl->quit(std::move(cb)); });
}
void ClientSession::close() {
    auto impl = impl_;
    asio::post(impl->executor, [impl] { impl->close(); });
}
bool ClientSession::is_open() const { return impl_->open.load(); }
} // namespace mail_system::smtp
