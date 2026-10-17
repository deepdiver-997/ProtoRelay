#ifndef MAIL_SYSTEM_LOGGER_H
#define MAIL_SYSTEM_LOGGER_H
// ================================================================
// 应用层日志别名表（2026-10-18 去枚举化）。
//
// 框架只提供 LOG_AT(模块标签, 级别, fmt, ...)（framework/log/log_macros.h）：
// 模块是字符串标签，首次使用惰性注册，新增业务模块零框架改动。
// 本文件只剩两类内容：
//   1. 业务模块 ENABLE_*_DEBUG_LOG 编译开关（DEBUG/TRACE 的 Release 消除）
//   2. 业务模块历史 LOG_<MODULE>_<LEVEL> 别名，统一委托 LOG_AT；
//      日志变换工具链（tools/log_transform.py）按宏名解析，level 保持大写
// 新代码两种写法皆可：直接 LOG_AT("新模块", info, ...)，或在此登记别名。
// 框架自有模块（SERVER/NETWORK/DATABASE/DB_QUERY/SESSION/THREAD_POOL/FILE_IO）
// 的别名在框架侧，见 log_macros.h。
// ================================================================
#include "framework/log/log_macros.h"

namespace mail_system {
    using pr::Logger;
    using pr::log;
    using pr::set_log_level;
    using pr::set_module_log_level;
}

