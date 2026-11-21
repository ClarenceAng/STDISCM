#pragma once

// Reads the TLS files that run.bat / run.sh make with mkcert in certs/ and that
// docker-compose.yml mounts into the containers. Every check says which file and
// why, because gRPC itself only reports that a secure port or channel failed.

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "error_text.h"

// OpenSSL's reasons for the last failure, oldest first. This empties its error queue.
inline std::string openSslErrors() {
    std::string text;
    while (const unsigned long code = ERR_get_error()) {
        char buffer[256];
        ERR_error_string_n(code, buffer, sizeof(buffer));
        if (!text.empty()) text += "; ";
        text += buffer;
    }
    return text.empty() ? std::string("OpenSSL gave no reason") : text;
}

// The whole file. `what` names it in errors, for example "the consumer's TLS certificate".
inline std::string readPemFile(const std::filesystem::path& path, const std::string& what) {
    const std::string made = ". run.bat or run.sh makes it with mkcert before starting the containers";
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) throw std::runtime_error("could not check " + what + " " + path.string() + ": " + error.message());
    if (!exists) throw std::runtime_error(what + " " + path.string() + " does not exist" + made);
    if (std::filesystem::is_directory(path, error)) {
        // Docker Desktop mounts a missing file as an empty folder it makes in its place
        throw std::runtime_error(what + " " + path.string() + " is a folder, not a file. Docker makes an empty folder in place "
                                 "of a file it was told to mount that didn't exist yet" + made);
    }

    errno = 0;
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("could not open " + what + " " + path.string() + ": " + lastErrorText());
    errno = 0;
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) throw std::runtime_error("could not read " + what + " " + path.string() + ": " + lastErrorText());

    if (text.empty()) throw std::runtime_error(what + " " + path.string() + " is empty" + made);
    if (text.find("-----BEGIN ") == std::string::npos) {
        throw std::runtime_error(what + " " + path.string() + " is not a PEM file (no -----BEGIN line)");
    }
    return text;
}

namespace tls_detail {

struct FreeBio {
    void operator()(BIO* bio) const { BIO_free(bio); }
};
struct FreeX509 {
    void operator()(X509* certificate) const { X509_free(certificate); }
};
struct FreeKey {
    void operator()(EVP_PKEY* key) const { EVP_PKEY_free(key); }
};

using Bio = std::unique_ptr<BIO, FreeBio>;
using Certificate = std::unique_ptr<X509, FreeX509>;
using Key = std::unique_ptr<EVP_PKEY, FreeKey>;

inline Bio memoryBio(const std::string& text, const std::string& what) {
    Bio bio(BIO_new_mem_buf(text.data(), static_cast<int>(text.size())));
    if (!bio) throw std::runtime_error("OpenSSL could not take in " + what + ": " + openSslErrors());
    return bio;
}

// Reading past the last certificate in a PEM file always leaves a "no start line"
// error, so that one only means the file has ended.
inline bool onlyReachedTheEnd() {
    const unsigned long last = ERR_peek_last_error();
    return ERR_GET_LIB(last) == ERR_LIB_PEM && ERR_GET_REASON(last) == PEM_R_NO_START_LINE;
}

}  // namespace tls_detail

// Throws unless every block in the PEM text is a certificate OpenSSL can read and
// there is at least one. For the certificate authority a client trusts.
inline void checkCertificates(const std::string& pem, const std::string& what) {
    using namespace tls_detail;
    ERR_clear_error();
    Bio bio = memoryBio(pem, what);
    int count = 0;
    while (Certificate certificate{PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)}) count++;
    if (count > 0 && onlyReachedTheEnd()) {
        ERR_clear_error();
        return;
    }
    throw std::runtime_error(what + " holds " + (count == 0 ? "no certificate" : "something after its certificates") +
                             " that OpenSSL can read: " + openSslErrors());
}

// Puts a certificate chain and its private key into a TLS context, checking that
// they belong together. Throws with OpenSSL's reason if not.
inline void useCertificateAndKey(SSL_CTX& context, const std::string& certificatePem, const std::string& keyPem,
                                 const std::string& certificateWhat, const std::string& keyWhat) {
    using namespace tls_detail;
    ERR_clear_error();

    Bio certificates = memoryBio(certificatePem, certificateWhat);
    Certificate leaf{PEM_read_bio_X509(certificates.get(), nullptr, nullptr, nullptr)};
    if (!leaf) throw std::runtime_error(certificateWhat + " holds no certificate OpenSSL can read: " + openSslErrors());
    if (SSL_CTX_use_certificate(&context, leaf.get()) != 1) {
        throw std::runtime_error(certificateWhat + " can't be used: " + openSslErrors());
    }
    // a certificate signed by an intermediate authority comes with it in the same file
    while (Certificate intermediate{PEM_read_bio_X509(certificates.get(), nullptr, nullptr, nullptr)}) {
        if (SSL_CTX_add1_chain_cert(&context, intermediate.get()) != 1) {
            throw std::runtime_error("an intermediate certificate in " + certificateWhat + " can't be used: " + openSslErrors());
        }
    }
    if (!onlyReachedTheEnd()) {
        throw std::runtime_error(certificateWhat + " has something after its certificate that OpenSSL can't read: " + openSslErrors());
    }
    ERR_clear_error();

    Bio keyBio = memoryBio(keyPem, keyWhat);
    Key key{PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr)};
    if (!key) throw std::runtime_error(keyWhat + " holds no private key OpenSSL can read: " + openSslErrors());
    // checked on its own first, as OpenSSL would otherwise only say "key values mismatch"
    if (X509_check_private_key(leaf.get(), key.get()) != 1) {
        ERR_clear_error();
        throw std::runtime_error(keyWhat + " is not the key of " + certificateWhat +
                                 ", they were made separately (run run.bat or run.sh again to make a new pair)");
    }
    if (SSL_CTX_use_PrivateKey(&context, key.get()) != 1 || SSL_CTX_check_private_key(&context) != 1) {
        throw std::runtime_error(keyWhat + " can't be used: " + openSslErrors());
    }
}

// The consumer's certificate and private key, read once and used by both the
// HTTPS GUI and the gRPC port, so the two can't disagree.
struct ServerTls {
    std::string certificatePem;
    std::string privateKeyPem;
    std::string certificateWhat;  // "the TLS certificate /certs/cert.pem", for messages
    std::string keyWhat;
};

// Throws if either file can't be read or they don't belong together.
inline ServerTls loadServerTls(const std::filesystem::path& certificatePath, const std::filesystem::path& keyPath) {
    ServerTls tls;
    tls.certificatePem = readPemFile(certificatePath, "the TLS certificate");
    tls.privateKeyPem = readPemFile(keyPath, "the TLS private key");
    tls.certificateWhat = "the TLS certificate " + certificatePath.string();
    tls.keyWhat = "the TLS private key " + keyPath.string();

    // checked in a throwaway context, so a bad pair stops the start-up before anything listens
    std::unique_ptr<SSL_CTX, void (*)(SSL_CTX*)> check(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    if (!check) throw std::runtime_error("OpenSSL could not create a TLS context: " + openSslErrors());
    useCertificateAndKey(*check, tls.certificatePem, tls.privateKeyPem, tls.certificateWhat, tls.keyWhat);
    return tls;
}
