#pragma once

#include "framework/net/client_transport.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include "mail_system/back/smtp_client/secret.h"
#include <string>
#include <string_view>
#include <vector>

namespace mail_system::smtp {

using TransportProvider = std::function<std::shared_ptr<pr::net::IClientTransport>()>;

enum class TlsMode { None, Implicit, StartTlsRequired };
enum class Outcome { Ready, Accepted, Rejected, TemporaryFailure, AuthFailed,
                     ProtocolError, OutcomeUnknown, Cancelled };

struct RecipientResult {
    std::string address;
    int code = 0;
};
struct Result {
    Outcome outcome = Outcome::TemporaryFailure;
    int code = 0;
    std::string stage;
    std::string error;
    std::vector<RecipientResult> recipients;
    bool reusable = false;
};

struct ConnectOptions {
    std::string host;
    uint16_t port = 25;
    std::string helo = "localhost";
    TlsMode tls = TlsMode::None;
    // TLS 始终验证证书链和主机名；私有 CA 通过数据配置注入。
    pr::net::ClientTlsConfig tls_config;
    std::string tls_server_name; // 空时使用 host
    std::string auth_user;
    SecretPtr password; // 非空则要求服务器通告 AUTH PLAIN
    std::chrono::milliseconds timeout = std::chrono::seconds(30);
};
struct Message {
    std::string sender; // 允许空 reverse-path（退信）
    std::vector<std::string> recipients;
    std::shared_ptr<const std::string> raw;
    std::chrono::milliseconds timeout = std::chrono::seconds(30);
};

// 一条固定 executor/strand 上的连接；调用可来自任意线程，回调在该 strand。
// 每次只允许一个操作；调用方负责排队、连接池、重试和业务策略。
// RCPT 默认全有或全无，失败关闭连接；只有 DATA 最终 250 才返回 Accepted。
class ClientSession {
public:
    using Complete = std::function<void(Result)>;
    explicit ClientSession(std::shared_ptr<pr::net::IClientTransport> transport);
    ~ClientSession();
    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;
    void async_connect(ConnectOptions options, Complete complete);
    void async_send(Message message, Complete complete);
    void async_quit(Complete complete = {});
    void close();
    bool is_open() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace mail_system::smtp
