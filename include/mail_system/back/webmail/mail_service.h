#ifndef MAIL_SYSTEM_WEBMAIL_MAIL_SERVICE_H
#define MAIL_SYSTEM_WEBMAIL_MAIL_SERVICE_H

// Webmail read-side application orchestration. Repositories own metadata and
// ownership checks; this service normalizes pagination and decodes stored MIME.
// Completions retain their provider's execution context; HTTP adapters serialize
// response writes on their connection executor.
#include "framework/log/log_macros.h"
#include "mail_system/back/common/mime_parser.h"
#include "mail_system/back/repository/webmail_read_repository.h"
#include "mail_system/back/router/i_shard_router.h"
#include "mail_system/back/webmail/mime_view.h"
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mail_system::webmail {

// 读信：元数据 + 解码后正文（html 未 sanitize——那是 API 层的职责，
// 见 html_sanitize.h）+ 附件清单（下载走后续的鉴权流式端点）。
struct MessageDetail {
    bool ok = false;
    bool server_error = false;   // DB 不可用 → 503
    bool not_found = false;      // 无此信 / 不在此箱 / 非本人 → 404
    bool body_available = false; // body 文件缺失/存储故障时 false（信壳照常返回）

    uint64_t id = 0;
    std::string subject;
    std::time_t send_time = 0;
    std::string sender;
    std::vector<std::string> recipients;
    int status = 0;
    bool starred = false;
    bool important = false;
    bool deleted = false;

    bool text_available = false;      // MIME 树里有 text/plain（或全文兜底）
    bool html_available = false;      // MIME 树里有 text/html
    std::string text;                 // 解码后纯文本
    std::string html;                 // 解码后 HTML（未 sanitize）
    std::string charset_text;
    std::string charset_html;
    bool text_truncated = false;
    bool html_truncated = false;

    std::vector<MailAttachment> attachments;
};

class MailService {
public:
    static constexpr uint32_t kMaxPageSize = 100;

    explicit MailService(std::shared_ptr<router::IShardRouter> shard_router,
        std::shared_ptr<repository::IWebmailReadRepository> read_repository = nullptr)
        : router_(std::move(shard_router)),
          repository_(read_repository ? std::move(read_repository)
              : repository::make_sql_webmail_read_repository(router_)) {}

    void list_mailboxes_async(uint64_t user_id, const std::string& address,
                             std::function<void(MailboxListResult)> cb) {
        repository_->list_mailboxes_async(user_id, address, std::move(cb));
    }

    // Keep pagination defense at the application boundary, including callers
    // that bypass the HTTP parser. The repository only translates SQL offset.
    void list_messages_async(uint64_t user_id, const std::string& address,
                             uint64_t mailbox_id, uint32_t page, uint32_t page_size,
                             std::function<void(MessagePageResult)> cb) {
        if (page_size == 0) page_size = 1;
        if (page_size > kMaxPageSize) page_size = kMaxPageSize;
        if (page == 0) page = 1;
        repository_->list_messages_async(user_id, address, mailbox_id,
                                        page, page_size, std::move(cb));
    }

    void get_message_async(uint64_t user_id, const std::string& address,
                           uint64_t mailbox_id, uint64_t mail_id,
                           std::function<void(MessageDetail)> cb) {
        repository_->get_message_metadata_async(user_id, address, mailbox_id, mail_id,
            [router = router_, address, cb = std::move(cb)](MessageMetadataResult metadata) mutable {
                if (!metadata.ok) {
                    MessageDetail out;
                    out.server_error = metadata.server_error;
                    out.not_found = metadata.not_found;
                    cb(std::move(out));
                    return;
                }
                auto ctx = std::make_shared<MessageFetchCtx>();
                ctx->cb = std::move(cb);
                auto& out = ctx->detail;
                out.id = metadata.id;
                out.subject = std::move(metadata.subject);
                out.send_time = metadata.send_time;
                out.sender = std::move(metadata.sender);
                out.recipients = std::move(metadata.recipients);
                out.status = metadata.status;
                out.starred = metadata.starred;
                out.important = metadata.important;
                out.deleted = metadata.deleted;
                out.attachments = std::move(metadata.attachments);
                storage_stage(ctx, address, metadata.body_key, router);
            });
    }

private:
    // 读信请求的可重入上下文：结果 + 完成回调一起走 shared_ptr。
    struct MessageFetchCtx {
        MessageDetail detail;
        std::function<void(MessageDetail)> cb;
    };

    // 读信存储段：body → MIME 树 → 解码正文。provider/路径/文件任一缺失
    // 都降级为 body_available=false 的成功结果（信壳可用）。
    static void storage_stage(std::shared_ptr<MessageFetchCtx> ctx,
                       const std::string& address, const std::string& body_path,
                       const std::shared_ptr<router::IShardRouter>& router) {
        int shard = 0;
        if (router) { int r = router->route(address); if (r >= 0) shard = r; }
        auto provider = router ? router->get_storage(static_cast<size_t>(shard)) : nullptr;
        if (!provider || body_path.empty()) {
            LOG_AT("WEBAPI", warn, "message {}: no storage provider or empty body_path",
                   ctx->detail.id);
            ctx->detail.ok = true;
            ctx->cb(std::move(ctx->detail));
            return;
        }
        provider->async_read_all(body_path,
            [ctx, body_path, provider](bool ok, std::string raw, const pr::storage::IoError& error) mutable {
                if (!ok || raw.empty()) {
                    LOG_AT("WEBAPI", warn, "message {}: body read failed ({})",
                           ctx->detail.id, error.message);
                    ctx->detail.ok = true;
                    ctx->cb(std::move(ctx->detail));
                    return;
                }
                ctx->detail.body_available = true;
                fill_body(ctx->detail, body_path, raw, provider);
                ctx->detail.ok = true;
                ctx->cb(std::move(ctx->detail));
            });
    }

    // MIME 取材：sidecar 优先（miss 则现场解析并回写，与 IMAP 同一份缓存）。
    // 纯文本兜底：无 MIME 树的旧邮件按"顶层 header 后全文"处理。
    static void fill_body(MessageDetail& st, const std::string& body_path,
                          const std::string& raw,
                          const std::shared_ptr<pr::storage::IStorageProvider>& provider) {
        MimePart tree;
        auto metadata = mime_metadata_store_for(provider);
        if (!ensure_mime_tree(metadata, body_path, raw, tree)) {
            size_t sep = raw.find("\r\n\r\n");
            if (sep == std::string::npos) sep = raw.find("\n\n");
            if (sep != std::string::npos) {
                st.text = raw.substr(sep + (raw[sep] == '\r' ? 4 : 2));
                if (st.text.size() > mime_view::kMaxDecodedBytes) {
                    st.text.resize(mime_view::kMaxDecodedBytes);
                    st.text_truncated = true;
                }
                st.text_available = true;
            }
            return;
        }
        if (const auto* p = mime_view::find_text_leaf(tree, "plain")) {
            auto d = mime_view::decode_part(extract_part_content(raw, *p), *p);
            st.text = std::move(d.content);
            st.text_truncated = d.truncated;
            st.charset_text = p->charset;
            st.text_available = true;
        }
        if (const auto* p = mime_view::find_text_leaf(tree, "html")) {
            auto d = mime_view::decode_part(extract_part_content(raw, *p), *p);
            st.html = std::move(d.content);
            st.html_truncated = d.truncated;
            st.charset_html = p->charset;
            st.html_available = true;
        }
    }

    std::shared_ptr<router::IShardRouter> router_;
    std::shared_ptr<repository::IWebmailReadRepository> repository_;
};

} // namespace mail_system::webmail
#endif // MAIL_SYSTEM_WEBMAIL_MAIL_SERVICE_H
