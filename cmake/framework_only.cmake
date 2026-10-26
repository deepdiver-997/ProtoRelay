# ================================================================
# PROTORELAY_BUILD_MAIL=OFF 分支体（原样拆出；root 侧 if 包裹 + return，include 内不再 return）
# ================================================================
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
        # TLS extension test: only when the OpenSSL-bearing module exists.
        if(TARGET protorelay_native_tls)
            add_executable(native_tls_test test/unit/native_tls_test.cpp)
            target_include_directories(native_tls_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
            target_link_libraries(native_tls_test PRIVATE protorelay_native_tls
                protorelay_native_net OpenSSL::SSL OpenSSL::Crypto)
            add_test(NAME native_tls_test COMMAND native_tls_test)
            set_tests_properties(native_tls_test PROPERTIES TIMEOUT 120)
        endif()
        # QUIC transport backend: only when the vendored third party was built.
        if(TARGET protorelay_quic)
            add_executable(quic_loopback_test test/unit/quic_loopback_test.cpp)
            target_include_directories(quic_loopback_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
            # The prefixed BoringSSL headers must precede any system OpenSSL
            # include path (e.g. Homebrew via nlohmann's interface).
            target_include_directories(quic_loopback_test SYSTEM PRIVATE
                ${CMAKE_SOURCE_DIR}/third_party/quic/boringssl/include
                ${CMAKE_SOURCE_DIR}/third_party/quic/lsquic/include)
            target_link_libraries(quic_loopback_test PRIVATE protorelay_quic protorelay_native_net quic_lsquic)
            add_test(NAME quic_loopback_test COMMAND quic_loopback_test)
            set_tests_properties(quic_loopback_test PROPERTIES TIMEOUT 120)
        endif()
        add_executable(node_link_test test/unit/node_link_test.cpp)
        target_link_libraries(node_link_test PRIVATE protorelay_node_link)
        add_test(NAME node_link_test COMMAND node_link_test)
        set_tests_properties(node_link_test PROPERTIES TIMEOUT 120)
        # Node link v1 (signed handshake / TLS links / epoch config): only when
        # the OpenSSL-bearing auth module exists.
        set(NODE_UNIT_TESTS node_link_test)
        if(TARGET protorelay_node_auth)
            add_executable(node_auth_test test/unit/node_auth_test.cpp)
            target_include_directories(node_auth_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
            target_link_libraries(node_auth_test PRIVATE protorelay_node_auth
                protorelay_node_link protorelay_native_net OpenSSL::SSL OpenSSL::Crypto)
            if(TARGET protorelay_native_tls)
                target_link_libraries(node_auth_test PRIVATE protorelay_native_tls)
                target_compile_definitions(node_auth_test PRIVATE NODE_AUTH_TEST_HAVE_NATIVE_TLS=1)
            endif()
            add_test(NAME node_auth_test COMMAND node_auth_test)
            set_tests_properties(node_auth_test PROPERTIES TIMEOUT 120)
            list(APPEND NODE_UNIT_TESTS node_auth_test)
        endif()
        add_executable(node_proto_test test/unit/node_proto_test.cpp)
        target_link_libraries(node_proto_test PRIVATE protorelay_node_proto)
        if(TARGET OpenSSL::Crypto)
            target_compile_definitions(node_proto_test PRIVATE PR_NODE_PROTO_TEST_AEAD=1)
        endif()
        add_test(NAME node_proto_test COMMAND node_proto_test)
        set_tests_properties(node_proto_test PROPERTIES TIMEOUT 120)
        list(APPEND NODE_UNIT_TESTS node_proto_test)
        # QUIC multiplexing contracts: header-only; must compile with no
        # backend at all -- and without any third-party QUIC/TLS headers.
        # nlohmann is linked only because the test builds a complete
        # TlsConfig (server_transport.h); the contract header itself is
        # std-only.
        add_executable(quic_contract_test test/unit/quic_contract_test.cpp)
        target_include_directories(quic_contract_test PRIVATE "${CMAKE_SOURCE_DIR}/include")
        target_link_libraries(quic_contract_test PRIVATE nlohmann_json::nlohmann_json)
        add_test(NAME quic_contract_test COMMAND quic_contract_test)
        set_tests_properties(quic_contract_test PROPERTIES TIMEOUT 20)
        list(APPEND NODE_UNIT_TESTS quic_contract_test)
        add_custom_target(unit_tests DEPENDS logger_test std_thread_pool_test connection_interface_test server_transport_test metrics_core_test
            native_poller_test native_runtime_test native_transport_test ${NODE_UNIT_TESTS})
        if(TARGET protorelay_native_tls)
            add_dependencies(unit_tests native_tls_test)
        endif()
    endif()
    add_test(NAME dependency_boundaries COMMAND ${CMAKE_COMMAND} -DPR_SOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/test/backend_contract/check_dependency_boundaries.cmake)
    add_test(NAME object_tree_hygiene COMMAND ${CMAKE_COMMAND} -DPR_SOURCE_ROOT=${CMAKE_SOURCE_DIR} -P ${CMAKE_SOURCE_DIR}/test/backend_contract/check_object_tree_hygiene.cmake)
    return()
