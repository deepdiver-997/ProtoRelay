#include "framework/node/node_auth.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

namespace pr::node::auth {
namespace {
using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

KeyPtr read_private_key(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) throw std::runtime_error("node auth: out of memory");
    EVP_PKEY* raw = nullptr;
    if (!PEM_read_bio_PrivateKey(bio, &raw, nullptr, nullptr)) {
        BIO_free(bio);
        throw std::runtime_error("node auth: unreadable private key PEM");
    }
    BIO_free(bio);
    return KeyPtr(raw, &EVP_PKEY_free);
}

KeyPtr generate_key() {
    EVP_PKEY* raw = nullptr;
    EVP_PKEY_CTX* context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!context) throw std::runtime_error("node auth: out of memory");
    const bool ok = EVP_PKEY_keygen_init(context) == 1 && EVP_PKEY_keygen(context, &raw) == 1;
    EVP_PKEY_CTX_free(context);
    if (!ok) throw std::runtime_error("node auth: Ed25519 keygen failed");
    return KeyPtr(raw, &EVP_PKEY_free);
}

std::string raw_public_key(const EVP_PKEY* key) {
    size_t length = 0;
    if (EVP_PKEY_get_raw_public_key(key, nullptr, &length) != 1 || length != 32)
        throw std::runtime_error("node auth: not an Ed25519 key");
    std::string out(length, '\0');
    if (EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char*>(out.data()), &length) != 1)
        throw std::runtime_error("node auth: raw public key export failed");
    return out;
}

std::string pem_private_key(const EVP_PKEY* key) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) throw std::runtime_error("node auth: out of memory");
    if (PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
        BIO_free(bio);
        throw std::runtime_error("node auth: private key PEM export failed");
    }
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    std::string out(data, static_cast<size_t>(size));
    BIO_free(bio);
    return out;
}

KeyPtr from_raw_public_key(const std::string& raw) {
    if (raw.size() != 32) return KeyPtr(nullptr, &EVP_PKEY_free);
    EVP_PKEY* raw_key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
        reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
    return KeyPtr(raw_key, &EVP_PKEY_free);
}
} // namespace

Ed25519KeyPair generate_ed25519_key_pair() {
    const auto key = generate_key();
    return Ed25519KeyPair{raw_public_key(key.get()), pem_private_key(key.get())};
}

void store_ed25519_private_key(const Ed25519KeyPair& key, const std::string& pem_path) {
    {
        std::ofstream out(pem_path, std::ios::binary | std::ios::trunc);
        if (!out.write(key.private_key_pem.data(), static_cast<std::streamsize>(key.private_key_pem.size())))
            throw std::runtime_error("node auth: cannot write key file " + pem_path);
    }
    if (::chmod(pem_path.c_str(), 0600) != 0)
        throw std::runtime_error("node auth: cannot restrict key file permissions");
}

Ed25519KeyPair load_ed25519_key_pair(const std::string& pem_path) {
    struct stat info = {};
    if (::stat(pem_path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
        throw std::runtime_error("node auth: key file is not a regular file: " + pem_path);
    if (info.st_mode & (S_IRWXG | S_IRWXO))
        throw std::runtime_error("node auth: key file permissions too open (want 0600): " + pem_path);
    if (!(info.st_mode & S_IRUSR))
        throw std::runtime_error("node auth: key file not owner-readable: " + pem_path);
    std::ifstream in(pem_path, std::ios::binary);
    if (!in) throw std::runtime_error("node auth: cannot read key file " + pem_path);
    std::string pem((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto key = read_private_key(pem);
    if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_ED25519)
        throw std::runtime_error("node auth: key file does not hold an Ed25519 key");
    return Ed25519KeyPair{raw_public_key(key.get()), pem};
}

std::string sign(const std::string& private_key_pem, const std::string& message) {
    const auto key = read_private_key(private_key_pem);
    if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_ED25519)
        throw std::runtime_error("node auth: signing requires an Ed25519 key");
    std::string signature(64, '\0');
    size_t length = signature.size();
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) throw std::runtime_error("node auth: out of memory");
    const bool ok = EVP_DigestSignInit(context, nullptr, nullptr, nullptr, key.get()) == 1 &&
                    EVP_DigestSign(context, reinterpret_cast<unsigned char*>(signature.data()), &length,
                                   reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
    EVP_MD_CTX_free(context);
    if (!ok || length != signature.size()) throw std::runtime_error("node auth: signing failed");
    return signature;
}

bool verify(const std::string& raw_public_key, const std::string& message, const std::string& signature) {
    if (signature.size() != 64) return false;
    const auto key = from_raw_public_key(raw_public_key);
    if (!key) return false;
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) return false;
    const bool ok = EVP_DigestVerifyInit(context, nullptr, nullptr, nullptr, key.get()) == 1 &&
                    EVP_DigestVerify(context, reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
                                     reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
    EVP_MD_CTX_free(context);
    return ok;
}

NodeCredentials make_credentials(const Ed25519KeyPair& own) {
    NodeCredentials credentials;
    credentials.sign = [pem = own.private_key_pem](const std::string& message) { return sign(pem, message); };
    credentials.verify = [](const std::string& key, const std::string& message, const std::string& signature) {
        return verify(key, message, signature);
    };
    return credentials;
}
} // namespace pr::node::auth
