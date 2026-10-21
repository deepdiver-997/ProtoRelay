// ================================================================
// OutboundServer — 出站投递调度器实现
//
// 实现从 include/mail_system/back/mailServer/outbound_server.h 拆出
// （参考 src/framework/server_base.cpp 与 include/framework/server_base.h
//  的 .h+.cpp 范式）
//
// 拆分原因：原 outbound_server.h 404 行所有实现都 inline 在 class 内
//   加重 include 链传播时间、且与同模块其它文件（outbound_smtp_session.h
//   / outbound_smtp_fsm.h 模板类）混淆 .h/.cpp 范式
// 拆后：.h 仅声明 + 成员，.cpp 实现 21 个函数
// SMTP 协议现由 smtp::ClientSession 共享内核实现；OutboundSmtpSession 是策略适配器。
// ================================================================

#include "mail_system/back/mailServer/outbound_server.h"
#include "mail_system/back/common/logger.h"
#include "mail_system/back/router/i_shard_router.h"  // 解引用 m_shardRouter：server_base.h 已不再传递完整类型（泄漏清理）
#include "mail_system/back/mailServer/outbound/dns_resolver.h"  // SyncDnsWrapper（MX 解析）
#include <stdexcept>
#include <unordered_set>

namespace mail_system {
namespace outbound {

// ── 构造 / 析构 ────────────────────────────────────────────
OutboundServer::OutboundServer(mail_system::MailServerBase* server,
                               int max_per_mx,
                               int idle_timeout)
    : transports_(std::dynamic_pointer_cast<pr::net::IClientTransportProvider>(server->m_ioThreadPool))
    , executors_(std::dynamic_pointer_cast<pr::net::IExecutorProvider>(server->m_ioThreadPool))
    , worker_pool_(server->m_workerThreadPool)
    , shard_router_(server->m_shardRouter)
    , dns_resolver_(server->get_dns_resolver())
    , max_sessions_per_mx_(max_per_mx)
    , idle_timeout_sec_(idle_timeout)
    , worker_id_("ob-" + std::to_string(reinterpret_cast<uintptr_t>(this)))
    , scheduler_executor_(server->get_executor())
    , evict_timer_(scheduler_executor_->make_timer())
    , retry_timer_(scheduler_executor_->make_timer())
{
    if (!transports_ || !executors_)
        throw std::invalid_argument("OutboundServer: runtime must provide client transports and executors");
    auto cfg = std::atomic_load(&server->m_config);
    helo_domain_ = cfg && !cfg->outbound_helo_domain.empty() && cfg->outbound_helo_domain != "outbound.local"
        ? cfg->outbound_helo_domain : server->m_domain;
}

OutboundServer::~OutboundServer() { stop(); }

// ── 生命周期 ────────────────────────────────────────────────
void OutboundServer::start() {
    auto weak = weak_from_this();
    if (weak.expired()) throw std::logic_error("OutboundServer::start requires shared ownership");
    if (running_.exchange(true)) return;
    completion_cb_ = [weak](uint64_t record_id, smtp::Result result) {
        if (auto self = weak.lock(); self && self->running_.load())
            self->on_delivery_complete(record_id, std::move(result));
    };

    // 初始拉取：post 到 worker 池，不占常驻线程
    worker_pool_->post([weak]() { if (auto self = weak.lock()) self->try_pull(); });

    // 空闲回收定时器
    schedule_evict();

    LOG_SMTP_INFO("OutboundServer started: worker={}", worker_id_);
}

void OutboundServer::on_delivery_complete(uint64_t record_id, smtp::Result result) {
    const bool success = result.outcome == smtp::Outcome::Accepted;
    const bool permanent = result.outcome == smtp::Outcome::Rejected || result.outcome == smtp::Outcome::AuthFailed;
    const std::string response = std::to_string(result.code) + " " + result.stage + " " + result.error;
    // DB 写入 post 到 worker 池，不阻塞 IO 线程
    auto db = shard_router_->get_db_pool(0);
    if (db) {
        worker_pool_->post([db, record_id, success, permanent, response]() {
            OutboxRepository repo;
            const bool saved = success ? repo.mark_sent(*db, record_id, response) :
                permanent ? repo.mark_dead(*db, record_id, response) : repo.mark_retry(*db, record_id, response, 30);
            if (!saved) LOG_SMTP_ERROR("Outbound: result persistence failed record_id={} response={}", record_id, response);
        });
    } else LOG_SMTP_ERROR("Outbound: no database for completion record_id={}", record_id);

    // 2026-08-27: 投递结果 metrics。domain label 在此 lambda 拿不到（task
    // 已 reset），仅用 {} 标签。counts 数本身比按域分桶价值高（聚合告警阈值）。
    if (auto m = m_metrics.lock()) {
        m->inc_counter(success ? "protorelay_outbound_delivered_total"
                               : permanent ? "protorelay_outbound_bounced_total"
                                           : "protorelay_outbound_deferred_total",
                       {}, 1);
    }

    // 计数递减和 try_pull 立即可执行（不依赖 DB 写入结果）
    int64_t prev = pending_count_.fetch_sub(1) - 1;
    if (prev < LOW_WATERMARK) try_pull();
}

void OutboundServer::stop() {
    if (!running_.exchange(false)) return;

    // 取消定时器
    auto evict = evict_timer_;
    auto retry = retry_timer_;
    scheduler_executor_->post([evict, retry] { evict->cancel(); retry->cancel(); });

    std::unique_lock<std::shared_mutex> lk(mutex_);
    for (auto& [mx, sessions] : mx_sessions_)
        for (auto& s : sessions)
            if (s) s->close();
    mx_sessions_.clear();
    LOG_SMTP_INFO("OutboundServer stopped");
}

// ── 投递入口 ────────────────────────────────────────────────
void OutboundServer::submit(std::unique_ptr<MailDeliveryTask> task, int port) {
    if (!running_.load()) { LOG_SMTP_WARN("Outbound: submit while stopped"); return; }
    std::string domain = extract_domain(task->recipient);
    if (domain.empty()) return;

    // 2026-08-27: 外部 submit 入口的 attempts 计数（on_claim_complete 是内部
    // 拉取路径，那里再 push 一次，外部 submit 直接跳过外层路由）。
    if (auto m = m_metrics.lock()) {
        m->inc_counter("protorelay_outbound_attempts_total", {{"domain", domain}}, 1);
    }

    // 端口解析顺序：static_routes[domain] > ports[0] > 25
    int target_port = (port > 0) ? port : resolve_port(domain);
    // target_host: static_routes 命中时是 IP/host（跳过 DNS），否则是 domain
    std::string target_host = resolve_target_host(domain);
    pending_count_.fetch_add(1);
    auto session = acquire_session(target_host, target_port);
    if (session) {
        // DNS 直投（无 static/default 路由 → target_host == 裸域名）→ 同步解析 MX，
        // 注入会话 failover 池，connect 逐个尝试真实邮件服务器 IP。
        if (target_host == domain) {
            auto pool = resolve_mx_pool(domain);
            if (!pool.empty()) session->set_mx_pool(std::move(pool));
        }
        session->submit(std::move(task));
        try_pull();  // 事件驱动：新任务到达后检查是否需要拉取
    } else {
        pending_count_.fetch_sub(1);
    }
}

// ── 拉取入口（水位检查 + CAS 抢占） ─────────────────────────
void OutboundServer::try_pull() {
    if (!running_.load()) return;
    // 快速路径：水位够高直接返回，无 CAS 开销
    if (pending_count_.load(std::memory_order_relaxed) >= LOW_WATERMARK)
        return;

    // CAS 抢占拉取周期
    bool expected = false;
    if (!is_pulling_.compare_exchange_strong(expected, true))
        return;

    // 预占水位：claim 返回后修正
    pending_count_.fetch_add(DEFAULT_BATCH_SIZE, std::memory_order_relaxed);
    do_claim_batch();
}

// ── 空闲回收 ────────────────────────────────────────────────
void OutboundServer::evict_idle() {
    auto now = std::chrono::steady_clock::now();
    std::unique_lock<std::shared_mutex> lk(mutex_);

    for (auto it = mx_sessions_.begin(); it != mx_sessions_.end();) {
        auto& sessions = it->second;
        sessions.erase(
            std::remove_if(sessions.begin(), sessions.end(),
                [](const SessionPtr& s) { return !s || (s->state() == SessionState::CLOSED && s->queue_size() == 0); }),
            sessions.end());

        bool all_idle = true;
        for (auto& s : sessions) {
            if (s->queue_size() > 0 || s->has_active_task()) {
                all_idle = false;
                last_active_[it->first] = now;
                break;
            }
        }

        if (all_idle && sessions.empty()) {
            last_active_.erase(it->first);
            it = mx_sessions_.erase(it);
        } else if (all_idle) {
            auto li = last_active_.find(it->first);
            auto last = (li != last_active_.end()) ? li->second : now;
            if (now - last > std::chrono::seconds(idle_timeout_sec_)) {
                for (auto& s : sessions) {
                    s->close();
                }
                sessions.clear();
                last_active_.erase(it->first);
                it = mx_sessions_.erase(it);
            } else {
                ++it;
            }
        } else {
            ++it;
        }
    }

    while (mx_sessions_.size() > static_cast<size_t>(DEFAULT_MAX_MX_ENTRIES)) {
        auto oldest = mx_sessions_.begin();
        auto oldest_t = now;
        for (auto mi = mx_sessions_.begin(); mi != mx_sessions_.end(); ++mi) {
            auto li = last_active_.find(mi->first);
            auto t = (li != last_active_.end()) ? li->second
                : std::chrono::steady_clock::time_point::min();
            if (t < oldest_t) { oldest_t = t; oldest = mi; }
        }
        for (auto& s : oldest->second) {
            s->close();
        }
        last_active_.erase(oldest->first);
        mx_sessions_.erase(oldest);
    }
}

// ── 健康检查 ────────────────────────────────────────────────
size_t OutboundServer::total_sessions() const {
    std::shared_lock<std::shared_mutex> lk(mutex_);
    size_t n = 0;
    for (auto& [mx, sessions] : mx_sessions_) n += sessions.size();
    return n;
}

bool OutboundServer::is_running() const { return running_.load(); }

// ── 配置注入 ────────────────────────────────────────────────
void OutboundServer::set_config(OutboundConfig c) { config_ = std::move(c); }
const OutboundConfig& OutboundServer::config() const { return config_; }

// ── 私有：端口/host 解析 ────────────────────────────────────
int OutboundServer::resolve_port(const std::string& domain) const {
    auto it = config_.static_routes.find(domain);
    if (it != config_.static_routes.end() && it->second.port > 0)
        return it->second.port;
    // catch-all 兜底路由：无逐域 static_route 时用 default_route.port（若配置）
    if (config_.default_route.port > 0)
        return config_.default_route.port;
    if (!config_.ports.empty()) return config_.ports[0];
    return 25;
}

std::string OutboundServer::resolve_target_host(const std::string& domain) const {
    auto it = config_.static_routes.find(domain);
    if (it != config_.static_routes.end() && !it->second.host.empty())
        return it->second.host;
    // catch-all 兜底路由：配了 default_route.host 则外投发往它（跳过 DNS MX）；默认空走 DNS
    if (!config_.default_route.host.empty())
        return config_.default_route.host;
    return domain;
}

std::vector<std::string> OutboundServer::resolve_mx_pool(const std::string& domain) const {
    std::vector<std::string> out;
    if (domain.empty()) return out;
    auto resolver = dns_resolver_;
    if (!resolver) return out;
    outbound::SyncDnsWrapper sync(*resolver);
    std::unordered_set<std::string> seen;
    auto mx_records = sync.resolve_mx(domain);
    for (const auto& mx : mx_records) {
        std::string h = mx.host;
        while (!h.empty() && h.back() == '.') h.pop_back();   // 去尾点
        if (h.empty()) continue;
        for (const auto& addr : sync.resolve_host_addresses(h)) {
            if (!addr.empty() && seen.insert(addr).second) out.push_back(addr);
        }
    }
    // RFC 5321 implicit-MX 兜底：无 MX 时把目标域名自身当邮件主机（A 记录）
    if (out.empty()) {
        for (const auto& addr : sync.resolve_host_addresses(domain)) {
            if (!addr.empty() && seen.insert(addr).second) out.push_back(addr);
        }
    }
    return out;
}

// ── 私有：DB 拉取（发送到 worker 线程池） ────────────────────
void OutboundServer::do_claim_batch() {
    auto db = shard_router_->get_db_pool(0);
    if (!db) {
        // DB 不可用，退还预占并放弃本次拉取周期
        pending_count_.fetch_sub(DEFAULT_BATCH_SIZE, std::memory_order_relaxed);
        is_pulling_.store(false);
        return;
    }

    auto weak = weak_from_this();
    worker_pool_->post([weak, db]() {
        if (auto self = weak.lock(); self && self->running_.load()) {
            auto records = self->repo_.claim_batch(*db, self->worker_id_, DEFAULT_BATCH_SIZE, 120);
            self->on_claim_complete(std::move(records));
        }
    });
}

// ── 私有：claim 回调 ────────────────────────────────────────
void OutboundServer::on_claim_complete(std::vector<OutboxRecord> records) {
    if (!running_.load()) return;
    int pulled = static_cast<int>(records.size());
    LOG_SMTP_INFO("Outbound: on_claim_complete pulled={}", pulled);

    // 修正预占水位（只会少不会多，over >= 0）
    int over = DEFAULT_BATCH_SIZE - pulled;
    if (over > 0)
        pending_count_.fetch_sub(over, std::memory_order_relaxed);

    // 无记录：退避重试，不释放 is_pulling_
    if (records.empty()) {
        schedule_retry();
        return;
    }

    // 有数据，重置退避延迟
    scheduler_executor_->post([weak = weak_from_this()] {
        if (auto self = weak.lock()) self->retry_delay_sec_ = MIN_RETRY_DELAY_SEC;
    });

    // 加载邮件并分发到 session
    auto db = shard_router_->get_db_pool(0);
    if (!db) {
        is_pulling_.store(false);
        return;
    }

    for (auto& rec : records) {
        auto mail_ptr = repo_.load_mail(*db, rec.mail_id);
        auto task = std::make_unique<MailDeliveryTask>();
        task->mail_id = rec.mail_id;
        task->record_id = rec.id;
        task->sender = rec.sender;
        task->recipient = rec.recipient;
        task->mail_ptr = std::move(mail_ptr);
        task->attempt_count = rec.attempt_count;
        task->max_attempts = rec.max_attempts;

        // 2026-08-27: 内部 claim 路径的 attempts 计数。submit() 也 push 一次
        // （外部入站用），这里是 DB outbox 拉取的入站用，两条路径互斥。
        std::string domain = extract_domain(task->recipient);
        if (auto m = m_metrics.lock()) {
            m->inc_counter("protorelay_outbound_attempts_total",
                           {{"domain", domain}}, 1);
        }

        // 直接路由到 session，不通过 public submit() 避免重复计数
        int target_port = resolve_port(domain);
        // target_host: static_routes 命中时是 IP/host（跳过 DNS），否则是 domain
        std::string target_host = resolve_target_host(domain);
        LOG_SMTP_INFO("Outbound: dispatching mail_id={} recipient={} target_host={} port={}",
                      rec.mail_id, rec.recipient, target_host, target_port);
        auto session = acquire_session(target_host, target_port);
        if (session) {
            // DNS 直投 → 解析 MX 注入会话 failover 池（同 public submit 路径）
            if (target_host == domain) {
                auto pool = resolve_mx_pool(domain);
                if (!pool.empty()) session->set_mx_pool(std::move(pool));
            }
            session->submit(std::move(task));
        }
    }

    // 水位仍低 → 链式续拉，不释放 is_pulling_
    if (pending_count_.load(std::memory_order_relaxed) < LOW_WATERMARK) {
        pending_count_.fetch_add(DEFAULT_BATCH_SIZE, std::memory_order_relaxed);
        do_claim_batch();
    } else {
        is_pulling_.store(false);
    }
}

// ── 私有：退避重试（claim 返回空时） ─────────────────────────
void OutboundServer::schedule_retry() {
    auto weak = weak_from_this();
    scheduler_executor_->post([weak] {
        auto self = weak.lock();
        if (!self || !self->running_.load()) return;
        self->retry_delay_sec_ = std::min(self->retry_delay_sec_ * 2, MAX_RETRY_DELAY_SEC);
        self->retry_timer_->expires_after(std::chrono::seconds(self->retry_delay_sec_));
        self->retry_timer_->async_wait([weak](const std::error_code& ec) {
            auto self = weak.lock();
            if (ec || !self || !self->running_.load()) return;
            self->pending_count_.fetch_add(DEFAULT_BATCH_SIZE, std::memory_order_relaxed);
            self->do_claim_batch();
        });
    });
}

void OutboundServer::schedule_evict() {
    auto weak = weak_from_this();
    scheduler_executor_->post([weak] {
        auto self = weak.lock();
        if (!self || !self->running_.load()) return;
        self->evict_timer_->expires_after(std::chrono::seconds(30));
        self->evict_timer_->async_wait([weak](const std::error_code& ec) {
            auto self = weak.lock();
            if (ec || !self || !self->running_.load()) return;
            self->evict_idle();
            self->schedule_evict();
        });
    });
}

// ── 私有：session 池管理 ────────────────────────────────────
OutboundServer::SessionPtr OutboundServer::acquire_session(const std::string& mx, int port) {
    std::unique_lock<std::shared_mutex> lk(mutex_);
    if (!running_.load()) return {};
    last_active_[mx] = std::chrono::steady_clock::now();

    auto it = mx_sessions_.find(mx);
    if (it != mx_sessions_.end()) {
        auto& sessions = it->second;
        sessions.erase(
            std::remove_if(sessions.begin(), sessions.end(),
                [](const SessionPtr& s) { return !s || (s->state() == SessionState::CLOSED && s->queue_size() == 0); }),
            sessions.end());

        SessionPtr best;
        size_t min_q = SIZE_MAX;
        for (auto& s : sessions) {
            size_t qs = s->queue_size();
            if (qs < min_q) { min_q = qs; best = s; }
        }
        if (best && min_q < 100) return best;

        if (sessions.size() < static_cast<size_t>(max_sessions_per_mx_)) {
            auto new_s = create_session(mx, port);
            sessions.push_back(new_s);
            return new_s;
        }
        return best;
    }

    auto new_s = create_session(mx, port);
    mx_sessions_[mx] = {new_s};
    return new_s;
}

OutboundServer::SessionPtr OutboundServer::create_session(const std::string& mx, int port) {
    auto transports = transports_;
    auto worker = worker_pool_;
    auto s = std::make_shared<OutboundSmtpSession>(
        [transports] { return transports->make_client_transport(); }, executors_->next_executor(),
        [worker](std::function<void()> job) { worker->post(std::move(job)); }, mx, port, helo_domain_, m_metrics);
    s->set_outbound_config(config_);
    if (completion_cb_) s->set_completion_cb(completion_cb_);
    return s;
}

std::string OutboundServer::extract_domain(const std::string& addr) {
    auto at = addr.find('@');
    if (at != std::string::npos) return addr.substr(at + 1);
    return addr;
}

} // namespace outbound
} // namespace mail_system
