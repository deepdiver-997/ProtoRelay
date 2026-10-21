#ifndef SQL_QUERIES_H
#define SQL_QUERIES_H

#include "mail_system/back/db/db_service.h"
#include "mail_system/back/entities/mail.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mail_system {
namespace db {
namespace sql {

// ============================================================
// 邮件持久化 (mails / mail_recipients / mail_mailbox)
// ============================================================

// mails.subject 列宽（utf8mb4 字符数）。RFC 5322 单行上限 998 字节，encoded-word
// 展开后的原始主题基本都落在此量级。超列入参会 1406 报错导致整个持久化事务
// 回滚丢信（2026-09-04 入站事故），故入库/去重前一律先经 clip_subject_for_db。
inline constexpr std::size_t kMaxSubjectChars = 998;

// 按 DB 列宽截断主题：按 escape_string 后的字符数计预算（' " \ 等转义后翻倍），
// 且不切断 UTF-8 多字节序列。未超限时原样返回。
std::string clip_subject_for_db(const std::string& subject);

// 插入邮件元数据 (完整版: 含 send_time)
std::string build_insert_mail(std::uint64_t mail_id,
                               const std::string& subject,
                               const std::string& body_path,
                               std::time_t send_time,
                               IDBConnection* conn);

// 插入邮件元数据 (简化版: 含 status，用于 FSM DATA_END 路径)
std::string build_insert_mail_with_status(std::uint64_t mail_id,
                                           const std::string& subject,
                                           const std::string& body_path,
                                           int status,
                                           IDBConnection* conn);

// 插入收件人关系（批量 VALUES）
std::string build_insert_recipients(const mail& mail_data,
                                     const std::string& local_domain,
                                     IDBConnection* conn);

// 单个收件人的 mail_recipients 插入
std::string build_insert_recipient_single(std::uint64_t mail_id,
                                           std::uint64_t recipient_row_id,
                                           const std::string& sender,
                                           const std::string& recipient,
                                           int status,
                                           const std::string& source_message_id,
                                           IDBConnection* conn);

// 插入 mail_mailbox（关联到收件人收件箱 box_type=1）
std::string build_insert_mailbox_for_recipient(std::uint64_t mail_id,
                                                const std::string& recipient_address,
                                                IDBConnection* conn);

// 插入附件元数据（批量 VALUES）
std::string build_insert_attachments(std::uint64_t mail_id,
                                      const std::vector<attachment>& attachments,
                                      IDBConnection* conn);

// 插入单个附件元数据（包含 upload_time）
std::string build_insert_attachment_single(std::uint64_t mail_id,
                                            const attachment& att,
                                            IDBConnection* conn);

// ============================================================
// 出站队列 (mail_outbox)
// ============================================================

// 插入出站记录（PENDING 状态）
std::string build_insert_outbox_pending(std::uint64_t mail_id,
                                         const std::string& sender,
                                         const std::string& recipient,
                                         int max_attempts,
                                         IDBConnection* conn);

// 插入出站记录（SENDING 状态，预留 lease）
std::string build_insert_outbox_reserved(std::uint64_t mail_id,
                                          const std::string& sender,
                                          const std::string& recipient,
                                          const std::string& lease_owner,
                                          int lease_seconds,
                                          IDBConnection* conn);

// 领取待投递批次
std::string build_outbox_claim_select(int limit);

// 标记单条 outbox 为 SENDING（竞争更新）
std::string build_outbox_claim_update(std::uint64_t outbox_id,
                                       const std::string& worker_id,
                                       int lease_seconds,
                                       IDBConnection* conn);

// 释放本地预留
std::string build_outbox_release_reservations(const std::vector<std::uint64_t>& outbox_ids);

// 标记已发送
std::string build_outbox_mark_sent(std::uint64_t outbox_id,
                                    const std::string& smtp_response,
                                    IDBConnection* conn);

// 查询 attempt 信息
std::string build_outbox_select_attempts(std::uint64_t outbox_id);

// 标记重试/死亡
std::string build_outbox_mark_retry_or_dead(std::uint64_t outbox_id,
                                             int status,
                                             const std::string& error_message,
                                             int retry_delay_seconds,
                                             IDBConnection* conn);

// 标记死亡
std::string build_outbox_mark_dead(std::uint64_t outbox_id,
                                    const std::string& error_message,
                                    IDBConnection* conn);

// 回收过期租约
std::string build_outbox_requeue_expired_leases();

// ============================================================
// 邮件加载（出站时从 DB 重建 mail 对象）
// ============================================================

std::string build_load_mail_metadata(std::uint64_t mail_id);
std::string build_load_mail_recipients(std::uint64_t mail_id);
std::string build_load_mail_attachments(std::uint64_t mail_id);

// ============================================================
// 入站去重
// ============================================================

// 基于主题+发送者的模糊去重
std::string build_dedup_by_subject_sender(const std::string& subject,
                                           const std::string& sender,
                                           int window_seconds,
                                           IDBConnection* conn);

// 基于主题+发送者+收件人的精确去重
std::string build_dedup_by_subject_sender_recipient(const std::string& subject,
                                                     const std::string& sender,
                                                     const std::string& recipient,
                                                     int window_seconds,
                                                     IDBConnection* conn);

// 基于 Message-ID 去重
std::string build_dedup_by_message_id(const std::string& sender,
                                       const std::string& recipient,
                                       const std::string& message_id,
                                       IDBConnection* conn);

// ============================================================
// 认证（SMTP AUTH / IMAP LOGIN）
// ============================================================

// 参数化查询模板（使用 ? 占位符）
std::string build_auth_user_query();
std::string build_update_last_login();

// rehash-on-login：把 MD5/明文历史行升级为 bcrypt。CAS 语义——WHERE 同时限定旧
// password 值：用户用旧密码登录中若管理员恰好改了密码，这条升级不会把新密码覆盖
// 回旧值（影响 0 行，下次登录按新密码走）。参数顺序 {new_hash, mail_address, old_stored}
std::string build_update_password_hash();

// RCPT TO 校验：本地收件人是否存在（未认证 MTA 入站路径）
std::string build_recipient_exists_query();

// RCPT TO 别名展开（mail_aliases，alias→target 多行=邮件组）。JOIN users(status=1)
// 在读侧兜底：目标必须是在用真实账号；别名不是 users 行，alias→alias 链即使被
// 脏写入也查不出目标，读路径天然无环。参数 {alias_address}，多行结果按序展开。
std::string build_alias_targets_query();

// 每日发信配额：条件 UPDATE 原子占用今日一个配额（超限 affected rows = 0）
std::string build_increment_sent_today_query();

// ============================================================
// IMAP 命令
// ============================================================

std::string build_imap_list_mailboxes();
std::string build_imap_get_mailbox_by_name();
std::string build_imap_get_inbox_id();
std::string build_imap_get_mailbox_mails();
std::string build_imap_mailbox_exists_count();
std::string build_imap_mailbox_unseen_count();
std::string build_imap_uidnext_advance();   // 原子推进 per-mailbox UIDNEXT 高水位
std::string build_imap_uidnext_read();      // 读推进后的 UIDNEXT
std::string build_imap_update_mail_flag_deleted();
std::string build_imap_update_mail_flag_starred();
std::string build_imap_append_mail_metadata();
std::string build_imap_append_mail_recipient();
// mail_mailbox 关联（APPEND/COPY）。生产表无 add_time 列（2026-10-21 生产
// 实测 Unknown column 'add_time'），列清单以生产为准，与 SMTP 入站路径同款。
// 参数 {mail_id, mailbox_id, user_id}。
std::string build_imap_append_mailbox();
// APPEND 失败回滚：逐表显式删，不赌生产 FK 级联。参数 {mail_id}。
std::string build_delete_mail_by_id();
std::string build_delete_mail_recipients_by_mail();
std::string build_delete_mailbox_links_by_mail();
std::string build_imap_select_status_total();
std::string build_imap_select_status_recent();
std::string build_imap_expunge_delete_mailbox();
std::string build_imap_expunge_select_ids();
std::string build_imap_create_mailbox();
std::string build_imap_rename_mailbox();
std::string build_imap_delete_mailbox_messages();
std::string build_imap_check_mailbox_is_system();
std::string build_imap_delete_mailbox();
std::string build_imap_copy_check_exists();
std::string build_imap_copy_insert_mailbox();

// ============================================================
// webmail API（mail_system/back/webmail，只读列表/分页）
// ============================================================

// 用户全部邮箱 + 每箱 total/unread 聚合（侧边栏一条查询）。LEFT JOIN 限定
// 本人收件行（mr.recipient = ?，(mail_id, recipient) 唯一）故聚合不放大；
// 发件副本无本人收件行 → unread=0。参数 {mail_address, user_id}。
std::string build_webmail_list_mailboxes();

// 邮箱归属校验 + 元信息：id 不属于 user_id 即 0 行（跨用户访问 → 上层 404）。
// 参数 {mailbox_id, user_id}。
std::string build_webmail_get_mailbox_by_id();

// 单箱邮件总数（分页 total）。参数 {mailbox_id, user_id}。
std::string build_webmail_count_mailbox_mails();

// 单箱邮件分页，每封一行。LEFT JOIN mr 聚合出双视角：sender=本人收件行的
// 发件人（收件箱视角）；recipients=本人发件行的收件人 GROUP_CONCAT（发件箱
// 视角，跨收件人行聚合）。EXISTS 探测附件（列表角标，读信页另有详情）。
// 排序 send_time DESC, id DESC（id 为雪花 ID，同秒内仍稳定）。
// LIMIT/OFFSET 由调用方严格 parse 后内联（纯数字，见 mail_api 参数校验）。
// 参数 {mail_address, mail_address, mail_address, mailbox_id, user_id}。
std::string build_webmail_list_mailbox_mails(std::uint32_t limit, std::uint32_t offset);

// 读信：邮件必须同时在该 mailbox 且属于该 user（mm 唯一键保证单行），
// 0 行 = 无此信/不在此箱/非本人 → 上层统一 404。参数 {mail_id, mailbox_id, user_id}。
std::string build_webmail_get_mail_in_mailbox();

// 读信发件人/收件人/附件（读信页 From/To 行与附件清单）。
// 附件清单来自 attachments 表（SMTP 收信时落库），下载按 attachment id 走
// 后续的鉴权流式端点。参数均 {mail_id}。
std::string build_webmail_get_mail_sender();
std::string build_webmail_get_mail_recipients();
std::string build_webmail_list_mail_attachments();

// ============================================================
// 分片路由
// ============================================================

std::string build_shard_lookup(const std::string& table_name,
                                const std::string& email_column,
                                const std::string& shard_column,
                                const std::string& escaped_email);

// ============================================================
// 清理
// ============================================================

// 简化版 recipients 插入（FSM 路径，无 id/status 列）
std::string build_insert_recipients_simple(const mail& mail_data, IDBConnection* conn);

std::string build_delete_attachments_by_mail(std::uint64_t mail_id);
std::string build_delete_recipients_by_mail(std::uint64_t mail_id);
std::string build_delete_mail_by_id(std::uint64_t mail_id);
std::string build_delete_mail_by_body_path();
std::string build_delete_mail_recipients_by_id_list(const std::string& id_list);
std::string build_delete_mails_by_id_list(const std::string& id_list);

// ============================================================
// 工具
// ============================================================

std::string build_select_last_insert_id();
std::string build_select_row_count();

} // namespace sql
} // namespace db
} // namespace mail_system

#endif // SQL_QUERIES_H
