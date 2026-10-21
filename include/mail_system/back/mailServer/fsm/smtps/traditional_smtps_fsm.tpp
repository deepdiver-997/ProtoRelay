#ifndef TRADITIONAL_SMTPS_FSM_TPP
#define TRADITIONAL_SMTPS_FSM_TPP

#include "mail_system/back/algorithm/smtp_utils.h"
#include "mail_system/back/common/mail_crypto.h"
#include "mail_system/back/common/password_rehash.h"
#include "mail_system/back/common/safe_parse.h"
#include "mail_system/back/inbound/inbound_verifier.h"
#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/common/mime_parser.h"
#include <filesystem>
#include "mail_system/back/mailServer/session/smtps_session.h"
#include "mail_system/back/mailServer/smtps_server.h"
#include <arpa/inet.h>

namespace mail_system {

namespace {

// trusted_relay_networks 匹配：条目为精确 IP（含 IPv6）或 IPv4 CIDR。
// 精确条目走字符串比较；CIDR 用 inet_pton 转主机序后按前缀比较，解析失败则跳过该条目。
// 前缀用严格解析：这段跑在 io 线程的 RCPT handler 里，std::stoi 遇到 "10.0.0.0/" 或
// "/x" 这类配置笔误会抛 invalid_argument 穿出 handler，io 线程的 run() 退出后不再重进，
// 挂在该 io_context 上的全部连接静默僵死；而宽松解析把坏前缀当成 0 = 信任全网 = 开放中继。
inline bool ip_in_trusted_networks(const std::string& ip,
                                   const std::vector<std::string>& networks) {
    if (ip.empty() || networks.empty()) return false;
    for (const auto& entry : networks) {
        if (entry == ip) return true;
        const auto slash = entry.find('/');
        if (slash == std::string::npos) continue;

        uint64_t prefix = 0;
        if (!parse_uint(std::string_view(entry).substr(slash + 1), prefix) || prefix > 32) continue;

        struct in_addr base{}, addr{};
        if (inet_pton(AF_INET, entry.substr(0, slash).c_str(), &base) != 1 ||
            inet_pton(AF_INET, ip.c_str(), &addr) != 1) {
            continue;  // 非 IPv4 CIDR/IPv4 客户端：只可能被上面的精确匹配命中
        }
        const uint32_t mask = prefix == 0 ? 0u : ~0u << (32 - prefix);
        if ((ntohl(base.s_addr) & mask) == (ntohl(addr.s_addr) & mask)) return true;
    }
    return false;
}

// 密码校验见 common/bcrypt.h::verify_password_hash（bcrypt / MD5 / 明文自动识别，
// 三协议共用）；原先此处的文件内副本已并入。
} // namespace

// ========== 工具函数实现 ==========
template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::cleanup_streamed_attachments(SmtpsContext* ctx) {
    if (ctx) algorithm::cleanup_streamed_attachments(*ctx);
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::cleanup_mail_files(mail* m) {
    if (!m->body_path.empty()) std::remove(m->body_path.c_str());
    for (const auto& p : m->attachments)
        if (!p.filepath.empty()) std::remove(p.filepath.c_str());
}

// ========== 持久化函数实现 ==========
template <typename ConnectionType>
bool TraditionalSmtpsFsm<ConnectionType>::persist_mails_sync(
    SmtpsSession<ConnectionType>* session, std::string& error)
{
    if (!session->mail_) { LOG_SMTP_DETAIL_WARN("No mail to persist"); return true; }
    LOG_SMTP_DETAIL_DEBUG("Starting to persist 1 mail");
    auto& m = session->mail_;
    LOG_SMTP_DETAIL_DEBUG("Mail ID: {} Body size: {}", m->id, m->body.size());
    LOG_SMTP_DETAIL_DEBUG("Saving to file: {}", m->body_path);

    if (m->persist_status == mail_system::persist_storage::PersistStatus::SUCCESS) {
        LOG_SMTP_DETAIL_DEBUG("Mail already persisted, skipping");
        return true;
    } else {
        if (m->persist_status == mail_system::persist_storage::PersistStatus::PENDING) {
            LOG_SMTP_DETAIL_DEBUG("Mail persist status not started, skipping");
            m->persist_status = mail_system::persist_storage::PersistStatus::CANCELLED;
        }
        cleanup_mail_files(m.get());
        LOG_SMTP_DETAIL_DEBUG("Cleaned up previous mail files due to failed persist");
        return false;
    }
    LOG_SMTP_DETAIL_DEBUG("All mails persisted successfully");
    return true;
}

template <typename ConnectionType>
bool TraditionalSmtpsFsm<ConnectionType>::persist_and_reply(std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    auto* smtp_session = static_cast<SmtpsSession<ConnectionType>*>(session.get());
    std::string error;
    bool ok = persist_mails_sync(smtp_session, error);
    std::string reply = ok ? "221 Bye\r\n" : ("451 " + error + "\r\n");
    session->do_async_write(reply,
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
            if (ec) LOG_SMTP_DETAIL_ERROR("Error sending QUIT reply: {}", ec.message());
            auto timer = s->get_connection().executor()->make_timer();
            timer->expires_after(std::chrono::milliseconds(100));
            timer->async_wait([s, timer](const std::error_code& ec) mutable {
                if (!ec) s->close();
            });
        });
    return ok;
}

// ========== 初始化 ==========
template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::init_transition_table() {
    this->add_transition(SmtpsState::INIT, SmtpsEvent::CONNECT, SmtpsState::GREETING);
    this->add_transition(SmtpsState::WAIT_EHLO, SmtpsEvent::EHLO, SmtpsState::WAIT_AUTH);
    this->add_transition(SmtpsState::GREETING, SmtpsEvent::EHLO, SmtpsState::WAIT_AUTH);
    if constexpr (!std::is_same_v<ConnectionType, pr::SslConnection>)
        this->add_transition(SmtpsState::WAIT_AUTH, SmtpsEvent::STARTTLS, SmtpsState::INIT);
    this->add_transition(SmtpsState::WAIT_AUTH, SmtpsEvent::AUTH, SmtpsState::WAIT_AUTH_USERNAME);
    this->add_transition(SmtpsState::WAIT_AUTH_USERNAME, SmtpsEvent::AUTH, SmtpsState::WAIT_AUTH_PASSWORD);
    this->add_transition(SmtpsState::WAIT_AUTH_PASSWORD, SmtpsEvent::AUTH, SmtpsState::WAIT_MAIL_FROM);
    this->add_transition(SmtpsState::WAIT_AUTH, SmtpsEvent::EHLO, SmtpsState::WAIT_AUTH);
    this->add_transition(SmtpsState::WAIT_AUTH, SmtpsEvent::MAIL_FROM, SmtpsState::WAIT_RCPT_TO);
    this->add_transition(SmtpsState::WAIT_MAIL_FROM, SmtpsEvent::MAIL_FROM, SmtpsState::WAIT_RCPT_TO);
    this->add_transition(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::MAIL_FROM, SmtpsState::WAIT_RCPT_TO);
    this->add_transition(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::RCPT_TO, SmtpsState::WAIT_RCPT_TO);
    this->add_transition(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::DATA, SmtpsState::IN_MESSAGE);
    this->add_transition(SmtpsState::WAIT_DATA, SmtpsEvent::DATA, SmtpsState::IN_MESSAGE);
    this->add_transition(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA, SmtpsState::IN_MESSAGE);
    this->add_transition(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA_END, SmtpsState::WAIT_QUIT);
    this->add_transition(SmtpsState::WAIT_QUIT, SmtpsEvent::MAIL_FROM, SmtpsState::WAIT_RCPT_TO);

    for (int i = 0; i < static_cast<int>(SmtpsState::CLOSED); ++i) {
        this->add_transition(static_cast<SmtpsState>(i), SmtpsEvent::QUIT, SmtpsState::CLOSED);
    }
    for (int i = 0; i < static_cast<int>(SmtpsState::CLOSED); ++i) {
        this->add_transition(static_cast<SmtpsState>(i), SmtpsEvent::ERROR, static_cast<SmtpsState>(i));
        this->add_transition(static_cast<SmtpsState>(i), SmtpsEvent::TIMEOUT, static_cast<SmtpsState>(i));
    }
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::init_state_handlers() {
    auto h = [this](auto handler) {
        return [this, handler](auto s) { (this->*handler)(s); };
    };

    this->add_handler(SmtpsState::INIT, SmtpsEvent::CONNECT,
        h(&TraditionalSmtpsFsm::handle_init_connect));
    this->add_handler(SmtpsState::WAIT_EHLO, SmtpsEvent::EHLO,
        h(&TraditionalSmtpsFsm::handle_greeting_ehlo));
    this->add_handler(SmtpsState::GREETING, SmtpsEvent::EHLO,
        h(&TraditionalSmtpsFsm::handle_greeting_ehlo));

    if constexpr (!std::is_same_v<ConnectionType, pr::SslConnection>)
        this->add_handler(SmtpsState::WAIT_AUTH, SmtpsEvent::STARTTLS,
            h(&TraditionalSmtpsFsm::handle_wait_auth_starttls));

    this->add_handler(SmtpsState::WAIT_AUTH, SmtpsEvent::AUTH,
        h(&TraditionalSmtpsFsm::handle_wait_auth_auth));
    this->add_handler(SmtpsState::WAIT_AUTH, SmtpsEvent::EHLO,
        h(&TraditionalSmtpsFsm::handle_greeting_ehlo));
    this->add_handler(SmtpsState::WAIT_AUTH_USERNAME, SmtpsEvent::AUTH,
        h(&TraditionalSmtpsFsm::handle_wait_auth_username));
    this->add_handler(SmtpsState::WAIT_AUTH_PASSWORD, SmtpsEvent::AUTH,
        h(&TraditionalSmtpsFsm::handle_wait_auth_password));
    this->add_handler(SmtpsState::WAIT_AUTH, SmtpsEvent::MAIL_FROM,
        h(&TraditionalSmtpsFsm::handle_wait_auth_mail_from));
    this->add_handler(SmtpsState::WAIT_MAIL_FROM, SmtpsEvent::MAIL_FROM,
        h(&TraditionalSmtpsFsm::handle_wait_mail_from_mail_from));
    this->add_handler(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::RCPT_TO,
        h(&TraditionalSmtpsFsm::handle_wait_rcpt_to_rcpt_to));
    this->add_handler(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::DATA,
        h(&TraditionalSmtpsFsm::handle_wait_data_data));
    this->add_handler(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::MAIL_FROM,
        h(&TraditionalSmtpsFsm::handle_wait_auth_mail_from));
    this->add_handler(SmtpsState::WAIT_DATA, SmtpsEvent::DATA,
        h(&TraditionalSmtpsFsm::handle_wait_data_data));
    this->add_handler(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA,
        h(&TraditionalSmtpsFsm::handle_in_message_data));
    this->add_handler(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA_END,
        h(&TraditionalSmtpsFsm::handle_in_message_data_end));

    for (int i = 1; i < static_cast<int>(SmtpsState::WAIT_QUIT) + 1; ++i)
        this->add_handler(static_cast<SmtpsState>(i), SmtpsEvent::QUIT,
            h(&TraditionalSmtpsFsm::handle_wait_quit_quit));
    this->add_handler(SmtpsState::WAIT_QUIT, SmtpsEvent::MAIL_FROM,
        h(&TraditionalSmtpsFsm::handle_wait_auth_mail_from));

    for (int i = 0; i < static_cast<int>(SmtpsState::WAIT_QUIT) + 1; ++i) {
        this->add_handler(static_cast<SmtpsState>(i), SmtpsEvent::ERROR,
            h(&TraditionalSmtpsFsm::handle_error));
        this->add_handler(static_cast<SmtpsState>(i), SmtpsEvent::TIMEOUT,
            h(&TraditionalSmtpsFsm::handle_timeout));
    }
}

// ========== 事件处理 ==========
template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::process_event(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session, SmtpsEvent event)
{
    if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) {
        LOG_SMTP_DETAIL_DEBUG("Current State: {}, Event: {}",
            TraditionalSmtpsFsm<ConnectionType>::get_state_name(static_cast<SmtpsState>(session->get_current_state())),
            TraditionalSmtpsFsm<ConnectionType>::get_event_name(event));
    }
    this->dispatch(session, static_cast<SmtpsState>(session->get_current_state()), event);
}

template <typename ConnectionType>
bool TraditionalSmtpsFsm<ConnectionType>::is_terminal_state(SmtpsState s) const {
    return s == SmtpsState::CLOSED;
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::on_invalid_transition(
    SmtpsState, SmtpsEvent,
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    session->set_error(pr::SessionError::InvalidCommand);
    session->do_async_write("500 Error: Invalid command sequence\r\n",
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
            if (ec) return;
            s->do_async_read();
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::auto_process_event(std::shared_ptr<pr::SessionBase<ConnectionType>> session) {
    process_event(session,
        static_cast<SmtpsEvent>(session->get_next_event()));
}

// ========== 各状态处理函数 ==========

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_init_connect(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    session->set_current_state(static_cast<int>(SmtpsState::GREETING));
    session->do_async_write("220 SMTPS Server\r\n",
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> self, const std::error_code& ec) mutable {
            if (ec) { LOG_SMTP_DETAIL_ERROR("Error sending greeting: {}", ec.message()); return; }
            self->set_current_state(static_cast<int>(SmtpsState::WAIT_EHLO));
            self->do_async_read();
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_greeting_ehlo(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    LOG_SMTP_DETAIL_DEBUG("Received EHLO: {}", session->get_last_command_args());
    if (auto* ctx = static_cast<SmtpsContext*>(session->get_context()))
        ctx->ehlo_domain = session->get_last_command_args();

    // RFC 5321: EHLO 响应首行必须是服务器自身的域名（非回显客户端 EHLO 参数）。
    // Gmail 会校验该主机名：若服务器自称是 "*.google.com"（回显客户端）会触发安全中止。
    auto cfg = std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config);
    std::string helo = cfg->helo_hostname.empty() ? cfg->system_domain : cfg->helo_hostname;
    if (helo.empty()) helo = session->get_last_command_args();

    std::string response = "250-" + helo + " Hello\r\n"
        "250-SIZE 10240000\r\n"
        "250-8BITMIME\r\n";
    {
        auto* ctx = static_cast<SmtpsContext*>(session->get_context());
        if constexpr (!std::is_same_v<ConnectionType, pr::SslConnection>) {
            if (ctx->listener_config.advertise_starttls)
                response += "250-STARTTLS\r\n";
        }
        if (ctx->listener_config.auth_policy != InboundAuthPolicy::OFF)
            response += "250-AUTH LOGIN PLAIN\r\n";
    }
    response += "250 SMTPUTF8\r\n";

    session->do_async_write(response,
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
            if (ec) { LOG_SMTP_DETAIL_ERROR("Error sending EHLO: {}", ec.message()); return; }
            s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
            s->do_async_read();
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_starttls(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    // 未通告的能力不得生效（RFC 3207 §4）：显式拒绝，会话保持在 WAIT_AUTH 可继续收信
    if (auto* ctx = static_cast<SmtpsContext*>(session->get_context());
        !ctx->listener_config.advertise_starttls) {
        session->do_async_write("502 5.5.1 STARTTLS not supported\r\n",
            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                if (ec) return;
                s->do_async_read();
            });
        return;
    }
    session->do_async_write("220 Ready to start TLS\r\n",
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> self, const std::error_code& ec) mutable {
            if (ec) { LOG_SMTP_DETAIL_ERROR("Error sending STARTTLS: {}", ec.message()); return; }
            self->set_current_state(static_cast<int>(SmtpsState::INIT));
            auto server = static_cast<SmtpsServer*>(static_cast<MailServerBase*>(self->get_server()));
            auto trace = self->take_trace_buffer();   // 交接给 TLS 会话，延续对话记录
            server->handoff_starttls(self->release_connection(), std::move(trace));
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_auth(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<SmtpsContext*>(session->get_context());

    // AUTH PLAIN step2
    if (ctx->plain_auth_expected) {
        ctx->plain_auth_expected = false;
        std::string decoded = mail_system::outbound::base64_decode(session->get_last_command_args());
        auto null1 = decoded.find('\0');
        if (null1 != std::string::npos) {
            auto null2 = decoded.find('\0', null1 + 1);
            std::string username = decoded.substr(null1 + 1, null2 - null1 - 1);
            std::string password = (null2 != std::string::npos) ? decoded.substr(null2 + 1) : "";
            ctx->client_username = username;
            if (username.find('@') == std::string::npos) {
                auto cfg = std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config);
                username += "@" + cfg->system_domain;
                ctx->client_username = username;
            }
            this->auth_user_async(session, username, password,
                [session, ctx](bool ok, int shard) {
                    if (ok) {
                        ctx->is_authenticated = true; ctx->shard_index = shard;
                        session->do_async_write("235 Authentication successful\r\n",
                            [session](auto s, auto& ec) {
                                if (!ec) s->set_current_state(static_cast<int>(SmtpsState::WAIT_MAIL_FROM));
                                s->drain_buffered_commands();
                                if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                            });
                    } else {
                        if (session->record_auth_failure_and_check()) { session->close_after_flush(); return; }
                        session->do_async_write("535 Authentication failed\r\n",
                            [session](auto s, auto& ec) {
                                if (!ec) s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                                s->drain_buffered_commands();
                                if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                            });
                    }
                });
            return;
        }
    }

    // AUTH PLAIN 1-step
    std::string upper_args = session->get_last_command_args();
    std::transform(upper_args.begin(), upper_args.end(), upper_args.begin(), ::toupper);
    if (upper_args.find("PLAIN") == 0) {
        std::string token = session->get_last_command_args().length() > 6 ? session->get_last_command_args().substr(6) : "";
        if (token.empty()) {
            ctx->plain_auth_expected = true;
            session->do_async_write("334 \r\n",
                [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                    if (ec) return;
                    s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                    s->do_async_read();
                });
            return;
        }
        std::string decoded = mail_system::outbound::base64_decode(token);
        auto null1 = decoded.find('\0');
        if (null1 != std::string::npos) {
            auto null2 = decoded.find('\0', null1 + 1);
            std::string username = decoded.substr(null1 + 1, null2 - null1 - 1);
            std::string password = (null2 != std::string::npos) ? decoded.substr(null2 + 1) : "";
            ctx->client_username = username;
            if (username.find('@') == std::string::npos) {
                auto cfg = std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config);
                username += "@" + cfg->system_domain;
                ctx->client_username = username;
            }
            this->auth_user_async(session, username, password,
                [session, ctx](bool ok, int shard) {
                    if (ok) {
                        ctx->is_authenticated = true; ctx->shard_index = shard;
                        session->do_async_write("235 Authentication successful\r\n",
                            [session](auto s, auto& ec) {
                                if (!ec) s->set_current_state(static_cast<int>(SmtpsState::WAIT_MAIL_FROM));
                                s->drain_buffered_commands();
                                if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                            });
                    } else {
                        if (session->record_auth_failure_and_check()) { session->close_after_flush(); return; }
                        session->do_async_write("535 Authentication failed\r\n",
                            [session](auto s, auto& ec) {
                                if (!ec) s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                                s->drain_buffered_commands();
                                if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                            });
                    }
                });
            return;
        }
        handle_wait_auth_auth_login(session);
        return;
    }
    handle_wait_auth_auth_login(session);
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_auth_login(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    session->do_async_write("334 VXNlcm5hbWU6\r\n",
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
            if (ec) { LOG_SMTP_DETAIL_ERROR("AUTH username prompt: {}", ec.message()); return; }
            s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH_USERNAME));
            s->do_async_read();
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_username(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    std::string decoded = mail_system::outbound::base64_decode(session->get_last_command_args());
    static_cast<SmtpsContext*>(session->get_context())->client_username = decoded;
    session->do_async_write("334 UGFzc3dvcmQ6\r\n",
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
            if (ec) { LOG_SMTP_DETAIL_ERROR("AUTH password prompt: {}", ec.message()); return; }
            s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH_PASSWORD));
            s->do_async_read();
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_password(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    std::string username = static_cast<SmtpsContext*>(session->get_context())->client_username;
    std::string password = mail_system::outbound::base64_decode(session->get_last_command_args());
    if (username.find('@') == std::string::npos) {
        auto cfg = std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config);
        username += "@" + cfg->system_domain;
        static_cast<SmtpsContext*>(session->get_context())->client_username = username;
    }
    this->auth_user_async(session, username, password,
        [session](bool ok, int shard) {
            auto* ctx = static_cast<SmtpsContext*>(session->get_context());
            if (ok) {
                ctx->is_authenticated = true; ctx->shard_index = shard;
                session->do_async_write("235 Authentication successful\r\n",
                    [session](auto s, auto& ec) {
                        if (!ec) s->set_current_state(static_cast<int>(SmtpsState::WAIT_MAIL_FROM));
                        s->drain_buffered_commands();
                        if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                    });
            } else {
                if (session->record_auth_failure_and_check()) { session->close_after_flush(); return; }
                session->do_async_write("535 Authentication failed\r\n",
                    [session](auto s, auto& ec) {
                        if (!ec) s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                        s->drain_buffered_commands();
                        if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                    });
            }
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_mail_from(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    // AUTH policy check — 外部发件人是否需要认证：
    // - listener auth_policy=ON（465/587 提交端口）始终要求认证
    // - listener auth_policy=AUTO（25）按 inbound_auth_policy 决定，默认 off = 无需认证
    //   之前错误地按 listener AUTO 处理（require_auth=!is_trusted_server，而该值从未被置真），
    //   导致所有外部入站投递都被 530 拒绝
    {
        auto* ctx = static_cast<SmtpsContext*>(session->get_context());
        const auto& lc = ctx->listener_config;
        auto cfg = std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config);

        bool require_auth = false;
        switch (lc.auth_policy) {
        case InboundAuthPolicy::ON:
            require_auth = true;
            break;
        case InboundAuthPolicy::AUTO:
            switch (cfg->inbound_auth_policy) {
            case InboundAuthPolicy::ON:   require_auth = true; break;
            case InboundAuthPolicy::AUTO: require_auth = !ctx->is_trusted_server; break;
            case InboundAuthPolicy::OFF:  require_auth = false; break;
            }
            break;
        case InboundAuthPolicy::OFF:
            break;
        }

        if (require_auth && !ctx->is_authenticated) {
            session->do_async_write("530 5.7.1 Authentication required\r\n",
                [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                    if (ec) return;
                    s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                    s->do_async_read();
                });
            return;
        }
    }

    auto addr_start = session->get_last_command_args().find('<');
    auto addr_end   = session->get_last_command_args().find('>', addr_start != std::string::npos ? addr_start + 1 : 0);
    if (addr_start != std::string::npos && addr_end != std::string::npos) {
        auto* ctx = static_cast<SmtpsContext*>(session->get_context());

        // 本域发件人边界（规则 1）：未认证且不在 trusted_relay_networks 的会话
        // （公网 25 直连）信封发件人落在本域只能是伪造——合法本域邮件只走认证
        // 提交 / webmail / 信任中继三种入口。空信封 <>（退信）豁免；必须在进入
        // SPF/配额 proceed 之前确定性拒绝，不依赖 DNS。状态处理与下方 SPF 硬拒
        // 一致：回 WAIT_AUTH 继续读。
        {
            auto cfg = std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config);
            const std::string envelope_sender = session->get_last_command_args().substr(
                addr_start + 1, addr_end - addr_start - 1);
            if (cfg->reject_unauthenticated_local_sender && !ctx->is_authenticated &&
                !ip_in_trusted_networks(session->get_client_ip(), cfg->trusted_relay_networks) &&
                AccountPolicy::envelope_sender_is_local(envelope_sender, cfg->system_domain)) {
                LOG_SMTP_INFO("MAIL FROM local domain sender rejected: {} (from {})",
                              envelope_sender, session->get_client_ip());
                session->do_async_write(
                    "550 5.7.1 Sender address rejected: local domain requires authentication\r\n",
                    [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                        if (ec) return;
                        s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                        s->do_async_read();
                    });
                return;
            }
        }

        ctx->sender_address = AccountPolicy::submission_sender(
            ctx->is_authenticated ? ctx->client_username : "",
            session->get_last_command_args().substr(addr_start + 1, addr_end - addr_start - 1));
        ctx->recipient_addresses.clear();
        ctx->spf_checked = false;

        // MAIL FROM 后续处理（SPF 检查 → 250 / 拒绝）抽成 proceed：
        // 认证账号要先过每日发信配额，通过后再进入 SPF 逻辑
        auto cfg = std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config);
        // 认证提交豁免 SPF：SPF 只该管 MTA 间未认证投递（RFC 7208 语义）。
        // 认证客户端已用密码证明身份，且发件人强制等于自身账号，无从伪装，
        // 对它们做 SPF 只会把从非授权 IP 登录的合法用户（家里/咖啡馆）误杀。
        bool need_spf = !cfg->perf_mode && cfg->inbound_spf_mode != "off" &&
                        !ctx->is_authenticated &&
                        !ctx->sender_address.empty() && ctx->sender_address != "<>";
        std::string client_ip = session->get_client_ip();
        std::string sender = ctx->sender_address;
        std::string ehlo = ctx->ehlo_domain;
        std::string spf_mode = cfg->inbound_spf_mode;

        auto proceed = [session, need_spf, client_ip, sender, ehlo, spf_mode]() {
            // SPF check — 异步（worker 线程池执行 DNS 查询，不阻塞 io_context）
            if (!need_spf) {
                session->do_async_write("250 Ok\r\n",
                    [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                        if (ec) return;
                        s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                        // Quota may have completed after the original input loop
                        // paused. Resume buffered RCPT/DATA before requesting IO.
                        s->drain_buffered_commands();
                        if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                    });
                return;
            }

            auto* dns = static_cast<MailServerBase*>(session->get_server())->get_dns_resolver().get();
            if (!dns) {
                session->do_async_write("250 Ok\r\n",
                    [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                        if (ec) return;
                        s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                        // Quota may have completed after the original input loop
                        // paused. Resume buffered RCPT/DATA before requesting IO.
                        s->drain_buffered_commands();
                        if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                    });
                return;
            }

            // 发起 DNS 前显式置阻塞：状态机函数返回后 io_context 线程不会继续处理本 session
            // （do_async_read 流水线循环检查 is_paused），DNS 回调独占操作，无需 post 回 io_context
            session->set_paused(true);
            inbound::InboundVerifier::check_spf_only_async(*dns, client_ip, sender, ehlo,
                [session, sender, spf_mode](inbound::SpfResult spf) {
                    session->set_paused(false);
                    auto* c = static_cast<SmtpsContext*>(session->get_context());
                    c->spf_checked = true;
                    c->spf_result = spf.result;
                    c->spf_reason = spf.reason;
                    std::string reject;
                    if (spf.result == "fail" && spf_mode == "hard")
                        reject = "SPF hard-fail for " + sender;
                    if (!reject.empty()) {
                        session->do_async_write("550 5.7.1 SPF verification failed: " + reject + "\r\n",
                            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                                if (ec) return;
                                s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                                s->do_async_read();
                            });
                    } else {
                        session->do_async_write("250 Ok\r\n",
                            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                                if (ec) return;
                                s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                                s->drain_buffered_commands();
                                if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                            });
                    }
                });
        };

        // 认证账号（465/587 提交）：MAIL FROM 时原子占用当日一个配额，超限 550 拒收。
        // 未认证入站（25 端口 MTA 投递）无账号，不计数。
        if (ctx->is_authenticated) {
            session->set_paused(true);
            this->check_send_quota_async(session, ctx->sender_address,
                [session, proceed](bool allowed) {
                    session->set_paused(false);
                    if (!allowed) {
                        session->do_async_write("550 5.7.0 Daily sending quota exceeded\r\n",
                            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                                if (ec) return;
                                s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                                s->drain_buffered_commands();
                                if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                            });
                        return;
                    }
                    proceed();
                });
        } else {
            proceed();
        }
    } else {
        session->do_async_write("501 Syntax error in parameters or arguments\r\n",
            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                if (ec) return;
                s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                s->do_async_read();
            });
    }
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_mail_from_mail_from(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    handle_wait_auth_mail_from(session);
}

namespace {
// 别名展开目标追加进收件人列表，跳过已存在的：同一封邮件对同一收件人重复
// 记录会撞 mail_mailbox 的 uk_mail_box_user 唯一键，回滚整个持久化事务。
template <typename Context>
void push_recipient_deduped(Context* c, std::vector<std::string>& targets) {
    for (auto& t : targets) {
        if (std::find(c->recipient_addresses.begin(), c->recipient_addresses.end(), t)
                == c->recipient_addresses.end())
            c->recipient_addresses.push_back(std::move(t));
    }
}
} // namespace

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_rcpt_to_rcpt_to(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto addr_start = session->get_last_command_args().find('<');
    auto addr_end   = session->get_last_command_args().find('>', addr_start != std::string::npos ? addr_start + 1 : 0);
    if (addr_start == std::string::npos || addr_end == std::string::npos) {
        session->do_async_write("501 Syntax error in parameters or arguments\r\n",
            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                if (ec) return;
                s->do_async_read();
            });
        return;
    }

    std::string recipient = algorithm::trim(
        session->get_last_command_args().substr(addr_start + 1, addr_end - addr_start - 1));
    if (recipient.empty()) {
        session->do_async_write("501 Syntax error in parameters or arguments\r\n",
            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                if (ec) return;
                s->do_async_read();
            });
        return;
    }

    auto* ctx = static_cast<SmtpsContext*>(session->get_context());

    // 认证客户端（465/587 提交）：允许任意收件人（含外部域名，按提交模式转发）
    if (ctx->is_authenticated) {
        // 外部投递开关：external_delivery_enabled=false 时拒外部域收件人。
        // 内部投递（system_domain）不受影响；未认证 25 端口 MTA 入站本来就禁中继。
        auto cfg = std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config);
        std::string domain;
        auto at = recipient.rfind('@');
        if (at != std::string::npos) domain = recipient.substr(at + 1);
        const bool is_local =
            algorithm::to_lower(algorithm::trim(domain)) == algorithm::to_lower(cfg->system_domain);
        if (!AccountPolicy::recipient_allowed(recipient, cfg->system_domain, cfg->external_delivery_enabled)) {
            LOG_SMTP_INFO("RCPT external delivery disabled: {}", recipient);
            session->do_async_write("550 5.7.1 External delivery disabled\r\n",
                [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                    if (ec) return;
                    s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                    s->do_async_read();
                });
            return;
        }
        // 本地域收件人：别名展开（邮件组，webmail/客户端 587 提交同一语义）。
        // 非别名或别名查询失败 → 原样接受：提交模式不因别名查询新增拒绝理由，
        // 保持"未认证路径才做存在性校验"的既有语义。
        if (is_local) {
            this->alias_targets_async(session, recipient,
                [session, recipient](std::vector<std::string> targets) {
                    auto* c = static_cast<SmtpsContext*>(session->get_context());
                    if (targets.empty()) {
                        c->recipient_addresses.push_back(recipient);
                    } else {
                        push_recipient_deduped(c, targets);
                    }
                    session->do_async_write("250 Ok\r\n",
                        [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                            if (ec) return;
                            s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                            s->drain_buffered_commands();
                            if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                        });
                });
            return;
        }
        ctx->recipient_addresses.push_back(recipient);
        session->do_async_write("250 Ok\r\n",
            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                if (ec) return;
                s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                s->do_async_read();
            });
        return;
    }

    // 未认证（25 端口 MTA 入站）：禁止中继，只收本地已注册用户
    auto cfg = std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config);
    std::string local = recipient;
    std::string domain;
    auto at = recipient.rfind('@');
    if (at != std::string::npos) {
        local = recipient.substr(0, at);
        domain = recipient.substr(at + 1);
    }
    local = algorithm::trim(local);

    // 信任中继网段（上游 MTA 信任跳）：接受任意收件人，跳过 relay-denied 与
    // 本地用户校验（中继节点本地本就无用户体系）
    if (ip_in_trusted_networks(session->get_client_ip(), cfg->trusted_relay_networks)) {
        LOG_SMTP_INFO("RCPT trusted relay accepted: {} (from {})",
                      recipient, session->get_client_ip());
        ctx->recipient_addresses.push_back(recipient);
        session->do_async_write("250 Ok\r\n",
            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                if (ec) return;
                s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                s->drain_buffered_commands();
                if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
            });
        return;
    }

    // 外部域名（非本系统域名）→ 拒绝中继
    if (algorithm::to_lower(domain) != algorithm::to_lower(cfg->system_domain)) {
        LOG_SMTP_INFO("RCPT relay denied: {} (domain {})", recipient, domain);
        session->do_async_write("550 5.7.1 Relay access denied\r\n",
            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                if (ec) return;
                s->do_async_read();
            });
        return;
    }

    // 本地域名但无本地部分 → 语法错误
    if (local.empty()) {
        session->do_async_write("501 Syntax error in parameters or arguments\r\n",
            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                if (ec) return;
                s->do_async_read();
            });
        return;
    }

    // 本地域名 → 收件人解析：真实用户直收；别名展开为全部在用目标（邮件组）；
    // 两者皆非 → 550 User unknown。
    this->resolve_recipient_async(session, recipient,
        [session, recipient](std::vector<std::string> targets) {
            auto* c = static_cast<SmtpsContext*>(session->get_context());
            if (targets.empty()) {
                LOG_SMTP_WARN("RCPT user unknown: {}", recipient);
                session->do_async_write("550 5.1.1 User unknown\r\n",
                    [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                        if (ec) return;
                        s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                        s->drain_buffered_commands();
                        if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                    });
                return;
            }
            push_recipient_deduped(c, targets);
            session->do_async_write("250 Ok\r\n",
                [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                    if (ec) return;
                    s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                    s->drain_buffered_commands();
                    if (!s->has_buffered_input() && !s->is_paused() && !s->is_closed()) s->do_async_read();
                });
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_data_data(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* smtp_session = dynamic_cast<SmtpsSession<ConnectionType>*>(session.get());
    if (smtp_session) smtp_session->create_mail_on_data_command();

    session->do_async_write("354 Start mail input; end with <CRLF>.<CRLF>\r\n",
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
            if (ec) return;
            s->set_current_state(static_cast<int>(SmtpsState::IN_MESSAGE));
            s->do_async_read();
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_in_message_data(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    session->do_async_read();
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_in_message_data_end(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<SmtpsContext*>(session->get_context());
    std::string resp = ctx->abort_reason.empty() ? "552 Message size exceeds limit\r\n" : ctx->abort_reason + "\r\n";
    if (ctx->body_limit_exceeded) {
        cleanup_streamed_attachments(ctx);
        session->do_async_write(resp,
            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code&) mutable { s->close(); });
        return;
    }

    auto* smtp_session = dynamic_cast<SmtpsSession<ConnectionType>*>(session.get());
    if (!smtp_session) { session->close(); return; }

    // 正文没能落盘就绝不能往下走去回 250：那等于骗发送方 MTA 把邮件从队列里删掉。
    // 回 451 让对方稍后重投。
    //
    // commit 走异步形状：本地后端回调内联执行（与原同步版行为完全一致）；
    // 远程后端将来覆写真异步时回调来自 provider 线程 —— pause 期间 io 线程不再
    // 消费本 session（do_async_read 的流水线循环检查 is_paused，且不再发起新的
    // socket 读），回调独占操作 session，无需 post 回 io_context（SPF/DNS
    // 回调同一约定）。
    session->set_paused(true);
    smtp_session->commit_body_async(
        [session](bool committed, const storage::IoError& commit_err) {
            session->set_paused(false);
            auto* c = static_cast<SmtpsContext*>(session->get_context());
            auto* smtp_s = dynamic_cast<SmtpsSession<ConnectionType>*>(session.get());
            if (!committed) {
                cleanup_streamed_attachments(c);
                if (smtp_s) smtp_s->discard_current_mail();
                // retryable（ENOSPC/网络/5xx）→ 451 让发送方稍后重投；
                // permanent（EACCES/4xx/超上限）→ 550，重投结果不会改变。
                const bool retry = commit_err.retryable();
                LOG_SMTP_DETAIL_ERROR("DATA_END commit failed (retryable={}): {} {}",
                                      retry, commit_err.code, commit_err.message);
                session->do_async_write(retry
                        ? "451 4.3.0 Failed to store message, try again later\r\n"
                        : "550 5.3.0 Failed to store message\r\n",
                    [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code&) mutable {
                        s->close();
                    });
                return;
            }
            finish_data_end_after_commit(session);
        });
}

// DATA_END 在正文确认落盘之后的处理：MIME 预解析（异步读）→ 入站校验
// （可选异步）→ 入队 + 250/451。commit_body 的回调（本地内联 / 远程
// provider 线程）在这里续跑。
template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::finish_data_end_after_commit(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    auto* ctx = static_cast<SmtpsContext*>(session->get_context());
    auto* smtp_session = dynamic_cast<SmtpsSession<ConnectionType>*>(session.get());
    if (!smtp_session) { session->close(); return; }

    auto cfg = std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config);

    // MIME 预解析：仅当消息 ≤ 阈值才 eager 解析（大消息跳过，交给 IMAP lazy 解析）
    // 供 IMAP BODYSTRUCTURE / BODY[section] 直接使用。
    //
    // 走 provider 的只读句柄：这里只解析、不修改内容，因此用 open_read。
    // 本地后端会把它实现成 mmap（零拷贝）；远程后端退化成下载进缓冲区。
    // 读走异步形状：本地内联（行为与同步版一致），远程将来覆写真异步时
    // 回调来自 provider 线程 —— pause 期间回调独占 session，无需 post 回
    // （与 commit 回调同一约定）。
    const bool has_body = smtp_session->get_mail() &&
                          !smtp_session->get_mail()->body_path.empty();
    auto provider = has_body ? static_cast<MailServerBase*>(session->get_server())->m_shardRouter->get_storage(
                                   static_cast<size_t>(ctx->shard_index))
                             : nullptr;
    if (!has_body || !provider) {
        try {
            if (has_body) {
                // 无 provider 的兜底路径：本地文件系统直开 mmap（µs 级，无需异步）
                std::error_code fec;
                const auto& body_key = smtp_session->get_mail()->body_path;
                const auto local_sz = std::filesystem::file_size(body_key, fec);
                if (!fec && local_sz > 0 &&
                    static_cast<std::uintmax_t>(cfg->inbound_mime_parse_limit_bytes) >= local_sz) {
                    storage::IoError read_err;
                    auto body_read = storage::MappedReadStream::open(body_key, read_err);
                    if (!body_read && !read_err.message.empty()) {
                        LOG_FILE_IO_ERROR("Failed to open mail body for MIME parse: {}",
                                          read_err.message);
                    }
                    parse_and_save_mime_tree(session, std::move(body_read));
                }
            }
        } catch (const std::exception& e) {
        LOG_SMTP_DETAIL_ERROR("DATA_END processing exception: {}", e.what());
        auto* smtp_s = dynamic_cast<SmtpsSession<ConnectionType>*>(session.get());
        if (smtp_s) smtp_s->discard_current_mail();
        session->do_async_write("451 Requested action aborted: local processing error\r\n",
            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code&) mutable { s->close(); });
        return;
    }
        finish_data_end_after_read(session);
        return;
    }

    session->set_paused(true);
    provider->async_open_read(smtp_session->get_mail()->body_path,
        [session, cfg](std::unique_ptr<storage::IReadStream> stream,
                       const storage::IoError& read_err) {
            session->set_paused(false);
            try {
                if (!stream && !read_err.message.empty()) {
                    LOG_FILE_IO_ERROR("Failed to open mail body for MIME parse: {}",
                                      read_err.message);
                }
                // 超过解析阈值的大消息跳过 eager 解析（大小只有拿到流才知道，
                // 远程后端为此下载一次是这笔账里省不掉的部分）
                if (stream && !stream->empty() &&
                    static_cast<std::uint64_t>(cfg->inbound_mime_parse_limit_bytes) >= stream->size()) {
                    parse_and_save_mime_tree(session, std::move(stream));
                }
            }catch (const std::exception& e) {
        LOG_SMTP_DETAIL_ERROR("DATA_END processing exception: {}", e.what());
        auto* smtp_s = dynamic_cast<SmtpsSession<ConnectionType>*>(session.get());
        if (smtp_s) smtp_s->discard_current_mail();
        session->do_async_write("451 Requested action aborted: local processing error\r\n",
            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code&) mutable { s->close(); });
        return;
    }
            finish_data_end_after_read(session);
        });
}

// 在只读视图上做 MIME 预解析并写 sidecar JSON（失败不阻塞收信）。
// 只允许在持有 session 独占（IO 线程 handler 内 / pause 期间的回调）时调用。
template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::parse_and_save_mime_tree(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    std::unique_ptr<storage::IReadStream> body_read)
{
    auto* smtp_session = dynamic_cast<SmtpsSession<ConnectionType>*>(session.get());
    if (!smtp_session || !smtp_session->get_mail() || !body_read || body_read->empty()) {
        return;
    }
    parse_mime_tree(body_read->view(), smtp_session->get_mail()->mime_root);
    auto* ctx = static_cast<SmtpsContext*>(smtp_session->get_context());
    auto router = smtp_session->get_server()->get_shard_router();
    auto provider = ctx && router ? router->get_storage(
                              static_cast<size_t>(ctx->shard_index)) : nullptr;
    auto metadata = mime_metadata_store_for(provider);
    save_mime_tree(metadata, smtp_session->get_mail()->body_path,
                   smtp_session->get_mail()->mime_root);
}

// MIME 预解析之后的收尾：本域发件人边界（规则 2/3）→ 入站校验（可选异步）→ 入队 + 250/451。
template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::finish_data_end_after_read(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    // 收尾主体（边界检查通过后续跑）：入站校验（可选异步）→ 入队 + 250/451。
    // 以值捕获 session：规则 3 的别名分支会在 DB/缓存回调线程重入本 lambda
    // （与下方 verify 回调同一形状，style 11.2），故内部自行重取 ctx/cfg。
    auto finish_after_sender_boundary = [session]() {
        auto* ctx = static_cast<SmtpsContext*>(session->get_context());
        auto* smtp_session = dynamic_cast<SmtpsSession<ConnectionType>*>(session.get());
        if (!smtp_session) { session->close(); return; }
        auto cfg = std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config);

    try {

    // Inbound verification (DKIM/DMARC/SPF) — 异步（worker 线程池执行 DNS/验签，不阻塞 io_context）
    bool needs_verify = !cfg->perf_mode && (
        (!ctx->spf_checked && cfg->inbound_spf_mode != "off") ||
         cfg->inbound_dkim_mode != "off" || cfg->inbound_dmarc_mode != "off");

    // 入队 + 响应（after_enqueue / after_persist）。vr 有值则先记录校验结果。
    auto finalize_mail = [session, smtp_session](std::optional<inbound::VerificationResult> vr) {
        try {
            if (vr) {
                auto* c = static_cast<SmtpsContext*>(session->get_context());
                c->dkim_result = vr->dkim.result;
                c->dmarc_result = vr->dmarc.result;
                if (!c->spf_checked) { c->spf_result = vr->spf.result; c->spf_checked = true; }
                c->verification_run = true;
            }
            auto submit_result = smtp_session->submit_mail_to_queue();
            if (!submit_result.accepted) {
                smtp_session->discard_current_mail();
                session->do_async_write("451 Requested action aborted: insufficient storage or backend pressure\r\n",
                    [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code&) mutable { s->close(); });
                return;
            }
            const bool ack_after_enqueue =
                std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config)->inbound_ack_mode == InboundAckMode::AFTER_ENQUEUE;
            if (ack_after_enqueue) {
                session->set_current_state(static_cast<int>(SmtpsState::WAIT_MAIL_FROM));
                smtp_session->reset_mail_state();
                static_cast<MailServerBase*>(session->get_server())->increment_mails_accepted();
                session->do_async_write("250 OK\r\n",
                    [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                        if (ec) return;
                        s->do_async_read();
                    });
                return;
            }
            // 在连接 executor 上等待完成/超时，不占用持久化需要的 worker。
            session->set_paused(true);
            const auto timeout = std::chrono::milliseconds(
                std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config)->inbound_persist_wait_timeout_ms);
            smtp_session->async_wait_pending_mail(timeout, [session](persist_storage::PersistResult result) {
                auto* smtp = static_cast<SmtpsSession<ConnectionType>*>(session.get());
                if (session->is_closed()) { smtp->clear_pending_mail_submission(); return; }
                session->set_paused(false);
                if (!result.timed_out && result.status == persist_storage::PersistStatus::SUCCESS) {
                    session->set_current_state(static_cast<int>(SmtpsState::WAIT_MAIL_FROM));
                    smtp->reset_mail_state();
                    static_cast<MailServerBase*>(session->get_server())->increment_mails_accepted();
                    session->do_async_write("250 OK\r\n",
                        [](auto s, const std::error_code& ec) {
                            if (!ec) s->do_async_read();
                        });
                    return;
                }
                LOG_SMTP_WARN("Persistence acknowledgement failed mail_id={} timeout={} reason={}",
                              result.mail_id, result.timed_out, result.error);
                if (result.timed_out) smtp->cancel_pending_mail_submission();
                smtp->clear_pending_mail_submission();
                session->do_async_write(result.timed_out
                    ? "451 Requested action aborted: local processing timeout\r\n"
                    : "451 Requested action aborted: local processing error\r\n",
                    [](auto s, const std::error_code&) { s->close(); });
            });
        } catch (const std::exception& e) {
            LOG_SMTP_DETAIL_ERROR("DATA_END processing exception: {}", e.what());
            if (smtp_session) smtp_session->discard_current_mail();
            session->do_async_write("451 Requested action aborted: local processing error\r\n",
                [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code&) mutable { s->close(); });
        }
    };

    if (needs_verify && !ctx->verification_run) {
        std::string client_ip = session->get_client_ip();
        std::string mail_from = ctx->sender_address;
        std::string helo = ctx->ehlo_domain;
        std::string headers = ctx->header_buffer;
        const std::string body_path = (smtp_session->get_mail() && !smtp_session->get_mail()->body_path.empty())
            ? smtp_session->get_mail()->body_path : std::string();

        auto* dns = static_cast<MailServerBase*>(session->get_server())->get_dns_resolver().get();
        if (dns && !body_path.empty()) {
            inbound::SpfResult pre_spf{ctx->spf_result, ""};
            session->set_paused(true);   // 校验期间暂停流水线，回调中恢复
            inbound::InboundVerifier::verify_all_from_file_async(*dns,
                client_ip, mail_from, helo, headers, body_path, *cfg,
                [session, finalize_mail = std::move(finalize_mail)](inbound::VerificationResult vr) mutable {
                    // c-ares 回调独占操作（session 已 paused），直接执行，无需 post 回 io_context
                    session->set_paused(false);
                    finalize_mail(std::move(vr));
                },
                ctx->spf_checked ? &pre_spf : nullptr);
            return;
        }
        // dns 不可用或 body_path 空 → 跳过校验，直接入队
    }

    // 同步路径：不需要校验 或 无法校验 → 直接入队 + 响应
    finalize_mail(std::nullopt);
    } catch (const std::exception& e) {
        LOG_SMTP_DETAIL_ERROR("DATA_END processing exception: {}", e.what());
        if (smtp_session) smtp_session->discard_current_mail();
        session->do_async_write("451 Requested action aborted: local processing error\r\n",
            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code&) mutable { s->close(); });
        return;
    }
    };

    // ---- 本域发件人边界（DATA_END，2026-10-20）----
    // 未认证且不在 trusted_relay_networks 的会话（公网 25 直连），From: 头域名等于
    // 本域即头级伪造（攻击模型 B：信封用自己域绕过 SPF、只改 From 头）。规则 3 对
    // 认证提交要求 From 头归属登录账号：精确相等，或 From 地址是登录账号的别名
    // （其别名目标集合包含登录账号；解析复用 alias_targets_async 的 AliasCache，
    // TTL 300s）。信封语义不变：MAIL FROM 仍强制登录账号、配额按信封计，别名
    // 只体现在 From 头。检查必须放在 needs_verify（DNS 工作）与 finalize_mail
    // （入队/持久化）之前：550 后直接丢弃，绝不入队。From 头缺失或解析不出
    // 地址 → 不因本规则拒收（超出规则范围，规则 3 同理）。
    auto* ctx = static_cast<SmtpsContext*>(session->get_context());
    auto* smtp_session = dynamic_cast<SmtpsSession<ConnectionType>*>(session.get());
    if (!smtp_session) { session->close(); return; }
    auto cfg = std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config);

    auto reject_and_close = [session, smtp_session](const char* reply) {
        smtp_session->discard_current_mail();
        session->do_async_write(reply,
            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code&) mutable {
                s->close();
            });
    };

    const std::string from_value =
        inbound::InboundVerifier::extract_from_header_value(ctx->header_buffer);

    // 规则 2：未认证非受信会话，From 头域名等于本域
    if (cfg->reject_unauthenticated_local_sender && !ctx->is_authenticated &&
        !ip_in_trusted_networks(session->get_client_ip(), cfg->trusted_relay_networks) &&
        AccountPolicy::from_header_is_local(from_value, cfg->system_domain)) {
        LOG_SMTP_INFO("DATA_END From header local domain rejected: {} (from {})",
                      from_value, session->get_client_ip());
        reject_and_close("550 5.7.1 From header rejected: local domain requires authentication\r\n");
        return;
    }

    // 规则 3：认证提交，From 头归属校验（精确匹配 → 别名感知）
    if (cfg->enforce_authenticated_from_match && ctx->is_authenticated) {
        const auto from_addr = AccountPolicy::from_header_address(from_value);
        const std::string auth_user = algorithm::trim(ctx->client_username);
        if (!from_addr.empty() && !AccountPolicy::from_header_matches_account(from_value, auth_user)) {
            if (!AccountPolicy::from_header_is_local(from_value, cfg->system_domain)) {
                // 非本域 From 无别名语义可言，维持精确匹配拒绝
                LOG_SMTP_INFO("DATA_END From header mismatch: from={} auth={} (from {})",
                              from_value, auth_user, session->get_client_ip());
                reject_and_close("550 5.7.1 From header does not match authenticated user\r\n");
                return;
            }
            // 本域 From：别名感知。alias_targets_async 包装器先 set_paused(true)；
            // 缓存（TTL 300s）命中在当前线程同步回调，DB 路径回调在 DB 线程 ——
            // pause 期间 io 流水线不消费本 session，回调独占操作，无需 post 回
            // io_context（与上方 verify 的 worker 续跑同一约定）。
            // 空目标（非别名 / DB 不可用 / 查询失败）= 不匹配，fail-closed 拒绝。
            // 本函数为 static（DATA_END 回调链无 this），经 session 取回 FSM 实例
            //（session 持有 fsm shared_ptr，回调存活期内 FSM 必存活）。
            auto* fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(smtp_session->get_fsm());
            fsm->alias_targets_async(session, from_addr,
                [session, from_addr, auth_user, reject_and_close,
                 pass = std::move(finish_after_sender_boundary)](std::vector<std::string> targets) {
                    session->set_paused(false);
                    if (AccountPolicy::account_in_alias_targets(targets, auth_user)) {
                        LOG_SMTP_INFO("DATA_END From header alias accepted: from={} as alias of {} (from {})",
                                      from_addr, auth_user, session->get_client_ip());
                        pass();
                        return;
                    }
                    LOG_SMTP_INFO("DATA_END From header mismatch: from={} auth={} (from {})",
                                  from_addr, auth_user, session->get_client_ip());
                    reject_and_close("550 5.7.1 From header does not match authenticated user\r\n");
                });
            return;
        }
    }

    // 精确匹配 / 规则关闭 / From 缺失或无法解析 → 直接续跑收尾
    finish_after_sender_boundary();
}
template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_quit_quit(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    session->set_trace_clean_close();   // 正常 QUIT → 连接追踪丢弃
    session->do_async_write("221 Bye\r\n",
        [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
            if (ec) LOG_SMTP_DETAIL_ERROR("QUIT reply error: {}", ec.message());
            if (s) s->close();
        });
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_timeout(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    session->handle_read("");
    if (static_cast<SmtpsEvent>(session->get_next_event()) != SmtpsEvent::TIMEOUT) {
        auto_process_event(session);
        return;
    }
    session->do_async_read();
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_error(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session)
{
    LOG_SMTP_DETAIL_WARN("SMTP error: state={} session->get_last_command_args()=[{}] stay={}",
        TraditionalSmtpsFsm<ConnectionType>::get_state_name(static_cast<SmtpsState>(session->get_current_state())),
        session->get_last_command_args(), session->stay_times_);
    session->stay_times_++;
    if (session->stay_times_ > 3) {
        session->close();
    } else {
        session->do_async_write("500 Error: " + session->get_last_command_args() + "\r\n",
            [](std::shared_ptr<pr::SessionBase<ConnectionType>> s, const std::error_code& ec) mutable {
                if (ec) return;
                s->do_async_read();
            });
    }
}

// ========== 从 SmtpsFsm 迁移的方法实现 ==========

template <typename ConnectionType>
std::string TraditionalSmtpsFsm<ConnectionType>::get_state_name(SmtpsState state) {
    static const std::unordered_map<SmtpsState, std::string> state_names = {
        {SmtpsState::INIT, "INIT"},
        {SmtpsState::GREETING, "GREETING"},
        {SmtpsState::WAIT_EHLO, "WAIT_EHLO"},
        {SmtpsState::WAIT_AUTH, "WAIT_AUTH"},
        {SmtpsState::WAIT_AUTH_USERNAME, "WAIT_AUTH_USERNAME"},
        {SmtpsState::WAIT_AUTH_PASSWORD, "WAIT_AUTH_PASSWORD"},
        {SmtpsState::WAIT_MAIL_FROM, "WAIT_MAIL_FROM"},
        {SmtpsState::WAIT_RCPT_TO, "WAIT_RCPT_TO"},
        {SmtpsState::WAIT_DATA, "WAIT_DATA"},
        {SmtpsState::IN_MESSAGE, "IN_MESSAGE"},
        {SmtpsState::WAIT_QUIT, "WAIT_QUIT"},
        {SmtpsState::CLOSED, "CLOSED"}
    };
    auto it = state_names.find(state);
    if (it != state_names.end()) {
        return it->second;
    }
    return "UNKNOWN_STATE";
}

template <typename ConnectionType>
std::string TraditionalSmtpsFsm<ConnectionType>::get_event_name(SmtpsEvent event) {
    static const std::unordered_map<SmtpsEvent, std::string> event_names = {
        {SmtpsEvent::CONNECT, "CONNECT"},
        {SmtpsEvent::EHLO, "EHLO"},
        {SmtpsEvent::AUTH, "AUTH"},
        {SmtpsEvent::STARTTLS, "STARTTLS"},
        {SmtpsEvent::MAIL_FROM, "MAIL_FROM"},
        {SmtpsEvent::RCPT_TO, "RCPT_TO"},
        {SmtpsEvent::DATA, "DATA"},
        {SmtpsEvent::DATA_END, "DATA_END"},
        {SmtpsEvent::QUIT, "QUIT"},
        {SmtpsEvent::ERROR, "ERROR"},
        {SmtpsEvent::TIMEOUT, "TIMEOUT"}
    };
    auto it = event_names.find(event);
    if (it != event_names.end()) {
        return it->second;
    }
    return "UNKNOWN_EVENT";
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::auth_user_async(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session, const std::string& mail_address,
    const std::string& password, AuthCallback cb)
{
    if (!cb) return;
    if (!session) { cb(false, 0); return; }
    session->set_paused(true);
    auto executor = session->get_connection().executor();
    account_policy_->login_async(mail_address, password,
        [session, executor, cb = std::move(cb)](AccountAuthResult out) mutable {
            // Password work completes on a worker/DB thread. Resume protocol
            // state and buffered commands on the connection's serial executor,
            // after the input-dispatch loop which paused this session has exited.
            executor->post([session, cb = std::move(cb), out = std::move(out)]() mutable {
                if (session->is_closed()) return;
                cb(out.ok, out.ok ? out.shard : 0);
            });
        });
}


template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::user_exists_async(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session, const std::string& mail_address,
    std::function<void(bool exists)> cb)
{
    if (!cb) return;
    if (!session) { cb(false); return; }
    session->set_paused(true);
    auto executor = session->get_connection().executor();
    account_policy_->user_exists_async(mail_address,
        [session, executor, cb = std::move(cb)](bool exists) mutable {
            executor->post([session, cb = std::move(cb), exists]() mutable {
                if (session->is_closed()) return;
                cb(exists);
            });
        });
}

// RCPT TO 收件人解析：用户（缓存→DB）→ 别名（缓存→DB）→ 空（550）。
// pause 语义与 user_exists_async 一致：DB 查询前 set_paused(true)，回调不恢复，
// 统一由调用方写完成回调的 drain_buffered_commands() 收口。
template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::resolve_recipient_async(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session, const std::string& mail_address,
    std::function<void(std::vector<std::string>)> cb)
{
    if (!session) { cb({}); return; }

    AuthCacheEntry ce;
    if (m_recipientCache->lookup(mail_address, ce)) {
        if (ce.status == 1) { cb({mail_address}); return; }
        alias_targets_async(std::move(session), mail_address, std::move(cb));
        return;
    }
    user_exists_async(session, mail_address,
        [this, session, mail_address, cb = std::move(cb)](bool exists) mutable {
            if (exists) { cb({mail_address}); return; }
            alias_targets_async(session, mail_address, std::move(cb));
        });
}

// 别名目标查询：缓存（含负缓存=空 vector）→ DB。空结果（确认非别名）同样入缓存；
// DB 失败返回空但不缓存——提交路径对空回退为原样接受，不能把一次故障固化成 5 分钟非别名。
template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::alias_targets_async(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session, const std::string& alias_address,
    std::function<void(std::vector<std::string>)> cb)
{
    if (!cb) return;
    if (!session) {
        account_policy_->alias_targets_async(alias_address, std::move(cb));
        return;
    }
    session->set_paused(true);
    auto executor = session->get_connection().executor();
    account_policy_->alias_targets_async(alias_address,
        [session, executor, cb = std::move(cb)](std::vector<std::string> targets) mutable {
            executor->post([session, cb = std::move(cb), targets = std::move(targets)]() mutable {
                if (session->is_closed()) return;
                cb(std::move(targets));
            });
        });
}


// 每日发信配额：认证账号 MAIL FROM 时原子占用今日一个配额。
// 条件 UPDATE（跨天归零 + 配额 WHERE）后 SELECT ROW_COUNT() 读实际占用行数：
//   1 行 → 占用成功（allowed=true）；0 行 → 当日配额已超（allowed=false）。
// 失败语义：DB 故障 / 无连接 / 配额关闭（config<=0）→ fail-open 放行并 WARN。
// Account policy may complete inline or on a DB thread; protocol continuation
// always returns to the connection executor, including the disabled-quota path.
template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::check_send_quota_async(
    std::shared_ptr<pr::SessionBase<ConnectionType>> session,
    const std::string& mail_address,
    std::function<void(bool allowed)> cb)
{
    if (!cb) return;
    if (!session) { cb(true); return; }
    auto cfg = std::atomic_load(&static_cast<MailServerBase*>(session->get_server())->m_config);
    session->set_paused(true);
    auto executor = session->get_connection().executor();
    auto complete = [session, executor, cb = std::move(cb)](bool allowed) mutable {
        executor->post([session, cb = std::move(cb), allowed]() mutable {
            if (session->is_closed()) return;
            cb(allowed);
        });
    };
    if (!cfg || cfg->smtp_daily_send_limit <= 0) { complete(true); return; }
    account_policy_->check_send_quota_async(mail_address, cfg->smtp_daily_send_limit, std::move(complete));
}


template <typename ConnectionType>
std::future<bool> TraditionalSmtpsFsm<ConnectionType>::save_mail_metadata_async(
    mail* data, const std::string& file_path_prefix)
{
    std::future<bool> future;

    if (!data) {
        LOG_DATABASE_ERROR("Mail data is null in save_mail_metadata_async");
        return std::future<bool>();
    }

    if (!m_workerThreadPool) {
        LOG_DATABASE_ERROR("WorkerThreadPool is null in save_mail_metadata_async");
        return std::future<bool>();
    }

    mail& mail_data = *data;

    auto task = [this, file_path_prefix, mail_data]() -> bool {
        auto conn = this->acquire_connection(0);
        if (!conn->is_valid()) {
            LOG_DATABASE_ERROR("Failed to get database connection in async task");
            return false;
        }

        auto* conn_ptr = conn->operator->();

        bool success = true;

        // 第一步：插入邮件元数据到 mails 表
        std::string body_path = file_path_prefix;
        std::string mail_sql = db::sql::build_insert_mail_with_status(
            mail_data.id, mail_data.subject, body_path,
            mail_data.status, conn_ptr);
        LOG_DATABASE_DEBUG("Executing SQL: {}", mail_sql);
        if (!conn_ptr->execute(mail_sql)) {
            LOG_DATABASE_ERROR("Failed to insert mail metadata. Error: {}", conn_ptr->get_last_error());
            return false;
        }

        // 第二步：插入邮件收发件人关系到 mail_recipients 表
        std::string recipient_sql = db::sql::build_insert_recipients_simple(
            mail_data, conn_ptr);
        LOG_DATABASE_DEBUG("Executing SQL: {}", recipient_sql);
        if (!recipient_sql.empty() && !conn_ptr->execute(recipient_sql)) {
            LOG_DATABASE_ERROR("Failed to insert mail recipients. Error: {}", conn_ptr->get_last_error());
            // 如果插入收件人失败，删除已插入的邮件元数据
            conn_ptr->execute(db::sql::build_delete_mail_by_id(mail_data.id));
            return false;
        }

        // 第三步：插入附件元数据
        if (!mail_data.attachments.empty()) {
            std::string att_sql = db::sql::build_insert_attachments(
                mail_data.id, mail_data.attachments, conn_ptr);
            LOG_DATABASE_DEBUG("Executing SQL: {}", att_sql);
            if (!conn_ptr->execute(att_sql)) {
                LOG_DATABASE_ERROR("Failed to insert attachment metadata. Error: {}", conn_ptr->get_last_error());
                success = false;
            }
        }

        return success;
    };

    future = this->m_workerThreadPool->submit(std::move(task));

    return future;
}

template <typename ConnectionType>
std::future<bool> TraditionalSmtpsFsm<ConnectionType>::save_attachment_metadata_async(
    const attachment& att, size_t mail_id)
{
    if (!m_workerThreadPool) {
        LOG_DATABASE_ERROR("DBPool or WorkerThreadPool is null in save_attachment_metadata_async");
        return std::future<bool>();
    }

    auto task = [this, att, mail_id]() -> bool {
        auto conn = this->acquire_connection(0);
        if (!conn->is_valid()) {
            LOG_DATABASE_ERROR("Failed to get database connection in save_attachment_metadata_async");
            return false;
        }

        auto* conn_ptr = conn->operator->();
        if (!conn_ptr) {
            LOG_DATABASE_ERROR("Failed to cast to MySQLConnection");
            return false;
        }

        std::string att_sql = db::sql::build_insert_attachment_single(
            mail_id, att, conn_ptr);
        LOG_DATABASE_DEBUG("Executing SQL: {}", att_sql);
        if (!conn_ptr->execute(att_sql)) {
            LOG_DATABASE_ERROR("Failed to insert attachment metadata. Error: {}", conn_ptr->get_last_error());
            return false;
        }
        return true;
    };

    return this->m_workerThreadPool->submit(std::move(task));
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::remove_metadata_by_file_path(
    const std::vector<std::string>& file_paths)
{
    auto conn = acquire_connection(0);
    if (!conn->is_valid()) {
        LOG_DATABASE_ERROR("Failed to get database connection in remove_metadata_by_file_path");
        return;
    }

    // 注意：数据库中 body_path 字段存储的是文件路径
    std::string sql = "DELETE FROM mails WHERE body_path = ?";
    for (const auto& file_path : file_paths) {
        if (!conn->operator->()->execute(sql, {file_path})) {
            LOG_DATABASE_ERROR("Failed to delete mail metadata for file path: {}", file_path);
        }
    }
}

template <typename ConnectionType>
bool TraditionalSmtpsFsm<ConnectionType>::save_mail_body_to_file(
    mail* data, const std::string& file_path)
{
    if (!data) {
        LOG_FILE_IO_ERROR("Mail data is null in save_mail_body_to_file");
        return false;
    }

    LOG_FILE_IO_DEBUG("Saving mail body to file: {}, body size: {}", file_path, data->body.size());

    std::ofstream out(file_path);
    if (!out.is_open()) {
        LOG_FILE_IO_ERROR("Failed to open file for writing: {}", file_path);
        return false;
    }

    if (data->body.empty()) {
        out << data->header;
        out.close();
        LOG_FILE_IO_DEBUG("Mail body is empty, only header saved to file: {}", file_path);
        return true;
    }
    out << data->body;
    out.close();
    LOG_FILE_IO_DEBUG("Mail body saved successfully to file: {}", file_path);
    return true;
}

template <typename ConnectionType>
bool TraditionalSmtpsFsm<ConnectionType>::save_attachment_to_file(
    attachment& att, const std::string& file_path)
{
    std::ofstream out(file_path, std::ios::binary);
    if (!out.is_open()) {
        LOG_FILE_IO_ERROR("Failed to open attachment file for writing: {}", file_path);
        return false;
    }
    out.write(att.content.data(), static_cast<std::streamsize>(att.content.size()));
    out.close();
    att.filepath = file_path;
    att.file_size = att.content.size();
    att.content.clear(); // 释放内存
    LOG_FILE_IO_DEBUG("Attachment saved to file: {}", file_path);
    return true;
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::cleanup_failed_saves(
    std::vector<std::future<bool>>& futures, const std::vector<std::string>& file_paths)
{
    for (size_t i = 0; i < futures.size(); ++i) {
        if (futures[i].valid()) {
            bool success = futures[i].get();
            if (!success && i < file_paths.size()) {
                LOG_FILE_IO_ERROR("Database save failed, removing file: {}", file_paths[i]);
                std::remove(file_paths[i].c_str());
            }
        }
    }
}

} // namespace mail_system

#endif // TRADITIONAL_SMTPS_FSM_TPP
