# ================================================================
# 测试 / Benchmark（原样拆出，含 pr_add_unit_test）
# ================================================================
# ================================================================
# 测试 / Benchmark（BUILD_TESTS=OFF 时跳过）
# ================================================================
if(NOT BUILD_OBJECT_ONLY AND BUILD_TESTS)
    # ---- CMake 卫生任务 1：数据驱动的一行式单测注册 ----
    # 内部仍走 setup_mail_target，编译/链接语义与手写块一致：
    #   SRCS 缺省 = ${COMMON_SRCS} + test/unit/<name>.cpp（最常见形态）；
    #   LIBS  缺省不额外链接（保持 setup_mail_target 的链接集合）。
    # 特殊块（额外源文件之外的非常规 include/定义/条件编译）保留手写。
    function(pr_add_unit_test NAME)
        cmake_parse_arguments(ARG "" "" "SRCS;LIBS" ${ARGN})
        if(NOT ARG_SRCS)
            set(ARG_SRCS ${COMMON_SRCS} test/unit/${NAME}.cpp)
        endif()
        add_executable(${NAME} ${ARG_SRCS})
        target_include_directories(${NAME} PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
        setup_mail_target(${NAME})
        if(ARG_LIBS)
            target_link_libraries(${NAME} PRIVATE ${ARG_LIBS})
        endif()
    endfunction()

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

    # 测试直接 include traditional_imaps_fsm.tpp，模板在本 TU 实例化；模板体里
    # 的 build_mail_body_key 定义在 protorelay_mail_storage，必须显式链接
    # （COMMON_SRCS 路径的目标自带该 .cpp，不走那条路的目标要补这条依赖）。
    pr_add_unit_test(mime_parser_test
        SRCS test/unit/mime_parser_test.cpp
             src/mail_system/back/mailServer/fsm/imaps/imap_utils.cpp
        LIBS protorelay_mail_storage)

    pr_add_unit_test(mail_body_writer_test SRCS test/unit/mail_body_writer_test.cpp
        LIBS protorelay_mail_storage)

    pr_add_unit_test(buffered_upload_stream_test SRCS test/unit/buffered_upload_stream_test.cpp)

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

    pr_add_unit_test(io_error_test SRCS test/unit/io_error_test.cpp)

    pr_add_unit_test(async_storage_provider_test SRCS test/unit/async_storage_provider_test.cpp
        LIBS protorelay_mail_storage)

    # ---- framework 层单元测试 ----
    # 纯逻辑 / 文件 IO / 线程池：只依赖 framework 头 + 对应 .cpp
    pr_add_unit_test(intrusion_detector_test SRCS test/unit/intrusion_detector_test.cpp)

    pr_add_unit_test(server_config_test SRCS test/unit/server_config_test.cpp)

    pr_add_unit_test(mapped_file_test SRCS test/unit/mapped_file_test.cpp)

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

    pr_add_unit_test(webmail_test
        SRCS ${COMMON_SRCS}
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
             test/unit/webmail_submission_test.cpp)

    pr_add_unit_test(http_session_read_test
        SRCS ${COMMON_SRCS}
             src/framework/http/http_parser.cpp
             src/framework/http/message_processor.cpp
             test/unit/http_session_read_test.cpp)

    pr_add_unit_test(persist_submission_test)

    pr_add_unit_test(smtp_persist_ack_test
        SRCS ${COMMON_SRCS} src/mail_system/back/mailServer/smtps/smtps_server.cpp
             test/unit/smtp_persist_ack_test.cpp)

    add_executable(smtp_client_test test/unit/smtp_client_test.cpp)
    setup_mail_target(smtp_client_test)

    add_executable(smtp_submitter_test
        src/mail_system/back/common/mime_composer.cpp
        src/mail_system/back/webmail/smtp_submitter.cpp
        test/unit/smtp_submitter_test.cpp
    )
    setup_mail_target(smtp_submitter_test)

    pr_add_unit_test(thread_pool_test
        SRCS src/framework/thread_pool/io_thread_pool.cpp
             src/framework/thread_pool/io_context_registry.cpp
             ${WORKER_SRCS}
             test/unit/thread_pool_test.cpp)

    pr_add_unit_test(readiness_waiter_test SRCS test/unit/readiness_waiter_test.cpp
        LIBS protorelay_readiness protorelay_asio)
    add_executable(async_db_adapter_test test/unit/async_db_adapter_test.cpp)
    target_include_directories(async_db_adapter_test PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/test/unit")
    target_link_libraries(async_db_adapter_test PRIVATE Threads::Threads)

    pr_add_unit_test(account_authentication_test SRCS test/unit/account_authentication_test.cpp
        LIBS protorelay_mail_core)
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

    pr_add_unit_test(mail_object_key_layout_test SRCS test/unit/mail_object_key_layout_test.cpp
        LIBS protorelay_mail_storage)

    add_executable(smtp_client_contract_test
        src/mail_system/back/smtp_client/client_session.cpp test/unit/smtp_client_contract_test.cpp)
    target_include_directories(smtp_client_contract_test PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/test/unit")
    target_link_libraries(smtp_client_contract_test PRIVATE protorelay_logging OpenSSL::Crypto nlohmann_json::nlohmann_json)
    add_executable(storage_boundary_test test/unit/storage_boundary_test.cpp)
    target_include_directories(storage_boundary_test PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/test/unit")
    target_link_libraries(storage_boundary_test PRIVATE Threads::Threads)

    add_executable(logger_test test/unit/logger_test.cpp)
    target_link_libraries(logger_test PRIVATE protorelay_logging)

    # This target intentionally has no Boost include/library requirements.
    add_executable(std_thread_pool_test
        src/framework/thread_pool/std_thread_pool.cpp test/unit/std_thread_pool_test.cpp)
    target_link_libraries(std_thread_pool_test PRIVATE protorelay_logging)

    add_executable(connection_interface_test test/unit/connection_interface_test.cpp)
    target_include_directories(connection_interface_test PRIVATE "${CMAKE_SOURCE_DIR}/include")

    # QUIC 多路复用契约：header-only，不链任何后端与第三方 QUIC/TLS 库；
    # nlohmann 仅为在测试里构造完整 TlsConfig（server_transport.h）而链，
    # 契约头本身只依赖标准库。ctest 注册与 unit_tests 聚合由下方
    # catch-all 扫描完成。
    add_executable(quic_contract_test test/unit/quic_contract_test.cpp)
    target_include_directories(quic_contract_test PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_link_libraries(quic_contract_test PRIVATE nlohmann_json::nlohmann_json)

    # Full server/metrics interface exercised without linking any Asio backend.
    add_executable(server_transport_test src/framework/server_base.cpp test/unit/server_transport_test.cpp)
    target_include_directories(server_transport_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    target_link_libraries(server_transport_test PRIVATE protorelay_logging nlohmann_json::nlohmann_json)

    # Native (non-Asio) network backend tests: parallel optional module, the
    # Asio production wiring is untouched. Registered like every other unit
    # test via the catch-all ctest scan below.
    foreach(_test native_poller_test native_runtime_test native_transport_test)
        add_executable(${_test} test/unit/${_test}.cpp)
        target_include_directories(${_test} PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
        target_link_libraries(${_test} PRIVATE protorelay_native_net)
    endforeach()
    # TLS extension: only when the OpenSSL-bearing module exists.
    if(TARGET protorelay_native_tls)
        add_executable(native_tls_test test/unit/native_tls_test.cpp)
        target_include_directories(native_tls_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
        target_link_libraries(native_tls_test PRIVATE protorelay_native_tls
            protorelay_native_net OpenSSL::SSL OpenSSL::Crypto)
    endif()
    # QUIC transport backend: only when the vendored third party was built.
    if(TARGET protorelay_quic)
        add_executable(quic_loopback_test test/unit/quic_loopback_test.cpp)
        target_include_directories(quic_loopback_test PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
        target_include_directories(quic_loopback_test SYSTEM PRIVATE
            ${CMAKE_SOURCE_DIR}/third_party/quic/boringssl/include
            ${CMAKE_SOURCE_DIR}/third_party/quic/lsquic/include)
        target_link_libraries(quic_loopback_test PRIVATE protorelay_quic protorelay_native_net quic_lsquic)
    endif()
    add_executable(node_link_test test/unit/node_link_test.cpp)
    target_link_libraries(node_link_test PRIVATE protorelay_node_link)
    # Node link v1 auth test: same OpenSSL-bearing-module condition as above.
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
    endif()
    add_executable(node_proto_test test/unit/node_proto_test.cpp)
    target_link_libraries(node_proto_test PRIVATE protorelay_node_proto)
    if(TARGET OpenSSL::Crypto)
        target_compile_definitions(node_proto_test PRIVATE PR_NODE_PROTO_TEST_AEAD=1)
    endif()

    add_executable(asio_executor_test test/unit/asio_executor_test.cpp)
    target_include_directories(asio_executor_test PRIVATE "${CMAKE_SOURCE_DIR}/include" SYSTEM ${Boost_INCLUDE_DIRS})
    target_link_libraries(asio_executor_test PRIVATE Threads::Threads protorelay_asio)
    add_executable(asio_error_test test/unit/asio_error_test.cpp)
    target_include_directories(asio_error_test PRIVATE "${CMAKE_SOURCE_DIR}/include" SYSTEM ${Boost_INCLUDE_DIRS})
    target_link_libraries(asio_error_test PRIVATE Threads::Threads)
    if(TARGET OpenSSL::Crypto)
        target_link_libraries(asio_error_test PRIVATE OpenSSL::Crypto)
    endif()
    add_executable(asio_server_transport_test
        src/framework/thread_pool/io_thread_pool.cpp src/framework/thread_pool/io_context_registry.cpp
        test/unit/asio_server_transport_test.cpp)
    target_link_libraries(asio_server_transport_test PRIVATE protorelay_asio protorelay_logging Threads::Threads)

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

    # SQLite 驱动回归：本地文件库（AGENT_TMP_ROOT 下进程独占目录，测试自清理）。
    # 库文件由测试自行创建，无外部服务依赖——77 仅在临时目录不可用时出现。
    if(HAVE_SQLITE3)
        add_executable(sqlite_driver_test test/unit/sqlite_driver_test.cpp)
        target_include_directories(sqlite_driver_test PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/test/unit")
        target_link_libraries(sqlite_driver_test PRIVATE protorelay_db_sqlite protorelay_runtime)
        add_test(NAME sqlite_driver_test COMMAND sqlite_driver_test)
        set_tests_properties(sqlite_driver_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    endif()

    # 会话/FSM/服务基座：TestSession::close() 引用 ServerBase 成员符号
    # （server_base.cpp），须链 COMMON_SRCS（同 smtps/imaps_fsm_test 模式）
    pr_add_unit_test(fsm_base_test)

    pr_add_unit_test(fsm_engine_test LIBS protorelay_framework)

    pr_add_unit_test(fsm_export_test LIBS protorelay_framework)

    pr_add_unit_test(fast_fsm_base_test)

    pr_add_unit_test(session_base_test)

    pr_add_unit_test(server_base_test)

    pr_add_unit_test(outbound_utils_test)

    pr_add_unit_test(starttls_handoff_test)

    pr_add_unit_test(pipeline_deferred_test)

    # MetricsServer 核心 API 不变量测试（counter/gauge/histogram 语义）。
    # 不链 COMMON_SRCS：MetricsServer 是 header-only，只依赖 boost::asio。
    pr_add_unit_test(metrics_core_test SRCS test/unit/metrics_core_test.cpp)

    add_executable(fsm_bench
        ${COMMON_SRCS}
        src/mail_system/back/mailServer/smtps/smtps_server.cpp
        test/bench/fsm/fsm_bench.cpp
    )
    target_include_directories(fsm_bench PRIVATE "${CMAKE_SOURCE_DIR}/test/unit")
    setup_mail_target(fsm_bench)

    pr_add_unit_test(smtps_fsm_test
        SRCS ${COMMON_SRCS} src/mail_system/back/mailServer/smtps/smtps_server.cpp
             test/unit/smtps_fsm_test.cpp)

    # 异步/并发 FSM 测试（Mock DNS/DB 延迟回调 + 真实 IO 线程）
    pr_add_unit_test(smtps_fsm_concurrency_test
        SRCS ${COMMON_SRCS} src/mail_system/back/mailServer/smtps/smtps_server.cpp
             test/unit/smtps_fsm_concurrency_test.cpp)
    if(ENABLE_TSAN)
        target_compile_options(smtps_fsm_test PRIVATE -fsanitize=thread -g -O1)
        target_link_options(smtps_fsm_test PRIVATE -fsanitize=thread)
        target_compile_options(smtps_fsm_concurrency_test PRIVATE -fsanitize=thread -g -O1)
        target_link_options(smtps_fsm_concurrency_test PRIVATE -fsanitize=thread)
    endif()


    pr_add_unit_test(imaps_fsm_test
        SRCS ${COMMON_SRCS} src/mail_system/back/mailServer/imaps/imaps_server.cpp
             test/unit/imaps_fsm_test.cpp)

    pr_add_unit_test(pop3_fsm_test
        SRCS ${COMMON_SRCS} src/mail_system/back/mailServer/pop3/pop3_server.cpp
             test/unit/pop3_fsm_test.cpp)

    pr_add_unit_test(pop3_timer_boundary_test
        SRCS ${COMMON_SRCS} src/mail_system/back/mailServer/pop3/pop3_server.cpp
             test/unit/pop3_timer_boundary_test.cpp)

    add_executable(smtp_client test/bench/smtp/smtp_client.cpp)
    target_compile_features(smtp_client PRIVATE cxx_std_17)
    add_executable(imap_client test/bench/imap/imap_client.cpp)
    target_compile_features(imap_client PRIVATE cxx_std_17)
    add_executable(pop3_client test/bench/pop3/pop3_client.cpp)
    target_compile_features(pop3_client PRIVATE cxx_std_17)

    add_executable(dispatch_bench test/bench/fsm/dispatch_bench.cpp)
    target_compile_features(dispatch_bench PRIVATE cxx_std_17)

    pr_add_unit_test(outbound_smoke)
endif()
