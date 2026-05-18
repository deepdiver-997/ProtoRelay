#pragma once

#include <cctype>

namespace mail_system {

// IMAP 命令名 → ImapEvent 映射表
namespace {
    inline const std::unordered_map<std::string, ImapEvent>& get_imap_command_map() {
        static const std::unordered_map<std::string, ImapEvent> map = {
            {"CAPABILITY", ImapEvent::CAPABILITY},
            {"LOGIN", ImapEvent::LOGIN},
            {"AUTHENTICATE", ImapEvent::AUTHENTICATE},
            {"LOGOUT", ImapEvent::LOGOUT},
            {"SELECT", ImapEvent::SELECT},
            {"EXAMINE", ImapEvent::EXAMINE},
            {"CREATE", ImapEvent::CREATE},
            {"DELETE", ImapEvent::DELETE},
            {"RENAME", ImapEvent::RENAME},
            {"SUBSCRIBE", ImapEvent::SUBSCRIBE},
            {"UNSUBSCRIBE", ImapEvent::UNSUBSCRIBE},
            {"LIST", ImapEvent::LIST},
            {"LSUB", ImapEvent::LSUB},
            {"STATUS", ImapEvent::STATUS},
            {"APPEND", ImapEvent::APPEND},
            {"CHECK", ImapEvent::CHECK},
            {"CLOSE", ImapEvent::CLOSE},
            {"EXPUNGE", ImapEvent::EXPUNGE},
            {"SEARCH", ImapEvent::SEARCH},
            {"FETCH", ImapEvent::FETCH},
            {"STORE", ImapEvent::STORE},
            {"COPY", ImapEvent::COPY},
            {"MOVE", ImapEvent::MOVE},
            {"UID", ImapEvent::UID},
            {"NOOP", ImapEvent::NOOP},
            {"IDLE", ImapEvent::IDLE},
            {"DONE", ImapEvent::DONE},
        };
        return map;
    }
}

// ====================================================================
// 构造函数
// ====================================================================
template <typename ConnectionType>
ImapsSession<ConnectionType>::ImapsSession(
    ServerBase* server,
    std::unique_ptr<ConnectionType> connection,
    std::shared_ptr<ImapsFsm<ConnectionType>> fsm)
    : SessionBase<ConnectionType>(std::move(connection), server)
    , fsm_(std::move(fsm))
    , state_(ImapState::INIT)
    , next_event_(ImapEvent::CONNECT)
    , command_read_buffer_()
    , context_()
{
}

// ====================================================================
// 启动入口
// ====================================================================
template <typename ConnectionType>
void ImapsSession<ConnectionType>::start(std::unique_ptr<ImapsSession> self) {
    SessionBase<ConnectionType>::do_handshake(
        std::move(self),
        boost::asio::ssl::stream_base::server,
        [](std::unique_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_SESSION_ERROR("IMAP handshake failed: {}", ec.message());
                return;
            }
            auto fsm = static_cast<TraditionalImapsFsm<ConnectionType>*>(s->get_fsm());
            fsm->process_event(std::move(s), ImapEvent::CONNECT, "", "");
        }
    );
}

template <typename ConnectionType>
void ImapsSession<ConnectionType>::start_after_starttls(std::unique_ptr<ImapsSession> self) {
    SessionBase<ConnectionType>::do_handshake(
        std::move(self),
        boost::asio::ssl::stream_base::server,
        [](std::unique_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_SESSION_ERROR("IMAP STARTTLS handshake failed: {}", ec.message());
                return;
            }
            s->set_current_state(static_cast<int>(ImapState::NOT_AUTHENTICATED));
            LOG_IMAP_INFO("STARTTLS handshake complete, waiting for commands on TLS session");
            SessionBase<ConnectionType>::do_async_read(std::move(s), nullptr);
        }
    );
}

// ====================================================================
// handle_read: 把裸数据追加到行缓冲区
// ====================================================================
template <typename ConnectionType>
void ImapsSession<ConnectionType>::handle_read(const std::string& data) {
    command_read_buffer_.append(data);
}

// ====================================================================
// compute_reply_delay
// ====================================================================
template <typename ConnectionType>
std::chrono::milliseconds ImapsSession<ConnectionType>::compute_reply_delay() const {
    return std::chrono::milliseconds(0);
}

