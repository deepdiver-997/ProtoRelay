cmake_minimum_required(VERSION 3.10)
project(mail_system LANGUAGES CXX)

# Set policy to use CMake's newer FindBoost behavior (suppress dev warning on newer CMake)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

# ===== 构建配置 =====
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build: Debug/Release" FORCE)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# 添加编译选项
option(USE_SSL "Enable SSL/TLS support" ON)
option(ENABLE_DEBUG_LOGS "Enable debug logs for all modules" OFF)

# 设置C++标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ===== Debug 模式配置 =====
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -DDEBUG -fno-omit-frame-pointer" CACHE STRING "Flags used by C++ compiler during DEBUG builds")
set(CMAKE_C_FLAGS_DEBUG "-O0 -g -DDEBUG -fno-omit-frame-pointer" CACHE STRING "Flags used by C compiler during DEBUG builds")

# ===== Release 模式配置 =====
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -march=native" CACHE STRING "Flags used by C++ compiler during RELEASE builds")
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG -march=native" CACHE STRING "Flags used by C compiler during RELEASE builds")

# 全局编译选项
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_compile_options(-fdiagnostics-color=auto)
endif()

# ===== SSL 选项 =====
if(USE_SSL)
    add_compile_definitions(USE_SSL)
    message(STATUS "✓ SSL/TLS support ENABLED")
else()
    message(STATUS "✗ SSL/TLS support DISABLED")
endif()

# ===== 日志级别配置 =====
# Debug 模式：默认启用所有 DEBUG 级别日志
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(NOT DEFINED ENABLE_DEBUG_LOGS)
        set(ENABLE_DEBUG_LOGS ON CACHE BOOL "Enable debug logs for all modules" FORCE)
    endif()
    message(STATUS "📝 Debug mode: All debug logs ENABLED")
endif()

# Release/Testing 模式：默认禁用 DEBUG 级别日志，只保留 INFO
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(NOT DEFINED ENABLE_DEBUG_LOGS)
        set(ENABLE_DEBUG_LOGS OFF CACHE BOOL "Enable debug logs for all modules" FORCE)
    endif()
    message(STATUS "📝 Release mode: Debug logs DISABLED (INFO level only)")
endif()

# 根据选项设置编译定义
if(ENABLE_DEBUG_LOGS)
    add_compile_definitions(
        ENABLE_SERVER_DEBUG_LOG=1
        ENABLE_NETWORK_DEBUG_LOG=1
        ENABLE_DATABASE_DEBUG_LOG=1
        ENABLE_DATABASE_QUERY_DEBUG_LOG=1
        ENABLE_SMTP_DEBUG_LOG=1
        ENABLE_SMTP_DETAIL_DEBUG_LOG=1
        ENABLE_SESSION_DEBUG_LOG=1
        ENABLE_THREAD_POOL_DEBUG_LOG=1
        ENABLE_FILE_IO_DEBUG_LOG=1
        ENABLE_AUTH_DEBUG_LOG=1
    )
else()
    add_compile_definitions(
        ENABLE_SERVER_DEBUG_LOG=0
        ENABLE_NETWORK_DEBUG_LOG=0
        ENABLE_DATABASE_DEBUG_LOG=0
        ENABLE_DATABASE_QUERY_DEBUG_LOG=0
        ENABLE_SMTP_DEBUG_LOG=0
        ENABLE_SMTP_DETAIL_DEBUG_LOG=0
        ENABLE_SESSION_DEBUG_LOG=0
        ENABLE_THREAD_POOL_DEBUG_LOG=0
        ENABLE_FILE_IO_DEBUG_LOG=0
        ENABLE_AUTH_DEBUG_LOG=0
    )
endif()

# -----------------------------
# 依赖查找（尽量自动化、跨平台）
# -----------------------------
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(spdlog REQUIRED)

if(USE_SSL)
    find_package(OpenSSL REQUIRED)
else()
    # 如果禁用SSL，不查找OpenSSL但仍然定义相关变量以避免CMake错误
    set(OPENSSL_FOUND FALSE)
    set(OPENSSL_INCLUDE_DIR "")
    set(OPENSSL_LIBRARIES "")
endif()

# If OpenSSL not found, on macOS try common Homebrew prefix and re-run find
if(USE_SSL AND NOT OpenSSL_FOUND AND APPLE)
    message(STATUS "OpenSSL not found by CMake; attempting Homebrew prefix...")
    if(EXISTS "/opt/homebrew/opt/openssl")
        set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl" CACHE PATH "OpenSSL root (Homebrew)")
    elseif(EXISTS "/usr/local/opt/openssl")
        set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl" CACHE PATH "OpenSSL root (Homebrew Intel)")
    endif()
    find_package(OpenSSL REQUIRED)
endif()
find_package(Threads REQUIRED)

