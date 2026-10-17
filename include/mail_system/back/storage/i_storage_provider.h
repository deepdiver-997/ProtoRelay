#ifndef MAIL_SYSTEM_STORAGE_I_STORAGE_PROVIDER_H_SHIM_H
#define MAIL_SYSTEM_STORAGE_I_STORAGE_PROVIDER_H_SHIM_H
// 向后兼容 shim（2026-10-18）：本组件已上移 framework/storage（namespace pr::storage）。
// 应用代码的 include 路径与 mail_system::storage 限定名保持不变。
#include "framework/storage/i_storage_provider.h"

namespace mail_system {
namespace storage {
using pr::IReadStream;
using pr::IWriteStream;
using pr::BufferedReadStream;
using pr::BufferedUploadStream;
using pr::LocalFileWriteStream;
using pr::MappedReadStream;
using CommitCallback = pr::IWriteStream::CommitCallback;
using pr::IoError;
using pr::storage::IStorageProvider;
using pr::storage::AppendWriteStream;
// OpenReadCallback 等是 IStorageProvider 的成员类型，经类名访问即可
} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_I_STORAGE_PROVIDER_H_SHIM_H
