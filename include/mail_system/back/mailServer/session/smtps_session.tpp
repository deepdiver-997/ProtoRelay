#pragma once

namespace mail_system {

template <typename ConnectionType>
SmtpsSession<ConnectionType>::SmtpsSession(
    ServerBase* server,
    std::unique_ptr<ConnectionType> connection,
    std::shared_ptr<SmtpsFsm<ConnectionType>> fsm
) : SessionBase<ConnectionType>(std::move(connection), server)
    , fsm_(std::move(fsm))
    , state_(SmtpsState::INIT)
    , next_event_(SmtpsEvent::CONNECT)
    , ignore_current_command_(false)
    , context_()
    , buffer_size_(INITIAL_BUFFER_SIZE)
    , buffer_(new char[INITIAL_BUFFER_SIZE])
    , buffer_used_(0)
    , buffer_expand_count_(0)
    , persistent_queue_(server->m_persistentQueue) {}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::start(std::unique_ptr<SmtpsSession> self) {
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

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::handle_read(const std::string& data) {
    parse_smtp_command(data);
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::process_read(std::unique_ptr<SessionBase<ConnectionType>> self) {
    if (ignore_current_command_) {
        ignore_current_command_ = false;
        SessionBase<ConnectionType>::do_async_read(std::move(self), nullptr);
        return;
    }

    auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(this->get_fsm());
    fsm->auto_process_event(std::move(self));
}

template <typename ConnectionType>
void* SmtpsSession<ConnectionType>::get_fsm() const {
    return fsm_.get();
}

template <typename ConnectionType>
void* SmtpsSession<ConnectionType>::get_context() {
    return &context_;
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::set_current_state(int state) {
    state_ = static_cast<SmtpsState>(state);
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::set_next_event(int event) {
    next_event_ = static_cast<SmtpsEvent>(event);
}

template <typename ConnectionType>
int SmtpsSession<ConnectionType>::get_current_state() const {
    return int(state_);
}

template <typename ConnectionType>
int SmtpsSession<ConnectionType>::get_next_event() const {
    return int(next_event_);
}

template <typename ConnectionType>
std::string SmtpsSession<ConnectionType>::get_last_command_args() const {
    return last_command_args_;
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::expand_buffer() {
    if (buffer_expand_count_ >= MAX_BUFFER_EXPAND_COUNT) {
        LOG_SESSION_INFO("Buffer expansion limit reached, flushing to disk asynchronously");
        async_flush_buffer_to_disk();
        return;
    }

    size_t new_size = buffer_size_ * BUFFER_GROWTH_FACTOR;
    if (new_size > MAX_BUFFER_SIZE) {
        LOG_SESSION_WARN("Buffer expansion would exceed MAX_BUFFER_SIZE, flushing asynchronously");
        async_flush_buffer_to_disk();
        return;
    }

    async_flush_buffer_to_disk();

    LOG_SESSION_DEBUG("Expanding buffer from {} to {} bytes", buffer_size_, new_size);
    auto new_buffer = std::make_unique<char[]>(new_size);
    buffer_ = std::move(new_buffer);
    buffer_size_ = new_size;
    buffer_used_ = 0;
    buffer_expand_count_++;

    LOG_SESSION_INFO("Buffer expanded, expand count: {}", buffer_expand_count_);
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::flush_buffer_to_disk() {
    if (buffer_used_ == 0 || !this->get_mail()) {
        return;
    }

    std::ofstream out(this->get_mail()->body_path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
        LOG_SESSION_ERROR("Failed to open mail body file for writing: {}", this->get_mail()->body_path);
        return;
    }

    out.write(buffer_.get(), static_cast<std::streamsize>(buffer_used_));
    if (!out.good()) {
        LOG_SESSION_ERROR("Failed to write mail body to file: {}", this->get_mail()->body_path);
        out.close();
        return;
    }

    out.close();
    LOG_SESSION_DEBUG("Flushed {} bytes to mail body file", buffer_used_);
    LOG_SESSION_INFO("Synchronous flush completed for mail {}, buffer_used_={}", this->get_mail()->id, buffer_used_);
    buffer_used_ = 0;
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::wait_for_async_writes() {
    for (auto& f : async_write_futures_) {
        if (!f.valid()) {
            continue;
        }
        bool ok = false;
        try {
            ok = f.get();
        } catch (const std::exception& e) {
            LOG_SESSION_ERROR("Exception waiting async write: {}", e.what());
            ok = false;
        }
        if (!ok) {
            handle_write_failure();
        }
    }
    async_write_futures_.clear();
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::flush_body_and_wait() {
    flush_buffer_to_disk();
    wait_for_async_writes();
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::async_flush_buffer_to_disk() {
    if (buffer_used_ == 0 || !this->get_mail()) {
        return;
    }

    std::string buffer_data(buffer_.get(), buffer_used_);
    buffer_used_ = 0;

    std::string body_path = this->get_mail()->body_path;

    auto future = this->m_server->m_workerThreadPool->submit([this, body_path, buffer_data]() -> bool {
        try {
            std::ofstream out(body_path, std::ios::binary | std::ios::app);
            if (!out.is_open()) {
                LOG_SESSION_ERROR("Failed to open file for async write: {}", body_path);
                return false;
            }

            out.write(buffer_data.data(), static_cast<std::streamsize>(buffer_data.size()));
            if (!out.good()) {
                LOG_SESSION_ERROR("Failed to write data to file: {}", body_path);
                out.close();
                return false;
            }

            out.close();
            LOG_SESSION_DEBUG("Async write {} bytes to file: {}", buffer_data.size(), body_path);
            return true;
        } catch (const std::exception& e) {
            LOG_SESSION_ERROR("Exception during async write: {}", e.what());
            return false;
        }
    });

    async_write_futures_.push_back(std::move(future));
    LOG_SESSION_INFO("Submitted async write task, pending tasks: {}", async_write_futures_.size());
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::handle_write_failure() {
    if (!this->get_mail()) {
        return;
    }

    if (this->get_mail()->persist_status == persist_storage::PersistStatus::PENDING) {
        this->get_mail()->persist_status = persist_storage::PersistStatus::CANCELLED;
        LOG_SESSION_WARN("Mail {} write failed, marked as CANCELLED", this->get_mail()->id);
    } else {
        LOG_SESSION_WARN("Mail {} write failed after processing started, submitting delete task", this->get_mail()->id);
        if (persistent_queue_) {
            auto mail_ptr = this->get_mail();
            this->m_server->m_workerThreadPool->submit([this, mail_ptr]() {
                persistent_queue_->delete_task(mail_ptr);
            });
        }
    }
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::append_to_buffer(const char* data, size_t size) {
    if (buffer_used_ + size > buffer_size_) {
        if (buffer_expand_count_ >= MAX_BUFFER_EXPAND_COUNT) {
            async_flush_buffer_to_disk();
        } else {
            expand_buffer();
        }

        if (size > buffer_size_) {
            if (this->get_mail()) {
                std::ofstream out(this->get_mail()->body_path, std::ios::binary | std::ios::app);
                if (out.is_open()) {
                    out.write(data, static_cast<std::streamsize>(size));
                    out.close();
                } else {
                    LOG_SESSION_ERROR("Failed to open mail body file for direct write: {}", this->get_mail()->body_path);
                }
            }
            return;
        }
    }

    std::memcpy(buffer_.get() + buffer_used_, data, size);
    buffer_used_ += size;
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::create_mail_on_data_command() {
    if (this->get_mail()) {
        LOG_SESSION_WARN("Mail already created, skipping");
        return;
    }

    LOG_SESSION_INFO("Creating new mail object on DATA command");
    this->mail_ = std::make_unique<mail>();
    auto& gen = algorithm::get_snowflake_generator();
    this->get_mail()->from = context_.sender_address;
    this->get_mail()->to = context_.recipient_addresses;
    this->mail_->id = gen.next_id();
    for (size_t i = 0; i < this->get_mail()->to.size(); ++i) {
        this->mail_->ids.push_back(gen.next_id());
    }
    this->get_mail()->send_time = std::time(nullptr);
    this->get_mail()->status = 1;
    this->get_mail()->box_id = 1;
    this->get_mail()->subject = "(无主题)";
    this->get_mail()->persist_status = persist_storage::PersistStatus::PENDING;

    std::string body_path = this->m_server->m_config.mail_storage_path;
    if (!body_path.empty() && body_path.back() != '/' && body_path.back() != '\\') {
        body_path.push_back('/');
    }
    body_path += std::to_string(this->get_mail()->id);
    this->get_mail()->body_path = body_path;

    LOG_SESSION_INFO("Created mail {} on DATA command, from: {}, recipients: {}",
        this->get_mail()->id, this->get_mail()->from, this->get_mail()->to.size());
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::submit_mail_to_queue() {
    if (!this->get_mail()) {
        LOG_SESSION_WARN("No mail to submit");
        return;
    }

    for (auto& att : context_.streamed_attachments) {
        this->get_mail()->attachments.push_back(std::move(att));
    }

    if (persistent_queue_) {
        if (!persistent_queue_->submit_mail(this->get_mail())) {
            LOG_SESSION_ERROR("Failed to submit mail {} to persistent queue", this->get_mail()->id);
            return;
        }
        LOG_SESSION_INFO("Submitted mail {} to persistent queue", this->get_mail()->id);
    } else {
        LOG_SESSION_ERROR("Persistent queue is null, cannot submit mail");
        return;
    }
}

template <typename ConnectionType>
bool SmtpsSession<ConnectionType>::check_mail_persist_status() {
    if (!this->get_mail()) {
        LOG_SESSION_WARN("No current mail to check persist status");
        return false;
    }

    mail* mail_ptr = this->get_mail();
    auto status = mail_ptr->persist_status;

    if (status == persist_storage::PersistStatus::SUCCESS) {
        this->flush_buffer_to_disk();
        LOG_SESSION_INFO("Mail {} persisted successfully", mail_ptr->id);
        return true;
    }

    if (status == persist_storage::PersistStatus::PENDING ||
        status == persist_storage::PersistStatus::PROCESSING) {
        // 仍在处理中，调用方应继续等待
        LOG_SESSION_DEBUG("Mail {} still processing (status {})", mail_ptr->id, static_cast<int>(status));
        return false;
    }

    LOG_SESSION_ERROR("Mail {} persist failed with status {}, cleaning up",
        mail_ptr->id, static_cast<int>(status));
    std::string error;
    this->persistent_queue_->batch_delete_metadata(mail_ptr, error);
    this->cleanup_mail_files(mail_ptr);
    return false;
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::transfer_mail_ownership_to_outbound() {
    if (!this->m_server || !this->m_server->m_outboundClient || !this->get_mail()) {
        return;
    }

    auto owned_mail = this->get_mail_ptr();
    if (!owned_mail) {
        return;
    }

    if (!this->m_server->m_outboundClient->accept_mail_ownership(std::move(owned_mail))) {
        LOG_SESSION_WARN("Failed to transfer mail ownership to outbound client");
    }
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::cleanup_mail_files(mail* mail_ptr) {
    if (!mail_ptr) {
        return;
    }

    if (!mail_ptr->body_path.empty()) {
        if (std::remove(mail_ptr->body_path.c_str()) == 0) {
            LOG_SESSION_INFO("Deleted mail body file: {}", mail_ptr->body_path);
        } else {
            LOG_SESSION_WARN("Failed to delete mail body file: {}", mail_ptr->body_path);
        }
    }

    for (const auto& att : mail_ptr->attachments) {
        if (!att.filepath.empty()) {
            if (std::remove(att.filepath.c_str()) == 0) {
                LOG_SESSION_INFO("Deleted attachment file: {}", att.filepath);
            } else {
                LOG_SESSION_WARN("Failed to delete attachment file: {}", att.filepath);
            }
        }
    }

    for (const auto& att : context_.streamed_attachments) {
        if (!att.filepath.empty()) {
            if (std::remove(att.filepath.c_str()) == 0) {
                LOG_SESSION_INFO("Deleted streamed attachment file: {}", att.filepath);
            } else {
                LOG_SESSION_WARN("Failed to delete streamed attachment file: {}", att.filepath);
            }
        }
    }
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::reset_mail_state() {
    this->mail_ = nullptr;
    buffer_used_ = 0;
    buffer_expand_count_ = 0;
    async_write_futures_.clear();

    context_.mail_data.clear();
    context_.header_buffer.clear();
    context_.text_body_buffer.clear();
    context_.line_buffer.clear();
    context_.body_limit_exceeded = false;
    context_.streaming_enabled = false;
    context_.multipart = false;
    context_.boundary.clear();
    context_.sender_address.clear();
    context_.recipient_addresses.clear();
    context_.streamed_attachments.clear();

    LOG_SESSION_DEBUG("Mail state reset");
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::process_message_data(const std::string& data) {
    algorithm::process_message_data(context_, data);
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::handle_multipart_line_and_write_attachment(const std::string& line) {
    algorithm::handle_multipart_line(context_, line);

    if (context_.current_part_is_attachment && !line.empty()) {
        append_to_attachment_buffer(line.data(), line.size());
    }
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::finalize_attachment_from_context() {
    if (context_.current_part_is_attachment && !context_.current_attachment_filename.empty()) {
        flush_attachment_buffer_to_disk();

        attachment att;
        att.filename = context_.current_attachment_filename;
        att.filepath = context_.current_attachment_path;
        att.mime_type = context_.current_part_mime.empty() ? "application/octet-stream" : context_.current_part_mime;
        att.file_size = context_.current_attachment_size;
        att.upload_time = std::time(nullptr);

        context_.streamed_attachments.push_back(std::move(att));
        LOG_SESSION_DEBUG("Attachment finalized and added, total count={}", context_.streamed_attachments.size());
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

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::expand_attachment_buffer() {
    if (context_.attachment_buffer_expand_count >= MAX_BUFFER_EXPAND_COUNT) {
        LOG_SESSION_INFO("Attachment buffer expansion limit reached, flushing to disk asynchronously");
        async_flush_attachment_buffer_to_disk();
        return;
    }

    size_t new_size = context_.attachment_buffer_size * BUFFER_GROWTH_FACTOR;
    if (new_size > MAX_BUFFER_SIZE) {
        LOG_SESSION_WARN("Attachment buffer expansion would exceed MAX_BUFFER_SIZE, flushing asynchronously");
        async_flush_attachment_buffer_to_disk();
        return;
    }

    async_flush_attachment_buffer_to_disk();

    LOG_SESSION_DEBUG("Expanding attachment buffer from {} to {} bytes", context_.attachment_buffer_size, new_size);
    auto new_buffer = std::make_unique<char[]>(new_size);
    context_.attachment_buffer = std::move(new_buffer);
    context_.attachment_buffer_size = new_size;
    context_.attachment_buffer_used = 0;
    context_.attachment_buffer_expand_count++;

    LOG_SESSION_INFO("Attachment buffer expanded, expand count: {}", context_.attachment_buffer_expand_count);
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::flush_attachment_buffer_to_disk() {
    if (context_.attachment_buffer_used == 0 || context_.current_attachment_path.empty()) {
        return;
    }

    try {
        std::ofstream out(context_.current_attachment_path, std::ios::binary | std::ios::app);
        if (!out.is_open()) {
            LOG_SESSION_ERROR("Failed to open attachment file for writing: {}", context_.current_attachment_path);
            return;
        }

        out.write(context_.attachment_buffer.get(), static_cast<std::streamsize>(context_.attachment_buffer_used));
        if (!out.good()) {
            LOG_SESSION_ERROR("Failed to write data to attachment file: {}", context_.current_attachment_path);
            out.close();
            return;
        }

        out.close();
        context_.current_attachment_size += context_.attachment_buffer_used;
        context_.attachment_buffer_used = 0;
        LOG_SESSION_DEBUG("Flushed {} bytes to attachment file", context_.current_attachment_size);
    } catch (const std::exception& e) {
        LOG_SESSION_ERROR("Exception during flush attachment buffer: {}", e.what());
    }
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::async_flush_attachment_buffer_to_disk() {
    if (context_.attachment_buffer_used == 0 || context_.current_attachment_path.empty()) {
        return;
    }

    std::string buffer_data(context_.attachment_buffer.get(), context_.attachment_buffer_used);
    std::string attachment_path = context_.current_attachment_path;
    context_.attachment_buffer_used = 0;

    auto future = this->m_server->m_workerThreadPool->submit([this, attachment_path, buffer_data]() -> bool {
        try {
            std::ofstream out(attachment_path, std::ios::binary | std::ios::app);
            if (!out.is_open()) {
                LOG_SESSION_ERROR("Failed to open attachment file for async write: {}", attachment_path);
                return false;
            }

            out.write(buffer_data.data(), static_cast<std::streamsize>(buffer_data.size()));
            if (!out.good()) {
                LOG_SESSION_ERROR("Failed to write data to attachment file: {}", attachment_path);
                out.close();
                return false;
            }

            out.close();
            LOG_SESSION_DEBUG("Async write {} bytes to attachment file: {}", buffer_data.size(), attachment_path);
            return true;
        } catch (const std::exception& e) {
            LOG_SESSION_ERROR("Exception during async attachment write: {}", e.what());
            return false;
        }
    });

    async_write_futures_.push_back(std::move(future));
    LOG_SESSION_INFO("Submitted async attachment write task, pending tasks: {}", async_write_futures_.size());
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::append_to_attachment_buffer(const char* data, size_t size) {
    if (!context_.attachment_buffer) {
        context_.attachment_buffer_size = INITIAL_BUFFER_SIZE;
        context_.attachment_buffer = std::make_unique<char[]>(INITIAL_BUFFER_SIZE);
        context_.attachment_buffer_used = 0;
        context_.attachment_buffer_expand_count = 0;
    }

    if (context_.attachment_buffer_used + size > context_.attachment_buffer_size) {
        if (context_.attachment_buffer_expand_count >= MAX_BUFFER_EXPAND_COUNT) {
            async_flush_attachment_buffer_to_disk();
        } else {
            expand_attachment_buffer();
        }

        if (size > context_.attachment_buffer_size) {
            try {
                std::ofstream out(context_.current_attachment_path, std::ios::binary | std::ios::app);
                if (out.is_open()) {
                    out.write(data, size);
                    context_.current_attachment_size += size;
                    out.close();
                }
            } catch (const std::exception& e) {
                LOG_SESSION_ERROR("Exception writing large attachment data: {}", e.what());
            }
            return;
        }
    }

    std::memcpy(context_.attachment_buffer.get() + context_.attachment_buffer_used, data, size);
    context_.attachment_buffer_used += size;
}

template <typename ConnectionType>
void SmtpsSession<ConnectionType>::parse_smtp_command(const std::string& data) {
    std::string trimmed = algorithm::trim(data);

    // In command phase, a standalone CRLF can arrive as a fragmented delimiter.
    // Emit TIMEOUT so FSM can continue reading without reusing a stale event.
    if (state_ != SmtpsState::IN_MESSAGE && trimmed.empty()) {
        ignore_current_command_ = false;
        next_event_ = SmtpsEvent::TIMEOUT;
        last_command_args_.clear();
        return;
    }

    LOG_SESSION_INFO("Handling data: {}", data);

    if (state_ == SmtpsState::IN_MESSAGE) {
        process_message_data(data);

        bool data_end_seen = (trimmed == ".") || (data.find("\r\n.\r\n") != std::string::npos);
        std::string write_chunk = data;
        if (data_end_seen) {
            // 去掉终止符“\r\n.\r\n”部分避免写入
            size_t pos = write_chunk.find("\r\n.\r\n");
            if (pos != std::string::npos) {
                write_chunk = write_chunk.substr(0, pos);
            } else if (trimmed == "." || trimmed == ".\r\n") {
                write_chunk.clear();
            }
        }

        if (!write_chunk.empty()) {
            // 对非流式正文去掉前导点（SMTP dot-stuffing）
            if (!context_.streaming_enabled && !write_chunk.empty() && write_chunk[0] == '.') {
                write_chunk.erase(0, 1);
            }
            append_to_buffer(write_chunk.data(), write_chunk.size());
        }

        if (data_end_seen) {
            finalize_attachment_from_context();
            next_event_ = SmtpsEvent::DATA_END;
            last_command_args_.clear();
        } else {
            next_event_ = SmtpsEvent::DATA;
            if (!context_.streaming_enabled) {
                if (!trimmed.empty() && trimmed[0] == '.') {
                    last_command_args_ = data.substr(data.find('.') + 1);
                } else {
                    last_command_args_ = data;
                }
            } else {
                last_command_args_.clear();
            }
        }

        if (fsm_) {
            LOG_SESSION_INFO("IN_MESSAGE next_event_: {}", fsm_->get_event_name(next_event_));
        }
        return;
    }

    ignore_current_command_ = false;

    if (state_ == SmtpsState::WAIT_AUTH_USERNAME || state_ == SmtpsState::WAIT_AUTH_PASSWORD) {
        next_event_ = SmtpsEvent::AUTH;
        last_command_args_ = trimmed;
        if (fsm_) {
            LOG_SESSION_INFO("AUTH next_event_: {}", fsm_->get_event_name(next_event_));
        }
        return;
    }

    std::string command;
    std::string args;
    std::string upper_trimmed = trimmed;
    std::transform(upper_trimmed.begin(), upper_trimmed.end(), upper_trimmed.begin(), ::toupper);

    if (upper_trimmed.compare(0, 9, "MAIL FROM") == 0) {
        command = "MAIL FROM";
        args = trimmed.substr(4);
    } else if (upper_trimmed.compare(0, 7, "RCPT TO") == 0) {
        command = "RCPT TO";
        args = trimmed.substr(4);
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
        next_event_ = SmtpsEvent::EHLO;
    } else if (command == "AUTH") {
        next_event_ = SmtpsEvent::AUTH;
    } else if (command == "MAIL FROM") {
        next_event_ = SmtpsEvent::MAIL_FROM;
    } else if (command == "RCPT TO") {
        next_event_ = SmtpsEvent::RCPT_TO;
    } else if (command == "DATA") {
        next_event_ = SmtpsEvent::DATA;
    } else if (command == "QUIT") {
        next_event_ = SmtpsEvent::QUIT;
    } else if (command == "STARTTLS") {
        next_event_ = SmtpsEvent::STARTTLS;
    } else {
        next_event_ = SmtpsEvent::ERROR;
        last_command_args_ = "Unknown command: " + command;
    }

    if (fsm_) {
        LOG_SESSION_INFO("next_event_: {}", fsm_->get_event_name(next_event_));
    }
}

} // namespace mail_system
