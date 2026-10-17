#ifndef PR_FRAMEWORK_LOG_LOG_MACROS_H
#define PR_FRAMEWORK_LOG_LOG_MACROS_H

// 模块化日志宏表 —— 框架层资产（2026-10-17 从 mail_system/back/common/logger.h 上移）。
//
// 为什么在框架：LogModule 枚举与 Logger 本就在 framework/log（logger.h），宏表是它们的
// 唯一消费面；此前宏表留在应用 shim 头里，迫使 framework/session_base.h、server_base.cpp、
// thread_pool 等框架文件反向 include 应用层（框架→应用 include 泄漏的主要来源）。
//
// 模块清单 = logger.h 的 pr::LogModule 枚举 + 本表三处（ENABLE 开关 / LOG_LOOK_UP /
// 正常模式）。新增模块两处各加一段即可；应用代码继续经 mail_system/back/common/logger.h
// 的 using 别名使用，无需改动。
//
// DEBUG/TRACE 在 Release 下编译期消除（ENABLE_*_DEBUG_LOG=0，可由构建系统 -D 覆盖）。

#include "framework/log/logger.h"

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
#ifndef ENABLE_SMTP_DEBUG_LOG
#define ENABLE_SMTP_DEBUG_LOG 0
#endif
#ifndef ENABLE_SMTP_DETAIL_DEBUG_LOG
#define ENABLE_SMTP_DETAIL_DEBUG_LOG 0
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
#ifndef ENABLE_AUTH_DEBUG_LOG
#define ENABLE_AUTH_DEBUG_LOG 0
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
#ifndef ENABLE_IMAP_DEBUG_LOG
#define ENABLE_IMAP_DEBUG_LOG 0
#endif
#ifndef ENABLE_IMAP_DETAIL_DEBUG_LOG
#define ENABLE_IMAP_DETAIL_DEBUG_LOG 0
#endif

// source_loc helper
#define _SRC_LOC spdlog::source_loc{__FILE__, __LINE__, SPDLOG_FUNCTION}

// ---- LOG_LOOK_UP 模式 ----
#ifdef LOG_LOOK_UP

// 每个 LOG_* 展开为 __LOG_MARK 标记，供 gcc -E 解析
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
// ── SESSION ──
#define LOG_SESSION_TRACE(fmt,...)     __LOG_MARK("SESSION","TRACE",fmt)
#define LOG_SESSION_DEBUG(fmt,...)     __LOG_MARK("SESSION","DEBUG",fmt)
#define LOG_SESSION_INFO(fmt,...)      __LOG_MARK("SESSION","INFO",fmt)
#define LOG_SESSION_WARN(fmt,...)      __LOG_MARK("SESSION","WARN",fmt)
#define LOG_SESSION_ERROR(fmt,...)     __LOG_MARK("SESSION","ERROR",fmt)
#define LOG_SESSION_CRITICAL(fmt,...)  __LOG_MARK("SESSION","CRITICAL",fmt)
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

// ==================== 模块化日志宏定义 ====================
// 每个模块提供: TRACE DEBUG INFO WARN ERROR CRITICAL 六个级别.
// DEBUG/TRACE 在 Release 下编译期消除 (ENABLE_*_DEBUG_LOG=0).

