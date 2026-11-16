# ================================================================
# install(EXPORT) + ProtoRelayConfig 包（find_package(ProtoRelay) 路线）。
#
# add_subdirectory / FetchContent 消费者不经过这里也能用（目标直接可见）；
# 本文件服务的是安装态：`cmake --install` 后第三方项目 find_package 消费
# 预编译库。框架层模块全部进导出集；mail 应用层模块与 protorelay_quic
# （依赖 vendored 产物）不导出——后者走 add_subdirectory 消费。
#
# 消毒构建才导出：sanitizer/coverage 的链接选项已烙在目标的 PUBLIC link
# options 里，导出会污染消费者。
# ================================================================
if(BUILD_OBJECT_ONLY OR ENABLE_TSAN OR ENABLE_ASAN OR ENABLE_ASAN_SERVER OR ENABLE_COVERAGE)
    return()
endif()

set(PROTORELAY_EXPORT_TARGETS
    protorelay_logging
    protorelay_framework
    protorelay_readiness
    protorelay_runtime
    protorelay_native_net
    protorelay_node_link
    protorelay_node_proto
    protorelay_http
    protorelay_h2)
set(PROTORELAY_INSTALL_NEED_BOOST 0)
if(TARGET protorelay_asio)
    list(APPEND PROTORELAY_EXPORT_TARGETS protorelay_asio)
    set(PROTORELAY_INSTALL_NEED_BOOST 1)
endif()
set(PROTORELAY_INSTALL_NEED_OPENSSL 0)
if(TARGET protorelay_native_tls)
    list(APPEND PROTORELAY_EXPORT_TARGETS protorelay_native_tls)
    set(PROTORELAY_INSTALL_NEED_OPENSSL 1)
endif()
if(TARGET protorelay_node_auth)
    list(APPEND PROTORELAY_EXPORT_TARGETS protorelay_node_auth)
    set(PROTORELAY_INSTALL_NEED_OPENSSL 1)
endif()
set(PROTORELAY_INSTALL_NEED_SPDLOG 0)
if(PROTORELAY_LOG_BACKEND STREQUAL "SPDLOG")
    set(PROTORELAY_INSTALL_NEED_SPDLOG 1)
endif()

install(TARGETS ${PROTORELAY_EXPORT_TARGETS} EXPORT ProtoRelayTargets
        ARCHIVE DESTINATION lib)
install(EXPORT ProtoRelayTargets
        FILE ProtoRelayTargets.cmake
        NAMESPACE protorelay::
        DESTINATION lib/cmake/ProtoRelay)
install(DIRECTORY include/framework DESTINATION include)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/ProtoRelayConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/generated/ProtoRelayConfig.cmake"
    INSTALL_DESTINATION lib/cmake/ProtoRelay)
write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/generated/ProtoRelayConfigVersion.cmake"
    VERSION ${PROTORELAY_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${PROJECT_BINARY_DIR}/generated/ProtoRelayConfig.cmake"
    "${PROJECT_BINARY_DIR}/generated/ProtoRelayConfigVersion.cmake"
    DESTINATION lib/cmake/ProtoRelay)
