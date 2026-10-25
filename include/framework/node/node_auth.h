#ifndef PR_FRAMEWORK_NODE_NODE_AUTH_H
#define PR_FRAMEWORK_NODE_NODE_AUTH_H
// OpenSSL-backed Ed25519 credentials for the node link signed handshake.
// Optional module (protorelay_node_auth): node_link.h itself carries only the
// transport-independent NodeCredentials seam, so OpenSSL-free builds never
// touch this header. Selection note: Ed25519 via the OpenSSL EVP API keeps the
// dependency footprint at the OpenSSL already required by the TLS adapters
// (no new third-party library), with small keys/signatures (32/64 bytes),
// deterministic signing, and fast verification.
#include "framework/node/node_link.h"

#include <string>

namespace pr::node::auth {

// Raw 32-byte public key plus the unencrypted PEM private key.
struct Ed25519KeyPair {
    std::string public_key;
    std::string private_key_pem;
};

// In-process key generation (tests and tools; no real key material ships).
// Throws std::runtime_error on OpenSSL failure.
Ed25519KeyPair generate_ed25519_key_pair();

// Writes the private key PEM with owner-only permissions (0600). Throws
// std::runtime_error when the file cannot be created or chmod'ed.
void store_ed25519_private_key(const Ed25519KeyPair& key, const std::string& pem_path);

// Loads an Ed25519 private key from a PEM file. Key hygiene: refuses any file
// whose permissions reach group/other (expects 0600) or that is not a regular
// file. Throws std::runtime_error on bad permissions, unreadable file, or a
// non-Ed25519 key.
Ed25519KeyPair load_ed25519_key_pair(const std::string& pem_path);

// One-shot Ed25519 signing (Ed25519 has no streaming API). Returns the raw
// 64-byte signature; throws std::runtime_error on a malformed key.
std::string sign(const std::string& private_key_pem, const std::string& message);

// Ed25519 verification against a raw 32-byte public key; false on any
// malformation (never throws).
bool verify(const std::string& raw_public_key, const std::string& message, const std::string& signature);

// Wires the free functions into the transport-independent NodeCredentials seam.
NodeCredentials make_credentials(const Ed25519KeyPair& own);

} // namespace pr::node::auth
#endif
