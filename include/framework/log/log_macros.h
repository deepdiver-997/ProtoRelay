#ifndef PR_FRAMEWORK_LOG_LOG_MACROS_H
#define PR_FRAMEWORK_LOG_LOG_MACROS_H

// 框架日志宏：LOG_AT 唯一入口 + 框架自有模块的便利别名（2026-10-18 去枚举化）。
//
// 模块 = 字符串标签，首次使用惰性注册（logger.h），新增业务模块零框架改动：
//   LOG_AT("WEBAPI", info, "request {} done", id);
// 业务模块（SMTP/IMAP/AUTH/...）的 LOG_<MODULE>_<LEVEL> 别名在应用层
// mail_system/back/common/logger.h；本文件只保留框架自有模块的别名，
// 供框架头文件使用。
//
// LOG_LOOK_UP 分支是日志变换工具链（tools/log_transform.py）的标记形态。

#include "framework/log/logger.h"
#include "framework/log/log_pure.h"

#ifdef LOG_LOOK_UP
#define LOG_AT(mod, LEVEL, fmt, ...) (void)("LUK\001" mod "\001" #LEVEL "\001" fmt)
#else
#define _SRC_LOC pr::SourceLocation{__FILE__, __LINE__, __func__}
#define LOG_AT(mod, LEVEL, ...) pr::log(mod, pr::LogLevel::LEVEL, _SRC_LOC, __VA_ARGS__)
#endif

// ── 框架模块 ENABLE 开关（DEBUG/TRACE 的 Release 编译期消除）──
#ifndef ENABLE_SERVER_DEBUG_LOG
#define ENABLE_SERVER_DEBUG_LOG 0
#endif
#ifndef ENABLE_NETWORK_DEBUG_LOG
#define ENABLE_NETWORK_DEBUG_LOG 0
#endif
#ifndef ENABLE_DATABASE_DEBUG_LOG
#define ENABLE_DATABASE_DEBUG_LOG 0
#endif
#ifndef ENABLE_DATABASE_QUERY_DEBUG_LOG
#define ENABLE_DATABASE_QUERY_DEBUG_LOG 0
#endif
#ifndef ENABLE_SESSION_DEBUG_LOG
#define ENABLE_SESSION_DEBUG_LOG 0
#endif
#ifndef ENABLE_THREAD_POOL_DEBUG_LOG
#define ENABLE_THREAD_POOL_DEBUG_LOG 0
#endif
#ifndef ENABLE_FILE_IO_DEBUG_LOG
#define ENABLE_FILE_IO_DEBUG_LOG 0
#endif
#ifdef LOG_LOOK_UP
// ── SERVER ──
#define LOG_SERVER_TRACE(fmt,...)     __LOG_MARK("SERVER","TRACE",fmt)
#define LOG_SERVER_DEBUG(fmt,...)     __LOG_MARK("SERVER","DEBUG",fmt)
#define LOG_SERVER_INFO(fmt,...)      __LOG_MARK("SERVER","INFO",fmt)
#define LOG_SERVER_WARN(fmt,...)      __LOG_MARK("SERVER","WARN",fmt)
#define LOG_SERVER_ERROR(fmt,...)     __LOG_MARK("SERVER","ERROR",fmt)
#define LOG_SERVER_CRITICAL(fmt,...)  __LOG_MARK("SERVER","CRITICAL",fmt)
// ── NETWORK ──
#define LOG_NETWORK_TRACE(fmt,...)     __LOG_MARK("NETWORK","TRACE",fmt)
#define LOG_NETWORK_DEBUG(fmt,...)     __LOG_MARK("NETWORK","DEBUG",fmt)
#define LOG_NETWORK_INFO(fmt,...)      __LOG_MARK("NETWORK","INFO",fmt)
#define LOG_NETWORK_WARN(fmt,...)      __LOG_MARK("NETWORK","WARN",fmt)
#define LOG_NETWORK_ERROR(fmt,...)     __LOG_MARK("NETWORK","ERROR",fmt)
#define LOG_NETWORK_CRITICAL(fmt,...)  __LOG_MARK("NETWORK","CRITICAL",fmt)
// ── DATABASE ──
#define LOG_DATABASE_TRACE(fmt,...)     __LOG_MARK("DATABASE","TRACE",fmt)
#define LOG_DATABASE_DEBUG(fmt,...)     __LOG_MARK("DATABASE","DEBUG",fmt)
#define LOG_DATABASE_INFO(fmt,...)      __LOG_MARK("DATABASE","INFO",fmt)
#define LOG_DATABASE_WARN(fmt,...)      __LOG_MARK("DATABASE","WARN",fmt)
#define LOG_DATABASE_ERROR(fmt,...)     __LOG_MARK("DATABASE","ERROR",fmt)
#define LOG_DATABASE_CRITICAL(fmt,...)  __LOG_MARK("DATABASE","CRITICAL",fmt)
// ── DATABASE_QUERY ──
#define LOG_DB_QUERY_TRACE(fmt,...)     __LOG_MARK("DATABASE_QUERY","TRACE",fmt)
#define LOG_DB_QUERY_DEBUG(fmt,...)     __LOG_MARK("DATABASE_QUERY","DEBUG",fmt)
#define LOG_DB_QUERY_INFO(fmt,...)      __LOG_MARK("DATABASE_QUERY","INFO",fmt)
#define LOG_DB_QUERY_WARN(fmt,...)      __LOG_MARK("DATABASE_QUERY","WARN",fmt)
#define LOG_DB_QUERY_ERROR(fmt,...)     __LOG_MARK("DATABASE_QUERY","ERROR",fmt)
#define LOG_DB_QUERY_CRITICAL(fmt,...)  __LOG_MARK("DATABASE_QUERY","CRITICAL",fmt)
// ── SESSION ──
#define LOG_SESSION_TRACE(fmt,...)     __LOG_MARK("SESSION","TRACE",fmt)
#define LOG_SESSION_DEBUG(fmt,...)     __LOG_MARK("SESSION","DEBUG",fmt)
#define LOG_SESSION_INFO(fmt,...)      __LOG_MARK("SESSION","INFO",fmt)
#define LOG_SESSION_WARN(fmt,...)      __LOG_MARK("SESSION","WARN",fmt)
#define LOG_SESSION_ERROR(fmt,...)     __LOG_MARK("SESSION","ERROR",fmt)
#define LOG_SESSION_CRITICAL(fmt,...)  __LOG_MARK("SESSION","CRITICAL",fmt)
// ── THREAD_POOL ──
#define LOG_THREAD_POOL_TRACE(fmt,...)     __LOG_MARK("THREAD_POOL","TRACE",fmt)
#define LOG_THREAD_POOL_DEBUG(fmt,...)     __LOG_MARK("THREAD_POOL","DEBUG",fmt)
#define LOG_THREAD_POOL_INFO(fmt,...)      __LOG_MARK("THREAD_POOL","INFO",fmt)
#define LOG_THREAD_POOL_WARN(fmt,...)      __LOG_MARK("THREAD_POOL","WARN",fmt)
#define LOG_THREAD_POOL_ERROR(fmt,...)     __LOG_MARK("THREAD_POOL","ERROR",fmt)
#define LOG_THREAD_POOL_CRITICAL(fmt,...)  __LOG_MARK("THREAD_POOL","CRITICAL",fmt)
// ── FILE_IO ──
#define LOG_FILE_IO_TRACE(fmt,...)     __LOG_MARK("FILE_IO","TRACE",fmt)
#define LOG_FILE_IO_DEBUG(fmt,...)     __LOG_MARK("FILE_IO","DEBUG",fmt)
#define LOG_FILE_IO_INFO(fmt,...)      __LOG_MARK("FILE_IO","INFO",fmt)
#define LOG_FILE_IO_WARN(fmt,...)      __LOG_MARK("FILE_IO","WARN",fmt)
#define LOG_FILE_IO_ERROR(fmt,...)     __LOG_MARK("FILE_IO","ERROR",fmt)
#define LOG_FILE_IO_CRITICAL(fmt,...)  __LOG_MARK("FILE_IO","CRITICAL",fmt)
#else // 正常编译模式

