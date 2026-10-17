#ifndef MAIL_SYSTEM_BOOTSTRAP_H
#define MAIL_SYSTEM_BOOTSTRAP_H

// ================================================================
// bootstrap — 装配根（composition root）
//
// 2026-10-18 起 pr::ServerBase 是纯依赖注入：构造函数只接受现成资源，空指针
// 响亮报错。本模块是全仓库唯一认识具体 DB/存储/路由实现的地方——按配置
// 把 mysql/mariadb/null、local/s3/hdfs、三种 router 装配成抽象接口。
// （这段代码原是 pr::ServerBase 构造函数体，依赖反转问题的修复就是把"框架
// 认识所有驱动"搬出框架。）
// ================================================================

#include "framework/server_config.h"
#include "framework/thread_pool/thread_pool_base.h"
#include "mail_system/back/router/i_shard_router.h"

namespace mail_system {
namespace bootstrap {

struct Assembled {
    std::shared_ptr<pr::ThreadPoolBase> io_pool;             // 已 start
    std::shared_ptr<pr::ThreadPoolBase> worker_pool;         // 已 start
    std::shared_ptr<router::IShardRouter> shard_router;
};

struct Options {
    // 预建资源（多 server 共享同一池时传入，如 mailServer 合并入口）；
    // 留空则按 config.io_thread_count / worker_thread_count 新建并 start。
    std::shared_ptr<pr::ThreadPoolBase> io_pool;
    std::shared_ptr<pr::ThreadPoolBase> worker_pool;
};

// 按配置装配全部运行资源。任何资源构建失败（连接池创建异常、存储 ensure_ready
// 失败、router 配置非法、线程数 <= 0）→ 抛 std::runtime_error，调用方别继续。
Assembled assemble(const ServerConfig& config, const Options& opts = {});

} // namespace bootstrap
} // namespace mail_system

#endif // MAIL_SYSTEM_BOOTSTRAP_H