#ifndef ENABLE_SMTP_DEBUG_LOG
#define ENABLE_SMTP_DEBUG_LOG 0
#endif
#ifndef ENABLE_SMTP_DETAIL_DEBUG_LOG
#define ENABLE_SMTP_DETAIL_DEBUG_LOG 0
#endif
#ifndef ENABLE_PERSISTENT_QUEUE_DEBUG_LOG
#define ENABLE_PERSISTENT_QUEUE_DEBUG_LOG 0
#endif
#ifndef ENABLE_OUTBOUND_DEBUG_LOG
#define ENABLE_OUTBOUND_DEBUG_LOG 0
#endif
#ifndef ENABLE_INBOUND_DEBUG_LOG
#define ENABLE_INBOUND_DEBUG_LOG 0
#endif
#ifndef ENABLE_AUTH_DEBUG_LOG
#define ENABLE_AUTH_DEBUG_LOG 0
#endif
#ifndef ENABLE_IMAP_DEBUG_LOG
#define ENABLE_IMAP_DEBUG_LOG 0
#endif
#ifndef ENABLE_IMAP_DETAIL_DEBUG_LOG
#define ENABLE_IMAP_DETAIL_DEBUG_LOG 0
#endif
#ifdef LOG_LOOK_UP
// ── SMTP ──
#define LOG_SMTP_TRACE(fmt,...)     __LOG_MARK("SMTP","TRACE",fmt)
#define LOG_SMTP_DEBUG(fmt,...)     __LOG_MARK("SMTP","DEBUG",fmt)
#define LOG_SMTP_INFO(fmt,...)      __LOG_MARK("SMTP","INFO",fmt)
#define LOG_SMTP_WARN(fmt,...)      __LOG_MARK("SMTP","WARN",fmt)
#define LOG_SMTP_ERROR(fmt,...)     __LOG_MARK("SMTP","ERROR",fmt)
#define LOG_SMTP_CRITICAL(fmt,...)  __LOG_MARK("SMTP","CRITICAL",fmt)
// ── SMTP_DETAIL ──
#define LOG_SMTP_DETAIL_TRACE(fmt,...)     __LOG_MARK("SMTP_DETAIL","TRACE",fmt)
#define LOG_SMTP_DETAIL_DEBUG(fmt,...)     __LOG_MARK("SMTP_DETAIL","DEBUG",fmt)
#define LOG_SMTP_DETAIL_INFO(fmt,...)      __LOG_MARK("SMTP_DETAIL","INFO",fmt)
#define LOG_SMTP_DETAIL_WARN(fmt,...)      __LOG_MARK("SMTP_DETAIL","WARN",fmt)
#define LOG_SMTP_DETAIL_ERROR(fmt,...)     __LOG_MARK("SMTP_DETAIL","ERROR",fmt)
#define LOG_SMTP_DETAIL_CRITICAL(fmt,...)  __LOG_MARK("SMTP_DETAIL","CRITICAL",fmt)
// ── PERSISTENT_QUEUE ──
#define LOG_PERSISTENT_QUEUE_TRACE(fmt,...)     __LOG_MARK("PERSISTENT_QUEUE","TRACE",fmt)
#define LOG_PERSISTENT_QUEUE_DEBUG(fmt,...)     __LOG_MARK("PERSISTENT_QUEUE","DEBUG",fmt)
#define LOG_PERSISTENT_QUEUE_INFO(fmt,...)      __LOG_MARK("PERSISTENT_QUEUE","INFO",fmt)
#define LOG_PERSISTENT_QUEUE_WARN(fmt,...)      __LOG_MARK("PERSISTENT_QUEUE","WARN",fmt)
#define LOG_PERSISTENT_QUEUE_ERROR(fmt,...)     __LOG_MARK("PERSISTENT_QUEUE","ERROR",fmt)
#define LOG_PERSISTENT_QUEUE_CRITICAL(fmt,...)  __LOG_MARK("PERSISTENT_QUEUE","CRITICAL",fmt)
// ── OUTBOUND ──
#define LOG_OUTBOUND_TRACE(fmt,...)     __LOG_MARK("OUTBOUND","TRACE",fmt)
#define LOG_OUTBOUND_DEBUG(fmt,...)     __LOG_MARK("OUTBOUND","DEBUG",fmt)
#define LOG_OUTBOUND_INFO(fmt,...)      __LOG_MARK("OUTBOUND","INFO",fmt)
#define LOG_OUTBOUND_WARN(fmt,...)      __LOG_MARK("OUTBOUND","WARN",fmt)
#define LOG_OUTBOUND_ERROR(fmt,...)     __LOG_MARK("OUTBOUND","ERROR",fmt)
#define LOG_OUTBOUND_CRITICAL(fmt,...)  __LOG_MARK("OUTBOUND","CRITICAL",fmt)
// ── INBOUND ──
#define LOG_INBOUND_TRACE(fmt,...)     __LOG_MARK("INBOUND","TRACE",fmt)
#define LOG_INBOUND_DEBUG(fmt,...)     __LOG_MARK("INBOUND","DEBUG",fmt)
#define LOG_INBOUND_INFO(fmt,...)      __LOG_MARK("INBOUND","INFO",fmt)
#define LOG_INBOUND_WARN(fmt,...)      __LOG_MARK("INBOUND","WARN",fmt)
#define LOG_INBOUND_ERROR(fmt,...)     __LOG_MARK("INBOUND","ERROR",fmt)
#define LOG_INBOUND_CRITICAL(fmt,...)  __LOG_MARK("INBOUND","CRITICAL",fmt)
// ── AUTH ──
#define LOG_AUTH_TRACE(fmt,...)     __LOG_MARK("AUTH","TRACE",fmt)
#define LOG_AUTH_DEBUG(fmt,...)     __LOG_MARK("AUTH","DEBUG",fmt)
#define LOG_AUTH_INFO(fmt,...)      __LOG_MARK("AUTH","INFO",fmt)
#define LOG_AUTH_WARN(fmt,...)      __LOG_MARK("AUTH","WARN",fmt)
#define LOG_AUTH_ERROR(fmt,...)     __LOG_MARK("AUTH","ERROR",fmt)
#define LOG_AUTH_CRITICAL(fmt,...)  __LOG_MARK("AUTH","CRITICAL",fmt)
// ── IMAP ──
#define LOG_IMAP_TRACE(fmt,...)     __LOG_MARK("IMAP","TRACE",fmt)
#define LOG_IMAP_DEBUG(fmt,...)     __LOG_MARK("IMAP","DEBUG",fmt)
#define LOG_IMAP_INFO(fmt,...)      __LOG_MARK("IMAP","INFO",fmt)
#define LOG_IMAP_WARN(fmt,...)      __LOG_MARK("IMAP","WARN",fmt)
#define LOG_IMAP_ERROR(fmt,...)     __LOG_MARK("IMAP","ERROR",fmt)
#define LOG_IMAP_CRITICAL(fmt,...)  __LOG_MARK("IMAP","CRITICAL",fmt)
// ── IMAP_DETAIL ──
#define LOG_IMAP_DETAIL_TRACE(fmt,...)     __LOG_MARK("IMAP_DETAIL","TRACE",fmt)
#define LOG_IMAP_DETAIL_DEBUG(fmt,...)     __LOG_MARK("IMAP_DETAIL","DEBUG",fmt)
#define LOG_IMAP_DETAIL_INFO(fmt,...)      __LOG_MARK("IMAP_DETAIL","INFO",fmt)
#define LOG_IMAP_DETAIL_WARN(fmt,...)      __LOG_MARK("IMAP_DETAIL","WARN",fmt)
#define LOG_IMAP_DETAIL_ERROR(fmt,...)     __LOG_MARK("IMAP_DETAIL","ERROR",fmt)
#define LOG_IMAP_DETAIL_CRITICAL(fmt,...)  __LOG_MARK("IMAP_DETAIL","CRITICAL",fmt)

