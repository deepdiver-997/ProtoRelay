#ifndef MAIL_SYSTEM_IDLE_PUSH_REGISTRY_H
#define MAIL_SYSTEM_IDLE_PUSH_REGISTRY_H

// ====================================================================
// IdlePushRegistry — IMAP IDLE 会话注册表（进程级单例，header-only）
//
// 用途：本地投递（SMTP DATA_END 持久化成功 → mail_mailbox 关联行落库）把
// 新邮件放进某账号的邮箱时，让正 IDLE 在该邮箱上的 IMAP 会话立刻收到
// untagged `* N EXISTS`（RFC 2177 / RFC 3501 §7.3.1）。
//
// 键设计：以小写账号地址（users.mail_address，即 ImapContext::username）为键。
// 不用 (user_id, mailbox_id) 的原因：SMTP 侧投递链路（persistence_repository
// 的 build_insert_mailbox_for_recipient 是 INSERT...SELECT）手里只有收件人
// 地址字符串，拿不到 user_id/mailbox_id；地址是两侧唯一免 DB 查询的公共键。
// 精确性由会话侧兜底：notify 只保证"这个账号有投递"，收到通知的会话在自己的
// executor 上重查已选中邮箱的邮件数，与已宣告数比较，变大才写 EXISTS——
// 投递落进该账号其他邮箱（非当前 SELECT 的）时计数不变，不会误推。
//
// 线程纪律：notify_account 可从任意线程（SMTP io 线程 / persistence worker）
// 调用。锁只保护 map 读写；通知动作（NotifyFn）在锁外执行，且实现方必须把
// 真正的连接写入 post 到目标会话自己的 executor——绝不跨线程直写连接。
//
// 生命周期：注册表只存 weak_ptr（会话可能随时死掉）；notify 时清理过期项。
// IDLE 进入时注册（handle_idle），DONE/退出时注销（handle_done）；IDLE 中被
// 客户端断开而未经 DONE 的会话靠 weak 过期 + notify 时清理兜底，注册表不持有
// 任何强引用。
//
// MVP 范围：只推"新邮件 → EXISTS"。EXPUNGE/FLAGS 推送、RECENT、非 IDLE 态
// 主动推、配置开关——一律不做（后续工作）。
// ====================================================================

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mail_system {

class IdlePushRegistry {
public:
    using NotifyFn = std::function<void()>;

    static IdlePushRegistry& instance() {
        static IdlePushRegistry registry;
        return registry;
    }

    // 注册一个 IDLE 会话。account：登录地址（大小写不敏感，内部归一小写）；
    // owner：会话 shared_ptr（仅作生命周期令牌与去重键，注册表不持有强引用，
    // 允许传任意能转 shared_ptr<void> 的智能指针）；notify：任意线程可调用，
    // 实现自行 post 到会话 executor。同一 owner 重复注册视为幂等（覆盖回调）。
    void register_watcher(const std::string& account,
                          const std::shared_ptr<void>& owner,
                          NotifyFn notify) {
        if (account.empty() || !owner || !notify) return;
        const void* key = owner.get();
        std::lock_guard<std::mutex> lk(mutex_);
        auto& entries = watchers_[normalize(account)];
        for (auto& e : entries) {
            if (e.raw == key) { e.notify = std::move(notify); return; }
        }
        entries.push_back(Entry{key, owner, std::move(notify)});
    }

    // 注销（handle_done 等显式退出 IDLE 时调用）。owner_raw 即注册时
    // owner.get() 的别名。
    void unregister_watcher(const std::string& account, const void* owner_raw) {
        if (account.empty() || !owner_raw) return;
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = watchers_.find(normalize(account));
        if (it == watchers_.end()) return;
        auto& entries = it->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [owner_raw](const Entry& e) { return e.raw == owner_raw; }),
            entries.end());
        if (entries.empty()) watchers_.erase(it);
    }

    // 本地投递成功提交后调用（任意线程）。锁内只做快照拷贝 + 过期项清理；
    // 通知动作全部在锁外执行，通知实现方负责把后续工作搬进会话 executor。
    void notify_account(const std::string& account) {
        if (account.empty()) return;
        std::vector<NotifyFn> snapshot;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = watchers_.find(normalize(account));
            if (it == watchers_.end()) return;
            auto& entries = it->second;
            for (auto& e : entries) {
                if (e.owner.expired()) continue;   // 会话已死：跳过，下面统一清理
                if (e.notify) snapshot.push_back(e.notify);
            }
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                [](const Entry& e) { return e.owner.expired(); }),
                entries.end());
            if (entries.empty()) watchers_.erase(it);
        }
        for (auto& fn : snapshot) {
            // 快照到调用之间会话可能刚好死亡：NotifyFn 实现须弱锁定会话，
            // 过期即空操作。此处绝不触碰任何会话内部状态。
            fn();
        }
    }

    // ── 测试观测/复位 ──
    size_t watcher_count(const std::string& account) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = watchers_.find(normalize(account));
        return it == watchers_.end() ? 0 : it->second.size();
    }

    void reset() {
        std::lock_guard<std::mutex> lk(mutex_);
        watchers_.clear();
    }

private:
    struct Entry {
        const void* raw;              // owner 的原生指针：去重 / 注销键
        std::weak_ptr<void> owner;    // 会话生命周期令牌（不持有强引用）
        NotifyFn notify;              // 弱捕获会话；内部 post 到会话 executor
    };

    static std::string normalize(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<Entry>> watchers_;
};

} // namespace mail_system

#endif // MAIL_SYSTEM_IDLE_PUSH_REGISTRY_H
