#ifndef TRADITIONAL_SMTPS_FSM_H
#define TRADITIONAL_SMTPS_FSM_H

#include "mail_system/back/mailServer/session/session_base.h"
#include "mail_system/back/db/db_pool.h"
#include "mail_system/back/thread_pool/thread_pool_base.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_fsm.hpp"
#include "mail_system/back/common/logger.h"
#include <boost/asio/ssl.hpp>
#include <cstddef>
#include <functional>
#include <iostream>
#include <iostream>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>
#include <sstream>
#include <ctime>
#include <cctype>
#include <cstdio>
#include <atomic>

namespace mail_system {

// 传统的 SMTPS FSM 实现
template <typename ConnectionType>
class TraditionalSmtpsFsm : public SmtpsFsm<ConnectionType> {
public:
    TraditionalSmtpsFsm(
        std::shared_ptr<ThreadPoolBase> io_thread_pool,
        std::shared_ptr<ThreadPoolBase> worker_thread_pool,
        std::shared_ptr<DBPool> db_pool
    ) : SmtpsFsm<ConnectionType>(io_thread_pool, worker_thread_pool, db_pool) {
        init_transition_table();
        init_state_handlers();
    }

    ~TraditionalSmtpsFsm() override = default;

    // 处理事件
    void process_event(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        SmtpsEvent event,
        const std::string& args
    ) override {
        // 状态机详细日志（默认关闭）
        if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) {
            LOG_SMTP_DETAIL_DEBUG("Current State: {}, Event: {}",
                              SmtpsFsm<ConnectionType>::get_state_name(static_cast<SmtpsState>(session->get_current_state())),
                              SmtpsFsm<ConnectionType>::get_event_name(event));
        }

        if (static_cast<SmtpsState>(session->get_current_state()) == SmtpsState::CLOSED) {
            session->close();
            return;
        }

        auto transition_key = std::make_pair(
            static_cast<SmtpsState>(session->get_current_state()),
            event
        );
        auto transition_it = transition_table_.find(transition_key);

        if (transition_it != transition_table_.end()) {
            // 查找状态处理函数
            auto state_handler_it = state_handlers_.find(static_cast<SmtpsState>(session->get_current_state()));
            if (state_handler_it != state_handlers_.end()) {
                auto event_handler_it = state_handler_it->second.find(event);
                if (event_handler_it != state_handler_it->second.end()) {
                    // 执行状态处理函数
                    // 状态机详细日志（默认关闭）
                    if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) {
                        LOG_SMTP_DETAIL_DEBUG("Handling event {} in state {}",
                                          SmtpsFsm<ConnectionType>::get_event_name(event),
                                          SmtpsFsm<ConnectionType>::get_state_name(static_cast<SmtpsState>(session->get_current_state())));
                    }
                    auto handler = event_handler_it->second;
                    handler(std::move(session), args);
                    return;
                }
            }

            // 状态机详细日志（默认关闭）
            if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) {
                LOG_SMTP_DETAIL_WARN("No handler for state {} and event {}",
                                     SmtpsFsm<ConnectionType>::get_state_name(static_cast<SmtpsState>(session->get_current_state())),
                                     SmtpsFsm<ConnectionType>::get_event_name(event));
            }
        } else {
            // 无效的状态转换
            if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) {
                LOG_SMTP_ERROR("Invalid transition from {} on event {}",
                               SmtpsFsm<ConnectionType>::get_state_name(static_cast<SmtpsState>(session->get_current_state())),
                               SmtpsFsm<ConnectionType>::get_event_name(event));
            }
            handle_error(std::move(session), "Invalid command sequence");
        }
    }

    void auto_process_event(
        std::unique_ptr<SessionBase<ConnectionType>> session
    ) {
        SmtpsEvent event = static_cast<SmtpsEvent>(session->get_next_event());
        std::string args = session->get_last_command_args();
        process_event(std::move(session), event, args);
    }