#else // 正常编译模式

// ── SMTP ──
#define LOG_SMTP_TRACE(...)  if constexpr (ENABLE_SMTP_DEBUG_LOG) { LOG_AT("SMTP", trace, __VA_ARGS__); }
#define LOG_SMTP_DEBUG(...)  if constexpr (ENABLE_SMTP_DEBUG_LOG) { LOG_AT("SMTP", debug, __VA_ARGS__); }
#define LOG_SMTP_INFO(...)   LOG_AT("SMTP", info, __VA_ARGS__)
#define LOG_SMTP_WARN(...)   LOG_AT("SMTP", warn, __VA_ARGS__)
#define LOG_SMTP_ERROR(...)  LOG_AT("SMTP", err, __VA_ARGS__)
#define LOG_SMTP_CRITICAL(...) LOG_AT("SMTP", critical, __VA_ARGS__)

// ── SMTP_DETAIL ──
#define LOG_SMTP_DETAIL_TRACE(...)  if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { LOG_AT("SMTP_DETAIL", trace, __VA_ARGS__); }
#define LOG_SMTP_DETAIL_DEBUG(...)  if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { LOG_AT("SMTP_DETAIL", debug, __VA_ARGS__); }
#define LOG_SMTP_DETAIL_INFO(...)   if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { LOG_AT("SMTP_DETAIL", info, __VA_ARGS__); }
#define LOG_SMTP_DETAIL_WARN(...)   if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { LOG_AT("SMTP_DETAIL", warn, __VA_ARGS__); }
#define LOG_SMTP_DETAIL_ERROR(...)  LOG_AT("SMTP_DETAIL", err, __VA_ARGS__)
#define LOG_SMTP_DETAIL_CRITICAL(...) LOG_AT("SMTP_DETAIL", critical, __VA_ARGS__)

// ── PERSISTENT_QUEUE ──
#define LOG_PERSISTENT_QUEUE_TRACE(...)  if constexpr (ENABLE_PERSISTENT_QUEUE_DEBUG_LOG) { LOG_AT("PERSISTENT_QUEUE", trace, __VA_ARGS__); }
#define LOG_PERSISTENT_QUEUE_DEBUG(...)  if constexpr (ENABLE_PERSISTENT_QUEUE_DEBUG_LOG) { LOG_AT("PERSISTENT_QUEUE", debug, __VA_ARGS__); }
#define LOG_PERSISTENT_QUEUE_INFO(...)   LOG_AT("PERSISTENT_QUEUE", info, __VA_ARGS__)
#define LOG_PERSISTENT_QUEUE_WARN(...)   LOG_AT("PERSISTENT_QUEUE", warn, __VA_ARGS__)
#define LOG_PERSISTENT_QUEUE_ERROR(...)  LOG_AT("PERSISTENT_QUEUE", err, __VA_ARGS__)
#define LOG_PERSISTENT_QUEUE_CRITICAL(...) LOG_AT("PERSISTENT_QUEUE", critical, __VA_ARGS__)

