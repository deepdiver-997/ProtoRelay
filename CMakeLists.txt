cmake_minimum_required(VERSION 3.10)
project(mail_system)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 编译选项（与原 Makefile 对齐）
add_compile_options(-Wall -g)

# 查找依赖库
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# 包含目录（优先使用项目内的 OuterLib）
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/OuterLib/json/single_include
    /opt/homebrew/include
)
link_directories(/opt/homebrew/lib)

# 源文件列表（参考原 Makefile）
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

# 链接依赖库
target_link_libraries(smtpsServer PRIVATE ${Boost_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto Threads::Threads mysqlclient pthread)
target_link_libraries(imapsServer PRIVATE ${Boost_LIBRARIES} OpenSSL::SSL OpenSSL::Crypto Threads::Threads mysqlclient pthread)

# 目标编译选项
target_compile_features(smtpsServer PRIVATE cxx_std_17)
target_compile_features(imapsServer PRIVATE cxx_std_17)

# 可选：安装规则（不强制）
install(TARGETS smtpsServer imapsServer RUNTIME DESTINATION bin)
