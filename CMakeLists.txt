cmake_minimum_required(VERSION 3.10)
project(ProtoRelay LANGUAGES CXX)

option(PROTORELAY_BUILD_BACKEND_CONTRACTS_ONLY "Build framework contracts without any Asio backend or service dependencies" OFF)
if(PROTORELAY_BUILD_BACKEND_CONTRACTS_ONLY)
    enable_testing()
    add_subdirectory(test/backend_contract)
    return()
endif()

set(PROTORELAY_VERSION "0.1.0-dev" CACHE STRING "ProtoRelay semantic version")

# Prefer classic FindBoost module behavior to avoid Homebrew BoostConfig
# component resolution issues on macOS.
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

# ===== 构建选项 =====
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type: Debug/Release" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

option(PROTORELAY_BUILD_MAIL "Build the mail application, database drivers and mail protocol servers" ON)
option(USE_SSL "Enable SSL/TLS support" ON)
option(ENABLE_DEBUG_LOGS "Enable debug logs for all modules" OFF)
option(BUILD_OBJECT_ONLY "Compile object files only" OFF)
option(ENABLE_INBOUND_DEDUP_CHECK "Enable inbound duplicate check" ON)
option(ENABLE_HDFS_WEB_STORAGE "Enable WebHDFS storage (requires libcurl)" ON)
option(ENABLE_S3_STORAGE "Enable S3/MinIO storage (requires libcurl, OpenSSL)" ON)
option(BUILD_TESTS "Build test and benchmark targets" ON)
set(PROTORELAY_LOG_BACKEND "SPDLOG" CACHE STRING "Default log backend: SPDLOG, STANDARD, NONE")
set_property(CACHE PROTORELAY_LOG_BACKEND PROPERTY STRINGS SPDLOG STANDARD NONE)
set(PROTORELAY_WORKER_BACKEND "STD" CACHE STRING "Worker pool backend: STD, BOOST")
set_property(CACHE PROTORELAY_WORKER_BACKEND PROPERTY STRINGS STD BOOST)
if(NOT PROTORELAY_LOG_BACKEND MATCHES "^(SPDLOG|STANDARD|NONE)$")
    message(FATAL_ERROR "Invalid PROTORELAY_LOG_BACKEND: ${PROTORELAY_LOG_BACKEND}")
endif()
if(NOT PROTORELAY_WORKER_BACKEND MATCHES "^(STD|BOOST)$")
    message(FATAL_ERROR "Invalid PROTORELAY_WORKER_BACKEND: ${PROTORELAY_WORKER_BACKEND}")
endif()
if(PROTORELAY_WORKER_BACKEND STREQUAL "BOOST")
    add_compile_definitions(PROTORELAY_WORKER_BOOST=1)
    set(WORKER_SRCS src/framework/thread_pool/boost_thread_pool.cpp)
else()
    add_compile_definitions(PROTORELAY_WORKER_BOOST=0)
    set(WORKER_SRCS src/framework/thread_pool/std_thread_pool.cpp)
endif()
option(ENABLE_FUZZING "Build libFuzzer harnesses, e.g. mime_fuzz (clang only)" OFF)
option(ENABLE_TSAN "Build FSM test targets with ThreadSanitizer (-fsanitize=thread -g -O1)" OFF)
option(ENABLE_ASAN "Build unit test targets with AddressSanitizer+UBSan (-fsanitize=address,undefined -g -O1)" OFF)
option(ENABLE_ASAN_SERVER "Build server binaries (smtps/imaps/mailServer) with ASan+UBSan — 线上内存问题诊断构建" OFF)
option(ENABLE_COVERAGE "Build with coverage instrumentation (clang llvm-cov / gcc lcov)" OFF)

# ===== Feature flags — 必须始终定义为 0/1（help_text.cpp 等 #if 使用）=====
if(USE_SSL)
    set(PROTORELAY_FEATURE_SSL 1)
else()
    set(PROTORELAY_FEATURE_SSL 0)
endif()
if(ENABLE_INBOUND_DEDUP_CHECK)
    set(PROTORELAY_FEATURE_INBOUND_DEDUP 1)
else()
    set(PROTORELAY_FEATURE_INBOUND_DEDUP 0)
endif()
if(BUILD_OBJECT_ONLY)
    set(PROTORELAY_FEATURE_OBJECT_ONLY 1)
else()
    set(PROTORELAY_FEATURE_OBJECT_ONLY 0)
endif()
if(ENABLE_HDFS_WEB_STORAGE)
    set(PROTORELAY_FEATURE_HDFS_WEB 1)
    add_compile_definitions(PROTORELAY_ENABLE_HDFS_WEB_STORAGE=1)
else()
    set(PROTORELAY_FEATURE_HDFS_WEB 0)
    add_compile_definitions(PROTORELAY_ENABLE_HDFS_WEB_STORAGE=0)
endif()

