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
foreach(service
    include/mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.h
    include/mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.tpp
    src/mail_system/back/webmail/mail_actions.cpp
    src/mail_system/back/webmail/session_store.cpp)
    file(READ "${PR_SOURCE_ROOT}/${service}" source)
    if(source MATCHES "db::sql::|IDBResult|->(async_query|async_execute|query|execute)[ \t]*[(]")
        message(FATAL_ERROR "Application service bypasses its repository: ${service}")
    endif()
    if(source MATCHES "#[ \t]*include[ \t]*[<\"][^>\"]*sql_queries")
        message(FATAL_ERROR "Application service includes SQL builders: ${service}")
    endif()
endforeach()
message(STATUS "Framework, client and repository dependency boundaries hold")
