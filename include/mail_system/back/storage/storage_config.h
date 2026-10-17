#ifndef MAIL_SYSTEM_STORAGE_STORAGE_CONFIG_H_SHIM_H
#define MAIL_SYSTEM_STORAGE_STORAGE_CONFIG_H_SHIM_H
// 向后兼容 shim（2026-10-18）：本组件已上移 framework/storage（namespace pr::storage）。
// 应用代码的 include 路径与 mail_system::storage 限定名保持不变。
#include "framework/storage/storage_config.h"

namespace mail_system {
namespace storage {
using pr::storage::LocalStorageConfig;
using pr::storage::S3StorageConfig;
using pr::storage::HdfsStorageConfig;
using pr::storage::DistributedStorageConfig;
using pr::storage::StorageProviderType;
using pr::storage::provider_type_from_string;
using pr::storage::StorageConfig;
} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_STORAGE_CONFIG_H_SHIM_H