# ===== MySQL 库查找 (自动推导路径) =====
find_package(PkgConfig)
set(MYSQL_INCLUDE_DIRS "")
set(MYSQL_LIBRARIES "")
set(CARES_INCLUDE_DIRS "")
set(CARES_LIBRARIES "")
set(CARES_LIBRARY_DIRS "")

if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_CARES QUIET libcares)
    if(NOT PC_CARES_FOUND)
        pkg_check_modules(PC_CARES QUIET cares)
    endif()
    if(PC_CARES_FOUND)
        set(CARES_INCLUDE_DIRS ${PC_CARES_INCLUDE_DIRS})
        set(CARES_LIBRARY_DIRS ${PC_CARES_LIBRARY_DIRS})
        if(PC_CARES_LINK_LIBRARIES)
            set(CARES_LIBRARIES ${PC_CARES_LINK_LIBRARIES})
        else()
            set(CARES_LIBRARIES ${PC_CARES_LIBRARIES})
        endif()
        message(STATUS "✓ Found c-ares via pkg-config")
    endif()
endif()

if(NOT CARES_LIBRARIES)
    find_path(CARES_INCLUDE_DIR ares.h PATHS /opt/homebrew/include /usr/local/include /usr/include)
    find_library(CARES_LIBRARY NAMES cares c-ares PATHS /opt/homebrew/lib /usr/local/lib /usr/lib)
    if(CARES_INCLUDE_DIR AND CARES_LIBRARY)
        set(CARES_INCLUDE_DIRS ${CARES_INCLUDE_DIR})
        set(CARES_LIBRARIES ${CARES_LIBRARY})
        get_filename_component(CARES_LIBRARY_DIRS ${CARES_LIBRARY} DIRECTORY)
        message(STATUS "✓ Found c-ares library: ${CARES_LIBRARY}")
    else()
        message(FATAL_ERROR "c-ares not found. Please install c-ares (e.g. brew install c-ares).")
    endif()
endif()

# 第一步：尝试 pkg-config
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_MYSQL QUIET mysqlclient libmysqlclient)
    if(PC_MYSQL_FOUND)
        message(STATUS "✓ Found mysqlclient via pkg-config: ${PC_MYSQL_VERSION}")
        set(MYSQL_INCLUDE_DIRS ${PC_MYSQL_INCLUDE_DIRS})
        set(MYSQL_LIBRARIES ${PC_MYSQL_LIBRARIES})
    endif()
endif()

# 第二步：若 pkg-config 未找到，尝试自动推导
if(NOT MYSQL_LIBRARIES)
    # 智能路径选择
    set(MYSQL_SEARCH_PATHS)
    
    # 苹果硅 Mac (Apple Silicon)
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        list(APPEND MYSQL_SEARCH_PATHS "/opt/homebrew/opt/mysql-client")
        list(APPEND MYSQL_SEARCH_PATHS "/opt/homebrew")
    # Intel Mac
    elseif(APPLE)
        list(APPEND MYSQL_SEARCH_PATHS "/usr/local/opt/mysql")
        list(APPEND MYSQL_SEARCH_PATHS "/usr/local")
    endif()
    
    # Linux 通用路径
    list(APPEND MYSQL_SEARCH_PATHS "/usr" "/usr/local" "/opt/mysql")
    
    find_path(MYSQL_INCLUDE_DIR mysql.h
        PATHS ${MYSQL_SEARCH_PATHS}
        PATH_SUFFIXES include
    )
    
    find_library(MYSQL_CLIENT_LIB
        NAMES mysqlclient libmysqlclient
        PATHS ${MYSQL_SEARCH_PATHS}
        PATH_SUFFIXES lib lib64
    )
    
    if(MYSQL_INCLUDE_DIR AND MYSQL_CLIENT_LIB)
        set(MYSQL_INCLUDE_DIRS "${MYSQL_INCLUDE_DIR}")
        set(MYSQL_LIBRARIES "${MYSQL_CLIENT_LIB}")
        message(STATUS "✓ Found MySQL: ${MYSQL_CLIENT_LIB}")
    else()
        message(WARNING "✗ mysqlclient not found. Set manually: -DMYSQL_INCLUDE_DIRS=<path> -DMYSQL_LIBRARIES=<path>")
    endif()
endif()

# -----------------------------
# 源文件列表
# -----------------------------
set(COMMON_SRCS
    src/mail_system/back/db/mysql_pool.cpp
    src/mail_system/back/db/mysql_service.cpp
    src/mail_system/back/mailServer/server_base.cpp
    src/mail_system/back/thread_pool/boost_thread_pool.cpp
    src/mail_system/back/thread_pool/io_thread_pool.cpp
    src/mail_system/back/algorithm/snow.cpp
    src/mail_system/back/outbound/outbox_repository.cpp
    src/mail_system/back/outbound/smtp_outbound_client.cpp
    src/mail_system/back/outbound/outbound_utils.cpp
    src/mail_system/back/outbound/mx_routing_utils.cpp
    src/mail_system/back/outbound/cares_dns_resolver.cpp
    src/smtp_utils.cpp
    src/persistent_queue.cpp
)