// ── SERVER ──
#define LOG_SERVER_TRACE(...)  if constexpr (ENABLE_SERVER_DEBUG_LOG) { pr::log(pr::LogModule::SERVER)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_SERVER_DEBUG(...)  if constexpr (ENABLE_SERVER_DEBUG_LOG) { pr::log(pr::LogModule::SERVER)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_SERVER_INFO(...)   pr::log(pr::LogModule::SERVER)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_SERVER_WARN(...)   pr::log(pr::LogModule::SERVER)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_SERVER_ERROR(...)  pr::log(pr::LogModule::SERVER)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_SERVER_CRITICAL(...) pr::log(pr::LogModule::SERVER)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── NETWORK ──
#define LOG_NETWORK_TRACE(...)  if constexpr (ENABLE_NETWORK_DEBUG_LOG) { pr::log(pr::LogModule::NETWORK)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_NETWORK_DEBUG(...)  if constexpr (ENABLE_NETWORK_DEBUG_LOG) { pr::log(pr::LogModule::NETWORK)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_NETWORK_INFO(...)   pr::log(pr::LogModule::NETWORK)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_NETWORK_WARN(...)   pr::log(pr::LogModule::NETWORK)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_NETWORK_ERROR(...)  pr::log(pr::LogModule::NETWORK)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_NETWORK_CRITICAL(...) pr::log(pr::LogModule::NETWORK)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── DATABASE ──
#define LOG_DATABASE_TRACE(...)  if constexpr (ENABLE_DATABASE_DEBUG_LOG) { pr::log(pr::LogModule::DATABASE)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_DATABASE_DEBUG(...)  if constexpr (ENABLE_DATABASE_DEBUG_LOG) { pr::log(pr::LogModule::DATABASE)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_DATABASE_INFO(...)   pr::log(pr::LogModule::DATABASE)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_DATABASE_WARN(...)   pr::log(pr::LogModule::DATABASE)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_DATABASE_ERROR(...)  pr::log(pr::LogModule::DATABASE)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_DATABASE_CRITICAL(...) pr::log(pr::LogModule::DATABASE)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── DATABASE_QUERY ──
#define LOG_DB_QUERY_TRACE(...)  if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { pr::log(pr::LogModule::DATABASE_QUERY)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_DB_QUERY_DEBUG(...)  if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { pr::log(pr::LogModule::DATABASE_QUERY)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_DB_QUERY_INFO(...)   if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { pr::log(pr::LogModule::DATABASE_QUERY)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__); }
#define LOG_DB_QUERY_WARN(...)   if constexpr (ENABLE_DATABASE_QUERY_DEBUG_LOG) { pr::log(pr::LogModule::DATABASE_QUERY)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__); }
#define LOG_DB_QUERY_ERROR(...)  pr::log(pr::LogModule::DATABASE_QUERY)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_DB_QUERY_CRITICAL(...) pr::log(pr::LogModule::DATABASE_QUERY)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── SMTP ──
#define LOG_SMTP_TRACE(...)  if constexpr (ENABLE_SMTP_DEBUG_LOG) { pr::log(pr::LogModule::SMTP)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_SMTP_DEBUG(...)  if constexpr (ENABLE_SMTP_DEBUG_LOG) { pr::log(pr::LogModule::SMTP)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_SMTP_INFO(...)   pr::log(pr::LogModule::SMTP)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_SMTP_WARN(...)   pr::log(pr::LogModule::SMTP)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_SMTP_ERROR(...)  pr::log(pr::LogModule::SMTP)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_SMTP_CRITICAL(...) pr::log(pr::LogModule::SMTP)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── SMTP_DETAIL ──
#define LOG_SMTP_DETAIL_TRACE(...)  if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { pr::log(pr::LogModule::SMTP_DETAIL)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_SMTP_DETAIL_DEBUG(...)  if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { pr::log(pr::LogModule::SMTP_DETAIL)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_SMTP_DETAIL_INFO(...)   if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { pr::log(pr::LogModule::SMTP_DETAIL)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__); }
#define LOG_SMTP_DETAIL_WARN(...)   if constexpr (ENABLE_SMTP_DETAIL_DEBUG_LOG) { pr::log(pr::LogModule::SMTP_DETAIL)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__); }
#define LOG_SMTP_DETAIL_ERROR(...)  pr::log(pr::LogModule::SMTP_DETAIL)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_SMTP_DETAIL_CRITICAL(...) pr::log(pr::LogModule::SMTP_DETAIL)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── SESSION ──
#define LOG_SESSION_TRACE(...)  if constexpr (ENABLE_SESSION_DEBUG_LOG) { pr::log(pr::LogModule::SESSION)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_SESSION_DEBUG(...)  if constexpr (ENABLE_SESSION_DEBUG_LOG) { pr::log(pr::LogModule::SESSION)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_SESSION_INFO(...)   pr::log(pr::LogModule::SESSION)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_SESSION_WARN(...)   pr::log(pr::LogModule::SESSION)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_SESSION_ERROR(...)  pr::log(pr::LogModule::SESSION)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_SESSION_CRITICAL(...) pr::log(pr::LogModule::SESSION)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── PERSISTENT_QUEUE ──
#define LOG_PERSISTENT_QUEUE_TRACE(...)  if constexpr (ENABLE_PERSISTENT_QUEUE_DEBUG_LOG) { pr::log(pr::LogModule::PERSISTENT_QUEUE)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_PERSISTENT_QUEUE_DEBUG(...)  if constexpr (ENABLE_PERSISTENT_QUEUE_DEBUG_LOG) { pr::log(pr::LogModule::PERSISTENT_QUEUE)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_PERSISTENT_QUEUE_INFO(...)   pr::log(pr::LogModule::PERSISTENT_QUEUE)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_PERSISTENT_QUEUE_WARN(...)   pr::log(pr::LogModule::PERSISTENT_QUEUE)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_PERSISTENT_QUEUE_ERROR(...)  pr::log(pr::LogModule::PERSISTENT_QUEUE)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_PERSISTENT_QUEUE_CRITICAL(...) pr::log(pr::LogModule::PERSISTENT_QUEUE)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── OUTBOUND ──
#define LOG_OUTBOUND_TRACE(...)  if constexpr (ENABLE_OUTBOUND_DEBUG_LOG) { pr::log(pr::LogModule::OUTBOUND)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_OUTBOUND_DEBUG(...)  if constexpr (ENABLE_OUTBOUND_DEBUG_LOG) { pr::log(pr::LogModule::OUTBOUND)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_OUTBOUND_INFO(...)   pr::log(pr::LogModule::OUTBOUND)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_OUTBOUND_WARN(...)   pr::log(pr::LogModule::OUTBOUND)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_OUTBOUND_ERROR(...)  pr::log(pr::LogModule::OUTBOUND)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_OUTBOUND_CRITICAL(...) pr::log(pr::LogModule::OUTBOUND)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── INBOUND ──
#define LOG_INBOUND_TRACE(...)  if constexpr (ENABLE_INBOUND_DEBUG_LOG) { pr::log(pr::LogModule::INBOUND)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_INBOUND_DEBUG(...)  if constexpr (ENABLE_INBOUND_DEBUG_LOG) { pr::log(pr::LogModule::INBOUND)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_INBOUND_INFO(...)   pr::log(pr::LogModule::INBOUND)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_INBOUND_WARN(...)   pr::log(pr::LogModule::INBOUND)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_INBOUND_ERROR(...)  pr::log(pr::LogModule::INBOUND)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_INBOUND_CRITICAL(...) pr::log(pr::LogModule::INBOUND)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── THREAD_POOL ──
#define LOG_THREAD_POOL_TRACE(...)  if constexpr (ENABLE_THREAD_POOL_DEBUG_LOG) { pr::log(pr::LogModule::THREAD_POOL)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_THREAD_POOL_DEBUG(...)  if constexpr (ENABLE_THREAD_POOL_DEBUG_LOG) { pr::log(pr::LogModule::THREAD_POOL)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_THREAD_POOL_INFO(...)   pr::log(pr::LogModule::THREAD_POOL)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_THREAD_POOL_WARN(...)   pr::log(pr::LogModule::THREAD_POOL)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_THREAD_POOL_ERROR(...)  pr::log(pr::LogModule::THREAD_POOL)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_THREAD_POOL_CRITICAL(...) pr::log(pr::LogModule::THREAD_POOL)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── FILE_IO ──
#define LOG_FILE_IO_TRACE(...)  if constexpr (ENABLE_FILE_IO_DEBUG_LOG) { pr::log(pr::LogModule::FILE_IO)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_FILE_IO_DEBUG(...)  if constexpr (ENABLE_FILE_IO_DEBUG_LOG) { pr::log(pr::LogModule::FILE_IO)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_FILE_IO_INFO(...)   pr::log(pr::LogModule::FILE_IO)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_FILE_IO_WARN(...)   pr::log(pr::LogModule::FILE_IO)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_FILE_IO_ERROR(...)  pr::log(pr::LogModule::FILE_IO)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_FILE_IO_CRITICAL(...) pr::log(pr::LogModule::FILE_IO)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── AUTH ──
#define LOG_AUTH_TRACE(...)  if constexpr (ENABLE_AUTH_DEBUG_LOG) { pr::log(pr::LogModule::AUTH)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_AUTH_DEBUG(...)  if constexpr (ENABLE_AUTH_DEBUG_LOG) { pr::log(pr::LogModule::AUTH)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_AUTH_INFO(...)   pr::log(pr::LogModule::AUTH)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_AUTH_WARN(...)   pr::log(pr::LogModule::AUTH)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_AUTH_ERROR(...)  pr::log(pr::LogModule::AUTH)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_AUTH_CRITICAL(...) pr::log(pr::LogModule::AUTH)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── IMAP ──
#define LOG_IMAP_TRACE(...)  if constexpr (ENABLE_IMAP_DEBUG_LOG) { pr::log(pr::LogModule::IMAP)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_IMAP_DEBUG(...)  if constexpr (ENABLE_IMAP_DEBUG_LOG) { pr::log(pr::LogModule::IMAP)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_IMAP_INFO(...)   pr::log(pr::LogModule::IMAP)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_IMAP_WARN(...)   pr::log(pr::LogModule::IMAP)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_IMAP_ERROR(...)  pr::log(pr::LogModule::IMAP)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_IMAP_CRITICAL(...) pr::log(pr::LogModule::IMAP)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

