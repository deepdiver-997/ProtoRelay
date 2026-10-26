cmake_minimum_required(VERSION 3.10)
project(ProtoRelay LANGUAGES CXX)

option(PROTORELAY_BUILD_BACKEND_CONTRACTS_ONLY "Build framework contracts without any Asio backend or service dependencies" OFF)
if(PROTORELAY_BUILD_BACKEND_CONTRACTS_ONLY)
    enable_testing()
    add_subdirectory(test/backend_contract)
    return()
endif()

# ================================================================
# ProtoRelay 构建编排（CMake 卫生任务 3：实现体在 cmake/ 下，include()
# 同作用域语义让 COMMON_SRCS/LINK_LIBRARIES 等全局变量跨文件共享；
# 禁止 add_subdirectory。各文件内容自根文件原样拆出，顺序保持不变。）
# ================================================================
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/options.cmake")            # 选项/标准/flags/日志
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/deps.cmake")               # 依赖查找/链接库/COMMON_SRCS
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/mail_targets_setup.cmake") # 输出目录/setup_mail_target/模块表

if(NOT PROTORELAY_BUILD_MAIL)
    # framework-only 约简配置：体在 cmake/framework_only.cmake；return() 必须
    # 留在根作用域——include 内的 return() 只退出 include 文件，拦不住后续编排。
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/framework_only.cmake")
    return()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/mail_targets.cmake")       # 邮件协议/服务器目标
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/unit_tests.cmake")         # 测试/Benchmark
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ctest.cmake")              # CTest 集成/Fuzz
