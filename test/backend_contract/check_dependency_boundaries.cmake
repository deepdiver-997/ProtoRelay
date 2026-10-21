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
message(STATUS "Framework/application and client/backend include boundaries hold")