// ── IMAP_DETAIL ──
#define LOG_IMAP_DETAIL_TRACE(...)  if constexpr (ENABLE_IMAP_DETAIL_DEBUG_LOG) { pr::log(pr::LogModule::IMAP_DETAIL)->log(_SRC_LOC, spdlog::level::trace, __VA_ARGS__); }
#define LOG_IMAP_DETAIL_DEBUG(...)  if constexpr (ENABLE_IMAP_DETAIL_DEBUG_LOG) { pr::log(pr::LogModule::IMAP_DETAIL)->log(_SRC_LOC, spdlog::level::debug, __VA_ARGS__); }
#define LOG_IMAP_DETAIL_INFO(...)   pr::log(pr::LogModule::IMAP_DETAIL)->log(_SRC_LOC, spdlog::level::info, __VA_ARGS__)
#define LOG_IMAP_DETAIL_WARN(...)   pr::log(pr::LogModule::IMAP_DETAIL)->log(_SRC_LOC, spdlog::level::warn, __VA_ARGS__)
#define LOG_IMAP_DETAIL_ERROR(...)  pr::log(pr::LogModule::IMAP_DETAIL)->log(_SRC_LOC, spdlog::level::err, __VA_ARGS__)
#define LOG_IMAP_DETAIL_CRITICAL(...) pr::log(pr::LogModule::IMAP_DETAIL)->log(_SRC_LOC, spdlog::level::critical, __VA_ARGS__)

#endif // LOG_LOOK_UP


#endif // PR_FRAMEWORK_LOG_LOG_MACROS_H