# Build info
string(TIMESTAMP PROTORELAY_BUILD_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
set(PROTORELAY_BUILD_TARGET "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(PROTORELAY_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE PROTORELAY_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PROTORELAY_GIT_COMMIT)
    set(PROTORELAY_GIT_COMMIT "unknown")
endif()
configure_file("${CMAKE_SOURCE_DIR}/include/mail_system/back/version/build_info.h.in"
               "${CMAKE_BINARY_DIR}/generated/build_info.h" @ONLY)

if(BUILD_OBJECT_ONLY)
    add_compile_definitions(FMT_HEADER_ONLY=1)
endif()

# ===== C++ 标准 =====
# 代码仅使用 C++17 特性（if constexpr / std::optional / std::filesystem 等），
# 未使用任何 C++20 特性。为降低编译器版本要求，标准定为 C++17。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ===== 编译 flags =====
set(CMAKE_CXX_FLAGS_DEBUG   "-O0 -g -DDEBUG -fno-omit-frame-pointer")
set(CMAKE_C_FLAGS_DEBUG     "-O0 -g -DDEBUG -fno-omit-frame-pointer")
if(NOT DEFINED CMAKE_CXX_FLAGS_RELEASE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native")
endif()
if(NOT DEFINED CMAKE_C_FLAGS_RELEASE)
    set(CMAKE_C_FLAGS_RELEASE   "-O3 -DNDEBUG -march=native")
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fdiagnostics-color=auto)
endif()

# ===== 覆盖率 =====
# ENABLE_COVERAGE=ON 时打桩。clang 系产出 .profraw（llvm-profdata/llvm-cov 分析），
# gcc 产出 .gcda（lcov/genhtml 分析）。脚本见 test/scripts/coverage.sh。
# 用 flags 而非 add_link_options（后者要求 CMake 3.13+，本项目最低 3.10）。
if(ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        set(CMAKE_CXX_FLAGS   "${CMAKE_CXX_FLAGS} -fprofile-instr-generate -fcoverage-mapping -g -O0")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fprofile-instr-generate")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        # -fprofile-update=atomic：单测多线程（线程池/mock executor）并发更新
        # gcov 计数器，非原子读改写会竞态出负计数，geninfo 直接报错（CI coverage job）
        set(CMAKE_CXX_FLAGS   "${CMAKE_CXX_FLAGS} --coverage -fprofile-update=atomic -g -O0")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    else()
        message(WARNING "ENABLE_COVERAGE unsupported for compiler ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

# ===== 日志级别 =====
if(ENABLE_DEBUG_LOGS)
    set(LOG_DEF 1)
else()
    set(LOG_DEF 0)
endif()
add_compile_definitions(
    ENABLE_SERVER_DEBUG_LOG=${LOG_DEF}
    ENABLE_NETWORK_DEBUG_LOG=${LOG_DEF}
    ENABLE_DATABASE_DEBUG_LOG=${LOG_DEF}
    ENABLE_DATABASE_QUERY_DEBUG_LOG=${LOG_DEF}
    ENABLE_SMTP_DEBUG_LOG=${LOG_DEF}
    ENABLE_SMTP_DETAIL_DEBUG_LOG=${LOG_DEF}
    ENABLE_SESSION_DEBUG_LOG=${LOG_DEF}
    ENABLE_THREAD_POOL_DEBUG_LOG=${LOG_DEF}
    ENABLE_FILE_IO_DEBUG_LOG=${LOG_DEF}
    ENABLE_AUTH_DEBUG_LOG=${LOG_DEF}
)

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

# ===== 输出目录 =====
set(TEST_OUTPUT_DIR "${CMAKE_BINARY_DIR}")
file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}")

# ================================================================
# setup_mail_target — 为 target 注入公共 include/lib
# ================================================================
function(setup_mail_target NAME)
    target_include_directories(${NAME} PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_include_directories(${NAME} PRIVATE "${CMAKE_BINARY_DIR}/generated")
    target_include_directories(${NAME} PRIVATE SYSTEM ${nlohmann_json_INCLUDE_DIRS})
    target_include_directories(${NAME} PRIVATE SYSTEM ${Boost_INCLUDE_DIRS})
    target_compile_features(${NAME} PRIVATE cxx_std_17)
    target_compile_definitions(${NAME} PRIVATE
        PROTORELAY_HAS_SPDLOG=${PROTORELAY_HAS_SPDLOG}
        PROTORELAY_LOG_NONE=${PROTORELAY_LOG_NONE})

    if(SYSROOT_INCLUDE)
        target_include_directories(${NAME} BEFORE PRIVATE SYSTEM ${SYSROOT_INCLUDE})
    endif()
    if(BUILD_OBJECT_ONLY AND MYSQL_INCLUDE_DIRS)
        target_include_directories(${NAME} PRIVATE SYSTEM ${MYSQL_INCLUDE_DIRS})
    endif()
    if(BUILD_OBJECT_ONLY AND MARIADB_INCLUDE_DIR)
        target_include_directories(${NAME} PRIVATE SYSTEM ${MARIADB_INCLUDE_DIR})
        target_compile_definitions(${NAME} PRIVATE HAVE_MARIADB=1)
    endif()
    if(BUILD_OBJECT_ONLY AND CARES_INCLUDE_DIRS)
        target_include_directories(${NAME} PRIVATE SYSTEM ${CARES_INCLUDE_DIRS})
    endif()
    if(CARES_LIBRARY_DIRS AND NOT BUILD_OBJECT_ONLY)
        target_link_directories(${NAME} PRIVATE ${CARES_LIBRARY_DIRS})
    endif()
    if(USE_SSL)
        if(DEFINED OPENSSL_INCLUDE_DIRS AND OPENSSL_INCLUDE_DIRS)
            target_include_directories(${NAME} PRIVATE SYSTEM ${OPENSSL_INCLUDE_DIRS})
        elseif(DEFINED OPENSSL_INCLUDE_DIR AND OPENSSL_INCLUDE_DIR)
            target_include_directories(${NAME} PRIVATE SYSTEM ${OPENSSL_INCLUDE_DIR})
        endif()
    endif()
    if(ENABLE_HDFS_WEB_STORAGE AND CURL_FOUND)
        target_include_directories(${NAME} PRIVATE SYSTEM ${CURL_INCLUDE_DIRS})
    endif()
    if(NOT BUILD_OBJECT_ONLY)
        target_link_libraries(${NAME} PRIVATE protorelay_logging Threads::Threads nlohmann_json::nlohmann_json)
        if(USE_SSL)
            target_link_libraries(${NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        endif()
        if(NAME IN_LIST PROTORELAY_MAIL_TARGETS)
            target_link_libraries(${NAME} PRIVATE protorelay_mail_protocols)
        elseif(NAME STREQUAL "smtp_client_test" OR NAME STREQUAL "smtp_submitter_test")
            target_link_libraries(${NAME} PRIVATE protorelay_smtp_client)
        elseif(NAME STREQUAL "thread_pool_test")
            target_link_libraries(${NAME} PRIVATE protorelay_runtime)
        endif()
    endif()
    if(ENABLE_INBOUND_DEDUP_CHECK)
        target_compile_definitions(${NAME} PRIVATE ENABLE_INBOUND_DEDUP_CHECK=1)
    else()
        target_compile_definitions(${NAME} PRIVATE ENABLE_INBOUND_DEDUP_CHECK=0)
    endif()
endfunction()

include(cmake/ProtoRelayModules.cmake)
if(NOT PROTORELAY_BUILD_MAIL)
    if(BUILD_OBJECT_ONLY)
        message(FATAL_ERROR "PROTORELAY_BUILD_MAIL=OFF requires a host library/executable build")
    endif()
    add_executable(webServer test/server/web_server_main.cpp)
    target_link_libraries(webServer PRIVATE protorelay_http protorelay_runtime)
    add_executable(h2web test/server/h2web_main.cpp)
    target_link_libraries(h2web PRIVATE protorelay_h2 protorelay_runtime)
    if(BUILD_TESTS)
        enable_testing()
        foreach(_test logger_test std_thread_pool_test connection_interface_test server_transport_test metrics_core_test)
            add_executable(${_test} test/unit/${_test}.cpp)
            target_include_directories(${_test} PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
            target_link_libraries(${_test} PRIVATE protorelay_framework protorelay_runtime)
            add_test(NAME ${_test} COMMAND ${_test})
            set_tests_properties(${_test} PROPERTIES TIMEOUT 20)
        endforeach()
        # Native (non-Asio) network backend: builds and runs in this reduced
        # configuration too — it must not depend on Boost/OpenSSL/spdlog.
        foreach(_test native_poller_test native_runtime_test native_transport_test)
            add_executable(${_test} test/unit/${_test}.cpp)
            target_include_directories(${_test} PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
            target_link_libraries(${_test} PRIVATE protorelay_native_net)
            add_test(NAME ${_test} COMMAND ${_test})
            set_tests_properties(${_test} PROPERTIES TIMEOUT 120)
        endforeach()
        add_custom_target(unit_tests DEPENDS logger_test std_thread_pool_test connection_interface_test server_transport_test metrics_core_test
            native_poller_test native_runtime_test native_transport_test)
    endif()
    add_test(NAME dependency_boundaries COMMAND ${CMAKE_COMMAND} -DPR_SOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/test/backend_contract/check_dependency_boundaries.cmake)
    add_test(NAME object_tree_hygiene COMMAND ${CMAKE_COMMAND} -DPR_SOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/test/backend_contract/check_object_tree_hygiene.cmake)
    return()
endif()

# ================================================================
# smtpsServer — SMTP + IMAP 合并入口
# ================================================================
if(BUILD_OBJECT_ONLY)
    add_library(smtpsServer_obj OBJECT
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/server/smtps_test.cpp
    )
    setup_mail_target(smtpsServer_obj)
    add_library(imapsServer_obj OBJECT test/server/imaps_test.cpp)
    setup_mail_target(imapsServer_obj)
    add_library(mailServer_obj OBJECT test/server/mail_server_combined.cpp)
    setup_mail_target(mailServer_obj)
    add_library(webServer_obj OBJECT
        ${COMMON_SRCS}
        src/framework/http/message_processor.cpp
        src/framework/http/http_parser.cpp
        src/framework/http/http_fsm_inst.cpp
        src/framework/http/http_session_inst.cpp
        src/framework/http/http_server.cpp
        test/server/web_server_main.cpp
    )
    setup_mail_target(webServer_obj)
    add_library(webmailServer_obj OBJECT
        ${COMMON_SRCS}
        src/framework/http/message_processor.cpp
        src/framework/http/http_parser.cpp
        src/framework/http/http_fsm_inst.cpp
        src/framework/http/http_session_inst.cpp
        src/framework/http/http_server.cpp
        src/mail_system/back/webmail/webmail_server.cpp
        src/mail_system/back/webmail/api/auth_api.cpp
        src/mail_system/back/webmail/api/mail_api.cpp
        src/mail_system/back/webmail/api/compose_api.cpp
        src/mail_system/back/webmail/api/actions_api.cpp
        src/mail_system/back/webmail/mail_actions.cpp
        src/mail_system/back/webmail/smtp_submitter.cpp
        src/mail_system/back/webmail/session_store.cpp
        src/mail_system/back/webmail/direct_submitter.cpp
        src/mail_system/back/common/mime_composer.cpp
        test/server/webmail_main.cpp
    )
    setup_mail_target(webmailServer_obj)
else()
    add_executable(smtpsServer
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/server/smtps_test.cpp
    )
    setup_mail_target(smtpsServer)

    add_executable(imapsServer
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/server/imaps_test.cpp
    )
    setup_mail_target(imapsServer)

    add_executable(mailServer
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/server/mail_server_combined.cpp
    )
    setup_mail_target(mailServer)

    add_executable(pop3Server
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/pop3/pop3_server.cpp
        test/server/pop3_test.cpp
    )
    setup_mail_target(pop3Server)

    add_executable(webServer test/server/web_server_main.cpp)
    target_link_libraries(webServer PRIVATE protorelay_http protorelay_runtime)
    add_executable(h2web test/server/h2web_main.cpp)
    target_link_libraries(h2web PRIVATE protorelay_h2 protorelay_runtime)

    # webmailServer — 第四协议前端（HTTP/JSON API + 静态 SPA），webmail MVP
    add_executable(webmailServer
        ${COMMON_SRCS}
        src/framework/http/message_processor.cpp
        src/framework/http/http_parser.cpp
        src/framework/http/http_fsm_inst.cpp
        src/framework/http/http_session_inst.cpp
        src/framework/http/http_server.cpp
        src/mail_system/back/webmail/webmail_server.cpp
        src/mail_system/back/webmail/api/auth_api.cpp
        src/mail_system/back/webmail/api/mail_api.cpp
        src/mail_system/back/webmail/api/compose_api.cpp
        src/mail_system/back/webmail/api/actions_api.cpp
        src/mail_system/back/webmail/mail_actions.cpp
        src/mail_system/back/webmail/smtp_submitter.cpp
        src/mail_system/back/webmail/session_store.cpp
        src/mail_system/back/webmail/direct_submitter.cpp
        src/mail_system/back/common/mime_composer.cpp
        test/server/webmail_main.cpp
    )
    setup_mail_target(webmailServer)
endif()

# ================================================================
# ENABLE_ASAN_SERVER — 邮件服务器二进制的 ASan+UBSan 插桩（线上内存问题诊断）
# 与单测用的 ENABLE_ASAN 互相独立；object-only 交叉部署时只影响 *_obj 的编译，
# 链接侧的 -fsanitize 由 link.sh 的 LINK_EXTRA_FLAGS 提供（见 deploy.sh）。
# ================================================================
if(ENABLE_ASAN_SERVER)
    foreach(_t smtpsServer_obj imapsServer_obj mailServer_obj webServer_obj
               smtpsServer imapsServer mailServer pop3Server webServer)
        if(TARGET ${_t})
            target_compile_options(${_t} PRIVATE
                -fsanitize=address,undefined
                -fno-sanitize=vptr
                -fno-sanitize-recover=undefined
                -fno-omit-frame-pointer
                -O1)
            # 注意：这里不写 -g/-g1，调试级别由 build.sh 的 Asan profile
            # 通过 CMAKE_*_FLAGS_RELEASE 统一给（-g1，避免小内存服务器链接 OOM）
            get_target_property(_t_type ${_t} TYPE)
            if(NOT _t_type STREQUAL "OBJECT_LIBRARY")
                target_link_options(${_t} PRIVATE -fsanitize=address,undefined)
            endif()
            unset(_t_type)
        endif()
    endforeach()
    message(STATUS "ENABLE_ASAN_SERVER=ON: server binaries instrumented with ASan+UBSan (-O1)")
endif()

# ================================================================
# 测试 / Benchmark（BUILD_TESTS=OFF 时跳过）
# ================================================================
if(NOT BUILD_OBJECT_ONLY AND BUILD_TESTS)
    add_executable(test_inbound_verifier
        test/unit/test_inbound_verifier.cpp
        src/mail_system/back/inbound/inbound_verifier.cpp
        src/mail_system/back/algorithm/smtp_utils.cpp   # AccountPolicy 边界判定用的 to_lower/trim
    )
    setup_mail_target(test_inbound_verifier)
    target_include_directories(test_inbound_verifier PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")

    add_executable(sql_queries_test
        test/unit/sql_queries_test.cpp
        src/mail_system/back/db/sql_queries.cpp
    )
    setup_mail_target(sql_queries_test)

    add_executable(line_folder_test
        test/unit/line_folder_test.cpp
    )
    setup_mail_target(line_folder_test)

    add_executable(auth_cache_test
        test/unit/auth_cache_test.cpp
    )
    setup_mail_target(auth_cache_test)

    add_executable(mime_parser_test
        test/unit/mime_parser_test.cpp
        src/mail_system/back/mailServer/fsm/imaps/imap_utils.cpp
    )
    target_include_directories(mime_parser_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(mime_parser_test)
    # 测试直接 include traditional_imaps_fsm.tpp，模板在本 TU 实例化；模板体里
    # 的 build_mail_body_key 定义在 protorelay_mail_storage，必须显式链接
    # （COMMON_SRCS 路径的目标自带该 .cpp，不走那条路的目标要补这条依赖）。
    target_link_libraries(mime_parser_test PRIVATE protorelay_mail_storage)

    add_executable(mail_body_writer_test test/unit/mail_body_writer_test.cpp)
    target_include_directories(mail_body_writer_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(mail_body_writer_test)
    target_link_libraries(mail_body_writer_test PRIVATE protorelay_mail_storage)

    add_executable(buffered_upload_stream_test
        test/unit/buffered_upload_stream_test.cpp
    )
    target_include_directories(buffered_upload_stream_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(buffered_upload_stream_test)

    # web_server 纯函数层单测：MessageProcessor / h2_framer / h2_hpack / h2_codec
    add_executable(h2_web_test
        test/unit/h2_web_test.cpp
        src/framework/http/message_processor.cpp
        src/framework/http/http_parser.cpp
        src/framework/http/h2/h2_framer.cpp
        src/framework/http/h2/h2_hpack.cpp
        src/framework/http/codec/h2_codec.cpp
    )
    setup_mail_target(h2_web_test)

    add_executable(io_error_test
        test/unit/io_error_test.cpp
    )
    target_include_directories(io_error_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(io_error_test)

    add_executable(async_storage_provider_test
        test/unit/async_storage_provider_test.cpp
    )
    target_include_directories(async_storage_provider_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(async_storage_provider_test)
    target_link_libraries(async_storage_provider_test PRIVATE protorelay_mail_storage)

    # ---- framework 层单元测试 ----
    # 纯逻辑 / 文件 IO / 线程池：只依赖 framework 头 + 对应 .cpp
    add_executable(intrusion_detector_test test/unit/intrusion_detector_test.cpp)
    target_include_directories(intrusion_detector_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(intrusion_detector_test)

    add_executable(server_config_test test/unit/server_config_test.cpp)
    target_include_directories(server_config_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(server_config_test)

    add_executable(mapped_file_test test/unit/mapped_file_test.cpp)
    target_include_directories(mapped_file_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(mapped_file_test)

    add_executable(bcrypt_test test/unit/bcrypt_test.cpp src/mail_system/back/common/bcrypt.cpp)
    target_include_directories(bcrypt_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(bcrypt_test)

    add_executable(password_rehash_test
        test/unit/password_rehash_test.cpp
        src/mail_system/back/common/bcrypt.cpp
        src/mail_system/back/db/sql_queries.cpp
    )
    target_include_directories(password_rehash_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(password_rehash_test)

    add_executable(safe_parse_test test/unit/safe_parse_test.cpp)
    setup_mail_target(safe_parse_test)

    # 本域发件人边界纯函数（AccountPolicy）单测：判定是头文件内联实现；
    # 另链 smtp_utils.cpp 提供 to_lower/trim（判定内部使用的归一化工具）
    add_executable(account_policy_boundary_test
        test/unit/account_policy_boundary_test.cpp
        src/mail_system/back/algorithm/smtp_utils.cpp
    )
    target_include_directories(account_policy_boundary_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(account_policy_boundary_test)

    add_executable(webmail_test
        ${COMMON_SRCS}
        src/mail_system/back/webmail/api/auth_api.cpp
        src/mail_system/back/webmail/api/mail_api.cpp
        src/mail_system/back/webmail/api/compose_api.cpp
        src/mail_system/back/webmail/api/actions_api.cpp
        src/mail_system/back/webmail/mail_actions.cpp
        src/mail_system/back/webmail/smtp_submitter.cpp
        src/mail_system/back/webmail/session_store.cpp
        src/mail_system/back/webmail/direct_submitter.cpp
        src/mail_system/back/common/mime_composer.cpp
        test/unit/webmail_test.cpp
        test/unit/webmail_submission_test.cpp
    )
    target_include_directories(webmail_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(webmail_test)

    add_executable(http_session_read_test
        ${COMMON_SRCS}
        src/framework/http/http_parser.cpp
        src/framework/http/message_processor.cpp
        test/unit/http_session_read_test.cpp
    )
    target_include_directories(http_session_read_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(http_session_read_test)

    add_executable(persist_submission_test ${COMMON_SRCS} test/unit/persist_submission_test.cpp)
    target_include_directories(persist_submission_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(persist_submission_test)

    add_executable(smtp_persist_ack_test ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        test/unit/smtp_persist_ack_test.cpp)
    target_include_directories(smtp_persist_ack_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(smtp_persist_ack_test)

    add_executable(smtp_client_test test/unit/smtp_client_test.cpp)
    setup_mail_target(smtp_client_test)

    add_executable(smtp_submitter_test
        src/mail_system/back/common/mime_composer.cpp
        src/mail_system/back/webmail/smtp_submitter.cpp
        test/unit/smtp_submitter_test.cpp
    )
    setup_mail_target(smtp_submitter_test)

    add_executable(thread_pool_test
        src/framework/thread_pool/io_thread_pool.cpp
        src/framework/thread_pool/io_context_registry.cpp
        ${WORKER_SRCS}
        test/unit/thread_pool_test.cpp
    )
    target_include_directories(thread_pool_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(thread_pool_test)

    add_executable(readiness_waiter_test test/unit/readiness_waiter_test.cpp)
    target_include_directories(readiness_waiter_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(readiness_waiter_test)
    target_link_libraries(readiness_waiter_test PRIVATE protorelay_readiness protorelay_asio)
    add_executable(async_db_adapter_test test/unit/async_db_adapter_test.cpp)
    target_include_directories(async_db_adapter_test PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/test/unit")
    target_link_libraries(async_db_adapter_test PRIVATE Threads::Threads)

    add_executable(account_authentication_test test/unit/account_authentication_test.cpp)
    target_include_directories(account_authentication_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(account_authentication_test)
    target_link_libraries(account_authentication_test PRIVATE protorelay_mail_core)
    add_executable(mailbox_repository_test
        src/mail_system/back/repository/mailbox_repository.cpp
        src/mail_system/back/db/sql_queries.cpp test/unit/mailbox_repository_test.cpp)
    target_include_directories(mailbox_repository_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(mailbox_repository_test)

    foreach(_repository imap_mutation_repository webmail_read_repository)
        add_executable(${_repository}_test
            src/mail_system/back/repository/${_repository}.cpp
            src/mail_system/back/db/sql_queries.cpp
            test/unit/${_repository}_test.cpp)
        target_include_directories(${_repository}_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
        setup_mail_target(${_repository}_test)
    endforeach()

    foreach(_completion imap_completion_repository webmail_persistence_repository pop3_repository account_repository persistence_repository persistent_queue_contract)
        add_executable(${_completion}_test test/unit/${_completion}_test.cpp)
        target_include_directories(${_completion}_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
        setup_mail_target(${_completion}_test)
        target_link_libraries(${_completion}_test PRIVATE protorelay_mail_core)
    endforeach()

    add_executable(mail_object_key_layout_test test/unit/mail_object_key_layout_test.cpp)
    target_include_directories(mail_object_key_layout_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(mail_object_key_layout_test)
    target_link_libraries(mail_object_key_layout_test PRIVATE protorelay_mail_storage)

    add_executable(smtp_client_contract_test
        src/mail_system/back/smtp_client/client_session.cpp test/unit/smtp_client_contract_test.cpp)
    target_include_directories(smtp_client_contract_test PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/test/unit")
    target_link_libraries(smtp_client_contract_test PRIVATE protorelay_logging OpenSSL::Crypto nlohmann_json::nlohmann_json)
    add_executable(storage_boundary_test test/unit/storage_boundary_test.cpp)
    target_include_directories(storage_boundary_test PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/test/unit")
    target_link_libraries(storage_boundary_test PRIVATE Threads::Threads)

    add_executable(logger_test test/unit/logger_test.cpp)
    target_link_libraries(logger_test PRIVATE protorelay_logging)
    add_test(NAME logger_test COMMAND logger_test)

    # This target intentionally has no Boost include/library requirements.
    add_executable(std_thread_pool_test
        src/framework/thread_pool/std_thread_pool.cpp test/unit/std_thread_pool_test.cpp)
    target_link_libraries(std_thread_pool_test PRIVATE protorelay_logging)
    add_test(NAME std_thread_pool_test COMMAND std_thread_pool_test)

    add_executable(connection_interface_test test/unit/connection_interface_test.cpp)
    target_include_directories(connection_interface_test PRIVATE "${CMAKE_SOURCE_DIR}/include")
    add_test(NAME connection_interface_test COMMAND connection_interface_test)

    # Full server/metrics interface exercised without linking any Asio backend.
    add_executable(server_transport_test src/framework/server_base.cpp test/unit/server_transport_test.cpp)
    target_include_directories(server_transport_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    target_link_libraries(server_transport_test PRIVATE protorelay_logging nlohmann_json::nlohmann_json)
    add_test(NAME server_transport_test COMMAND server_transport_test)
    set_tests_properties(server_transport_test PROPERTIES TIMEOUT 20)

    # Native (non-Asio) network backend tests: parallel optional module, the
    # Asio production wiring is untouched. Registered like every other unit
    # test via the catch-all ctest scan below.
    foreach(_test native_poller_test native_runtime_test native_transport_test)
        add_executable(${_test} test/unit/${_test}.cpp)
        target_include_directories(${_test} PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
        target_link_libraries(${_test} PRIVATE protorelay_native_net)
    endforeach()

    add_executable(asio_executor_test test/unit/asio_executor_test.cpp)
    target_include_directories(asio_executor_test PRIVATE "${CMAKE_SOURCE_DIR}/include" SYSTEM ${Boost_INCLUDE_DIRS})
    target_link_libraries(asio_executor_test PRIVATE Threads::Threads protorelay_asio)
    add_test(NAME asio_executor_test COMMAND asio_executor_test)
    add_executable(asio_error_test test/unit/asio_error_test.cpp)
    target_include_directories(asio_error_test PRIVATE "${CMAKE_SOURCE_DIR}/include" SYSTEM ${Boost_INCLUDE_DIRS})
    target_link_libraries(asio_error_test PRIVATE Threads::Threads)
    add_test(NAME asio_error_test COMMAND asio_error_test)
    add_executable(asio_server_transport_test
        src/framework/thread_pool/io_thread_pool.cpp src/framework/thread_pool/io_context_registry.cpp
        test/unit/asio_server_transport_test.cpp)
    target_link_libraries(asio_server_transport_test PRIVATE protorelay_asio protorelay_logging Threads::Threads)
    add_test(NAME asio_server_transport_test COMMAND asio_server_transport_test)
    set_tests_properties(asio_server_transport_test PROPERTIES TIMEOUT 20)
    set_tests_properties(logger_test std_thread_pool_test connection_interface_test asio_executor_test PROPERTIES TIMEOUT 20)

    # MariaDB 非阻塞 async 回归（阶段 2）：连接池 available 在批量 async 查询后回基线
    if(HAVE_MARIADB)
        add_executable(mariadb_async_test ${COMMON_SRCS} test/unit/mariadb_async_test.cpp)
        target_include_directories(mariadb_async_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
        setup_mail_target(mariadb_async_test)
        target_include_directories(mariadb_async_test SYSTEM PRIVATE ${MARIADB_INCLUDE_DIR})
        target_compile_definitions(mariadb_async_test PRIVATE HAVE_MARIADB=1)
        add_test(NAME mariadb_async_test COMMAND mariadb_async_test)
        set_tests_properties(mariadb_async_test PROPERTIES SKIP_RETURN_CODE 77)
    endif()

    # 会话/FSM/服务基座：TestSession::close() 引用 ServerBase 成员符号
    # （server_base.cpp），须链 COMMON_SRCS（同 smtps/imaps_fsm_test 模式）
    add_executable(fsm_base_test ${COMMON_SRCS} test/unit/fsm_base_test.cpp)
    target_include_directories(fsm_base_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(fsm_base_test)

    add_executable(fast_fsm_base_test ${COMMON_SRCS} test/unit/fast_fsm_base_test.cpp)
    target_include_directories(fast_fsm_base_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(fast_fsm_base_test)

    add_executable(session_base_test ${COMMON_SRCS} test/unit/session_base_test.cpp)
    target_include_directories(session_base_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(session_base_test)

    add_executable(server_base_test ${COMMON_SRCS} test/unit/server_base_test.cpp)
    target_include_directories(server_base_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(server_base_test)

    add_executable(outbound_utils_test ${COMMON_SRCS} test/unit/outbound_utils_test.cpp)
    target_include_directories(outbound_utils_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(outbound_utils_test)

    add_executable(starttls_handoff_test ${COMMON_SRCS} test/unit/starttls_handoff_test.cpp)
    target_include_directories(starttls_handoff_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(starttls_handoff_test)

    add_executable(pipeline_deferred_test ${COMMON_SRCS} test/unit/pipeline_deferred_test.cpp)
    target_include_directories(pipeline_deferred_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(pipeline_deferred_test)

    # MetricsServer 核心 API 不变量测试（counter/gauge/histogram 语义）。
    # 不链 COMMON_SRCS：MetricsServer 是 header-only，只依赖 boost::asio。
    add_executable(metrics_core_test test/unit/metrics_core_test.cpp)
    target_include_directories(metrics_core_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(metrics_core_test)

    add_executable(fsm_bench
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        test/bench/fsm/fsm_bench.cpp
    )
    target_include_directories(fsm_bench PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(fsm_bench)

    add_executable(smtps_fsm_test
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        test/unit/smtps_fsm_test.cpp
    )
    target_include_directories(smtps_fsm_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(smtps_fsm_test)

    # 异步/并发 FSM 测试（Mock DNS/DB 延迟回调 + 真实 IO 线程）
    add_executable(smtps_fsm_concurrency_test
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        test/unit/smtps_fsm_concurrency_test.cpp
    )
    target_include_directories(smtps_fsm_concurrency_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(smtps_fsm_concurrency_test)
    if(ENABLE_TSAN)
        target_compile_options(smtps_fsm_test PRIVATE -fsanitize=thread -g -O1)
        target_link_options(smtps_fsm_test PRIVATE -fsanitize=thread)
        target_compile_options(smtps_fsm_concurrency_test PRIVATE -fsanitize=thread -g -O1)
        target_link_options(smtps_fsm_concurrency_test PRIVATE -fsanitize=thread)
    endif()


    add_executable(imaps_fsm_test
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/imaps/imaps_server.cpp
        test/unit/imaps_fsm_test.cpp
    )
    target_include_directories(imaps_fsm_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(imaps_fsm_test)

    add_executable(pop3_fsm_test
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/pop3/pop3_server.cpp
        test/unit/pop3_fsm_test.cpp
    )
    target_include_directories(pop3_fsm_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(pop3_fsm_test)

    add_executable(pop3_timer_boundary_test ${COMMON_SRCS}
        src/mail_system/back/mailServer/pop3/pop3_server.cpp test/unit/pop3_timer_boundary_test.cpp)
    target_include_directories(pop3_timer_boundary_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(pop3_timer_boundary_test)
    add_test(NAME pop3_timer_boundary_test COMMAND pop3_timer_boundary_test)
    set_tests_properties(pop3_timer_boundary_test asio_error_test PROPERTIES TIMEOUT 20)

    add_executable(smtp_client test/bench/smtp/smtp_client.cpp)
    target_compile_features(smtp_client PRIVATE cxx_std_17)
    add_executable(imap_client test/bench/imap/imap_client.cpp)
    target_compile_features(imap_client PRIVATE cxx_std_17)
    add_executable(pop3_client test/bench/pop3/pop3_client.cpp)
    target_compile_features(pop3_client PRIVATE cxx_std_17)

    add_executable(dispatch_bench test/bench/fsm/dispatch_bench.cpp)
    target_compile_features(dispatch_bench PRIVATE cxx_std_17)

    add_executable(outbound_smoke ${COMMON_SRCS} test/unit/outbound_smoke.cpp)
    setup_mail_target(outbound_smoke)
    target_include_directories(outbound_smoke PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
endif()

# ================================================================
# CTest 集成
# ================================================================
if(NOT BUILD_OBJECT_ONLY AND BUILD_TESTS)
    enable_testing()
    add_test(NAME smtps_fsm_test   COMMAND smtps_fsm_test)
    add_test(NAME smtps_fsm_concurrency_test COMMAND smtps_fsm_concurrency_test)
    add_test(NAME imaps_fsm_test   COMMAND imaps_fsm_test)
    add_test(NAME pop3_fsm_test    COMMAND pop3_fsm_test)
    add_test(NAME test_inbound_verifier COMMAND test_inbound_verifier)
    add_test(NAME outbound_smoke   COMMAND outbound_smoke)
    set_tests_properties(outbound_smoke PROPERTIES TIMEOUT 20)
    add_test(NAME sql_queries_test COMMAND sql_queries_test)
    add_test(NAME mime_parser_test COMMAND mime_parser_test)
    add_test(NAME mail_body_writer_test COMMAND mail_body_writer_test)
    add_test(NAME buffered_upload_stream_test COMMAND buffered_upload_stream_test)
    add_test(NAME io_error_test COMMAND io_error_test)
    add_test(NAME async_storage_provider_test COMMAND async_storage_provider_test)
    add_test(NAME fsm_base_test COMMAND fsm_base_test)
    add_test(NAME fast_fsm_base_test COMMAND fast_fsm_base_test)
    add_test(NAME intrusion_detector_test COMMAND intrusion_detector_test)
    add_test(NAME server_config_test COMMAND server_config_test)
    add_test(NAME mapped_file_test COMMAND mapped_file_test)
    add_test(NAME thread_pool_test COMMAND thread_pool_test)
    add_test(NAME session_base_test COMMAND session_base_test)
    add_test(NAME server_base_test COMMAND server_base_test)
    add_test(NAME outbound_utils_test COMMAND outbound_utils_test)
    add_test(NAME starttls_handoff_test COMMAND starttls_handoff_test)
    add_test(NAME pipeline_deferred_test COMMAND pipeline_deferred_test)
    add_test(NAME metrics_core_test COMMAND metrics_core_test)
    add_test(NAME bcrypt_test COMMAND bcrypt_test)
    add_test(NAME password_rehash_test COMMAND password_rehash_test)
    add_test(NAME safe_parse_test COMMAND safe_parse_test)
    add_test(NAME account_policy_boundary_test COMMAND account_policy_boundary_test)
    add_test(NAME webmail_test COMMAND webmail_test)
    add_test(NAME persist_submission_test COMMAND persist_submission_test)
    set_tests_properties(persist_submission_test PROPERTIES TIMEOUT 20)
    add_test(NAME smtp_persist_ack_test COMMAND smtp_persist_ack_test)
    set_tests_properties(smtp_persist_ack_test PROPERTIES TIMEOUT 15)
    add_test(NAME smtp_client_test COMMAND smtp_client_test)
    set_tests_properties(smtp_client_test PROPERTIES TIMEOUT 20)
    add_test(NAME smtp_submitter_test COMMAND smtp_submitter_test)
    add_test(NAME http_session_read_test COMMAND http_session_read_test)
    set_tests_properties(smtp_submitter_test PROPERTIES TIMEOUT 15)

    # 聚合 target：新增单测只需在上面 add_executable/add_test，
    # CI 与本地都构建这一个 target，避免测试清单漂移（此前 CI 的
    # 显式 target 列表缺了新增的 4 个测试，ctest 因二进制缺失而红）
    add_custom_target(unit_tests DEPENDS
        logger_test
        std_thread_pool_test
        connection_interface_test
        server_transport_test
        asio_executor_test
        asio_error_test
        asio_server_transport_test
        pop3_timer_boundary_test
        smtps_fsm_test
        smtps_fsm_concurrency_test
        imaps_fsm_test
        pop3_fsm_test
        test_inbound_verifier
        outbound_smoke
        sql_queries_test
        mime_parser_test
        mail_body_writer_test
        buffered_upload_stream_test
        io_error_test
        async_storage_provider_test
        fsm_base_test
        fast_fsm_base_test
        intrusion_detector_test
        server_config_test
        mapped_file_test
        thread_pool_test
        session_base_test
        server_base_test
        outbound_utils_test
        starttls_handoff_test
        pipeline_deferred_test
        metrics_core_test
        bcrypt_test
        password_rehash_test
        safe_parse_test
        account_policy_boundary_test
        webmail_test
        smtp_submitter_test
        smtp_client_test
        smtp_persist_ack_test
        persist_submission_test
        http_session_read_test
    )
    if(HAVE_MARIADB)
        add_dependencies(unit_tests mariadb_async_test)
    endif()
    # ASan 对全部单测生效：string_view 越界索引不一定 segfault，
    # 没有地址消毒的模糊测试发现不了这类堆/栈越界
    if(ENABLE_ASAN)
        set(_asan_tests smtps_fsm_test smtps_fsm_concurrency_test imaps_fsm_test
            readiness_waiter_test async_db_adapter_test account_authentication_test
            mailbox_repository_test imap_mutation_repository_test webmail_read_repository_test
            imap_completion_repository_test webmail_persistence_repository_test
            pop3_repository_test account_repository_test
            persistence_repository_test persistent_queue_contract_test mail_object_key_layout_test
            smtp_client_contract_test storage_boundary_test
            logger_test std_thread_pool_test connection_interface_test server_transport_test asio_executor_test asio_error_test asio_server_transport_test pop3_timer_boundary_test
            pop3_fsm_test
            test_inbound_verifier outbound_smoke sql_queries_test mime_parser_test
            mail_body_writer_test buffered_upload_stream_test io_error_test
            async_storage_provider_test fsm_base_test fast_fsm_base_test
            intrusion_detector_test server_config_test mapped_file_test
            thread_pool_test session_base_test server_base_test
            outbound_utils_test metrics_core_test bcrypt_test
            webmail_test smtp_submitter_test smtp_client_test smtp_persist_ack_test persist_submission_test http_session_read_test)
        foreach(_t IN LISTS _asan_tests)
            # vptr 排除：它给多态向下转型插 typeinfo 引用，跨翻译单元链接不了
            target_compile_options(${_t} PRIVATE -fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined -g -O1)
            target_link_options(${_t} PRIVATE -fsanitize=address,undefined)
        endforeach()
        target_compile_options(protorelay_logging PRIVATE
            -fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined -g -O1)
        target_link_options(protorelay_logging PUBLIC -fsanitize=address,undefined)
        target_compile_options(protorelay_asio PRIVATE
            -fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined -g -O1)
        target_link_options(protorelay_asio PUBLIC -fsanitize=address,undefined)
    endif()

endif()

# Register every unit-test executable, including those historically omitted from CTest.
# Discovery and build aggregation share the actual CMake target inventory.
if(NOT BUILD_OBJECT_ONLY AND BUILD_TESTS)
    add_test(NAME dependency_boundaries COMMAND ${CMAKE_COMMAND} -DPR_SOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/test/backend_contract/check_dependency_boundaries.cmake)
    add_test(NAME object_tree_hygiene COMMAND ${CMAKE_COMMAND} -DPR_SOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/test/backend_contract/check_object_tree_hygiene.cmake)
    get_property(_unit_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _unit_targets)
        if(_target MATCHES "(_test$|^test_inbound_verifier$|^outbound_smoke$)" AND NOT _target STREQUAL "unit_tests")
            get_target_property(_kind ${_target} TYPE)
            if(_kind STREQUAL "EXECUTABLE")
                if(NOT TEST ${_target})
                    add_test(NAME ${_target} COMMAND ${_target})
                    set_tests_properties(${_target} PROPERTIES TIMEOUT 120)
                endif()
                add_dependencies(unit_tests ${_target})
            endif()
        endif()
    endforeach()
endif()

if(NOT BUILD_OBJECT_ONLY)
    install(TARGETS smtpsServer imapsServer mailServer RUNTIME DESTINATION bin)
endif()

# ---- Fuzz harnesses（libFuzzer，默认关闭） ----
# 注意：Apple 自带 clang 不带 libFuzzer 运行时（链接报 libclang_rt.fuzzer_osx.a
# not found）。macOS 上用 Homebrew LLVM：
#   CC=/opt/homebrew/opt/llvm/bin/clang CXX=/opt/homebrew/opt/llvm/bin/clang++ \
#     cmake -B build-fuzz -DENABLE_FUZZING=ON -DBUILD_TESTS=OFF
# Linux CI 的 clang 直接可用。
if(ENABLE_FUZZING)
    # fuzzer + ASan/UBSan 是标准组合：光靠 segfault 抓不住 string_view 的
    # 越界读（很多落在分配器余量里）
    set(FUZZ_SANITIZERS fuzzer-no-link,address,undefined)
    set(FUZZ_NOSAN vptr)

    add_executable(mime_fuzz test/fuzz/mime_fuzz.cpp)
    target_include_directories(mime_fuzz PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_compile_options(mime_fuzz PRIVATE -fsanitize=${FUZZ_SANITIZERS} -fno-sanitize=${FUZZ_NOSAN} -g -O1)
    target_link_options(mime_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

    # SMTP DATA 解析核心。刻意不走 setup_mail_target：它会带进 SDK 的
    # -isystem（CURL/MySQL 的 macOS 路径），破坏 Homebrew clang 下 libc++
    # 头的搜索链。手动只挂实际需要的 include + spdlog 链接。
    add_executable(smtp_fuzz
        test/fuzz/smtp_data_fuzz.cpp
        src/mail_system/back/algorithm/smtp_utils.cpp)
    target_include_directories(smtp_fuzz PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        SYSTEM ${nlohmann_json_INCLUDE_DIRS}
        SYSTEM ${Boost_INCLUDE_DIRS})
    target_link_libraries(smtp_fuzz PRIVATE protorelay_logging)
    target_compile_options(smtp_fuzz PRIVATE -fsanitize=${FUZZ_SANITIZERS} -fno-sanitize=${FUZZ_NOSAN} -g -O1)
    target_link_options(smtp_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    foreach(_ft mime_fuzz smtp_fuzz)
    if(APPLE)
        # Homebrew LLVM 的 fuzzer 运行时依赖它自带的（更新的）libc++；
        # 不显式指过去的话 ld 会用 Xcode SDK 的旧 libc++，符号对不上。
        # HOMEBREW_LLVM_PREFIX 可按环境覆盖。
        set(HOMEBREW_LLVM_PREFIX "/opt/homebrew/opt/llvm" CACHE PATH "Homebrew LLVM prefix")
        # -nostdlib++ 去掉隐式 -lc++，再显式链 Homebrew 的 libc++：
        # 隐式那个会解析到 Xcode SDK 的旧版（缺 __hash_memory 等新符号），
        # 而 -L 搜索顺序竞争不过 SDK 路径，干脆用绝对路径
        target_link_options(mime_fuzz PRIVATE
            -nostdlib++
            -Wl,-rpath,${HOMEBREW_LLVM_PREFIX}/lib/c++)
        target_link_libraries(${_ft} PRIVATE ${HOMEBREW_LLVM_PREFIX}/lib/c++/libc++.1.dylib)
    endif()
    endforeach()
    message(STATUS "Fuzz harnesses enabled: mime_fuzz, smtp_fuzz")
endif()
