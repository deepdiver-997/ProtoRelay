#ifndef SMTPS_SESSION_H
#define SMTPS_SESSION_H

#include "mail_system/back/common/logger.h"
#include "mail_system/back/mailServer/connection/tcp_connection.h"
#include "mail_system/back/mailServer/connection/ssl_connection.h"
#include "mail_system/back/mailServer/session/session_base.h"
#include "mail_system/back/mailServer/fsm/smtps/smtps_fsm.hpp"
#include "mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.h"
#include <memory>
#include <algorithm>
#include <cctype>
#include <regex>
#include <fstream>
#include <ctime>
#include <unordered_map>
#include <sstream>
#include <cstdio>

namespace mail_system {

// SmtpsSession 模板类
template <typename ConnectionType>
class SmtpsSession : public SessionBase<ConnectionType> {
public:
    // 构造函数
    SmtpsSession(
        ServerBase* server,
        std::unique_ptr<ConnectionType> connection,
        std::shared_ptr<SmtpsFsm<ConnectionType>> fsm
    ) : SessionBase<ConnectionType>(std::move(connection), server)
        , fsm_(std::move(fsm))
        , current_state_(static_cast<int>(SmtpsState::INIT))
        , next_event_(static_cast<int>(SmtpsEvent::CONNECT))
        , context_() {}

    // 启动会话
    static void start(std::unique_ptr<SmtpsSession> self) {
        SessionBase<ConnectionType>::do_handshake(
            std::move(self),
            boost::asio::ssl::stream_base::server,
            [](std::unique_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
                if (ec) {
                    LOG_SESSION_ERROR("Handshake failed: {}", ec.message());
                    return;
                }
                auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(s->get_fsm());
                fsm->process_event(std::move(s), SmtpsEvent::CONNECT, "");
            }
        );
    }

    // 处理读取的数据
    void handle_read(const std::string& data) override {
        // 解析 SMTP 命令，设置 event 和 args
        parse_smtp_command(data);
    }

    // 处理读取后的逻辑（回调为空时自动调用）
    void process_read(std::unique_ptr<SessionBase<ConnectionType>> self) override {
        auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(this->get_fsm());
        fsm->auto_process_event(std::move(self));
    }

    // 获取 FSM
    void* get_fsm() const override {
        return fsm_.get();
    }

    // 获取上下文
    void* get_context() override {
        return &context_;
    }

    // 设置当前状态
    void set_current_state(int state) override {
        current_state_ = state;
    }

    // 设置下一个事件
    void set_next_event(int event) override {
        next_event_ = event;
    }

    // 获取当前状态
    int get_current_state() const override {
        return current_state_;
    }

    // 获取下一个事件
    int get_next_event() const override {
        return next_event_;
    }

    // 获取最后一条命令的参数
    std::string get_last_command_args() const override {
        return last_command_args_;
    }

private:
    static constexpr size_t MAX_BODY_BYTES = 10 * 1024 * 1024; // 10MB limit for mail body

