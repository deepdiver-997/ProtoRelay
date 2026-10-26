# ================================================================
# 邮件协议/服务器目标（原样拆出）
# ================================================================
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