// ====================================================================
// process_read: 从缓冲区提取一条完整命令，调用 FSM 处理
// 这是 SMTP 模式的正确对照 —— FSM 通过 do_async_write 发响应
// 且持有 session 的 unique_ptr 生命周期
// ====================================================================
template <typename ConnectionType>
void ImapsSession<ConnectionType>::process_read(std::unique_ptr<SessionBase<ConnectionType>> self) {
    auto* session = static_cast<ImapsSession*>(self.get());
    auto& buf = session->command_read_buffer_;

    // IDLE 状态特殊处理：等待 DONE
    if (session->context_.idle_mode) {
        // 检查是否是 DONE
        size_t line_end = buf.find("\r\n");
        if (line_end != std::string::npos) {
            std::string line = buf.substr(0, line_end);
            buf.erase(0, line_end + 2);
            // Trim
            line.erase(line.find_last_not_of(" \t\r\n") + 1);
            std::string upper = line;
            std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
            if (upper == "DONE") {
                session->context_.idle_mode = false;
                auto fsm = static_cast<TraditionalImapsFsm<ConnectionType>*>(session->fsm_.get());
                // For DONE, write OK response and go back to reading
                fsm->send_tagged(std::move(self), session->context_.current_tag, "OK", "IDLE terminated");
                return;
            }
        }
        // No DONE yet, keep reading
        SessionBase<ConnectionType>::do_async_read(std::move(self), nullptr);
        return;
    }

    // 检查是否在等待 literal
    if (session->awaiting_literal_) {
        size_t needed = session->expected_literal_size_ - session->literal_data_buffer_.size();
        size_t available = buf.size();

        if (available >= needed) {
            session->literal_data_buffer_.append(buf.substr(0, needed));
            buf.erase(0, needed);
            session->awaiting_literal_ = false;

            LOG_IMAP_DEBUG("Literal completed: {} bytes", session->literal_data_buffer_.size());

            // 如果还有 \r\n，消耗掉
            if (buf.size() >= 2 && buf[0] == '\r' && buf[1] == '\n') {
                buf.erase(0, 2);
            }

            // 处理 APPEND 最终处理
            session->last_command_args_ = session->literal_data_buffer_;

            auto fsm = static_cast<TraditionalImapsFsm<ConnectionType>*>(session->fsm_.get());
            fsm->process_event(std::move(self), ImapEvent::APPEND,
                               session->current_tag_, session->last_command_args_);
            return;
        } else {
            // 还不够，继续读
            session->literal_data_buffer_.append(buf);
            buf.clear();
            SessionBase<ConnectionType>::do_async_read(std::move(self), nullptr);
            return;
        }
    }

    // 找行结束 \r\n
    size_t line_end = buf.find("\r\n");
    if (line_end == std::string::npos) {
        // 没有完整行，继续读
        SessionBase<ConnectionType>::do_async_read(std::move(self), nullptr);
        return;
    }

    std::string line = buf.substr(0, line_end);
    buf.erase(0, line_end + 2);

    if (line.empty()) {
        // 空行，继续读
        SessionBase<ConnectionType>::do_async_read(std::move(self), nullptr);
        return;
    }

    LOG_IMAP_DETAIL_DEBUG("IMAP line: [{}]", line);

    // 检查是否 literal 声明（行尾的 {size}）
    size_t brace_pos = line.rfind('{');
    if (brace_pos != std::string::npos && line.back() == '}') {
        // 提取大小
        std::string size_str = line.substr(brace_pos + 1);
        size_str.pop_back();
        try {
            size_t literal_size = std::stoull(size_str);
            std::string cmd_part = line.substr(0, brace_pos);
            // Trim trailing space from command part
            while (!cmd_part.empty() && cmd_part.back() == ' ') {
                cmd_part.pop_back();
            }

            // 解析 tag + command
            size_t first_space = cmd_part.find(' ');
            if (first_space != std::string::npos) {
                session->current_tag_ = cmd_part.substr(0, first_space);
                std::string rest = cmd_part.substr(first_space + 1);
                size_t second_space = rest.find(' ');
                if (second_space != std::string::npos) {
                    session->current_command_ = rest.substr(0, second_space);
                    session->last_command_args_ = rest.substr(second_space + 1);
                } else {
                    session->current_command_ = rest;
                    session->last_command_args_.clear();
                }
            } else {
                session->current_tag_ = cmd_part;
                session->current_command_ = cmd_part;
                session->last_command_args_.clear();
            }

            std::transform(session->current_command_.begin(), session->current_command_.end(),
                          session->current_command_.begin(), ::toupper);
            LOG_IMAP_DEBUG("Literal declared: cmd={}, size={}", session->current_command_, literal_size);

            // 检查缓冲区是否已有足够的 literal 数据
            if (buf.size() >= literal_size) {
                session->literal_data_buffer_ = buf.substr(0, literal_size);
                buf.erase(0, literal_size);
                // 消耗尾随 \r\n
                if (buf.size() >= 2 && buf[0] == '\r' && buf[1] == '\n') {
                    buf.erase(0, 2);
                }
                session->last_command_args_ = session->literal_data_buffer_;
                session->context_.current_tag = session->current_tag_;

                auto fsm = static_cast<TraditionalImapsFsm<ConnectionType>*>(session->fsm_.get());
                fsm->process_event(std::move(self), ImapEvent::APPEND,
                                   session->current_tag_, session->literal_data_buffer_);
                return;
            } else {
                // 需要更多数据
                session->awaiting_literal_ = true;
                session->expected_literal_size_ = literal_size;
                session->literal_data_buffer_ = buf;
                buf.clear();
                SessionBase<ConnectionType>::do_async_read(std::move(self), nullptr);
                return;
            }
        } catch (const std::exception& e) {
            LOG_IMAP_ERROR("Invalid literal size: {}", e.what());
            // Fall through to normal command processing
        }
    }

    // ===== 普通命令解析（无 literal）=====
    std::string tag, cmd, args;

    size_t first_space = line.find(' ');
    if (first_space == std::string::npos) {
        // 只有 tag 或只有命令
        tag = line;
        cmd = line;
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

        // DONE 特殊处理
        if (cmd == "DONE" && session->context_.idle_mode) {
            session->context_.idle_mode = false;
            auto fsm = static_cast<TraditionalImapsFsm<ConnectionType>*>(session->fsm_.get());
            fsm->send_tagged(std::move(self), session->context_.current_tag, "OK", "IDLE terminated");
            return;
        }

        auto it = get_imap_command_map().find(cmd);
        if (it != get_imap_command_map().end()) {
            session->next_event_ = it->second;
        } else {
            session->next_event_ = ImapEvent::ERROR;
            args = "Unknown command: " + cmd;
        }
    } else {
        tag = line.substr(0, first_space);
        std::string rest = line.substr(first_space + 1);

        size_t second_space = rest.find(' ');
        if (second_space != std::string::npos) {
            cmd = rest.substr(0, second_space);
            args = rest.substr(second_space + 1);
        } else {
            cmd = rest;
            args.clear();
        }

        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::toupper);

        LOG_IMAP_DETAIL_DEBUG("IMAP parsed: tag=[{}] cmd=[{}] args=[{}]", tag, cmd, args);

        auto it = get_imap_command_map().find(cmd);
        if (it != get_imap_command_map().end()) {
            session->next_event_ = it->second;
        } else {
            session->next_event_ = ImapEvent::ERROR;
            args = "Unknown command: " + cmd;
        }
    }

    // 保存到 context
    session->current_tag_ = tag;
    session->current_command_ = cmd;
    session->last_command_args_ = args;
    session->context_.current_tag = tag;

    // 调用 FSM 处理事件 —— FSM 内通过 do_async_write 发送响应
    // 并在回调中继续 do_async_read
    auto fsm = static_cast<TraditionalImapsFsm<ConnectionType>*>(session->fsm_.get());
    fsm->process_event(std::move(self), session->next_event_, tag, args);
}

// ====================================================================
// 状态机接口
// ====================================================================
template <typename ConnectionType>
void* ImapsSession<ConnectionType>::get_fsm() const {
    return fsm_.get();
}

template <typename ConnectionType>
void* ImapsSession<ConnectionType>::get_context() {
    return &context_;
}

template <typename ConnectionType>
void ImapsSession<ConnectionType>::set_current_state(int state) {
    state_ = static_cast<ImapState>(state);
}

template <typename ConnectionType>
void ImapsSession<ConnectionType>::set_next_event(int event) {
    next_event_ = static_cast<ImapEvent>(event);
}

template <typename ConnectionType>
int ImapsSession<ConnectionType>::get_current_state() const {
    return static_cast<int>(state_);
}

template <typename ConnectionType>
int ImapsSession<ConnectionType>::get_next_event() const {
    return static_cast<int>(next_event_);
}

template <typename ConnectionType>
std::string ImapsSession<ConnectionType>::get_last_command_args() const {
    return last_command_args_;
}

} // namespace mail_system
