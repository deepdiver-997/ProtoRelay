#include "mail_system/back/repository/imap_folder_repository.h"
#include "mail_system/back/db/sql_queries.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mail_system::repository {
namespace {
class SqlImapFolderRepository final : public IImapFolderRepository {
public:
    void create_async(Lease lease, uint64_t user, std::string name, Complete complete) override {
        if (!lease || !lease->is_valid()) { if (complete) complete({}); return; }
        const std::vector<std::string> params{std::to_string(user), std::move(name)};
        (*lease)->async_execute(db::sql::build_imap_create_mailbox(), params,
            [lease, complete = std::move(complete)](bool ok) {
                if (complete) complete({ok ? ImapFolderStatus::ok : ImapFolderStatus::database_error, true});
            });
    }
    void remove_async(Lease lease, ImapMailboxScope scope, Complete complete) override {
        if (!lease || !lease->is_valid()) { if (complete) complete({}); return; }
        (*lease)->async_query(db::sql::build_imap_check_mailbox_is_system() + " AND user_id = ?",
            {std::to_string(scope.mailbox_id), std::to_string(scope.user_id)},
            [lease, scope, complete = std::move(complete)](std::shared_ptr<pr::IDBResult> rows) mutable {
                if (!rows) { if (complete) complete({}); return; }
                if (rows->get_row_count() == 0) { if (complete) complete({ImapFolderStatus::not_found, false}); return; }
                const auto system = rows->get_value(0, "is_system");
                if (system == "1") {
                    if (complete) complete({ImapFolderStatus::protected_mailbox, false}); return;
                }
                if (system != "0") { if (complete) complete({}); return; }
                (*lease)->async_execute(db::sql::build_imap_delete_mailbox_messages() + " AND user_id = ?",
                    {std::to_string(scope.mailbox_id), std::to_string(scope.user_id)},
                    [lease, scope, complete = std::move(complete)](bool unlinked) mutable {
                        if (!unlinked) { if (complete) complete({ImapFolderStatus::database_error, true}); return; }
                        (*lease)->async_execute(db::sql::build_imap_delete_mailbox() + " AND user_id = ?",
                            {std::to_string(scope.mailbox_id), std::to_string(scope.user_id)},
                            [lease, complete = std::move(complete)](bool removed) {
                                if (complete) complete({removed ? ImapFolderStatus::ok : ImapFolderStatus::database_error, true});
                            });
                    });
            });
    }
    void rename_async(Lease lease, ImapMailboxScope scope, std::string name, Complete complete) override {
        if (!lease || !lease->is_valid()) { if (complete) complete({}); return; }
        const std::vector<std::string> params{std::move(name), std::to_string(scope.mailbox_id), std::to_string(scope.user_id)};
        (*lease)->async_execute(db::sql::build_imap_rename_mailbox() + " AND user_id = ?", params,
            [lease, complete = std::move(complete)](bool ok) {
                if (complete) complete({ok ? ImapFolderStatus::ok : ImapFolderStatus::database_error, true});
            });
    }
};
} // namespace
std::shared_ptr<IImapFolderRepository> make_sql_imap_folder_repository() { return std::make_shared<SqlImapFolderRepository>(); }
} // namespace mail_system::repository
