#include "record_codec.h"

#include <algorithm>
#include <openssl/evp.h>

namespace waste::m09::wire {

std::string digest(std::span<const std::byte> bytes) {
    unsigned char hash[EVP_MAX_MD_SIZE]{};
    unsigned int length{};
    if (EVP_Digest(bytes.data(), bytes.size(), hash, &length, EVP_sha256(), nullptr) != 1 || length != 32) {
        throw std::runtime_error("SHA-256 unavailable");
    }
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result;
    result.reserve(length * 2);
    for (unsigned i = 0; i < length; ++i) {
        result += hex[hash[i] >> 4];
        result += hex[hash[i] & 15];
    }
    return result;
}

bool identifier(std::string_view value) {
    return !value.empty() && value.size() <= 256 && std::ranges::all_of(value, [](unsigned char c) {
        return c >= 32 && c != 127;
    });
}

std::string key(std::initializer_list<std::string_view> parts) {
    Encoder encoded;
    for (auto part : parts) encoded.put(std::string(part));
    return digest(encoded.data);
}

Bytes envelope(std::string_view schema, std::span<const std::byte> body) {
    Encoder out;
    out.fields(std::string("M09"), std::string(schema), std::uint64_t{1},
               digest(body), Bytes(body.begin(), body.end()));
    return std::move(out.data);
}

std::optional<Bytes> open(std::string_view schema, std::span<const std::byte> bytes) {
    try {
        Decoder in(bytes);
        std::string magic, actual, checksum;
        std::uint64_t version{};
        Bytes body;
        in.fields(magic, actual, version, checksum, body);
        if (in.complete() && magic == "M09" && actual == schema && version == 1 && checksum == digest(body)) {
            return body;
        }
    } catch (const std::exception&) {
    }
    return {};
}

}  // namespace waste::m09::wire