    // 解析 SMTP 命令
    void parse_smtp_command(const std::string& data) {
        LOG_SESSION_INFO("Handling data: {}", data);
        std::string trimmed = trim(data);

        // 1. 处理邮件内容输入状态
        if (current_state_ == static_cast<int>(SmtpsState::IN_MESSAGE)) {
            // 先把数据送入行级处理（可流式写附件）
            process_message_data(data);

            // 检测是否包含 DATA 结束标志。可能整块里已有 \r\n.\r\n
            bool data_end_seen = (trimmed == ".") || (data.find("\r\n.\r\n") != std::string::npos);
            if (data_end_seen) {
                finalize_current_part();
                next_event_ = static_cast<int>(SmtpsEvent::DATA_END);
                last_command_args_.clear();
            } else {
                next_event_ = static_cast<int>(SmtpsEvent::DATA);
                if (!context_.streaming_enabled) {
                    // 如果行以.开头，去掉一个.（SMTP协议规定）
                    if (!trimmed.empty() && trimmed[0] == '.') {
                        last_command_args_ = data.substr(data.find('.') + 1);
                    } else {
                        last_command_args_ = data;
                    }
                } else {
                    // 流式模式下 FSM 不再需要累积的数据
                    last_command_args_.clear();
                }
            }

            if (fsm_) {
                LOG_SESSION_INFO("IN_MESSAGE next_event_: {}", fsm_->get_event_name(static_cast<SmtpsEvent>(next_event_)));
            }
            return;
        }

        if (trimmed.empty()) {
            next_event_ = static_cast<int>(SmtpsEvent::ERROR);
            return;
        }

        // 2. 处理身份验证状态（用户名/密码输入）
        if (current_state_ == static_cast<int>(SmtpsState::WAIT_AUTH_USERNAME) || 
            current_state_ == static_cast<int>(SmtpsState::WAIT_AUTH_PASSWORD)) {
            next_event_ = static_cast<int>(SmtpsEvent::AUTH);
            last_command_args_ = trimmed;
            if (fsm_) {
                LOG_SESSION_INFO("AUTH next_event_: {}", fsm_->get_event_name(static_cast<SmtpsEvent>(next_event_)));
            }
            return;
        }

        // 3. 解析常规命令
        std::string command;
        std::string args;
        std::string upper_trimmed = trimmed;
        std::transform(upper_trimmed.begin(), upper_trimmed.end(), upper_trimmed.begin(), ::toupper);

        if (upper_trimmed.compare(0, 9, "MAIL FROM") == 0) {
            command = "MAIL FROM";
            args = trimmed.substr(4); // 保留 "FROM..." 以供 FSM 正则解析
        } else if (upper_trimmed.compare(0, 7, "RCPT TO") == 0) {
            command = "RCPT TO";
            args = trimmed.substr(4); // 保留 "TO..." 以供 FSM 正则解析
        } else {
            size_t space_pos = trimmed.find(' ');
            if (space_pos != std::string::npos) {
                command = trimmed.substr(0, space_pos);
                args = trimmed.substr(space_pos + 1);
            } else {
                command = trimmed;
            }
            std::transform(command.begin(), command.end(), command.begin(), ::toupper);
        }

        last_command_args_ = args;

        LOG_SESSION_INFO("command: {}, args: {}", command, args);

        if (command == "EHLO" || command == "HELO") {
            next_event_ = static_cast<int>(SmtpsEvent::EHLO);
        } else if (command == "AUTH") {
            next_event_ = static_cast<int>(SmtpsEvent::AUTH);
        } else if (command == "MAIL FROM") {
            next_event_ = static_cast<int>(SmtpsEvent::MAIL_FROM);
        } else if (command == "RCPT TO") {
            next_event_ = static_cast<int>(SmtpsEvent::RCPT_TO);
        } else if (command == "DATA") {
            next_event_ = static_cast<int>(SmtpsEvent::DATA);
        } else if (command == "QUIT") {
            next_event_ = static_cast<int>(SmtpsEvent::QUIT);
        } else if (command == "STARTTLS") {
            next_event_ = static_cast<int>(SmtpsEvent::STARTTLS);
        } else {
            next_event_ = static_cast<int>(SmtpsEvent::ERROR);
            last_command_args_ = "Unknown command: " + command;
        }

        if (fsm_) {
            LOG_SESSION_INFO("next_event_: {}", fsm_->get_event_name(static_cast<SmtpsEvent>(next_event_)));
        }
    }

