#ifndef TRADITIONAL_SMTPS_FSM_TPP
#define TRADITIONAL_SMTPS_FSM_TPP

#include "mail_system/back/algorithm/smtp_utils.h"
#include "mail_system/back/mailServer/session/smtps_session.h"

namespace mail_system {

// ========== 工具函数实现 ==========

// 注意：工具函数已经移至 mail_system::algorithm::smtp_utils.h/cpp

// ========== 清理函数实现 ==========

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::cleanup_streamed_attachments(SmtpsContext* ctx) {
    if (!ctx) {
        return;
    }
    algorithm::cleanup_streamed_attachments(*ctx);
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::cleanup_mail_files(mail* m) {
    if (!m->body_path.empty()) {
        std::remove(m->body_path.c_str());
    }
    for (const auto& p : m->attachments) {
        if (!p.filepath.empty()) {
            std::remove(p.filepath.c_str());
        }
    }
}

// ========== 持久化函数实现 ==========

template <typename ConnectionType>
bool TraditionalSmtpsFsm<ConnectionType>::persist_mails_sync(
    SessionBase<ConnectionType>* session,
    std::string& error
) {
    if (!session->mail_) {
        LOG_SMTP_DETAIL_WARN("No mail to persist");
        return true;
    }

    LOG_SMTP_DETAIL_DEBUG("Starting to persist 1 mail");
    auto& m = session->mail_;
    // metadata set when creating mail object
    // m->id = static_cast<uint64_t>(mail_system::algorithm::get_snowflake_generator().next_id());
    // std::string base_path = algorithm::ensure_trailing_slash(session->get_server()->m_config.mail_storage_path);
    // std::string file_path = base_path + std::to_string(m->id);
    LOG_SMTP_DETAIL_DEBUG("Mail ID: " + std::to_string(m->id) + ", Body size: " + std::to_string(m->body.size()));
    LOG_SMTP_DETAIL_DEBUG("Saving to file: " + m->body_path);

    if (m->persist_status == mail_system::persist_storage::PersistStatus::SUCCESS) {
        LOG_SMTP_DETAIL_DEBUG("Mail already persisted successfully, skipping");
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
bool TraditionalSmtpsFsm<ConnectionType>::persist_and_reply(std::unique_ptr<SessionBase<ConnectionType>> session) {
    std::string error;
    bool ok = persist_mails_sync(session.get(), error);
    std::string reply = ok ? "221 Bye\r\n" : ("451 " + error + "\r\n");
    SessionBase<ConnectionType>::do_async_write(
        std::move(session),
        reply,
        [] (std::unique_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code& ec) mutable {
            if (ec) {
                LOG_SMTP_DETAIL_ERROR("Error sending QUIT reply: {}", ec.message());
            }
            auto io_context = s->get_server()->get_io_context();
            auto timer = std::make_shared<boost::asio::steady_timer>(*io_context);
            timer->expires_after(std::chrono::milliseconds(100));
            timer->async_wait([s = std::move(s), timer](const boost::system::error_code& ec) mutable {
                if (!ec) {
                    LOG_SMTP_DETAIL_INFO("Closing connection after QUIT");
                    s->close();
                }
            });
        }
    );
    return ok;
}

// ========== 初始化函数实现 ==========

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::init_transition_table() {
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

    for (int i = 0; i < static_cast<int>(SmtpsState::CLOSED); ++i) {
        transition_table_[std::make_pair(static_cast<SmtpsState>(i), SmtpsEvent::QUIT)] = SmtpsState::CLOSED;
    }

    for (int i = 0; i < static_cast<int>(SmtpsState::CLOSED); ++i) {
        transition_table_[std::make_pair(static_cast<SmtpsState>(i), SmtpsEvent::ERROR)] = static_cast<SmtpsState>(i);
        transition_table_[std::make_pair(static_cast<SmtpsState>(i), SmtpsEvent::TIMEOUT)] = static_cast<SmtpsState>(i);
    }
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::init_state_handlers() {
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

// ========== 事件处理函数实现 ==========

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::process_event(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    SmtpsEvent event,
    const std::string& args
) {
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
        auto state_handler_it = state_handlers_.find(static_cast<SmtpsState>(session->get_current_state()));
        if (state_handler_it != state_handlers_.end()) {
            auto event_handler_it = state_handler_it->second.find(event);
            if (event_handler_it != state_handler_it->second.end()) {
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

        if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) {
            LOG_SMTP_DETAIL_WARN("No handler for state {} and event {}",
                                 SmtpsFsm<ConnectionType>::get_state_name(static_cast<SmtpsState>(session->get_current_state())),
                                 SmtpsFsm<ConnectionType>::get_event_name(event));
        }
    } else {
        if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) {
            LOG_SMTP_ERROR("Invalid transition from {} on event {}",
                           SmtpsFsm<ConnectionType>::get_state_name(static_cast<SmtpsState>(session->get_current_state())),
                           SmtpsFsm<ConnectionType>::get_event_name(event));
        }
        handle_error(std::move(session), "Invalid command sequence");
    }
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::auto_process_event(std::unique_ptr<SessionBase<ConnectionType>> session) {
    SmtpsEvent event = static_cast<SmtpsEvent>(session->get_next_event());
    std::string args = session->get_last_command_args();
    process_event(std::move(session), event, args);
}

// ========== 各状态事件处理函数实现 ==========

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_init_connect(
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
                auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(s->get_fsm());
                fsm->auto_process_event(std::move(s));
            });
        }
    );
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_greeting_ehlo(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args
) {
    LOG_SMTP_DETAIL_DEBUG("Received EHLO: {}", args);

    std::string response = "250-" + args + " Hello\r\n";
    response += "250-SIZE 10240000\r\n"
               "250-8BITMIME\r\n";

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

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_starttls(
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

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_auth(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args
) {
    SessionBase<ConnectionType>::do_async_write(
        std::move(session),
        "334 VXNlcm5hbWU6\r\n",
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

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_username(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args
) {
    static_cast<SmtpsContext*>(session->get_context())->client_username = args;
    SessionBase<ConnectionType>::do_async_write(
        std::move(session),
        "334 UGFzc3dvcmQ6\r\n",
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

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_password(
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

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_auth_mail_from(
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

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_mail_from_mail_from(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args
) {
    handle_wait_auth_mail_from(std::move(session), args);
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_rcpt_to_rcpt_to(
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

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_data_data(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args
) {
    // 在 DATA 命令时创建 mail 对象
    auto* smtp_session = dynamic_cast<SmtpsSession<ConnectionType>*>(session.get());
    if (smtp_session) {
        smtp_session->create_mail_on_data_command();
    }
    
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

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_in_message_data(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args
) {
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

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_in_message_data_end(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args
) {
    auto* ctx = static_cast<SmtpsContext*>(session->get_context());
    std::string resp = ctx->abort_reason.empty() ? "552 Message size exceeds limit\r\n" : ctx->abort_reason + "\r\n";
    if (ctx->body_limit_exceeded) {
        cleanup_streamed_attachments(ctx);
        SessionBase<ConnectionType>::do_async_write(
            std::move(session),
            resp,
            [] (std::unique_ptr<SessionBase<ConnectionType>> s, const boost::system::error_code&) mutable {
                s->close();
            }
        );
        return;
    }

    // 将邮件提交到持久化队列，确保正文写入后再检查状态
    if (auto* smtp_session = dynamic_cast<SmtpsSession<ConnectionType>*>(session.get())) {
        smtp_session->flush_body_and_wait();
        smtp_session->submit_mail_to_queue();
    } else {
        LOG_SMTP_DETAIL_ERROR("Failed to cast to SmtpsSession before persistence");
        session->close();
        return;
    }

    // 检查邮件持久化状态并回复客户端
    // 在后台线程轮询持久化结果，等待 SUCCESS 或失败/超时
    auto pool = session->get_server()->m_workerThreadPool;
    pool->post(make_copyable([s = std::move(session)]() mutable {
        auto* smtp_session = dynamic_cast<SmtpsSession<ConnectionType>*>(s.get());
        if (!smtp_session) {
            LOG_SMTP_DETAIL_ERROR("Failed to cast to SmtpsSession before persistence check");
            if (s) {
                s->close();
            }
            return;
        }

        auto* mail_ptr = smtp_session->get_mail();
        if (!mail_ptr) {
            LOG_SMTP_DETAIL_ERROR("No mail found during persistence check");
            if (s) {
                s->close();
            }
            return;
        }

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto backoff = std::chrono::milliseconds(50);
        while (std::chrono::steady_clock::now() < deadline) {
            auto status = mail_ptr->persist_status;
            if (status == persist_storage::PersistStatus::SUCCESS) {
                // 成功则回复 250 OK 并继续
                s->set_current_state(static_cast<int>(SmtpsState::WAIT_QUIT));
                smtp_session->reset_mail_state();
                SessionBase<ConnectionType>::do_async_write(std::move(s), "250 OK\r\n", [] (
                    std::unique_ptr<SessionBase<ConnectionType>> sss,
                    const boost::system::error_code& error
                ) mutable {
                    if (error) {
                        LOG_SMTP_DETAIL_ERROR("Error reading after mail persistence check: {}", error.message());
                        return;
                    }
                    SessionBase<ConnectionType>::do_async_read(std::move(sss), [] (
                        std::unique_ptr<SessionBase<ConnectionType>> ssss,
                        const boost::system::error_code& error,
                        std::size_t bytes_transferred
                    ) mutable {
                        if (error) {
                            LOG_SMTP_DETAIL_ERROR("Error reading after mail persistence success: {}", error.message());
                            return;
                        }
                        auto fsm = static_cast<TraditionalSmtpsFsm<ConnectionType>*>(ssss->get_fsm());
                        fsm->auto_process_event(std::move(ssss));
                    });
                });
                return;
            }

            if (status == persist_storage::PersistStatus::FAILED ||
                status == persist_storage::PersistStatus::CANCELLED) {
                LOG_SMTP_DETAIL_ERROR("Mail persistence failed with status {}, closing session.", static_cast<int>(status));
                smtp_session->check_mail_persist_status();
                if (s) {
                    s->close();
                }
                return;
            }

            std::this_thread::sleep_for(backoff);
            backoff = std::min(backoff * 2, std::chrono::milliseconds(400));
        }

        LOG_SMTP_DETAIL_ERROR("Mail persistence check timed out, closing session.");
        mail_ptr->persist_status = persist_storage::PersistStatus::CANCELLED;
        smtp_session->check_mail_persist_status();
        if (s) {
            s->close();
        }
    }));
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_wait_quit_quit(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args
) {
    LOG_SMTP_DETAIL_DEBUG("Entered handle_wait_quit_quit");
    auto client_ip = session->get_client_ip();
    LOG_SMTP_DETAIL_INFO("QUIT from {}", client_ip);
    // this->m_workerThreadPool->post(make_copyable([this, session = std::move(session)]() mutable {
    //     persist_and_reply(std::move(session));
    // }));
}

template <typename ConnectionType>
void TraditionalSmtpsFsm<ConnectionType>::handle_error(
    std::unique_ptr<SessionBase<ConnectionType>> session,
    const std::string& args
) {
    session->stay_times_++;
    if (session->stay_times_ > 3) {
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
            });
    }
}

} // namespace mail_system

#endif // TRADITIONAL_SMTPS_FSM_TPP
