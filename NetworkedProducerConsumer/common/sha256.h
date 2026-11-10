#pragma once

#include <openssl/evp.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

// Incremental SHA-256, so a whole video never has to be held in memory.
class Sha256 {
public:
    Sha256() : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
        if (!context_ || EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1) {
            throw std::runtime_error("could not start SHA-256");
        }
    }

    void update(const void* data, std::size_t size) {
        if (EVP_DigestUpdate(context_.get(), data, size) != 1) {
            throw std::runtime_error("could not update SHA-256");
        }
    }

    // lowercase hex digest; the object can't be used again after this
    std::string finishHex() {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        if (EVP_DigestFinal_ex(context_.get(), digest, &length) != 1) {
            throw std::runtime_error("could not finish SHA-256");
        }
        static const char* digits = "0123456789abcdef";
        std::string hex;
        hex.reserve(length * 2);
        for (unsigned int i = 0; i < length; i++) {
            hex += digits[digest[i] >> 4];
            hex += digits[digest[i] & 0x0f];
        }
        return hex;
    }

private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
};
