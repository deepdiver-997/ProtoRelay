# Production modules. A consumer links the capabilities it actually uses.
# The object-only packaging contract intentionally retains COMMON_SRCS.
if(BUILD_OBJECT_ONLY)
    return()
endif()

function(protorelay_module target)
    add_library(${target} STATIC ${ARGN})
    target_include_directories(${target} PUBLIC "${CMAKE_SOURCE_DIR}/include")
    target_include_directories(${target} PRIVATE "${CMAKE_BINARY_DIR}/generated")
    target_compile_features(${target} PUBLIC cxx_std_17)
    target_compile_definitions(${target} PRIVATE ENABLE_INBOUND_DEDUP_CHECK=$<BOOL:${ENABLE_INBOUND_DEDUP_CHECK}>)
    target_link_libraries(${target} PUBLIC protorelay_logging nlohmann_json::nlohmann_json Threads::Threads)
    if(ENABLE_TSAN)
        target_compile_options(${target} PRIVATE -fsanitize=thread -g -O1)
        target_link_options(${target} PUBLIC -fsanitize=thread)
    endif()
    if(ENABLE_ASAN OR ENABLE_ASAN_SERVER)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined -g -O1)
        target_link_options(${target} PUBLIC -fsanitize=address,undefined)
    endif()
endfunction()

protorelay_module(protorelay_framework
    src/framework/server_base.cpp
    src/framework/session_base_inst.cpp)
protorelay_module(protorelay_readiness src/framework/net/poll_readiness_waiter.cpp)
protorelay_module(protorelay_runtime
    ${WORKER_SRCS}
    src/framework/thread_pool/io_thread_pool.cpp
    src/framework/thread_pool/io_context_registry.cpp)
target_link_libraries(protorelay_runtime PUBLIC protorelay_asio)

# Native (non-Asio) network backend: poller abstraction (kqueue on Apple/BSD,
# epoll on Linux, POSIX poll fallback), IO runtime and plain-TCP transports.
# Parallel optional module — nothing in the production services links it yet;
# the Asio pool remains the default.
set(NATIVE_NET_SRCS
    src/framework/net/native/poller.cpp
    src/framework/net/native/poller_poll.cpp
    src/framework/net/native_runtime.cpp
    src/framework/net/native_stream.cpp
    src/framework/net/native_server_transport.cpp
    src/framework/net/native_client_transport.cpp
    src/framework/net/native_tls_registry.cpp)
