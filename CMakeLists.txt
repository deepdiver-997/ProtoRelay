cmake_minimum_required(VERSION 3.10)
project(mail_system LANGUAGES CXX)

# Set policy to use CMake's newer FindBoost behavior (suppress dev warning on newer CMake)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

# 添加编译选项
option(USE_SSL "Enable SSL/TLS support" ON)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项（与原 Makefile 对齐）
add_compile_options(-Wall -g)

# 根据SSL选项设置编译定义
if(USE_SSL)
    add_compile_definitions(USE_SSL)
    message(STATUS "Building with SSL/TLS support")
else()
    message(STATUS "Building without SSL/TLS support")
endif()

# -----------------------------
# 依赖查找（尽量自动化、跨平台）
# -----------------------------
find_package(Boost REQUIRED COMPONENTS system thread)

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
    message(STATUS "OpenSSL not found by CMake; attempting Homebrew prefix /opt/homebrew")
    set(OPENSSL_ROOT_DIR /opt/homebrew CACHE PATH "OpenSSL root (Homebrew fallback)")
    find_package(OpenSSL REQUIRED)
endif()
find_package(Threads REQUIRED)

# 尝试通过 pkg-config 找 mysqlclient（更可靠地在不同系统上查找）
find_package(PkgConfig)
set(MYSQL_INCLUDE_DIRS "")
set(MYSQL_LIBRARIES "")
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_MYSQL QUIET mysqlclient libmysqlclient)
    if(PC_MYSQL_FOUND)
        message(STATUS "Found mysqlclient via pkg-config: ${PC_MYSQL_VERSION}")
        set(MYSQL_INCLUDE_DIRS ${PC_MYSQL_INCLUDE_DIRS})
        set(MYSQL_LIBRARIES ${PC_MYSQL_LIBRARIES})
    endif()
endif()

# 如果 pkg-config 未找到，再用传统的 find_path / find_library 回退
if(NOT MYSQL_LIBRARIES)
    # On macOS prefer common Homebrew prefixes (Apple Silicon and Intel)
    if(APPLE)
        set(HOMEBREW_PREFIXES /opt/homebrew/lib /usr/local /usr/local/opt/mysql)
        link_directories(/opt/homebrew/lib)
    else()
        set(HOMEBREW_PREFIXES /usr/local)
    endif()

    # Try several candidate paths including Homebrew locations
    # Prefer common system and Homebrew locations (explicitly include /opt/homebrew)
    find_path(MYSQL_INCLUDE_DIR mysql.h
        PATHS /opt/homebrew/include /opt/homebrew/lib /usr/include /usr/local/include ${HOMEBREW_PREFIXES} /opt/homebrew/mysql/include
        NO_DEFAULT_PATH
    )
    find_library(MYSQL_CLIENT_LIB NAMES mysqlclient libmysqlclient
        PATHS /opt/homebrew/lib /opt/homebrew/opt/mysql-client/lib /usr/lib /usr/local/lib ${HOMEBREW_PREFIXES} /usr/local/opt/mysql/lib
        NO_DEFAULT_PATH
    )

    if(MYSQL_INCLUDE_DIR AND MYSQL_CLIENT_LIB)
        set(MYSQL_INCLUDE_DIRS ${MYSQL_INCLUDE_DIR})
        set(MYSQL_LIBRARIES ${MYSQL_CLIENT_LIB})
        message(STATUS "Found mysqlclient via find_path/find_library: ${MYSQL_CLIENT_LIB}")
    else()
        # If still not found and on macOS, make an explicit best-effort check of /opt/homebrew
        if(APPLE AND NOT MYSQL_LIBRARIES)
            # Try explicit Homebrew locations often used on macOS (Apple Silicon: /opt/homebrew)
            find_path(MYSQL_INCLUDE_DIR_HOME mysql.h PATHS /opt/homebrew/include /usr/local/opt/mysql/include)
            find_library(MYSQL_CLIENT_LIB_HOME NAMES libmysqlclient.dylib libmysqlclient.a mysqlclient PATHS /opt/homebrew/lib /usr/local/opt/mysql/lib)
            if(MYSQL_INCLUDE_DIR_HOME AND MYSQL_CLIENT_LIB_HOME)
                set(MYSQL_INCLUDE_DIRS ${MYSQL_INCLUDE_DIR_HOME})
                set(MYSQL_LIBRARIES ${MYSQL_CLIENT_LIB_HOME})
                message(STATUS "Found mysqlclient in Homebrew prefix: ${MYSQL_CLIENT_LIB_HOME}")
            endif()
        endif()

        if(NOT MYSQL_LIBRARIES)
            if(APPLE)
                message(WARNING "mysqlclient not found automatically. On macOS with Homebrew try: -DMYSQL_INCLUDE_DIRS=/opt/homebrew/include -DMYSQL_LIBRARIES=/opt/homebrew/lib/libmysqlclient.dylib")
            else()
                message(WARNING "mysqlclient not found automatically. You can set MYSQL_INCLUDE_DIRS and MYSQL_LIBRARIES manually when invoking cmake, e.g. -DMYSQL_INCLUDE_DIRS=/path/to/include -DMYSQL_LIBRARIES=/path/to/libmysqlclient.a")
            endif()
        endif()
    endif()
