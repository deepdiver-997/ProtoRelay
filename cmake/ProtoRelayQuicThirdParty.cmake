# Optional QUIC third party: vendored lsquic + BoringSSL with a BORINGSSL_
# PREFIX symbol rename (see third_party/quic/MANIFEST.md and
# docs/architecture/quic-backend.md). The libraries are built once per
# checkout following the MANIFEST recipe; when the artifacts are
# present this file exposes an IMPORTED target and the protorelay_quic
# module gets compiled — otherwise the whole module is absent, exactly like
# the OpenSSL-gated protorelay_native_tls.
#
# The prefix define rides on the INTERFACE compile definitions: every
# consumer (module, tests) that includes <openssl/*.h> must see the same
# renamed declarations the libraries were built against. The BoringSSL
# include directory precedes system paths on purpose — nothing else in this
# build may resolve <openssl/ssl.h> to the system OpenSSL.
set(PROTORELAY_QUIC_THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party/quic")
set(PROTORELAY_QUIC_BORINGSSL_PREFIX "pr_bssl_")

set(_quic_lsquic_lib "${PROTORELAY_QUIC_THIRD_PARTY_DIR}/lsquic/build/src/liblsquic/liblsquic.a")
set(_quic_ssl_lib "${PROTORELAY_QUIC_THIRD_PARTY_DIR}/boringssl/build/libssl.a")
set(_quic_crypto_lib "${PROTORELAY_QUIC_THIRD_PARTY_DIR}/boringssl/build/libcrypto.a")

if(EXISTS "${_quic_lsquic_lib}" AND EXISTS "${_quic_ssl_lib}" AND EXISTS "${_quic_crypto_lib}")
    add_library(quic_lsquic STATIC IMPORTED)
    set_target_properties(quic_lsquic PROPERTIES
        IMPORTED_LOCATION "${_quic_lsquic_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${PROTORELAY_QUIC_THIRD_PARTY_DIR}/lsquic/include;${PROTORELAY_QUIC_THIRD_PARTY_DIR}/boringssl/include"
        INTERFACE_COMPILE_DEFINITIONS "BORINGSSL_PREFIX=${PROTORELAY_QUIC_BORINGSSL_PREFIX}"
        INTERFACE_LINK_LIBRARIES "${_quic_ssl_lib};${_quic_crypto_lib};-lz")
    set(PROTORELAY_HAVE_QUIC_THIRD_PARTY TRUE)
endif()