private:
    // 状态转换表类型
    using StateTransitionTable = std::map<std::pair<SmtpsState, SmtpsEvent>, SmtpsState>;

    // 状态转换表
    StateTransitionTable transition_table_;

    // 状态处理函数表
    std::map<SmtpsState, std::map<SmtpsEvent, StateHandler<ConnectionType>>> state_handlers_;

    struct MimeParseResult {
        std::string text_body;
        std::vector<attachment> attachments;
    };

    static std::string sanitize_filename(const std::string& name) {
        std::string out;
        out.reserve(name.size());
        for (char c : name) {
            if (c == '/' || c == '\\' || c == ' ') {
                out.push_back('_');
            } else {
                out.push_back(c);
            }
        }
        if (out.empty()) {
            return "attachment";
        }
        return out;
    }

    static std::string to_lower(std::string s) {
        for (auto& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    }

    static std::unordered_map<std::string, std::string> parse_headers_map(const std::string& header_block) {
        std::unordered_map<std::string, std::string> headers;
        std::istringstream iss(header_block);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            auto pos = line.find(':');
            if (pos == std::string::npos) {
                continue;
            }
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            // trim leading spaces
            size_t first = value.find_first_not_of(" \t");
            if (first != std::string::npos) {
                value = value.substr(first);
            } else {
                value.clear();
            }
            headers[to_lower(key)] = value;
        }
        return headers;
    }

    static std::string get_header_value(const std::unordered_map<std::string, std::string>& headers, const std::string& key) {
        auto it = headers.find(to_lower(key));
        if (it != headers.end()) {
            return it->second;
        }
        return "";
    }

    static std::string extract_boundary(const std::string& content_type) {
        std::regex boundary_regex(R"(boundary\s*=\s*\"?([^";]+)\"?)", std::regex_constants::icase);
        std::smatch m;
        if (std::regex_search(content_type, m, boundary_regex) && m.size() > 1) {
            return m[1];
        }
        return "";
    }

    static void cleanup_streamed_attachments(SmtpsContext* ctx) {
        if (!ctx) {
            return;
        }
        for (auto& att : ctx->streamed_attachments) {
            if (!att.filepath.empty()) {
                std::remove(att.filepath.c_str());
            }
        }
        ctx->streamed_attachments.clear();
    }

    static std::string ensure_trailing_slash(std::string path) {
        if (!path.empty() && path.back() != '/' && path.back() != '\\') {
            path.push_back('/');
        }
        return path;
    }

    static void cleanup_mail_files(const std::string& body_path, const std::vector<std::string>& attachment_paths) {
        if (!body_path.empty()) {
            std::remove(body_path.c_str());
        }
        for (const auto& p : attachment_paths) {
            if (!p.empty()) {
                std::remove(p.c_str());
            }
        }
    }

    bool persist_mails_sync(SessionBase<ConnectionType>* session, std::vector<std::unique_ptr<mail>>& mails, std::string& error) {
        if (mails.empty()) {
            LOG_SMTP_DETAIL_WARN("No mails to persist");
            return true;
        }

        static std::atomic<uint64_t> counter{static_cast<uint64_t>(std::time(nullptr)) << 32};
        auto gen_id = []() -> uint64_t { return ++counter; };

        LOG_SMTP_DETAIL_INFO("Starting to persist " + std::to_string(mails.size()) + " mail(s)");

        for (auto& m : mails) {
            m->id = gen_id();
            std::string base_path = ensure_trailing_slash(session->get_server()->m_config.mail_storage_path);
            std::string file_path = base_path + std::to_string(m->id);

            LOG_SMTP_DETAIL_INFO("Mail ID: " + std::to_string(m->id) + ", Body size: " + std::to_string(m->body.size()));
            LOG_SMTP_DETAIL_INFO("Saving to file: " + file_path);

            if (!this->save_mail_body_to_file(m.get(), file_path)) {
                error = "Failed to save mail body";
                cleanup_mail_files(file_path, {});
                return false;
            }

            std::vector<std::string> attachment_paths;
            LOG_SMTP_DETAIL_INFO("Processing " + std::to_string(m->attachments.size()) + " attachment(s)");
            for (auto& att : m->attachments) {
                std::string att_base_path = ensure_trailing_slash(session->get_server()->m_config.attachment_storage_path);
                if (att.filepath.empty()) {
                    std::string safe_name = sanitize_filename(att.filename);
                    att.filepath = att_base_path + std::to_string(std::time(nullptr)) + "_" + safe_name;
                    att.file_size = att.content.size();
                    att.upload_time = std::time(nullptr);
                    LOG_SMTP_DETAIL_INFO("Saving attachment: " + att.filepath);
                    if (!this->save_attachment_to_file(att, att.filepath)) {
                        error = "Failed to save attachment";
                        cleanup_mail_files(file_path, attachment_paths);
                        return false;
                    }
                } else {
                    LOG_SMTP_DETAIL_INFO("Attachment already saved: " + att.filepath);
                }
                attachment_paths.push_back(att.filepath);
            }

            auto future = this->save_mail_metadata_async(m.get(), file_path);
            bool ok = future.valid() ? future.get() : false;
            if (!ok) {
                error = "Failed to save mail metadata";
                cleanup_mail_files(file_path, attachment_paths);
                return false;
            }
            m->body_path = file_path;
            LOG_SMTP_DETAIL_INFO("Mail persisted successfully, body_path: " + file_path);
        }
        LOG_SMTP_DETAIL_INFO("All mails persisted successfully");
        return true;
    }

    bool persist_and_reply(std::unique_ptr<SessionBase<ConnectionType>> session) {
        std::string error;
        auto mails = session->get_mails();
        LOG_SMTP_DETAIL_INFO("Entered persist_and_reply");
        bool ok = persist_mails_sync(session.get(), mails, error);
        std::string reply = ok ? "221 Bye\r\n" : ("451 " + error + "\r\n");
        SessionBase<ConnectionType>::do_async_write(
            std::move(session),
            reply,
            [] (std::unique_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code&) mutable {
                s->close();
            }
        );
        return ok;
    }

    static std::string decode_base64(const std::string& input) {
        static const int decode_table[256] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
            52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
            15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
            41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
        };

        std::string out;
        int val = 0;
        int bits = -8;
        for (unsigned char c : input) {
            if (decode_table[c] == -1) {
                continue;
            }
            val = (val << 6) + decode_table[c];
            bits += 6;
            if (bits >= 0) {
                out.push_back(static_cast<char>((val >> bits) & 0xFF));
                bits -= 8;
            }
        }
        return out;
    }

    static MimeParseResult parse_mime(const std::string& header_block, const std::string& body_block) {
        MimeParseResult result;
        auto headers = parse_headers_map(header_block);
        std::string content_type = get_header_value(headers, "content-type");
        std::string lower_ct = to_lower(content_type);

        if (lower_ct.find("multipart/") == std::string::npos) {
            result.text_body = body_block;
            return result;
        }

        std::string boundary = extract_boundary(content_type);
        if (boundary.empty()) {
            result.text_body = body_block;
            return result;
        }

        std::string boundary_marker = "--" + boundary;
        std::string boundary_end = boundary_marker + "--";

        size_t pos = 0;
        while (true) {
            size_t start = body_block.find(boundary_marker, pos);
            if (start == std::string::npos) {
                break;
            }
            start += boundary_marker.size();
            if ((start + 1) < body_block.size() && body_block[start] == '\r' && body_block[start + 1] == '\n') {
                start += 2;
            }

            size_t next = body_block.find(boundary_marker, start);
            if (next == std::string::npos) {
                next = body_block.find(boundary_end, start);
            }
            if (next == std::string::npos) {
                break;
            }
            std::string part = body_block.substr(start, next - start);
            pos = next;

            size_t sep = part.find("\r\n\r\n");
            if (sep == std::string::npos) {
                continue;
            }
            std::string part_header = part.substr(0, sep);
            std::string part_body = part.substr(sep + 4);

            auto part_headers = parse_headers_map(part_header);
            std::string disp = get_header_value(part_headers, "content-disposition");
            std::string p_ct = to_lower(get_header_value(part_headers, "content-type"));
            std::string encoding = to_lower(get_header_value(part_headers, "content-transfer-encoding"));

            bool is_attachment = disp.find("attachment") != std::string::npos || disp.find("filename") != std::string::npos;
            if (is_attachment) {
                std::string filename;
                std::regex filename_regex(R"(filename\s*=\s*\"?([^";]+)\"?)", std::regex_constants::icase);
                std::smatch m;
                if (std::regex_search(disp, m, filename_regex) && m.size() > 1) {
                    filename = m[1];
                }
                if (filename.empty()) {
                    filename = "attachment";
                }
                attachment att;
                att.filename = filename;
                att.mime_type = p_ct.empty() ? "application/octet-stream" : p_ct;
                if (encoding.find("base64") != std::string::npos) {
                    att.content = decode_base64(part_body);
                } else {
                    att.content = part_body;
                }
                att.file_size = att.content.size();
                att.upload_time = std::time(nullptr);
                result.attachments.push_back(std::move(att));
                continue;
            }

            if (p_ct.find("text/plain") != std::string::npos && result.text_body.empty()) {
                result.text_body = part_body;
            } else if (p_ct.find("text/html") != std::string::npos && result.text_body.empty()) {
                result.text_body = part_body;
            }
        }

        if (result.text_body.empty()) {
            result.text_body = body_block;
        }
        return result;
    }

    // 初始化状态转换表
    void init_transition_table() {
        transition_table_[std::make_pair(SmtpsState::INIT, SmtpsEvent::CONNECT)] = SmtpsState::GREETING;
        transition_table_[std::make_pair(SmtpsState::WAIT_EHLO, SmtpsEvent::EHLO)] = SmtpsState::WAIT_AUTH;
        transition_table_[std::make_pair(SmtpsState::GREETING, SmtpsEvent::EHLO)] = SmtpsState::WAIT_AUTH;
        if constexpr (std::is_same_v<ConnectionType, boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>)
            transition_table_[std::make_pair(SmtpsState::WAIT_AUTH, SmtpsEvent::STARTTLS)] = SmtpsState::INIT;
        transition_table_[std::make_pair(SmtpsState::WAIT_AUTH, SmtpsEvent::AUTH)] = SmtpsState::WAIT_AUTH_USERNAME;
        transition_table_[std::make_pair(SmtpsState::WAIT_AUTH_USERNAME, SmtpsEvent::AUTH)] = SmtpsState::WAIT_AUTH_PASSWORD;
        transition_table_[std::make_pair(SmtpsState::WAIT_AUTH_PASSWORD, SmtpsEvent::AUTH)] = SmtpsState::WAIT_MAIL_FROM;
        transition_table_[std::make_pair(SmtpsState::WAIT_AUTH, SmtpsEvent::MAIL_FROM)] = SmtpsState::WAIT_RCPT_TO;
        transition_table_[std::make_pair(SmtpsState::WAIT_MAIL_FROM, SmtpsEvent::MAIL_FROM)] = SmtpsState::WAIT_RCPT_TO;
        transition_table_[std::make_pair(SmtpsState::WAIT_RCPT_TO, SmtpsEvent::RCPT_TO)] = SmtpsState::WAIT_DATA;
        transition_table_[std::make_pair(SmtpsState::WAIT_DATA, SmtpsEvent::DATA)] = SmtpsState::IN_MESSAGE;
        transition_table_[std::make_pair(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA)] = SmtpsState::IN_MESSAGE;
        transition_table_[std::make_pair(SmtpsState::IN_MESSAGE, SmtpsEvent::DATA_END)] = SmtpsState::WAIT_QUIT;
        transition_table_[std::make_pair(SmtpsState::WAIT_QUIT, SmtpsEvent::MAIL_FROM)] = SmtpsState::WAIT_RCPT_TO;

        // QUIT 命令可以在多个状态下接收
        for (int i = 0; i < static_cast<int>(SmtpsState::CLOSED); ++i) {
            transition_table_[std::make_pair(static_cast<SmtpsState>(i), SmtpsEvent::QUIT)] = SmtpsState::CLOSED;
        }

        for (int i = 0; i < static_cast<int>(SmtpsState::CLOSED); ++i) {
            transition_table_[std::make_pair(static_cast<SmtpsState>(i), SmtpsEvent::ERROR)] = static_cast<SmtpsState>(i);
            transition_table_[std::make_pair(static_cast<SmtpsState>(i), SmtpsEvent::TIMEOUT)] = static_cast<SmtpsState>(i);
        }
    }

    // 初始化状态处理函数
    void init_state_handlers() {
        state_handlers_[SmtpsState::INIT][SmtpsEvent::CONNECT] =
            std::bind(&TraditionalSmtpsFsm::handle_init_connect, this,
                      std::placeholders::_1, std::placeholders::_2);

        state_handlers_[SmtpsState::WAIT_EHLO][SmtpsEvent::EHLO] =
            std::bind(&TraditionalSmtpsFsm::handle_greeting_ehlo, this,
                      std::placeholders::_1, std::placeholders::_2);

        state_handlers_[SmtpsState::GREETING][SmtpsEvent::EHLO] =
            std::bind(&TraditionalSmtpsFsm::handle_greeting_ehlo, this,
                      std::placeholders::_1, std::placeholders::_2);

        if constexpr (std::is_same_v<ConnectionType, boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>)
            state_handlers_[SmtpsState::WAIT_AUTH][SmtpsEvent::STARTTLS] =
                std::bind(&TraditionalSmtpsFsm::handle_wait_auth_starttls, this,
                        std::placeholders::_1, std::placeholders::_2);

        state_handlers_[SmtpsState::WAIT_AUTH][SmtpsEvent::AUTH] =
            std::bind(&TraditionalSmtpsFsm::handle_wait_auth_auth, this,
                      std::placeholders::_1, std::placeholders::_2);

        state_handlers_[SmtpsState::WAIT_AUTH_USERNAME][SmtpsEvent::AUTH] =
            std::bind(&TraditionalSmtpsFsm::handle_wait_auth_username, this,
                      std::placeholders::_1, std::placeholders::_2);

        state_handlers_[SmtpsState::WAIT_AUTH_PASSWORD][SmtpsEvent::AUTH] =
            std::bind(&TraditionalSmtpsFsm::handle_wait_auth_password, this,
                      std::placeholders::_1, std::placeholders::_2);

        state_handlers_[SmtpsState::WAIT_AUTH][SmtpsEvent::MAIL_FROM] =
            std::bind(&TraditionalSmtpsFsm::handle_wait_auth_mail_from, this,
                      std::placeholders::_1, std::placeholders::_2);

        state_handlers_[SmtpsState::WAIT_MAIL_FROM][SmtpsEvent::MAIL_FROM] =
            std::bind(&TraditionalSmtpsFsm::handle_wait_mail_from_mail_from, this,
                      std::placeholders::_1, std::placeholders::_2);

        state_handlers_[SmtpsState::WAIT_RCPT_TO][SmtpsEvent::RCPT_TO] =
            std::bind(&TraditionalSmtpsFsm::handle_wait_rcpt_to_rcpt_to, this,
                      std::placeholders::_1, std::placeholders::_2);

        state_handlers_[SmtpsState::WAIT_DATA][SmtpsEvent::DATA] =
            std::bind(&TraditionalSmtpsFsm::handle_wait_data_data, this,
                      std::placeholders::_1, std::placeholders::_2);

        state_handlers_[SmtpsState::IN_MESSAGE][SmtpsEvent::DATA] =
            std::bind(&TraditionalSmtpsFsm::handle_in_message_data, this,
                      std::placeholders::_1, std::placeholders::_2);

        state_handlers_[SmtpsState::IN_MESSAGE][SmtpsEvent::DATA_END] =
            std::bind(&TraditionalSmtpsFsm::handle_in_message_data_end, this,
                      std::placeholders::_1, std::placeholders::_2);

        for (int i = 1; i < static_cast<int>(SmtpsState::WAIT_QUIT) + 1; ++i) {
            state_handlers_[static_cast<SmtpsState>(i)][SmtpsEvent::QUIT] =
                std::bind(&TraditionalSmtpsFsm::handle_wait_quit_quit, this,
                          std::placeholders::_1, std::placeholders::_2);
        }

        state_handlers_[SmtpsState::WAIT_QUIT][SmtpsEvent::MAIL_FROM] =
            std::bind(&TraditionalSmtpsFsm::handle_wait_auth_mail_from, this,
                      std::placeholders::_1, std::placeholders::_2);

        for (int i = 0; i < static_cast<int>(SmtpsState::WAIT_QUIT) + 1; ++i) {
            state_handlers_[static_cast<SmtpsState>(i)][SmtpsEvent::ERROR] =
                std::bind(&TraditionalSmtpsFsm::handle_error, this,
                          std::placeholders::_1, std::placeholders::_2);
        }
    }

    // 处理 CONNECT 事件
    void handle_init_connect(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        const std::string& args
    ) {
        session->set_current_state(static_cast<int>(SmtpsState::GREETING));
        SessionBase<ConnectionType>::do_async_write(
            std::move(session),
            "220 SMTPS Server\r\n",
            [] (
                std::unique_ptr<SessionBase<ConnectionType>> self,
                const boost::system::error_code& ec
            ) mutable {
                if (ec) {
                    LOG_SMTP_DETAIL_ERROR("Error sending greeting: {}", ec.message());
                    return;
                }
                self->set_current_state(static_cast<int>(SmtpsState::WAIT_EHLO));
                LOG_SMTP_DETAIL_INFO("Sent greeting, waiting for EHLO...");
                SessionBase<ConnectionType>::do_async_read(std::move(self), [] (
                    std::unique_ptr<SessionBase<ConnectionType>> s,
                    const boost::system::error_code& error,
                    std::size_t bytes_transferred
                ) mutable {
                    if (error) {
                        LOG_SMTP_DETAIL_ERROR("Error reading after greeting: {}", error.message());
                        return;
                    }
                    //s->set_next_event(static_cast<int>(SmtpsEvent::EHLO));
                    auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(s->get_fsm());
                    fsm->auto_process_event(std::move(s));
                });
            }
        );
    }

    // 处理 EHLO 事件
    void handle_greeting_ehlo(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        const std::string& args
    ) {
        LOG_SMTP_DETAIL_DEBUG("Received EHLO: {}", args);
        
        std::string response = "250-" + args + " Hello\r\n";
        response += "250-SIZE 10240000\r\n"
                   "250-8BITMIME\r\n";

        // 如果是 TCP 连接，通告 STARTTLS
        if constexpr (!std::is_same_v<ConnectionType, SslConnection>) {
            response += "250-STARTTLS\r\n";
        }

        response += "250 SMTPUTF8\r\n";

        SessionBase<ConnectionType>::do_async_write(
            std::move(session),
            response,
            [] (
                std::unique_ptr<SessionBase<ConnectionType>> s,
                const boost::system::error_code& ec
            ) mutable {
                if (ec) {
                    LOG_SMTP_DETAIL_ERROR("Error sending EHLO response: {}", ec.message());
                    return;
                }
                s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                SessionBase<ConnectionType>::do_async_read(std::move(s), [] (
                    std::unique_ptr<SessionBase<ConnectionType>> sss,
                    const boost::system::error_code& error,
                    std::size_t bytes_transferred
                ) mutable {
                    if (error) {
                        LOG_SMTP_DETAIL_ERROR("Error reading after EHLO: {}", error.message());
                        return;
                    }
                    auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(sss->get_fsm());
                    fsm->auto_process_event(std::move(sss));
                });
            }
        );
    }

    // 处理 STARTTLS 命令
    // template<typename T = ConnectionType>
    // std::enable_if_t<std::is_same_v<T, boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>>
    void handle_wait_auth_starttls(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        const std::string& args
    ) {
        SessionBase<ConnectionType>::do_async_write(
            std::move(session),
            "220 Ready to start TLS\r\n",
            [this](std::unique_ptr<SessionBase<ConnectionType>> self,
                     const boost::system::error_code& ec) mutable {
                if (ec) {
                    LOG_SMTP_DETAIL_ERROR("Error sending STARTTLS response: {}", ec.message());
                    return;
                }
                self->set_current_state(static_cast<int>(SmtpsState::INIT));
                auto server = self->get_server();
                auto tcp_sock = self->release_connection()->release_socket();
                auto ssl_stream = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(
                    std::move(*tcp_sock),
                    server->get_ssl_context()
                );
                server->pass_stream(std::move(ssl_stream));
            }
        );
    }

    // 处理 AUTH 命令
    void handle_wait_auth_auth(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        const std::string& args
    ) {
        SessionBase<ConnectionType>::do_async_write(
            std::move(session),
            "334 VXNlcm5hbWU6\r\n",  // "Username:" in base64
            [] (
                std::unique_ptr<SessionBase<ConnectionType>> s,
                const boost::system::error_code& ec
            ) mutable {
                if (ec) {
                    LOG_SMTP_DETAIL_ERROR("Error sending username prompt: {}", ec.message());
                    return;
                }
                s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH_USERNAME));
                SessionBase<ConnectionType>::do_async_read(std::move(s), [] (
                    std::unique_ptr<SessionBase<ConnectionType>> sss,
                    const boost::system::error_code& error,
                    std::size_t bytes_transferred
                ) mutable {
                    if (error) {
                        LOG_SMTP_DETAIL_ERROR("Error reading username: {}", error.message());
                        return;
                    }
                    sss->set_next_event(int(SmtpsEvent::AUTH));
                    auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(sss->get_fsm());
                    fsm->auto_process_event(std::move(sss));
                });
            }
        );
    }

    // 处理用户名输入
    void handle_wait_auth_username(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        const std::string& args
    ) {
        static_cast<SmtpsContext*>(session->get_context())->client_username = args;
        SessionBase<ConnectionType>::do_async_write(
            std::move(session),
            "334 UGFzc3dvcmQ6\r\n",  // "Password:" in base64
            [] (
                std::unique_ptr<SessionBase<ConnectionType>> s,
                const boost::system::error_code& ec
            ) mutable {
                if (ec) {
                    LOG_SMTP_DETAIL_ERROR("Error sending password prompt: {}", ec.message());
                    return;
                }
                s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH_PASSWORD));
                SessionBase<ConnectionType>::do_async_read(std::move(s), [] (
                    std::unique_ptr<SessionBase<ConnectionType>> sss,
                    const boost::system::error_code& error,
                    std::size_t bytes_transferred
                ) mutable {
                    if (error) {
                        LOG_SMTP_DETAIL_ERROR("Error reading password: {}", error.message());
                        return;
                    }
                    sss->set_next_event(int(SmtpsEvent::AUTH));
                    auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(sss->get_fsm());
                    fsm->auto_process_event(std::move(sss));
                });
            }
        );
    }

    // 处理密码输入
    void handle_wait_auth_password(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        const std::string& args
    ) {
        std::string username = static_cast<SmtpsContext*>(session->get_context())->client_username;
        if (this->auth_user(session.get(), username, args)) {
            static_cast<SmtpsContext*>(session->get_context())->is_authenticated = true;
            SessionBase<ConnectionType>::do_async_write(
                std::move(session),
                "235 Authentication successful\r\n",
                [] (
                    std::unique_ptr<SessionBase<ConnectionType>> s,
                    const boost::system::error_code& ec
                ) mutable {
                    if (ec) {
                        LOG_SMTP_DETAIL_ERROR("Error sending auth success: {}", ec.message());
                        return;
                    }
                    s->set_current_state(static_cast<int>(SmtpsState::WAIT_MAIL_FROM));
                    SessionBase<ConnectionType>::do_async_read(std::move(s), [] (
                        std::unique_ptr<SessionBase<ConnectionType>> sss,
                        const boost::system::error_code& error,
                        std::size_t bytes_transferred
                    ) mutable {
                        if (error) {
                            LOG_SMTP_DETAIL_ERROR("Error reading after auth: {}", error.message());
                            return;
                        }
                        auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(sss->get_fsm());
                        fsm->auto_process_event(std::move(sss));
                    });
                }
            );
        } else {
            SessionBase<ConnectionType>::do_async_write(
                std::move(session),
                "535 Authentication failed\r\n",
                [] (
                    std::unique_ptr<SessionBase<ConnectionType>> s,
                    const boost::system::error_code& ec
                ) mutable {
                    if (ec) {
                        LOG_SMTP_DETAIL_ERROR("Error sending auth failed: {}", ec.message());
                        return;
                    }
                    s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                    SessionBase<ConnectionType>::do_async_read(std::move(s), [] (
                        std::unique_ptr<SessionBase<ConnectionType>> sss,
                        const boost::system::error_code& error,
                        std::size_t bytes_transferred
                    ) mutable {
                        if (error) {
                            LOG_SMTP_DETAIL_ERROR("Error reading after auth failed: {}", error.message());
                            return;
                        }
                        auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(sss->get_fsm());
                        fsm->auto_process_event(std::move(sss));
                    });
                }
            );
        }
    }

    // 处理 MAIL FROM（可选认证路径）
    void handle_wait_auth_mail_from(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        const std::string& args
    ) {
        std::regex mail_from_regex(R"(FROM:\s*<([^>]*)>)", std::regex_constants::icase);
        std::smatch matches;
        if (std::regex_search(args, matches, mail_from_regex) && matches.size() > 1) {
            static_cast<SmtpsContext*>(session->get_context())->sender_address = matches[1];
            SessionBase<ConnectionType>::do_async_write(
                std::move(session),
                "250 Ok\r\n",
                [] (
                    std::unique_ptr<SessionBase<ConnectionType>> s,
                    const boost::system::error_code& ec
                ) mutable {
                    if (ec) {
                        LOG_SMTP_DETAIL_ERROR("Error sending MAIL FROM response: {}", ec.message());
                        return;
                    }
                    s->set_current_state(static_cast<int>(SmtpsState::WAIT_RCPT_TO));
                    SessionBase<ConnectionType>::do_async_read(std::move(s), [] (
                        std::unique_ptr<SessionBase<ConnectionType>> sss,
                        const boost::system::error_code& error,
                        std::size_t bytes_transferred
                    ) mutable {
                        if (error) {
                            LOG_SMTP_DETAIL_ERROR("Error reading after MAIL FROM: {}", error.message());
                            return;
                        }
                        auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(sss->get_fsm());
                        fsm->auto_process_event(std::move(sss));
                    });
                }
            );
        } else {
            SessionBase<ConnectionType>::do_async_write(
                std::move(session),
                "501 Syntax error in parameters or arguments\r\n",
                [] (
                    std::unique_ptr<SessionBase<ConnectionType>> s,
                    const boost::system::error_code& ec
                ) mutable {
                    if (ec) {
                        LOG_SMTP_DETAIL_ERROR("Error sending MAIL FROM error: {}", ec.message());
                        return;
                    }
                    s->set_current_state(static_cast<int>(SmtpsState::WAIT_AUTH));
                    SessionBase<ConnectionType>::do_async_read(std::move(s), [] (
                        std::unique_ptr<SessionBase<ConnectionType>> sss,
                        const boost::system::error_code& error,
                        std::size_t bytes_transferred
                    ) mutable {
                        if (error) {
                            LOG_SMTP_DETAIL_ERROR("Error reading after MAIL FROM error: {}", error.message());
                            return;
                        }
                        auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(sss->get_fsm());
                        fsm->auto_process_event(std::move(sss));
                    });
                }
            );
        }
    }

    // 处理 MAIL FROM（必需认证路径）
    void handle_wait_mail_from_mail_from(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        const std::string& args
    ) {
        handle_wait_auth_mail_from(std::move(session), args);
    }

    // 处理 RCPT TO
    void handle_wait_rcpt_to_rcpt_to(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        const std::string& args
    ) {
        std::regex rcpt_to_regex(R"(TO:\s*<([^>]*)>)", std::regex_constants::icase);
        std::smatch matches;
        if (std::regex_search(args, matches, rcpt_to_regex) && matches.size() > 1) {
            static_cast<SmtpsContext*>(session->get_context())->recipient_addresses.push_back(matches[1]);
            SessionBase<ConnectionType>::do_async_write(
                std::move(session),
                "250 Ok\r\n",
                [] (
                    std::unique_ptr<SessionBase<ConnectionType>> s,
                    const boost::system::error_code& ec
                ) mutable {
                    if (ec) {
                        LOG_SMTP_DETAIL_ERROR("Error sending RCPT TO response: {}", ec.message());
                        return;
                    }
                    s->set_current_state(static_cast<int>(SmtpsState::WAIT_DATA));
                    SessionBase<ConnectionType>::do_async_read(std::move(s), [] (
                        std::unique_ptr<SessionBase<ConnectionType>> sss,
                        const boost::system::error_code& error,
                        std::size_t bytes_transferred
                    ) mutable {
                        if (error) {
                            LOG_SMTP_DETAIL_ERROR("Error reading after RCPT TO: {}", error.message());
                            return;
                        }
                        auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(sss->get_fsm());
                        fsm->auto_process_event(std::move(sss));
                    });
                }
            );
        } else {
            SessionBase<ConnectionType>::do_async_write(
                std::move(session),
                "501 Syntax error in parameters or arguments\r\n",
                [] (
                    std::unique_ptr<SessionBase<ConnectionType>> s,
                    const boost::system::error_code& ec
                ) mutable {
                    if (ec) {
                        LOG_SMTP_DETAIL_ERROR("Error sending RCPT TO error: {}", ec.message());
                        return;
                    }
                    SessionBase<ConnectionType>::do_async_read(std::move(s), [] (
                        std::unique_ptr<SessionBase<ConnectionType>> sss,
                        const boost::system::error_code& error,
                        std::size_t bytes_transferred
                    ) mutable {
                        if (error) {
                            LOG_SMTP_DETAIL_ERROR("Error reading after RCPT TO error: {}", error.message());
                            return;
                        }
                        auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(sss->get_fsm());
                        fsm->auto_process_event(std::move(sss));
                    });
                }
            );
        }
    }

    // 处理 DATA 命令
    void handle_wait_data_data(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        const std::string& args
    ) {
        SessionBase<ConnectionType>::do_async_write(
            std::move(session),
            "354 Start mail input; end with <CRLF>.<CRLF>\r\n",
            [] (
                std::unique_ptr<SessionBase<ConnectionType>> s,
                const boost::system::error_code& ec
            ) mutable {
                if (ec) {
                    LOG_SMTP_DETAIL_ERROR("Error sending DATA response: {}", ec.message());
                    return;
                }
                s->set_current_state(static_cast<int>(SmtpsState::IN_MESSAGE));
                SessionBase<ConnectionType>::do_async_read(std::move(s), [] (
                    std::unique_ptr<SessionBase<ConnectionType>> sss,
                    const boost::system::error_code& error,
                    std::size_t bytes_transferred
                ) mutable {
                    if (error) {
                        LOG_SMTP_DETAIL_ERROR("Error reading mail data: {}", error.message());
                        return;
                    }
                    if (sss->get_next_event() < int(SmtpsEvent::DATA_END))
                        sss->set_next_event(int(SmtpsEvent::DATA));
                    auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(sss->get_fsm());
                    fsm->auto_process_event(std::move(sss));
                });
            }
        );
    }

    // 处理消息数据
    void handle_in_message_data(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        const std::string& args
    ) {
        // 正文内容已经在 session 层 parse_smtp_command/process_message_data 中累计，
        // 这里不再重复追加，避免正文被写入两次。
        // 继续读取
        SessionBase<ConnectionType>::do_async_read(std::move(session), [] (
            std::unique_ptr<SessionBase<ConnectionType>> s,
            const boost::system::error_code& error,
            std::size_t bytes_transferred
        ) mutable {
            if (error) {
                LOG_SMTP_DETAIL_ERROR("Error reading message data: {}", error.message());
                return;
            }
            auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(s->get_fsm());
            fsm->auto_process_event(std::move(s));
        });
    }

    // 处理消息结束
    void handle_in_message_data_end(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        const std::string& args
    ) {
        auto* ctx = static_cast<SmtpsContext*>(session->get_context());
        if (ctx->body_limit_exceeded) {
            cleanup_streamed_attachments(ctx);
            std::string resp = ctx->abort_reason.empty() ? "552 Message size exceeds limit\r\n" : ctx->abort_reason + "\r\n";
            SessionBase<ConnectionType>::do_async_write(
                std::move(session),
                resp,
                [] (std::unique_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code&) mutable {
                    s->close();
                }
            );
            return;
        }

        SessionBase<ConnectionType>::do_async_write(
            std::move(session),
            "250 Message accepted for delivery\r\n",
            [] (
                std::unique_ptr<SessionBase<ConnectionType>> s,
                const boost::system::error_code& ec
            ) mutable {
                if (ec) {
                    LOG_SMTP_DETAIL_ERROR("Error sending message end response: {}", ec.message());
                    return;
                }
                LOG_SMTP_DETAIL_INFO("Callback in handle_in_message_data_end");
                if (!s->mail_) {
                    s->mail_ = std::make_unique<mail>();
                }
                if (static_cast<SmtpsContext*>(s->get_context())->sender_address == "@mail.com") {  // 发件人为本系统用户
                    s->mail_->status = 1; // 已发送但未读
                } else {
                    s->mail_->status = 2; // 已接收但未发送
                }
                s->mail_->send_time = std::time(nullptr);
                auto* ctx = static_cast<SmtpsContext*>(s->get_context());
                std::string header;
                std::string body;
                LOG_SMTP_DETAIL_INFO("[handle_in_message_data_end] streaming_enabled: {}", ctx->streaming_enabled);

                if (ctx->streaming_enabled) {
                    header = ctx->header_buffer;
                    body = ctx->text_body_buffer;
                    LOG_SMTP_DETAIL_INFO("[handle_in_message_data_end] Streaming mode - header size: {}, body size: {}", header.size(), body.size());
                    for (auto& att : ctx->streamed_attachments) {
                        LOG_SMTP_DETAIL_INFO("[handle_in_message_data_end] Streaming attachment: {}, path: {}", att.filename, att.filepath);
                        s->mail_->attachments.push_back(std::move(att));
                    }
                } else {
                    std::string& mail_data = ctx->mail_data;
                    size_t pos = mail_data.find("\r\n\r\n");
                    if (pos != std::string::npos) {
                        header = mail_data.substr(0, pos);
                        body = mail_data.substr(pos + 4);
                    } else {
                        header = mail_data;
                        body = "";
                    }
                    LOG_SMTP_DETAIL_INFO("[handle_in_message_data_end] Non-streaming mode - header size: {}, body size: {}", header.size(), body.size());
                    auto parsed_mime = parse_mime(header, body);
                    body = parsed_mime.text_body;
                    LOG_SMTP_DETAIL_INFO("[handle_in_message_data_end] Parsed body size: {}", body.size());
                    for (auto& att : parsed_mime.attachments) {
                        LOG_SMTP_DETAIL_INFO("[handle_in_message_data_end] Parsed attachment: {}", att.filename);
                        s->mail_->attachments.push_back(std::move(att));
                    }
                }
                s->mail_->header = header;
                s->mail_->body = body;
                LOG_SMTP_DETAIL_INFO("[handle_in_message_data_end] Final mail body size: {}", s->mail_->body.size());
                s->mail_->from = static_cast<SmtpsContext*>(s->get_context())->sender_address;
                s->mail_->to = static_cast<SmtpsContext*>(s->get_context())->recipient_addresses;

                // 从 header 中提取 Subject 作为邮件主题
                std::string subject;
                size_t subject_pos = header.find("Subject:");
                if (subject_pos != std::string::npos) {
                    size_t start = subject_pos + 8; // "Subject:" 长度
                    size_t end = header.find("\r\n", start);
                    if (end != std::string::npos) {
                        subject = header.substr(start, end - start);
                        // 去除首尾空格
                        size_t first = subject.find_first_not_of(" \t");
                        size_t last = subject.find_last_not_of(" \t");
                        if (first != std::string::npos) {
                            subject = subject.substr(first, last - first + 1);
                        }
                    }
                } else {
                    subject = "(无主题)";
                }
                s->mail_->subject = subject;
                s->mails_.push_back(std::move(s->mail_));
                s->set_current_state(static_cast<int>(SmtpsState::WAIT_QUIT));
                SessionBase<ConnectionType>::do_async_read(std::move(s), [] (
                    std::unique_ptr<SessionBase<ConnectionType>> sss,
                    const boost::system::error_code& error,
                    std::size_t bytes_transferred
                ) mutable {
                    if (error) {
                        LOG_SMTP_DETAIL_ERROR("Error reading after message end: {}", error.message());
                        return;
                    }
                    auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(sss->get_fsm());
                    fsm->auto_process_event(std::move(sss));
                });
            }
        );
    }

    // 处理 QUIT 命令
    void handle_wait_quit_quit(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        const std::string& args
    ) {
        LOG_SMTP_DETAIL_INFO("Entered handle_wait_quit_quit");
        persist_and_reply(std::move(session));
    }

    // 处理错误
    void handle_error(
        std::unique_ptr<SessionBase<ConnectionType>> session,
        const std::string& args
    ) {
        session->stay_times_++;
        if (session->stay_times_ > 3) {
            // if ((session->get_current_state() == static_cast<int>(SmtpsState::IN_MESSAGE) && (session->get_current_event() == static_cast<int>(SmtpsEvent::DATA_END))) || session->get_current_state() == static_cast<int>(SmtpsState::WAIT_QUIT)) {
            //     session->get_server()->m_workerThreadPool->post(make_copyable([session = std::move(session), this](){
            //         this->handle_wait_quit_quit(std::move(session), "");
            //     }));
            //     return;
            // }
            LOG_SMTP_DETAIL_ERROR("Too many errors, closing session.");
            session->close();
        } else {
            SessionBase<ConnectionType>::do_async_write(
                std::move(session),
                "500 Error: " + args + "\r\n",
                [] (
                    std::unique_ptr<SessionBase<ConnectionType>> s,
                    const boost::system::error_code& ec
                ) mutable {
                    if (ec) {
                        LOG_SMTP_DETAIL_ERROR("Error sending error response: {}", ec.message());
                        return;
                    }
                    SessionBase<ConnectionType>::do_async_read(std::move(s), [] (
                        std::unique_ptr<SessionBase<ConnectionType>> sss,
                        const boost::system::error_code& error,
                        std::size_t bytes_transferred
                    ) mutable {
                        if (error) {
                            LOG_SMTP_DETAIL_ERROR("Error reading after error: {}", error.message());
                            return;
                        }
                        auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(sss->get_fsm());
                        fsm->auto_process_event(std::move(sss));
                    });
                }
            );
        }
    }
};

} // namespace mail_system

#endif // TRADITIONAL_SMTPS_FSM_H