    // 去除首尾空白字符
    std::string trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return "";
        }
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    // Lowercase helper for header keys/values
    static std::string to_lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // Clean filename so it is safe on disk
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
        return out.empty() ? std::string("attachment") : out;
    }

    // Parse raw header block into a map
    std::unordered_map<std::string, std::string> parse_headers_map(const std::string& header_block) {
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
            std::string key = to_lower(line.substr(0, pos));
            std::string value = line.substr(pos + 1);
            size_t first = value.find_first_not_of(" \t");
            if (first != std::string::npos) {
                value = value.substr(first);
            } else {
                value.clear();
            }
            headers[key] = value;
        }
        return headers;
    }

    // Extract multipart boundary value from Content-Type
    std::string extract_boundary(const std::string& content_type) {
        std::regex boundary_regex(R"(boundary\s*=\s*\"?([^";]+)\"?)", std::regex_constants::icase);
        std::smatch m;
        if (std::regex_search(content_type, m, boundary_regex) && m.size() > 1) {
            std::string boundary = m[1];
            LOG_SESSION_DEBUG("Extracted boundary: {}", boundary);
            return boundary;
        }
        return "";
    }

    // Inspect top-level headers to decide multipart/streaming behavior
    void analyze_top_header() {
        LOG_SESSION_INFO("analyze_top_header: header_buffer=[{}]", context_.header_buffer.substr(0, 200));
        auto headers = parse_headers_map(context_.header_buffer);
        auto it = headers.find("content-type");
        if (it != headers.end()) {
            LOG_SESSION_INFO("analyze_top_header: content-type raw=[{}]", it->second);
            std::string lower = to_lower(it->second);
            LOG_SESSION_INFO("analyze_top_header: content-type lower=[{}]", lower);
            context_.boundary = extract_boundary(lower);
            LOG_SESSION_INFO("analyze_top_header: extracted boundary=[{}]", context_.boundary);
            if (lower.find("multipart/") != std::string::npos && !context_.boundary.empty()) {
                context_.multipart = true;
                context_.streaming_enabled = true;
                LOG_SESSION_INFO("Multipart detected, boundary: {}", context_.boundary);
                // 加大读取缓冲，降低读调用次数
                const size_t bigger = 65536;
                if (this->read_buffer_.size() < bigger) {
                    this->read_buffer_.resize(bigger);
                }
            } else {
                LOG_SESSION_INFO("Not multipart or no boundary, content-type: {}", lower);
            }
        }

    }

    // Consume incoming DATA lines and dispatch to header/body handlers
    void process_message_data(const std::string& data) {
        context_.line_buffer += data;
        LOG_SESSION_INFO("process_message_data: buffer size={}, streaming_enabled={}", 
                         context_.line_buffer.size(), context_.streaming_enabled);

        while (true) {
            size_t pos = context_.line_buffer.find("\r\n");
            if (pos == std::string::npos) {
                break;
            }
            std::string line = context_.line_buffer.substr(0, pos);
            context_.line_buffer.erase(0, pos + 2);

            if (context_.body_limit_exceeded) {
                continue;
            }

            if (!context_.header_parsed) {
                context_.header_buffer += line + "\r\n";
                if (line.empty()) {
                    context_.header_parsed = true;
                    analyze_top_header();
                    LOG_SESSION_INFO("Header parsing complete, multipart={}, boundary=[{}], streaming={}",
                                     context_.multipart, context_.boundary, context_.streaming_enabled);
                }
                continue;
            }

            // Body行：streaming时流式处理，否则累积
            if (context_.streaming_enabled) {
                LOG_SESSION_INFO("Processing body line (streaming): [{}]", line.substr(0, 60));
                handle_multipart_line(line);
            } else {
                LOG_SESSION_INFO("Processing body line (buffered): [{}]", line.substr(0, 60));
                context_.buffered_body_size += line.size() + 2;
                if (context_.buffered_body_size > MAX_BODY_BYTES) {
                    mark_body_limit_exceeded("552 Message size exceeds 10MB");
                } else {
                    context_.mail_data += line + "\r\n";
                }
            }
        }
    }

    // Handle a single multipart body line (boundary detection, text or attachment)
    void handle_multipart_line(const std::string& line) {
        if (context_.body_limit_exceeded) {
            return;
        }

        if (!context_.multipart || context_.boundary.empty()) {
            LOG_SESSION_INFO("Multipart disabled or no boundary, appending to mail_data");
            context_.mail_data += line + "\r\n";
            return;
        }

        std::string boundary_marker = "--" + context_.boundary;
        std::string boundary_end = boundary_marker + "--";
        std::string line_lower = to_lower(line);

        LOG_SESSION_INFO("Checking line: [{}] vs boundary_marker: [{}]", line_lower, boundary_marker);

        if (line_lower == boundary_marker || line_lower == boundary_end) {
            LOG_SESSION_INFO("Boundary detected: [{}]", line_lower);
            finalize_current_part();
            if (line_lower == boundary_end) {
                LOG_SESSION_INFO("Multipart end detected");
                context_.in_part_header = false;
                context_.current_part_headers.clear();
                return;
            }
            LOG_SESSION_INFO("New part starting");
            context_.in_part_header = true;
            context_.current_part_headers.clear();
            return;
        }

        if (context_.in_part_header) {
            if (line.empty()) {
                LOG_SESSION_INFO("Part header complete, processing headers: [{}]", context_.current_part_headers.substr(0, 100));
                context_.in_part_header = false;
                handle_part_header_parsed();
                LOG_SESSION_INFO("After handle_part_header: mime=[{}], is_attachment={}, stream_open={}",
                                 context_.current_part_mime, context_.current_part_is_attachment,
                                 context_.current_attachment_stream.is_open());
            } else {
                LOG_SESSION_INFO("Part header line: [{}]", line);
                context_.current_part_headers += line + "\r\n";
            }
            return;
        }

        if (context_.current_part_is_attachment && context_.current_attachment_stream.is_open()) {
            write_attachment_body_line(line);
            return;
        }

        // 文本 part，保留第一个 text/plain，否则回退 text/html
        size_t added = line.size() + 2;
        if (context_.text_body_size + added > MAX_BODY_BYTES) {
            mark_body_limit_exceeded("552 Message size exceeds 10MB");
            return;
        }
        context_.text_body_size += added;
        if (context_.current_part_mime.find("text/plain") != std::string::npos) {
            context_.text_body_buffer += line + "\r\n";
        } else if (context_.text_body_buffer.empty()) {
            context_.text_body_buffer += line + "\r\n";
        }
    }

    // Parse current part headers and decide whether to stream to disk
    void handle_part_header_parsed() {
        LOG_SESSION_INFO("handle_part_header_parsed: headers=[{}]", context_.current_part_headers);
        auto headers = parse_headers_map(context_.current_part_headers);
        std::string disp = to_lower(headers["content-disposition"]);
        context_.current_part_encoding = to_lower(headers["content-transfer-encoding"]);
        context_.current_part_mime = to_lower(headers["content-type"]);
        
        LOG_SESSION_INFO("handle_part_header_parsed: disposition=[{}], mime=[{}], encoding=[{}]",
                         disp, context_.current_part_mime, context_.current_part_encoding);

        context_.current_part_is_attachment = disp.find("attachment") != std::string::npos || disp.find("filename") != std::string::npos;
        if (context_.current_part_is_attachment) {
            context_.has_attachment = true;
            std::string filename;
            std::regex filename_regex(R"(filename\s*=\s*\"?([^";]+)\"?)", std::regex_constants::icase);
            std::smatch m;
            if (std::regex_search(context_.current_part_headers, m, filename_regex) && m.size() > 1) {
                filename = m[1];
            }
            if (filename.empty()) {
                filename = "attachment";
            }
            open_attachment_stream(filename);
            LOG_SESSION_INFO("After opening attachment stream: is_attachment={}, stream_open={}", 
                             context_.current_part_is_attachment, context_.current_attachment_stream.is_open());
        } else {
            context_.current_attachment_filename.clear();
            context_.current_attachment_path.clear();
        }
    }

    // Open a file stream for the attachment part, ensuring previous part is finalized
    void open_attachment_stream(const std::string& filename) {
        // Save current state before finalize clears it
        bool is_attachment = context_.current_part_is_attachment;
        std::string mime = context_.current_part_mime;
        std::string encoding = context_.current_part_encoding;
        
        finalize_current_part();
        
        // Restore the state we just saved
        context_.current_part_is_attachment = is_attachment;
        context_.current_part_mime = mime;
        context_.current_part_encoding = encoding;
        
        std::string safe = sanitize_filename(filename);
        std::string base_path = this->m_server->m_config.attachment_storage_path;
        if (!base_path.empty() && base_path.back() != '/' && base_path.back() != '\\') {
            base_path.push_back('/');
        }
        std::string path = base_path + std::to_string(std::time(nullptr)) + "_" + safe;
        LOG_SESSION_INFO("Opening attachment stream: filename=[{}], safe=[{}], path=[{}]", 
                        filename, safe, path);
        context_.current_attachment_stream.open(path, std::ios::binary);
        if (!context_.current_attachment_stream.is_open()) {
            LOG_SESSION_ERROR("Failed to open attachment file: {}", path);
            context_.current_part_is_attachment = false;
            return;
        }
        LOG_SESSION_DEBUG("Attachment stream opened successfully");
        context_.current_attachment_filename = safe;
        context_.current_attachment_path = path;
        context_.current_attachment_size = 0;
        context_.base64_remainder.clear();
    }

    // Decode a base64 chunk (ignoring invalid chars) to raw bytes
    static std::string decode_base64_block(const std::string& input) {
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

    // Write a body line into the current attachment (base64-aware)
    void write_attachment_body_line(const std::string& line) {
        if (!context_.current_attachment_stream.is_open()) {
            LOG_SESSION_ERROR("Attachment stream not open when trying to write");
            return;
        }
        if (context_.current_part_encoding.find("base64") != std::string::npos) {
            std::string data = context_.base64_remainder + line;
            size_t usable = (data.size() / 4) * 4;
            std::string block = data.substr(0, usable);
            context_.base64_remainder = data.substr(usable);
            std::string decoded = decode_base64_block(block);
            if (!decoded.empty()) {
                context_.current_attachment_stream.write(decoded.data(), static_cast<std::streamsize>(decoded.size()));
                context_.current_attachment_stream.flush();
                context_.current_attachment_size += decoded.size();
                LOG_SESSION_DEBUG("Wrote {} bytes to attachment (base64 decoded)", decoded.size());
            }
        } else {
            std::string with_crlf = line + "\r\n";
            context_.current_attachment_stream.write(with_crlf.data(), static_cast<std::streamsize>(with_crlf.size()));
            context_.current_attachment_stream.flush();
            context_.current_attachment_size += with_crlf.size();
            LOG_SESSION_DEBUG("Wrote {} bytes to attachment (plain)", with_crlf.size());
        }
    }

    // Flush pending base64 padding and clear remainder
    void flush_attachment_remainder() {
        if (!context_.current_attachment_stream.is_open()) {
            return;
        }
        if (!context_.base64_remainder.empty()) {
            std::string decoded = decode_base64_block(context_.base64_remainder);
            if (!decoded.empty()) {
                context_.current_attachment_stream.write(decoded.data(), static_cast<std::streamsize>(decoded.size()));
                context_.current_attachment_size += decoded.size();
            }
        }
        context_.base64_remainder.clear();
    }

    // Remove any streamed attachment files from disk and clear tracking
    void cleanup_streamed_attachments() {
        for (auto& att : context_.streamed_attachments) {
            if (!att.filepath.empty()) {
                std::remove(att.filepath.c_str());
            }
        }
        context_.streamed_attachments.clear();
    }

    // Mark body limit exceeded, remember reason, and stop further processing
    void mark_body_limit_exceeded(const std::string& reason) {
        if (context_.body_limit_exceeded) {
            return;
        }
        context_.body_limit_exceeded = true;
        context_.abort_reason = reason;
        finalize_current_part();
        cleanup_streamed_attachments();
        context_.streaming_enabled = false;
        context_.line_buffer.clear();
    }

    // Close and record the current part (attachment or text) into context
    void finalize_current_part() {
        if (context_.current_attachment_stream.is_open()) {
            flush_attachment_remainder();
            context_.current_attachment_stream.flush();
            context_.current_attachment_stream.close();
            LOG_SESSION_INFO("Finalized attachment: filename=[{}], filepath=[{}], size={} bytes",
                            context_.current_attachment_filename, context_.current_attachment_path,
                            context_.current_attachment_size);
            attachment att;
            att.filename = context_.current_attachment_filename;
            att.filepath = context_.current_attachment_path;
            att.mime_type = context_.current_part_mime.empty() ? "application/octet-stream" : context_.current_part_mime;
            att.file_size = context_.current_attachment_size;
            att.upload_time = std::time(nullptr);
            context_.streamed_attachments.push_back(std::move(att));
            LOG_SESSION_DEBUG("Attachment added to streamed_attachments, total count={}", context_.streamed_attachments.size());
        }
        context_.current_attachment_filename.clear();
        context_.current_attachment_path.clear();
        context_.current_attachment_size = 0;
        context_.current_part_headers.clear();
        context_.current_part_mime.clear();
        context_.current_part_encoding.clear();
        context_.current_part_is_attachment = false;
        context_.base64_remainder.clear();
    }


    // FSM 指针
    std::shared_ptr<SmtpsFsm<ConnectionType>> fsm_;

    // 状态
    int current_state_;
    int next_event_;

    // 上下文
    SmtpsContext context_;

    // 最后一条命令的参数
    std::string last_command_args_;

};

// 类型别名
using TcpSmtpsSession = SmtpsSession<TcpConnection>;
using SslSmtpsSession = SmtpsSession<SslConnection>;

} // namespace mail_system

#endif // SMTPS_SESSION_H
