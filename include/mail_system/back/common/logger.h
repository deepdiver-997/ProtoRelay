#ifndef MAIL_SYSTEM_LOGGER_H
#define MAIL_SYSTEM_LOGGER_H
#include "framework/log/log_macros.h"
// ================================================================
// 向后兼容别名 — 现有代码无需修改
//
// 宏表已上移 framework/log/log_macros.h（LogModule 枚举与 Logger 本就在
// framework/log）。本文件只剩把 pr:: 别名提升到 mail_system 命名空间，
// 使 mail_system::log(mail_system::LogModule::SERVER) 继续有效。
// ================================================================
namespace mail_system {
    using pr::LogModule;
    using pr::Logger;
    using pr::log;
    using pr::set_log_level;
    using pr::set_module_log_level;
}

#endif // MAIL_SYSTEM_LOGGER_H
