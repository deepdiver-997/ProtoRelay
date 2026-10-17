#ifndef MAIL_SYSTEM_STORAGE_S3_STORAGE_PROVIDER_H_SHIM_H
#define MAIL_SYSTEM_STORAGE_S3_STORAGE_PROVIDER_H_SHIM_H
// 向后兼容 shim（2026-10-18）：本组件已上移 framework/storage（namespace pr::storage）。
// 应用代码的 include 路径与 mail_system::storage 限定名保持不变。
#include "mail_system/back/storage/i_storage_provider.h"  // IoError/IReadStream 等流别名的历史来源
#include "framework/storage/s3_storage_provider.h"

namespace mail_system {
namespace storage {
using pr::storage::S3StorageProvider;
} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_S3_STORAGE_PROVIDER_H_SHIM_H
