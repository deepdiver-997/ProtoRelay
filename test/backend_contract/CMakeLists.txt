cmake_minimum_required(VERSION 3.10)
project(ProtoRelayBackendContracts LANGUAGES CXX)
# Also runnable as an independent project: only Threads and header-only JSON.
get_filename_component(PR_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
enable_testing()
add_library(protorelay_contract_core STATIC
    "${PR_SOURCE_ROOT}/src/framework/server_base.cpp"
    "${PR_SOURCE_ROOT}/src/framework/thread_pool/std_thread_pool.cpp"
    "${PR_SOURCE_ROOT}/src/framework/log/logger.cpp"
    "${PR_SOURCE_ROOT}/src/framework/log/standard_log_sink.cpp")
target_include_directories(protorelay_contract_core PUBLIC "${PR_SOURCE_ROOT}/include")
target_compile_definitions(protorelay_contract_core PUBLIC PROTORELAY_HAS_SPDLOG=0 PROTORELAY_LOG_NONE=0)
target_link_libraries(protorelay_contract_core PUBLIC Threads::Threads nlohmann_json::nlohmann_json)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(protorelay_contract_core PUBLIC stdc++fs)
endif()
foreach(contract logger_test std_thread_pool_test connection_interface_test server_transport_test metrics_core_test)
    add_executable(${contract} "${PR_SOURCE_ROOT}/test/unit/${contract}.cpp")
    target_link_libraries(${contract} PRIVATE protorelay_contract_core)
    add_test(NAME ${contract} COMMAND ${contract})
    set_tests_properties(${contract} PROPERTIES TIMEOUT 20)
endforeach()

# Native (non-Asio) network backend: builds and passes here with no Boost,
# OpenSSL or spdlog — the proof that the contracts do not require Asio.
set(NATIVE_NET_SOURCES
    "${PR_SOURCE_ROOT}/src/framework/net/native/poller.cpp"
    "${PR_SOURCE_ROOT}/src/framework/net/native/poller_poll.cpp"
    "${PR_SOURCE_ROOT}/src/framework/net/native_runtime.cpp"
    "${PR_SOURCE_ROOT}/src/framework/net/native_stream.cpp"
    "${PR_SOURCE_ROOT}/src/framework/net/native_server_transport.cpp"
    "${PR_SOURCE_ROOT}/src/framework/net/native_client_transport.cpp")
if(APPLE OR CMAKE_SYSTEM_NAME MATCHES "BSD")
    list(APPEND NATIVE_NET_SOURCES "${PR_SOURCE_ROOT}/src/framework/net/native/poller_kqueue.cpp")
endif()
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    list(APPEND NATIVE_NET_SOURCES "${PR_SOURCE_ROOT}/src/framework/net/native/poller_epoll.cpp")
endif()
add_library(protorelay_native_net STATIC ${NATIVE_NET_SOURCES})
target_include_directories(protorelay_native_net PUBLIC "${PR_SOURCE_ROOT}/include")
target_compile_definitions(protorelay_native_net PUBLIC PROTORELAY_HAS_SPDLOG=0 PROTORELAY_LOG_NONE=0)
target_link_libraries(protorelay_native_net PUBLIC protorelay_contract_core Threads::Threads)
foreach(contract native_poller_test native_runtime_test native_transport_test)
    add_executable(${contract} "${PR_SOURCE_ROOT}/test/unit/${contract}.cpp")
    target_link_libraries(${contract} PRIVATE protorelay_native_net)
    add_test(NAME ${contract} COMMAND ${contract})
    set_tests_properties(${contract} PROPERTIES TIMEOUT 120)
endforeach()

add_test(NAME dependency_boundaries COMMAND ${CMAKE_COMMAND} -DPR_SOURCE_ROOT=${PR_SOURCE_ROOT} -P ${CMAKE_CURRENT_LIST_DIR}/check_dependency_boundaries.cmake)
