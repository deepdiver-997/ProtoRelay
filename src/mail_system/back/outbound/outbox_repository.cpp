#include "mail_system/back/outbound/outbox_repository.h"

#include "mail_system/back/common/logger.h"
#include "mail_system/back/db/mysql_pool.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace mail_system {
namespace outbound {

namespace {
constexpr int kDefaultMaxAttempts = 8;
constexpr int kDeadlockRetryMaxAttempts = 3;
constexpr int kDeadlockRetryBaseDelayMs = 40;

bool is_deadlock_error(const std::string& error_message) {
    return error_message.find("Deadlock found") != std::string::npos ||
           error_message.find("errno: 1213") != std::string::npos ||
           error_message.find("ERROR 1213") != std::string::npos;
}

bool execute_with_deadlock_retry(const std::shared_ptr<IDBConnection>& conn,
                                 const std::string& sql,
                                 const char* operation_name,
                                 std::uint64_t outbox_id) {
    if (!conn) {
        return false;
    }

    for (int attempt = 1; attempt <= kDeadlockRetryMaxAttempts; ++attempt) {
        if (conn->execute(sql)) {
            return true;
        }

        const auto error_message = conn->get_last_error();
        if (!is_deadlock_error(error_message) || attempt == kDeadlockRetryMaxAttempts) {
            return false;
        }

        const int delay_ms = kDeadlockRetryBaseDelayMs * (1 << (attempt - 1));
        LOG_OUTBOUND_WARN("OutboxRepository: deadlock on {}, outbox_id={}, retry={}/{}, delay_ms={}, error={}",
                        operation_name,
                        outbox_id,
                        attempt,
                        kDeadlockRetryMaxAttempts,
                        delay_ms,
                        error_message);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    return false;
}
}

OutboxRepository::OutboxRepository(std::shared_ptr<DBPool> db_pool)
    : db_pool_(std::move(db_pool)) {}

bool OutboxRepository::enqueue_from_mail(const mail& mail_data,
                                         const std::string& local_domain,
                                         std::vector<std::uint64_t>* outbox_ids) {
    if (!db_pool_) {
        LOG_OUTBOUND_ERROR("OutboxRepository: db_pool is null");
        return false;
    }

    auto mysql_pool = std::dynamic_pointer_cast<MySQLPool>(db_pool_);
    if (!mysql_pool) {
        LOG_OUTBOUND_ERROR("OutboxRepository: db_pool is not MySQLPool");
        return false;
    }

    auto raii_conn = mysql_pool->get_raii_connection();
    auto conn = raii_conn.get();
    if (!conn || !conn->is_connected()) {
        LOG_OUTBOUND_ERROR("OutboxRepository: failed to get DB connection");
        return false;
    }

    auto escaped_sender = escape_or_empty(conn, mail_data.from);
    bool inserted_any = false;

    for (const auto& recipient : mail_data.to) {
        const auto recipient_domain = extract_domain(recipient);
        if (recipient_domain.empty() || recipient_domain == local_domain) {
            continue;
        }

        std::ostringstream sql;
        sql << "INSERT INTO mail_outbox (mail_id, sender, recipient, status, priority, attempt_count, max_attempts, next_attempt_at) VALUES ("
            << mail_data.id << ", '" << escaped_sender << "', '"
            << escape_or_empty(conn, recipient) << "', "
            << static_cast<int>(OutboxStatus::PENDING) << ", 0, 0, "
            << kDefaultMaxAttempts << ", NOW())";

        if (!conn->execute(sql.str())) {
            LOG_OUTBOUND_ERROR("OutboxRepository: failed to insert outbox row, mail_id={}, recipient={}, error={}",
                             mail_data.id,
                             recipient,
                             conn->get_last_error());
            continue;
        }

        inserted_any = true;
        if (outbox_ids) {
            auto result = conn->query("SELECT LAST_INSERT_ID() AS id");
            if (result && result->get_row_count() > 0) {
                auto row = result->get_row(0);
                auto it = row.find("id");
                if (it != row.end()) {
                    outbox_ids->push_back(static_cast<std::uint64_t>(std::stoull(it->second)));
                }
            }
        }
    }

    return inserted_any;
}

std::vector<OutboxRecord> OutboxRepository::claim_batch(const std::string& worker_id,
                                                        std::size_t limit,
                                                        int lease_seconds) {
    std::vector<OutboxRecord> claimed;
    if (!db_pool_ || limit == 0) {
        return claimed;
    }

    auto mysql_pool = std::dynamic_pointer_cast<MySQLPool>(db_pool_);
    if (!mysql_pool) {
        return claimed;
    }

    auto raii_conn = mysql_pool->get_raii_connection();
    auto conn = raii_conn.get();
    if (!conn || !conn->is_connected()) {
        return claimed;
    }

    std::ostringstream select_sql;
    select_sql << "SELECT o.id, o.mail_id, o.sender, o.recipient, o.attempt_count, o.max_attempts, m.body_path "
               << "FROM mail_outbox o "
               << "LEFT JOIN mails m ON m.id = o.mail_id "
               << "WHERE (status = " << static_cast<int>(OutboxStatus::PENDING)
               << " OR status = " << static_cast<int>(OutboxStatus::RETRY) << ") "
               << "AND next_attempt_at <= NOW() "
               << "AND (lease_until IS NULL OR lease_until <= NOW()) "
               << "ORDER BY o.priority DESC, o.id ASC LIMIT " << limit;

    auto result = conn->query(select_sql.str());
    if (!result || result->get_row_count() == 0) {
        return claimed;
    }

    const auto escaped_worker = escape_or_empty(conn, worker_id);
    const auto rows = result->get_all_rows();
    for (const auto& row : rows) {
        if (row.find("id") == row.end() || row.find("mail_id") == row.end()) {
            continue;
        }

        const auto outbox_id = static_cast<std::uint64_t>(std::stoull(row.at("id")));
        std::ostringstream update_sql;
        update_sql << "UPDATE mail_outbox SET status = " << static_cast<int>(OutboxStatus::SENDING)
                   << ", lease_owner = '" << escaped_worker << "'"
                   << ", lease_until = DATE_ADD(NOW(), INTERVAL " << lease_seconds << " SECOND)"
                   << ", attempt_count = attempt_count + 1"
                   << ", updated_at = NOW()"
                   << " WHERE id = " << outbox_id
                   << " AND (status = " << static_cast<int>(OutboxStatus::PENDING)
                   << " OR status = " << static_cast<int>(OutboxStatus::RETRY) << ")"
                   << " AND (lease_until IS NULL OR lease_until <= NOW())";

        if (!execute_with_deadlock_retry(conn, update_sql.str(), "claim_batch", outbox_id)) {
            continue;
        }

        auto row_count_result = conn->query("SELECT ROW_COUNT() AS affected");
        if (!row_count_result || row_count_result->get_row_count() == 0) {
            continue;
        }
        auto affected_row = row_count_result->get_row(0);
        if (affected_row.find("affected") == affected_row.end() ||
            std::stoi(affected_row.at("affected")) <= 0) {
            continue;
        }

        OutboxRecord item;
        item.id = outbox_id;
        item.mail_id = static_cast<std::uint64_t>(std::stoull(row.at("mail_id")));
        item.sender = row.count("sender") ? row.at("sender") : std::string{};
        item.recipient = row.count("recipient") ? row.at("recipient") : std::string{};
        item.attempt_count = row.count("attempt_count") ? (std::stoi(row.at("attempt_count")) + 1) : 1;
        item.max_attempts = row.count("max_attempts") ? std::stoi(row.at("max_attempts")) : kDefaultMaxAttempts;
        item.body_path = row.count("body_path") ? row.at("body_path") : std::string{};
        claimed.push_back(std::move(item));
    }

    return claimed;
}

bool OutboxRepository::mark_sent(std::uint64_t outbox_id, const std::string& smtp_response) {
    if (!db_pool_) {
        return false;
    }
    auto mysql_pool = std::dynamic_pointer_cast<MySQLPool>(db_pool_);
    if (!mysql_pool) {
        return false;
    }
    auto raii_conn = mysql_pool->get_raii_connection();
    auto conn = raii_conn.get();
    if (!conn || !conn->is_connected()) {
        return false;
    }

    std::ostringstream sql;
    sql << "UPDATE mail_outbox SET status = " << static_cast<int>(OutboxStatus::SENT)
        << ", sent_at = NOW(), smtp_response = '" << escape_or_empty(conn, smtp_response) << "'"
        << ", lease_owner = NULL, lease_until = NULL, updated_at = NOW()"
        << " WHERE id = " << outbox_id;
    return execute_with_deadlock_retry(conn, sql.str(), "mark_sent", outbox_id);
}

bool OutboxRepository::mark_retry(std::uint64_t outbox_id,
                                  const std::string& error_message,
                                  int retry_delay_seconds) {
    if (!db_pool_) {
        return false;
    }
    auto mysql_pool = std::dynamic_pointer_cast<MySQLPool>(db_pool_);
    if (!mysql_pool) {
        return false;
    }
    auto raii_conn = mysql_pool->get_raii_connection();
    auto conn = raii_conn.get();
    if (!conn || !conn->is_connected()) {
        return false;
    }

    auto status = OutboxStatus::RETRY;
    std::ostringstream check_sql;
    check_sql << "SELECT attempt_count, max_attempts FROM mail_outbox WHERE id = " << outbox_id;
    auto result = conn->query(check_sql.str());
    if (result && result->get_row_count() > 0) {
        auto row = result->get_row(0);
        const int attempts = row.count("attempt_count") ? std::stoi(row.at("attempt_count")) : 0;
        const int max_attempts = row.count("max_attempts") ? std::stoi(row.at("max_attempts")) : kDefaultMaxAttempts;
        if (attempts >= max_attempts) {
            status = OutboxStatus::DEAD;
        }
    }

    std::ostringstream sql;
    sql << "UPDATE mail_outbox SET status = " << static_cast<int>(status)
        << ", last_error_message = '" << escape_or_empty(conn, error_message) << "'"
        << ", lease_owner = NULL, lease_until = NULL"
        << ", next_attempt_at = DATE_ADD(NOW(), INTERVAL " << std::max(1, retry_delay_seconds) << " SECOND)"
        << ", updated_at = NOW()"
        << " WHERE id = " << outbox_id;
    return execute_with_deadlock_retry(conn, sql.str(), "mark_retry", outbox_id);
}

bool OutboxRepository::mark_dead(std::uint64_t outbox_id, const std::string& error_message) {
    if (!db_pool_) {
        return false;
    }
    auto mysql_pool = std::dynamic_pointer_cast<MySQLPool>(db_pool_);
    if (!mysql_pool) {
        return false;
    }
    auto raii_conn = mysql_pool->get_raii_connection();
    auto conn = raii_conn.get();
    if (!conn || !conn->is_connected()) {
        return false;
    }

    std::ostringstream sql;
    sql << "UPDATE mail_outbox SET status = " << static_cast<int>(OutboxStatus::DEAD)
        << ", last_error_message = '" << escape_or_empty(conn, error_message) << "'"
        << ", lease_owner = NULL, lease_until = NULL, updated_at = NOW()"
        << " WHERE id = " << outbox_id;
    return execute_with_deadlock_retry(conn, sql.str(), "mark_dead", outbox_id);
}

bool OutboxRepository::requeue_expired_leases() {
    if (!db_pool_) {
        return false;
    }
    auto mysql_pool = std::dynamic_pointer_cast<MySQLPool>(db_pool_);
    if (!mysql_pool) {
        return false;
    }
    auto raii_conn = mysql_pool->get_raii_connection();
    auto conn = raii_conn.get();
    if (!conn || !conn->is_connected()) {
        return false;
    }

    std::ostringstream sql;
    sql << "UPDATE mail_outbox SET status = " << static_cast<int>(OutboxStatus::RETRY)
        << ", lease_owner = NULL, lease_until = NULL"
        << ", next_attempt_at = NOW(), updated_at = NOW()"
        << " WHERE status = " << static_cast<int>(OutboxStatus::SENDING)
        << " AND lease_until IS NOT NULL AND lease_until <= NOW()";
    return execute_with_deadlock_retry(conn, sql.str(), "requeue_expired_leases", 0);
}

std::string OutboxRepository::extract_domain(const std::string& email) {
    const auto at_pos = email.find('@');
    if (at_pos == std::string::npos || at_pos + 1 >= email.size()) {
        return {};
    }
    return email.substr(at_pos + 1);
}

std::string OutboxRepository::escape_or_empty(const std::shared_ptr<IDBConnection>& conn,
                                              const std::string& value) {
    if (!conn) {
        return value;
    }
    return conn->escape_string(value);
}

} // namespace outbound
} // namespace mail_system