// ── SERVER ──
#define LOG_SERVER_TRACE(...)  if constexpr (ENABLE_SERVER_DEBUG_LOG) { LOG_AT("SERVER", trace, __VA_ARGS__); }
#define LOG_SERVER_DEBUG(...)  if constexpr (ENABLE_SERVER_DEBUG_LOG) { LOG_AT("SERVER", debug, __VA_ARGS__); }
#define LOG_SERVER_INFO(...)   LOG_AT("SERVER", info, __VA_ARGS__)
#define LOG_SERVER_WARN(...)   LOG_AT("SERVER", warn, __VA_ARGS__)
#define LOG_SERVER_ERROR(...)  LOG_AT("SERVER", err, __VA_ARGS__)
#define LOG_SERVER_CRITICAL(...) LOG_AT("SERVER", critical, __VA_ARGS__)

// ── NETWORK ──
#define LOG_NETWORK_TRACE(...)  if constexpr (ENABLE_NETWORK_DEBUG_LOG) { LOG_AT("NETWORK", trace, __VA_ARGS__); }
#define LOG_NETWORK_DEBUG(...)  if constexpr (ENABLE_NETWORK_DEBUG_LOG) { LOG_AT("NETWORK", debug, __VA_ARGS__); }
#define LOG_NETWORK_INFO(...)   LOG_AT("NETWORK", info, __VA_ARGS__)
#define LOG_NETWORK_WARN(...)   LOG_AT("NETWORK", warn, __VA_ARGS__)
#define LOG_NETWORK_ERROR(...)  LOG_AT("NETWORK", err, __VA_ARGS__)
#define LOG_NETWORK_CRITICAL(...) LOG_AT("NETWORK", critical, __VA_ARGS__)