endif()

# -----------------------------
# 源文件列表
# -----------------------------
set(COMMON_SRCS
    src/mail_system/back/db/mysql_pool.cpp
    src/mail_system/back/db/mysql_service.cpp
    src/mail_system/back/mailServer/server_base.cpp
    src/mail_system/back/mailServer/session/session_base.cpp
    src/mail_system/back/thread_pool/boost_thread_pool.cpp
    src/mail_system/back/thread_pool/io_thread_pool.cpp
)

set(SMTP_SRCS
    ${COMMON_SRCS}
    src/mail_system/back/mailServer/smtps/smtps_server.cpp
    src/mail_system/back/mailServer/session/smtps_session.cpp
    src/mail_system/back/mailServer/fsm/smtps/smtps_fsm.cpp
    src/mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.cpp
    build/smtpsServer.cpp
)

set(IMAP_SRCS
    ${COMMON_SRCS}
    src/mail_system/back/mailServer/imaps/imaps_server.cpp
    src/mail_system/back/mailServer/session/imaps_session.cpp
    src/mail_system/back/mailServer/fsm/imaps/imaps_fsm.cpp
    src/mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.cpp
    build/imapsServer.cpp
)

# 输出目录设置：可执行文件放到项目根的 `test` 目录，中间产物放到 build
set(TEST_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/test)
set(MIDDLE_OUTPUT_DIR ${CMAKE_BINARY_DIR}/build)

file(MAKE_DIRECTORY ${TEST_OUTPUT_DIR})
file(MAKE_DIRECTORY ${MIDDLE_OUTPUT_DIR})

# 可执行目标
add_executable(smtpsServer ${SMTP_SRCS})
add_executable(imapsServer ${IMAP_SRCS})

# 全局输出目录（针对所有 target 的默认）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${MIDDLE_OUTPUT_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${MIDDLE_OUTPUT_DIR})

# 每个 target 的输出属性（确保一致）
set_target_properties(smtpsServer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
    ARCHIVE_OUTPUT_DIRECTORY ${MIDDLE_OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${MIDDLE_OUTPUT_DIR}
)
set_target_properties(imapsServer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${TEST_OUTPUT_DIR}
    ARCHIVE_OUTPUT_DIRECTORY ${MIDDLE_OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${MIDDLE_OUTPUT_DIR}
)

# -----------------------------
# 包含目录（优先使用项目内的 OuterLib），并以 SYSTEM 标记外部库头以抑制警告
# -----------------------------
# 项目头
target_include_directories(smtpsServer PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/OuterLib/json)
target_include_directories(imapsServer PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/OuterLib/json)

# Boost
if(Boost_FOUND)
    # BOOST 提供的变量通常包含 include dirs
    target_include_directories(smtpsServer PRIVATE SYSTEM ${Boost_INCLUDE_DIRS})
    target_include_directories(imapsServer PRIVATE SYSTEM ${Boost_INCLUDE_DIRS})
else()
    message(FATAL_ERROR "Boost not found. Install Boost or set BOOST_ROOT / BOOST_INCLUDEDIR.")
endif()

# MySQL（若找到则加入）
if(MYSQL_INCLUDE_DIRS)
    target_include_directories(smtpsServer PRIVATE SYSTEM ${MYSQL_INCLUDE_DIRS})
    target_include_directories(imapsServer PRIVATE SYSTEM ${MYSQL_INCLUDE_DIRS})
endif()

# -----------------------------
# 链接依赖（显式链接，避免 link_directories）
# -----------------------------
if(NOT MYSQL_LIBRARIES)
    if(APPLE)
        # If mysql client lib still not found, add Homebrew lib dir to link search and try -lmysqlclient as fallback
        message(STATUS "MYSQL_LIBRARIES empty — on macOS adding /opt/homebrew/lib to link search and trying -lmysqlclient fallback")
        if(EXISTS "/opt/homebrew/lib")
            # target_link_directories requires CMake 3.13+; use link_directories as fallback for older
            link_directories(/opt/homebrew/lib)
        endif()
        set(MYSQL_LIBRARIES mysqlclient)
    endif()
endif()

# 根据SSL选项链接不同的库
set(LINK_LIBRARIES ${Boost_LIBRARIES} Threads::Threads ${MYSQL_LIBRARIES})

if(USE_SSL)
    list(APPEND LINK_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
endif()

target_link_libraries(smtpsServer PRIVATE ${LINK_LIBRARIES})
target_link_libraries(imapsServer PRIVATE ${LINK_LIBRARIES})

# 目标编译选项
target_compile_features(smtpsServer PRIVATE cxx_std_17)
target_compile_features(imapsServer PRIVATE cxx_std_17)

# 可选：安装规则（不强制）
install(TARGETS smtpsServer imapsServer RUNTIME DESTINATION bin)