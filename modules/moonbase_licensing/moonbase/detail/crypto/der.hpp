#pragma once

// Minimal DER/TLV reader, just enough to normalize an RSA public key into
// PKCS#1 `RSAPublicKey` form and (for the Windows CNG backend) split it into
// its modulus and exponent. Shared by the Apple and Windows crypto backends so
// they accept exactly the same key inputs the OpenSSL backend does: PEM SPKI
// (`-----BEGIN PUBLIC KEY-----`), PEM PKCS#1 (`-----BEGIN RSA PUBLIC KEY-----`),
// and raw base64 of either DER encoding.
//
// The OpenSSL backend does not use this file — it lets OpenSSL parse the key.

#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "moonbase/detail/base64.hpp"
#include "moonbase/errors.hpp"

namespace moonbase::detail::crypto::der {

struct cursor {
    const unsigned char* p;
    const unsigned char* end;
};

struct tlv {
    unsigned char tag;
    const unsigned char* content;
    std::size_t length;
};

inline unsigned char read_byte(cursor& c)
{
    if (c.p >= c.end) {
        throw license_invalid_error("Public key is not a supported RSA public key");
    }
    return *c.p++;
}

// Definite-length encoding, short and long form. RSA-2048 keys push the inner
// structures past 127 bytes, so long-form (0x82 hi lo) is the common case, not
// a corner case — handle it or real keys mis-parse.
inline std::size_t read_length(cursor& c)
{
    const unsigned char first = read_byte(c);
    if ((first & 0x80U) == 0) {
        return first;
    }
    const unsigned count = first & 0x7FU;
    if (count == 0 || count > sizeof(std::size_t)) {
        throw license_invalid_error("Public key is not a supported RSA public key");
    }
    std::size_t length = 0;
    for (unsigned i = 0; i < count; ++i) {
        length = (length << 8U) | read_byte(c);
    }
    return length;
}

inline tlv read_tlv(cursor& c)
{
    const unsigned char tag = read_byte(c);
    const std::size_t length = read_length(c);
    // Bounds-check before advancing: a short- or long-form length larger than
    // the bytes that remain is a malformed key, not a licence to read past the
    // decoded buffer.
    if (static_cast<std::size_t>(c.end - c.p) < length) {
        throw license_invalid_error("Public key is not a supported RSA public key");
    }
    const unsigned char* content = c.p;
    c.p += length;
    return tlv{tag, content, length};
}

// Strip the PEM armor (if any) and base64-decode to raw DER bytes.
inline std::vector<unsigned char> decode_key_bytes(const std::string& key_material)
{
    if (key_material.find("-----BEGIN") != std::string::npos) {
        std::string body;
        std::istringstream stream(key_material);
        std::string line;
        bool inside = false;
        while (std::getline(stream, line)) {
            if (line.find("-----BEGIN") != std::string::npos) {
                inside = true;
                continue;
            }
            if (line.find("-----END") != std::string::npos) {
                break;
            }
            if (inside) {
                body += line;
            }
        }
        return base64_decode(body);
    }
    return base64_decode(key_material);
}

// Normalize any accepted key shape to PKCS#1 `RSAPublicKey` DER
// (`SEQUENCE { INTEGER n, INTEGER e }`).
inline std::vector<unsigned char> normalize_to_pkcs1(const std::string& key_material)
{
    std::vector<unsigned char> der = decode_key_bytes(key_material);
    if (der.empty()) {
        throw license_invalid_error("Public key is not a supported RSA public key");
    }

    const unsigned char* begin = der.data();
    cursor top{der.data(), der.data() + der.size()};
    const tlv outer = read_tlv(top);
    if (outer.tag != 0x30) {
        throw license_invalid_error("Public key is not a supported RSA public key");
    }
    const std::size_t outer_total = static_cast<std::size_t>(top.p - begin);

    // Peek the first element inside the outer SEQUENCE to discriminate.
    cursor inner{outer.content, outer.content + outer.length};
    cursor peek = inner;
    const tlv first = read_tlv(peek);

    if (first.tag == 0x02) {
        // INTEGER first => already PKCS#1 RSAPublicKey. Return the outer TLV.
        return std::vector<unsigned char>(der.begin(),
                                          der.begin() + static_cast<std::ptrdiff_t>(outer_total));
    }

    if (first.tag == 0x30) {
        // SEQUENCE first => SPKI: SEQUENCE { AlgorithmIdentifier, BIT STRING }.
        read_tlv(inner); // skip AlgorithmIdentifier
        const tlv bitstring = read_tlv(inner);
        if (bitstring.tag != 0x03 || bitstring.length < 1) {
            throw license_invalid_error("Public key is not a supported RSA public key");
        }
        // First content byte of a BIT STRING is the unused-bit count (0 here);
        // the remainder is the embedded PKCS#1 RSAPublicKey DER.
        const unsigned char* pk = bitstring.content + 1;
        const std::size_t pk_len = bitstring.length - 1;
        return std::vector<unsigned char>(pk, pk + pk_len);
    }

    throw license_invalid_error("Public key is not a supported RSA public key");
}

struct rsa_components {
    std::vector<unsigned char> modulus;  // big-endian, sign byte stripped
    std::vector<unsigned char> exponent; // big-endian, sign byte stripped
};

// Split PKCS#1 `SEQUENCE { INTEGER n, INTEGER e }` into raw big-endian (n, e),
// stripping the leading 0x00 sign byte DER prepends when the high bit is set.
inline rsa_components parse_rsa_pkcs1(const std::vector<unsigned char>& pkcs1)
{
    cursor top{pkcs1.data(), pkcs1.data() + pkcs1.size()};
    const tlv seq = read_tlv(top);
    if (seq.tag != 0x30) {
        throw license_invalid_error("Public key is not a supported RSA public key");
    }
    cursor in{seq.content, seq.content + seq.length};
    const tlv modulus = read_tlv(in);
    const tlv exponent = read_tlv(in);
    if (modulus.tag != 0x02 || exponent.tag != 0x02) {
        throw license_invalid_error("Public key is not a supported RSA public key");
    }

    const auto strip = [](const unsigned char* p, std::size_t len) {
        while (len > 1 && p[0] == 0x00) {
            ++p;
            --len;
        }
        return std::vector<unsigned char>(p, p + len);
    };

    return rsa_components{strip(modulus.content, modulus.length),
                          strip(exponent.content, exponent.length)};
}

} // namespace moonbase::detail::crypto::der