// ── DATABASE ──
#define LOG_DATABASE_TRACE(...)  if constexpr (ENABLE_DATABASE_DEBUG_LOG) { LOG_AT("DATABASE", trace, __VA_ARGS__); }
#define LOG_DATABASE_DEBUG(...)  if constexpr (ENABLE_DATABASE_DEBUG_LOG) { LOG_AT("DATABASE", debug, __VA_ARGS__); }
#define LOG_DATABASE_INFO(...)   LOG_AT("DATABASE", info, __VA_ARGS__)
#define LOG_DATABASE_WARN(...)   LOG_AT("DATABASE", warn, __VA_ARGS__)
#define LOG_DATABASE_ERROR(...)  LOG_AT("DATABASE", err, __VA_ARGS__)
#define LOG_DATABASE_CRITICAL(...) LOG_AT("DATABASE", critical, __VA_ARGS__)

// ── DATABASE_QUERY ──
#define LOG_DB_QUERY_TRACE(...)  if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { LOG_AT("DB_QUERY", trace, __VA_ARGS__); }
#define LOG_DB_QUERY_DEBUG(...)  if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { LOG_AT("DB_QUERY", debug, __VA_ARGS__); }
#define LOG_DB_QUERY_INFO(...)   if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { LOG_AT("DB_QUERY", info, __VA_ARGS__); }
#define LOG_DB_QUERY_WARN(...)   if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { LOG_AT("DB_QUERY", warn, __VA_ARGS__); }
#define LOG_DB_QUERY_ERROR(...)  LOG_AT("DB_QUERY", err, __VA_ARGS__)
#define LOG_DB_QUERY_CRITICAL(...) LOG_AT("DB_QUERY", critical, __VA_ARGS__)

// ── SESSION ──
#define LOG_SESSION_TRACE(...)  if constexpr (ENABLE_SESSION_DEBUG_LOG) { LOG_AT("SESSION", trace, __VA_ARGS__); }
#define LOG_SESSION_DEBUG(...)  if constexpr (ENABLE_SESSION_DEBUG_LOG) { LOG_AT("SESSION", debug, __VA_ARGS__); }
#define LOG_SESSION_INFO(...)   LOG_AT("SESSION", info, __VA_ARGS__)
#define LOG_SESSION_WARN(...)   LOG_AT("SESSION", warn, __VA_ARGS__)
#define LOG_SESSION_ERROR(...)  LOG_AT("SESSION", err, __VA_ARGS__)
#define LOG_SESSION_CRITICAL(...) LOG_AT("SESSION", critical, __VA_ARGS__)

// ── THREAD_POOL ──
#define LOG_THREAD_POOL_TRACE(...)  if constexpr (ENABLE_THREAD_POOL_DEBUG_LOG) { LOG_AT("THREAD_POOL", trace, __VA_ARGS__); }
#define LOG_THREAD_POOL_DEBUG(...)  if constexpr (ENABLE_THREAD_POOL_DEBUG_LOG) { LOG_AT("THREAD_POOL", debug, __VA_ARGS__); }
#define LOG_THREAD_POOL_INFO(...)   LOG_AT("THREAD_POOL", info, __VA_ARGS__)
#define LOG_THREAD_POOL_WARN(...)   LOG_AT("THREAD_POOL", warn, __VA_ARGS__)
#define LOG_THREAD_POOL_ERROR(...)  LOG_AT("THREAD_POOL", err, __VA_ARGS__)
#define LOG_THREAD_POOL_CRITICAL(...) LOG_AT("THREAD_POOL", critical, __VA_ARGS__)

// ── FILE_IO ──
#define LOG_FILE_IO_TRACE(...)  if constexpr (ENABLE_FILE_IO_DEBUG_LOG) { LOG_AT("FILE_IO", trace, __VA_ARGS__); }
#define LOG_FILE_IO_DEBUG(...)  if constexpr (ENABLE_FILE_IO_DEBUG_LOG) { LOG_AT("FILE_IO", debug, __VA_ARGS__); }
#define LOG_FILE_IO_INFO(...)   LOG_AT("FILE_IO", info, __VA_ARGS__)
#define LOG_FILE_IO_WARN(...)   LOG_AT("FILE_IO", warn, __VA_ARGS__)
#define LOG_FILE_IO_ERROR(...)  LOG_AT("FILE_IO", err, __VA_ARGS__)
#define LOG_FILE_IO_CRITICAL(...) LOG_AT("FILE_IO", critical, __VA_ARGS__)


#endif // LOG_LOOK_UP

#endif // PR_FRAMEWORK_LOG_LOG_MACROS_H
