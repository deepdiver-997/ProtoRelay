# Guard dependency direction using source-level includes, including header-only code.
if(NOT DEFINED PR_SOURCE_ROOT)
    get_filename_component(PR_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()
file(GLOB_RECURSE framework_files
    "${PR_SOURCE_ROOT}/include/framework/*.h" "${PR_SOURCE_ROOT}/include/framework/*.hpp"
    "${PR_SOURCE_ROOT}/include/framework/*.tpp" "${PR_SOURCE_ROOT}/src/framework/*.cpp")
foreach(path IN LISTS framework_files)
    # Kept solely as an application-facing compatibility include.
    if(path STREQUAL "${PR_SOURCE_ROOT}/include/framework/server_config.h")
        continue()
    endif()
    file(READ "${path}" source)
    if(source MATCHES "#[ \t]*include[ \t]*[<\"]mail_system/")
        message(FATAL_ERROR "Framework depends on the mail application: ${path}")
    endif()
endforeach()
foreach(header
    mail_system/back/smtp_client/client_session.h
    mail_system/back/submission/mail_submission_service.h
    mail_system/back/webmail/smtp_submitter.h)
    file(READ "${PR_SOURCE_ROOT}/include/${header}" source)
    if(source MATCHES "boost::|#[ \t]*include[ \t]*[<\"]boost/")
        message(FATAL_ERROR "Public client API exposes the Asio backend: ${header}")
    endif()
endforeach()
# The Webmail service orchestrates typed reads and body loading; SQL/result
# access belongs to the injected repository implementation.
file(READ "${PR_SOURCE_ROOT}/include/mail_system/back/webmail/mail_service.h" webmail_service)
if(webmail_service MATCHES "db::sql::|IDBResult|->(async_query|async_execute)[ \t]*[(]")
    message(FATAL_ERROR "Webmail service bypasses its read repository")
endif()
if(webmail_service MATCHES "#[ \t]*include[ \t]*[<\"]mail_system/back/db/")
    message(FATAL_ERROR "Webmail service directly includes the database layer")
endif()
file(GLOB_RECURSE repository_headers "${PR_SOURCE_ROOT}/include/mail_system/back/*repository.h")
foreach(path IN LISTS repository_headers)
    file(READ "${path}" source)
    if(source MATCHES "boost::|#[ \t]*include[ \t]*[<\"](boost/|mysql/|mariadb/)")
        message(FATAL_ERROR "Repository API exposes a concrete backend: ${path}")
    endif()
    if(source MATCHES "#[ \t]*include[ \t]*[<\"][^>\"]*sql_queries")
        message(FATAL_ERROR "Repository API includes SQL builders: ${path}")
    endif()
endforeach()
# SQL/result mapping is confined to explicit persistence and routing adapters.
file(GLOB_RECURSE application_files
    "${PR_SOURCE_ROOT}/include/mail_system/back/*.h"
    "${PR_SOURCE_ROOT}/include/mail_system/back/*.hpp"
    "${PR_SOURCE_ROOT}/include/mail_system/back/*.tpp"
    "${PR_SOURCE_ROOT}/src/mail_system/back/*.cpp")
foreach(path IN LISTS application_files)
    if(path MATCHES "/back/db/" OR path MATCHES "/back/repository/"
       OR path MATCHES "/back/mailServer/outbound/outbox_repository[.]cpp$"
       OR path MATCHES "/back/router/table_shard_router[.]cpp$")
        continue()
    endif()
    file(READ "${path}" source)
    if(source MATCHES "db::sql::|IDBResult|->(async_query|async_execute|query|execute)[ \t]*[(]")
        message(FATAL_ERROR "Application bypasses its persistence boundary: ${path}")
    endif()
    if(source MATCHES "#[ \t]*include[ \t]*[<\"][^>\"]*sql_queries")
        message(FATAL_ERROR "Application includes SQL builders: ${path}")
    endif()
endforeach()
# Business APIs may use abstract driver/runtime contracts, never backend headers.
file(GLOB_RECURSE mail_headers
    "${PR_SOURCE_ROOT}/include/mail_system/back/*.h"
    "${PR_SOURCE_ROOT}/include/mail_system/back/*.hpp"
    "${PR_SOURCE_ROOT}/include/mail_system/back/*.tpp")
foreach(path IN LISTS mail_headers)
    # Explicit driver and DNS adapter APIs intentionally know their own SDK.
    if(path MATCHES "/back/db/" OR path MATCHES "/cares_dns_resolver[.]h$")
        continue()
    endif()
    file(READ "${path}" source)
    if(source MATCHES "#[ \t]*include[ \t]*[<\"](boost/|mysql/|mariadb/|framework/net/asio_|framework/thread_pool/io_thread_pool|mail_system/back/db/(mysql_|mariadb_|distributed_mysql_))")
        message(FATAL_ERROR "Business API includes a concrete backend: ${path}")
    endif()
endforeach()
file(GLOB_RECURSE storage_files
    "${PR_SOURCE_ROOT}/include/framework/storage/*.h"
    "${PR_SOURCE_ROOT}/src/framework/storage/*.cpp")
foreach(path IN LISTS storage_files)
    file(READ "${path}" source)
    if(source MATCHES "build_mail_body_key|build_attachment_key|mail_path|attachment_path|attachment_seq_|mail_root_|attachment_root_")
        message(FATAL_ERROR "Generic storage contains mail layout policy: ${path}")
    endif()
endforeach()
message(STATUS "Framework, client and repository dependency boundaries hold")
