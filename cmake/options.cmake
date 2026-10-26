# ================================================================
# 构建选项 / C++ 标准 / 编译 flags / 覆盖率 / 日志级别（自根 CMakeLists 原样拆出，CMake 卫生任务 3）
# ================================================================
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

