# ================================================================
# 依赖查找 / 链接库列表 / COMMON_SRCS（原样拆出；变量靠 include 同作用域语义跨文件共享）
# ================================================================
# ===== 依赖查找 =====
set(Boost_NO_BOOST_CMAKE ON CACHE BOOL "" FORCE)
find_package(Boost REQUIRED)
set(SYSROOT_INCLUDE "${SYSROOT_INCLUDE}" CACHE PATH "Sysroot include path for cross-compilation")

if(USE_SSL)
    add_compile_definitions(USE_SSL)
    message(STATUS "SSL/TLS support ENABLED")
    if(BUILD_OBJECT_ONLY)
        find_package(OpenSSL)
    else()
        find_package(OpenSSL REQUIRED)
    endif()
    if(NOT OpenSSL_FOUND AND APPLE)
        if(EXISTS "/opt/homebrew/opt/openssl")
            set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl")
        elseif(EXISTS "/usr/local/opt/openssl")
            set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl")
        endif()
        find_package(OpenSSL REQUIRED)
    endif()
else()
    set(OPENSSL_FOUND FALSE)
    message(STATUS "SSL/TLS support DISABLED")
endif()

# spdlog is private to its adapter. STANDARD/NONE never look up spdlog or fmt.
if(PROTORELAY_LOG_BACKEND STREQUAL "SPDLOG")
    if(SYSROOT_INCLUDE)
        message(STATUS "Using sysroot spdlog/fmt: ${SYSROOT_INCLUDE}")
        if(NOT TARGET spdlog::spdlog)
            add_library(spdlog::spdlog INTERFACE IMPORTED)
            set_target_properties(spdlog::spdlog PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${SYSROOT_INCLUDE}")
        endif()
        add_compile_definitions(SPDLOG_FMT_EXTERNAL=OFF)
    else()
        find_package(spdlog REQUIRED)
    endif()
endif()

# nlohmann_json
find_package(nlohmann_json REQUIRED)

# curl
if(PROTORELAY_BUILD_MAIL AND (ENABLE_HDFS_WEB_STORAGE OR ENABLE_S3_STORAGE))
    find_package(CURL REQUIRED)
endif()

find_package(Threads REQUIRED)

set(LOGGING_SRCS
    src/framework/log/logger.cpp
    src/framework/log/standard_log_sink.cpp)
set(PROTORELAY_HAS_SPDLOG 0)
set(PROTORELAY_LOG_NONE 0)
if(PROTORELAY_LOG_BACKEND STREQUAL "SPDLOG")
    set(PROTORELAY_HAS_SPDLOG 1)
    list(APPEND LOGGING_SRCS src/framework/log/spdlog_log_sink.cpp)
elseif(PROTORELAY_LOG_BACKEND STREQUAL "NONE")
    set(PROTORELAY_LOG_NONE 1)
endif()
add_library(protorelay_logging STATIC ${LOGGING_SRCS})
target_include_directories(protorelay_logging PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(protorelay_logging PUBLIC Threads::Threads)
target_compile_definitions(protorelay_logging PUBLIC PROTORELAY_HAS_SPDLOG=${PROTORELAY_HAS_SPDLOG})
target_compile_definitions(protorelay_logging PRIVATE PROTORELAY_LOG_NONE=${PROTORELAY_LOG_NONE})
if(PROTORELAY_LOG_BACKEND STREQUAL "SPDLOG")
    target_link_libraries(protorelay_logging PRIVATE spdlog::spdlog)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(protorelay_logging PUBLIC stdc++fs)
endif()

# ===== MySQL / c-ares =====
if(PROTORELAY_BUILD_MAIL)
find_package(PkgConfig)
set(MYSQL_INCLUDE_DIRS "" MYSQL_LIBRARIES "")
set(CARES_INCLUDE_DIRS "" CARES_LIBRARIES "" CARES_LIBRARY_DIRS "")

if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_CARES QUIET libcares)
    if(NOT PC_CARES_FOUND)
        pkg_check_modules(PC_CARES QUIET cares)
    endif()
    if(PC_CARES_FOUND)
        set(CARES_INCLUDE_DIRS ${PC_CARES_INCLUDE_DIRS})
        set(CARES_LIBRARY_DIRS ${PC_CARES_LIBRARY_DIRS})
        set(CARES_LIBRARIES ${PC_CARES_LIBRARIES})
        message(STATUS "c-ares found via pkg-config")
    endif()
endif()

if(NOT CARES_LIBRARIES)
    find_path(CARES_INCLUDE_DIR ares.h PATHS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(CARES_LIBRARY NAMES cares c-ares PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
    if(CARES_INCLUDE_DIR AND CARES_LIBRARY)
        set(CARES_INCLUDE_DIRS ${CARES_INCLUDE_DIR})
        set(CARES_LIBRARIES ${CARES_LIBRARY})
        get_filename_component(CARES_LIBRARY_DIRS ${CARES_LIBRARY} DIRECTORY)
        message(STATUS "c-ares found: ${CARES_LIBRARY}")
    elseif(NOT BUILD_OBJECT_ONLY)
        message(FATAL_ERROR "c-ares not found. Install: brew install c-ares")
    endif()
endif()

if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_MYSQL QUIET mysqlclient libmysqlclient)
    if(PC_MYSQL_FOUND)
        set(MYSQL_INCLUDE_DIRS ${PC_MYSQL_INCLUDE_DIRS})
        set(MYSQL_LIBRARIES ${PC_MYSQL_LIBRARIES})
        message(STATUS "mysqlclient via pkg-config: ${PC_MYSQL_VERSION}")
    endif()
endif()

if(NOT MYSQL_LIBRARIES)
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(MYSQL_SEARCH /opt/homebrew/opt/mysql-client /opt/homebrew)
    elseif(APPLE)
        set(MYSQL_SEARCH /usr/local/opt/mysql /usr/local)
    else()
        set(MYSQL_SEARCH /usr /usr/local /opt/mysql)
    endif()
    find_path(MYSQL_INCLUDE_DIR mysql.h PATHS ${MYSQL_SEARCH} PATH_SUFFIXES include include/mysql)
    find_library(MYSQL_CLIENT_LIB NAMES mysqlclient libmysqlclient PATHS ${MYSQL_SEARCH} PATH_SUFFIXES lib lib64)
    if(MYSQL_INCLUDE_DIR AND MYSQL_CLIENT_LIB)
        set(MYSQL_INCLUDE_DIRS "${MYSQL_INCLUDE_DIR}")
        set(MYSQL_LIBRARIES "${MYSQL_CLIENT_LIB}")
        message(STATUS "MySQL found: ${MYSQL_CLIENT_LIB}")
    elseif(NOT BUILD_OBJECT_ONLY AND APPLE AND EXISTS "/opt/homebrew/lib/libmysqlclient.dylib")
        set(MYSQL_LIBRARIES "/opt/homebrew/lib/libmysqlclient.dylib")
        link_directories(/opt/homebrew/lib)
    elseif(NOT BUILD_OBJECT_ONLY)
        message(WARNING "mysqlclient not found. Set MYSQL_INCLUDE_DIRS / MYSQL_LIBRARIES")
    endif()
endif()

# ---- MariaDB Connector/C（可选引擎，dlopen 运行时加载 libmariadb，仅需头文件编译） ----
# 找包含 mariadb/mysql.h 的根目录（include/），使 #include <mariadb/mysql.h> 可解析
find_path(MARIADB_INCLUDE_DIR "mariadb/mysql.h"
          HINTS /opt/homebrew/opt/mariadb-connector-c /usr/local/opt/mariadb-connector-c /usr
          PATH_SUFFIXES include)
if(MARIADB_INCLUDE_DIR)
    set(HAVE_MARIADB 1)
    message(STATUS "MariaDB Connector/C headers found: ${MARIADB_INCLUDE_DIR}")
elseif(NOT BUILD_OBJECT_ONLY)
    message(STATUS "mariadb-connector-c headers not found — MariaDB 引擎不编译（achieve=mariadb 不可用）")
endif()

# ---- SQLite3（可选本地状态库驱动，直接链接；macOS 系统 SDK / Homebrew 均可） ----
# SDK 缺失时仅跳过 protorelay_db_sqlite 模块及其测试，不影响其余构建。
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    set(HAVE_SQLITE3 1)
    message(STATUS "SQLite3 found: ${SQLite3_VERSION} — protorelay_db_sqlite 启用")
else()
    set(HAVE_SQLITE3 0)
    message(STATUS "SQLite3 not found — SQLite 驱动模块不编译（本地状态库方向未启用）")
endif()

endif()

# ===== 链接库列表 =====
set(LINK_LIBRARIES Threads::Threads ${MYSQL_LIBRARIES} ${CARES_LIBRARIES} protorelay_logging)

if(PROTORELAY_BUILD_MAIL AND (ENABLE_HDFS_WEB_STORAGE OR ENABLE_S3_STORAGE))
    if(TARGET CURL::libcurl)
        list(APPEND LINK_LIBRARIES CURL::libcurl)
    elseif(CURL_LIBRARIES)
        list(APPEND LINK_LIBRARIES ${CURL_LIBRARIES})
    endif()
endif()

if(USE_SSL)
    if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
        list(APPEND LINK_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
    elseif(OPENSSL_LIBRARIES)
        list(APPEND LINK_LIBRARIES ${OPENSSL_LIBRARIES})
    else()
        message(FATAL_ERROR "USE_SSL=ON but OpenSSL not found")
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    list(APPEND LINK_LIBRARIES stdc++fs)
endif()

# ===== 公共源文件 =====
set(ASIO_NETWORK_SRCS src/framework/net/asio_connection.cpp src/framework/net/asio_server_transport.cpp src/framework/net/asio_client_transport.cpp src/framework/net/asio_readiness_waiter.cpp)
add_library(protorelay_asio STATIC ${ASIO_NETWORK_SRCS})
target_include_directories(protorelay_asio PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_include_directories(protorelay_asio SYSTEM PUBLIC ${Boost_INCLUDE_DIRS})
target_link_libraries(protorelay_asio PRIVATE protorelay_logging Threads::Threads)
if(TARGET OpenSSL::SSL)
    target_link_libraries(protorelay_asio PUBLIC OpenSSL::SSL OpenSSL::Crypto)
elseif(OPENSSL_LIBRARIES)
    target_include_directories(protorelay_asio SYSTEM PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_link_libraries(protorelay_asio PUBLIC ${OPENSSL_LIBRARIES})
endif()
list(APPEND LINK_LIBRARIES protorelay_asio)
if(ENABLE_TSAN)
    foreach(_adapter protorelay_logging protorelay_asio)
        target_compile_options(${_adapter} PRIVATE -fsanitize=thread -g -O1)
        target_link_options(${_adapter} PUBLIC -fsanitize=thread)
    endforeach()
endif()

set(COMMON_SRCS
    src/mail_system/back/cli/help_text.cpp
    src/mail_system/back/db/distributed_mysql_pool.cpp
    src/mail_system/back/db/mysql_pool.cpp
    src/mail_system/back/db/mysql_service.cpp
    src/mail_system/back/db/sql_queries.cpp
    src/framework/server_base.cpp
    src/framework/net/poll_readiness_waiter.cpp
    src/mail_system/back/bootstrap.cpp
    src/mail_system/back/mailServer/mail_server_base.cpp
    ${WORKER_SRCS}
    src/framework/thread_pool/io_thread_pool.cpp
    src/framework/thread_pool/io_context_registry.cpp
    src/mail_system/back/algorithm/snow.cpp
    src/framework/storage/distributed_file_storage_provider.cpp
    src/framework/storage/local_file_storage_provider.cpp
    src/framework/storage/local_file_write_stream.cpp
    src/mail_system/back/mailServer/outbound/outbox_repository.cpp
    src/mail_system/back/mailServer/outbound/outbound_utils.cpp
    src/mail_system/back/mailServer/outbound/mx_routing_utils.cpp
    src/mail_system/back/mailServer/outbound/cares_dns_resolver.cpp
    src/mail_system/back/mailServer/outbound/outbound_server.cpp
    src/mail_system/back/mailServer/outbound/outbound_smtp_session.cpp
    src/mail_system/back/smtp_client/client_session.cpp
    src/mail_system/back/common/bcrypt.cpp
    src/mail_system/back/common/account_policy.cpp
    src/mail_system/back/common/account_authentication.cpp
    src/mail_system/back/repository/mailbox_repository.cpp
    src/mail_system/back/repository/imap_mutation_repository.cpp
    src/mail_system/back/repository/webmail_read_repository.cpp
    src/mail_system/back/repository/imap_message_repository.cpp
    src/mail_system/back/repository/imap_folder_repository.cpp
    src/mail_system/back/common/imap_append_service.cpp
    src/mail_system/back/repository/webmail_action_repository.cpp
    src/mail_system/back/repository/web_session_repository.cpp
    src/mail_system/back/repository/pop3_mailbox_repository.cpp
    src/mail_system/back/repository/pop3_lock_repository.cpp
    src/mail_system/back/repository/account_repository.cpp
    src/mail_system/back/repository/persistence_repository.cpp
    src/mail_system/back/storage/mail_object_key_layout.cpp
    src/mail_system/back/submission/mail_submission_service.cpp
    src/mail_system/back/algorithm/smtp_utils.cpp
    src/mail_system/back/inbound/inbound_verifier.cpp
    src/mail_system/back/persist_storage/persistent_queue.cpp
    src/mail_system/back/persist_storage/persist_submission.cpp
    src/mail_system/back/router/table_shard_router.cpp
    # 显式模板实例化（避免每个翻译单元重复编译 ~4300 行模板代码）
    src/framework/session_base_inst.cpp
    src/mail_system/back/mailServer/fsm/imaps/imap_utils.cpp
    src/mail_system/back/mailServer/session/smtps_session_inst.cpp
    src/mail_system/back/mailServer/session/imaps_session_inst.cpp
    src/mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm_inst.cpp
    src/mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm_inst.cpp
    src/mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm_inst.cpp
    src/mail_system/back/mailServer/session/pop3_session_inst.cpp
)

# Cross-deployment transfers a single *_obj.dir tree. Keep logger objects in
# that tree rather than requiring a separately transferred static archive.
if(BUILD_OBJECT_ONLY)
    list(APPEND COMMON_SRCS ${LOGGING_SRCS} ${ASIO_NETWORK_SRCS})
endif()

if(ENABLE_HDFS_WEB_STORAGE)
    list(APPEND COMMON_SRCS src/framework/storage/hdfs_web_storage_provider.cpp)
endif()
if(ENABLE_S3_STORAGE)
    add_compile_definitions(ENABLE_S3_STORAGE=1)
    list(APPEND COMMON_SRCS src/framework/storage/s3_storage_provider.cpp)
endif()
if(HAVE_MARIADB)
    list(APPEND COMMON_SRCS
        src/mail_system/back/db/mariadb_service.cpp
        src/mail_system/back/db/mariadb_pool.cpp)
endif()