// ── OUTBOUND ──
#define LOG_OUTBOUND_TRACE(...)  if constexpr (ENABLE_OUTBOUND_DEBUG_LOG) { LOG_AT("OUTBOUND", trace, __VA_ARGS__); }
#define LOG_OUTBOUND_DEBUG(...)  if constexpr (ENABLE_OUTBOUND_DEBUG_LOG) { LOG_AT("OUTBOUND", debug, __VA_ARGS__); }
#define LOG_OUTBOUND_INFO(...)   LOG_AT("OUTBOUND", info, __VA_ARGS__)
#define LOG_OUTBOUND_WARN(...)   LOG_AT("OUTBOUND", warn, __VA_ARGS__)
#define LOG_OUTBOUND_ERROR(...)  LOG_AT("OUTBOUND", err, __VA_ARGS__)
#define LOG_OUTBOUND_CRITICAL(...) LOG_AT("OUTBOUND", critical, __VA_ARGS__)

// ── INBOUND ──
#define LOG_INBOUND_TRACE(...)  if constexpr (ENABLE_INBOUND_DEBUG_LOG) { LOG_AT("INBOUND", trace, __VA_ARGS__); }
#define LOG_INBOUND_DEBUG(...)  if constexpr (ENABLE_INBOUND_DEBUG_LOG) { LOG_AT("INBOUND", debug, __VA_ARGS__); }
#define LOG_INBOUND_INFO(...)   LOG_AT("INBOUND", info, __VA_ARGS__)
#define LOG_INBOUND_WARN(...)   LOG_AT("INBOUND", warn, __VA_ARGS__)
#define LOG_INBOUND_ERROR(...)  LOG_AT("INBOUND", err, __VA_ARGS__)
#define LOG_INBOUND_CRITICAL(...) LOG_AT("INBOUND", critical, __VA_ARGS__)

// ── AUTH ──
#define LOG_AUTH_TRACE(...)  if constexpr (ENABLE_AUTH_DEBUG_LOG) { LOG_AT("AUTH", trace, __VA_ARGS__); }
#define LOG_AUTH_DEBUG(...)  if constexpr (ENABLE_AUTH_DEBUG_LOG) { LOG_AT("AUTH", debug, __VA_ARGS__); }
#define LOG_AUTH_INFO(...)   LOG_AT("AUTH", info, __VA_ARGS__)
#define LOG_AUTH_WARN(...)   LOG_AT("AUTH", warn, __VA_ARGS__)
#define LOG_AUTH_ERROR(...)  LOG_AT("AUTH", err, __VA_ARGS__)
#define LOG_AUTH_CRITICAL(...) LOG_AT("AUTH", critical, __VA_ARGS__)

// ── IMAP ──
#define LOG_IMAP_TRACE(...)  if constexpr (ENABLE_IMAP_DEBUG_LOG) { LOG_AT("IMAP", trace, __VA_ARGS__); }
#define LOG_IMAP_DEBUG(...)  if constexpr (ENABLE_IMAP_DEBUG_LOG) { LOG_AT("IMAP", debug, __VA_ARGS__); }
#define LOG_IMAP_INFO(...)   LOG_AT("IMAP", info, __VA_ARGS__)
#define LOG_IMAP_WARN(...)   LOG_AT("IMAP", warn, __VA_ARGS__)
#define LOG_IMAP_ERROR(...)  LOG_AT("IMAP", err, __VA_ARGS__)
#define LOG_IMAP_CRITICAL(...) LOG_AT("IMAP", critical, __VA_ARGS__)

// ── IMAP_DETAIL ──
#define LOG_IMAP_DETAIL_TRACE(...)  if constexpr (ENABLE_IMAP_DETAIL_DEBUG_LOG) { LOG_AT("IMAP_DETAIL", trace, __VA_ARGS__); }
#define LOG_IMAP_DETAIL_DEBUG(...)  if constexpr (ENABLE_IMAP_DETAIL_DEBUG_LOG) { LOG_AT("IMAP_DETAIL", debug, __VA_ARGS__); }
#define LOG_IMAP_DETAIL_INFO(...)   LOG_AT("IMAP_DETAIL", info, __VA_ARGS__)
#define LOG_IMAP_DETAIL_WARN(...)   LOG_AT("IMAP_DETAIL", warn, __VA_ARGS__)
#define LOG_IMAP_DETAIL_ERROR(...)  LOG_AT("IMAP_DETAIL", err, __VA_ARGS__)
#define LOG_IMAP_DETAIL_CRITICAL(...) LOG_AT("IMAP_DETAIL", critical, __VA_ARGS__)


#endif // LOG_LOOK_UP

#endif // MAIL_SYSTEM_LOGGER_H
