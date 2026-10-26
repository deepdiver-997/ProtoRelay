# ================================================================
# CTest 集成 / catch-all 注册 / 属性覆盖 / Fuzz（原样拆出）
# ================================================================
# ================================================================
# CTest 集成
# ================================================================
if(NOT BUILD_OBJECT_ONLY AND BUILD_TESTS)
    enable_testing()

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
    if(HAVE_SQLITE3)
        add_dependencies(unit_tests sqlite_driver_test)
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

# 特殊属性覆盖：在 catch-all 注册之后执行（CMake 卫生任务 2——中段显式
# add_test 已删，注册以 catch-all 扫描为权威；条件编译块内的 add_test+属性对
# （mariadb_async/sqlite_driver/node_auth）原样保留，属性依赖存在性）。
if(NOT BUILD_OBJECT_ONLY AND BUILD_TESTS)
    set_tests_properties(outbound_smoke PROPERTIES TIMEOUT 20)
    set_tests_properties(persist_submission_test PROPERTIES TIMEOUT 20)
    set_tests_properties(smtp_persist_ack_test PROPERTIES TIMEOUT 15)
    set_tests_properties(smtp_client_test PROPERTIES TIMEOUT 20)
    set_tests_properties(smtp_submitter_test PROPERTIES TIMEOUT 15)
    set_tests_properties(server_transport_test PROPERTIES TIMEOUT 20)
    set_tests_properties(logger_test std_thread_pool_test connection_interface_test asio_executor_test PROPERTIES TIMEOUT 20)
    set_tests_properties(asio_server_transport_test PROPERTIES TIMEOUT 20)
    set_tests_properties(pop3_timer_boundary_test asio_error_test PROPERTIES TIMEOUT 20)
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