set(SMTP_SRCS
    ${COMMON_SRCS}
    src/mail_system/back/mailServer/smtps/smtps_server.cpp
    test/smtps_test.cpp
)

# ===== 输出目录配置 =====
# 可执行文件放到项目根的 test 目录，中间产物放到当前构建目录
set(TEST_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/test")

file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}")

message(STATUS "Build output: executables → ${TEST_OUTPUT_DIR}")
message(STATUS "Build output: intermediate → ${CMAKE_BINARY_DIR}")

# 可执行目标
add_executable(smtpsServer ${SMTP_SRCS})

# 全局输出目录 - 可执行文件到test目录，其他到build目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}")

# 每个 target 的输出属性
set_target_properties(smtpsServer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${TEST_OUTPUT_DIR}"
)

# -----------------------------
# 包含目录（优先使用项目内的 OuterLib），并以 SYSTEM 标记外部库头以抑制警告
# -----------------------------
# 项目头
target_include_directories(smtpsServer PRIVATE "${CMAKE_SOURCE_DIR}/include")

# spdlog
if(spdlog_FOUND)
    target_include_directories(smtpsServer PRIVATE SYSTEM ${spdlog_INCLUDE_DIRS})
endif()

# OuterLib (外部依赖库，如 nlohmann/json)
target_include_directories(smtpsServer PRIVATE "${CMAKE_SOURCE_DIR}/OuterLib/json")

# Boost
if(Boost_FOUND)
    # BOOST 提供的变量通常包含 include dirs
    target_include_directories(smtpsServer PRIVATE SYSTEM ${Boost_INCLUDE_DIRS})
else()
    message(FATAL_ERROR "Boost not found. Install Boost or set BOOST_ROOT / BOOST_INCLUDEDIR.")
endif()

# MySQL（若找到则加入）
if(MYSQL_INCLUDE_DIRS)
    target_include_directories(smtpsServer PRIVATE SYSTEM ${MYSQL_INCLUDE_DIRS})
endif()

if(CARES_INCLUDE_DIRS)
    target_include_directories(smtpsServer PRIVATE SYSTEM ${CARES_INCLUDE_DIRS})
endif()

if(CARES_LIBRARY_DIRS)
    target_link_directories(smtpsServer PRIVATE ${CARES_LIBRARY_DIRS})
endif()

# OpenSSL include（仅在未使用导入目标时作为兜底）
if(USE_SSL)
    if(DEFINED OPENSSL_INCLUDE_DIRS AND OPENSSL_INCLUDE_DIRS)
        target_include_directories(smtpsServer PRIVATE SYSTEM ${OPENSSL_INCLUDE_DIRS})
    elseif(DEFINED OPENSSL_INCLUDE_DIR AND OPENSSL_INCLUDE_DIR)
        target_include_directories(smtpsServer PRIVATE SYSTEM ${OPENSSL_INCLUDE_DIR})
    endif()
endif()

# -----------------------------
# 链接依赖（显式链接，避免 link_directories）
# -----------------------------
if(NOT MYSQL_LIBRARIES)
    if(APPLE)
        # If mysql client lib still not found, add Homebrew lib dir to link search and try -lmysqlclient as fallback
        message(STATUS "ℹ MYSQL_LIBRARIES empty — on macOS adding /opt/homebrew/lib to link search")
        if(EXISTS "/opt/homebrew/lib")
            link_directories(/opt/homebrew/lib)
            # Use full path instead of -l flag for MySQL client
            set(MYSQL_LIBRARIES "/opt/homebrew/lib/libmysqlclient.dylib")
        endif()
    endif()
endif()

# 根据SSL选项链接不同的库
set(LINK_LIBRARIES ${Boost_LIBRARIES} Threads::Threads ${MYSQL_LIBRARIES} ${CARES_LIBRARIES} spdlog::spdlog)

if(USE_SSL)
    if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
        list(APPEND LINK_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
    elseif(OPENSSL_LIBRARIES)
        message(WARNING "OpenSSL imported targets unavailable, fallback to OPENSSL_LIBRARIES")
        list(APPEND LINK_LIBRARIES ${OPENSSL_LIBRARIES})
    else()
        message(FATAL_ERROR "USE_SSL=ON but neither OpenSSL imported targets nor OPENSSL_LIBRARIES are available")
    endif()
endif()

target_link_libraries(smtpsServer PRIVATE ${LINK_LIBRARIES})

# 目标编译选项
target_compile_features(smtpsServer PRIVATE cxx_std_17)

# 可选：安装规则（不强制）
install(TARGETS smtpsServer RUNTIME DESTINATION bin)