if(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    list(APPEND NATIVE_NET_SRCS src/framework/net/native/poller_kqueue.cpp)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    list(APPEND NATIVE_NET_SRCS src/framework/net/native/poller_epoll.cpp)
endif()
protorelay_module(protorelay_native_net ${NATIVE_NET_SRCS})
protorelay_module(protorelay_node_link
    src/framework/node/node_link.cpp src/framework/node/node_trust.cpp)
target_link_libraries(protorelay_node_link PUBLIC protorelay_native_net)

# Node link Ed25519 credentials (see include/framework/node/node_auth.h).
# Optional — node_link itself stays OpenSSL-free; without this module callers
# wire their own NodeCredentials sign/verify implementations.
if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    protorelay_module(protorelay_node_auth src/framework/node/node_auth_ed25519.cpp)
    target_link_libraries(protorelay_node_auth PUBLIC protorelay_node_link OpenSSL::Crypto)
endif()

# Native TLS extension: OpenSSL adapter over protorelay_native_net (see
# include/framework/net/native_tls.h). Optional — without this module every
# native TLS entry point keeps its fail-closed behavior, and builds without
# an OpenSSL target (e.g. contracts-only) never compile it.
if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    protorelay_module(protorelay_native_tls src/framework/net/native_tls.cpp)
    target_link_libraries(protorelay_native_tls PRIVATE protorelay_native_net OpenSSL::SSL OpenSSL::Crypto)
endif()

protorelay_module(protorelay_http
    src/framework/http/message_processor.cpp src/framework/http/http_parser.cpp
    src/framework/http/http_fsm_inst.cpp src/framework/http/http_session_inst.cpp
    src/framework/http/http_server.cpp)
target_link_libraries(protorelay_http PUBLIC protorelay_framework)
protorelay_module(protorelay_h2
    src/framework/http/codec/h2_codec.cpp src/framework/http/h2/h2_framer.cpp
    src/framework/http/h2/h2_hpack.cpp src/framework/http/h2/h2_session_inst.cpp
    src/framework/http/h2/h2_server.cpp)
target_link_libraries(protorelay_h2 PUBLIC protorelay_http)

if(NOT PROTORELAY_BUILD_MAIL)
    set(COMMON_SRCS "")
    return()
endif()

protorelay_module(protorelay_storage
    src/framework/storage/distributed_file_storage_provider.cpp
    src/framework/storage/local_file_storage_provider.cpp
    src/framework/storage/local_file_write_stream.cpp)
if(ENABLE_HDFS_WEB_STORAGE)
    target_sources(protorelay_storage PRIVATE src/framework/storage/hdfs_web_storage_provider.cpp)
endif()
if(ENABLE_S3_STORAGE)
    target_sources(protorelay_storage PRIVATE src/framework/storage/s3_storage_provider.cpp)
endif()
if(ENABLE_HDFS_WEB_STORAGE OR ENABLE_S3_STORAGE)
    target_link_libraries(protorelay_storage PRIVATE CURL::libcurl)
endif()
if(ENABLE_S3_STORAGE)
    target_link_libraries(protorelay_storage PRIVATE OpenSSL::Crypto)
endif()

# Mail object layouts are application adapters, independent of DB and runtime.
protorelay_module(protorelay_mail_storage src/mail_system/back/storage/mail_object_key_layout.cpp)
target_link_libraries(protorelay_mail_storage PUBLIC protorelay_storage)

protorelay_module(protorelay_db_mysql
    src/mail_system/back/db/mysql_pool.cpp src/mail_system/back/db/mysql_service.cpp
    src/mail_system/back/db/distributed_mysql_pool.cpp)
target_include_directories(protorelay_db_mysql SYSTEM PRIVATE ${MYSQL_INCLUDE_DIRS})
target_link_libraries(protorelay_db_mysql PRIVATE ${MYSQL_LIBRARIES})
if(HAVE_MARIADB)
    protorelay_module(protorelay_db_mariadb
        src/mail_system/back/db/mariadb_pool.cpp src/mail_system/back/db/mariadb_service.cpp)
    target_include_directories(protorelay_db_mariadb SYSTEM PRIVATE ${MARIADB_INCLUDE_DIR})
    target_link_libraries(protorelay_db_mariadb PRIVATE protorelay_readiness protorelay_db_mysql ${CMAKE_DL_LIBS})
endif()

# SQLite 可选驱动：本地文件库（轻量节点本地状态存储方向）。PRIVATE 链
# protorelay_db_mysql（SQLitePoolFactory 复用引擎无关的 MySQLPool），静态库
# 的 PRIVATE 依赖在 CMake 中仍会向消费者传播链接（与 MariaDB 模块同惯例）。
if(HAVE_SQLITE3)
    protorelay_module(protorelay_db_sqlite
        src/mail_system/back/db/sqlite_service.cpp src/mail_system/back/db/sqlite_pool.cpp)
    target_link_libraries(protorelay_db_sqlite PRIVATE protorelay_db_mysql SQLite::SQLite3)
endif()

protorelay_module(protorelay_smtp_client src/mail_system/back/smtp_client/client_session.cpp)
target_link_libraries(protorelay_smtp_client PUBLIC protorelay_runtime)

protorelay_module(protorelay_mail_core
    src/mail_system/back/cli/help_text.cpp src/mail_system/back/bootstrap.cpp
    src/mail_system/back/mailServer/mail_server_base.cpp
    src/mail_system/back/db/sql_queries.cpp src/mail_system/back/algorithm/snow.cpp
    src/mail_system/back/algorithm/smtp_utils.cpp src/mail_system/back/common/bcrypt.cpp
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
    src/mail_system/back/inbound/inbound_verifier.cpp
    src/mail_system/back/submission/mail_submission_service.cpp
    src/mail_system/back/persist_storage/persistent_queue.cpp
    src/mail_system/back/persist_storage/persist_submission.cpp
    src/mail_system/back/router/table_shard_router.cpp
    src/mail_system/back/mailServer/outbound/outbox_repository.cpp
    src/mail_system/back/mailServer/outbound/outbound_utils.cpp
    src/mail_system/back/mailServer/outbound/mx_routing_utils.cpp
    src/mail_system/back/mailServer/outbound/cares_dns_resolver.cpp
    src/mail_system/back/mailServer/outbound/outbound_server.cpp
    src/mail_system/back/mailServer/outbound/outbound_smtp_session.cpp)
target_link_libraries(protorelay_mail_core PUBLIC
    protorelay_framework protorelay_runtime protorelay_mail_storage protorelay_smtp_client
    protorelay_db_mysql protorelay_native_net)
if(TARGET protorelay_native_tls)
    target_link_libraries(protorelay_mail_core PUBLIC protorelay_native_tls)
endif()
if(HAVE_SQLITE3)
    target_compile_definitions(protorelay_mail_core PRIVATE HAVE_SQLITE3=1)
    target_link_libraries(protorelay_mail_core PRIVATE protorelay_db_sqlite)
endif()
target_include_directories(protorelay_mail_core SYSTEM PRIVATE ${CARES_INCLUDE_DIRS} ${MYSQL_INCLUDE_DIRS})
if(CARES_LIBRARY_DIRS)
    target_link_directories(protorelay_mail_core PRIVATE ${CARES_LIBRARY_DIRS})
endif()
target_link_libraries(protorelay_mail_core PRIVATE ${CARES_LIBRARIES})
if(USE_SSL)
    target_link_libraries(protorelay_mail_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
if(HAVE_MARIADB)
    target_compile_definitions(protorelay_mail_core PRIVATE HAVE_MARIADB=1)
    target_link_libraries(protorelay_mail_core PRIVATE protorelay_db_mariadb)
endif()

protorelay_module(protorelay_mail_protocols
    src/mail_system/back/mailServer/fsm/imaps/imap_utils.cpp
    src/mail_system/back/mailServer/session/smtps_session_inst.cpp
    src/mail_system/back/mailServer/session/imaps_session_inst.cpp
    src/mail_system/back/mailServer/session/pop3_session_inst.cpp
    src/mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm_inst.cpp
    src/mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm_inst.cpp
    src/mail_system/back/mailServer/fsm/pop3/traditional_pop3_fsm_inst.cpp)
target_link_libraries(protorelay_mail_protocols PUBLIC protorelay_mail_core)
set(PROTORELAY_MAIL_TARGETS
    smtpsServer imapsServer mailServer pop3Server webmailServer webmail_test
    http_session_read_test persist_submission_test smtp_persist_ack_test
    mariadb_async_test fsm_base_test fast_fsm_base_test session_base_test
    server_base_test outbound_utils_test starttls_handoff_test pipeline_deferred_test
    fsm_bench smtps_fsm_test smtps_fsm_concurrency_test imaps_fsm_test pop3_fsm_test
    pop3_timer_boundary_test outbound_smoke)
set(COMMON_SRCS "")
