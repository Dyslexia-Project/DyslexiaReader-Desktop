#include "dyslexia_reader/caching_cloud_speech_client.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace dyslexia_reader {
namespace {

void append_byte(std::string& value, std::uint8_t byte) {
    value.push_back(static_cast<char>(byte));
}

void append_u32(std::string& value, std::uint32_t number) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        append_byte(value, static_cast<std::uint8_t>(number >> shift));
    }
}

void append_u64(std::string& value, std::uint64_t number) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        append_byte(value, static_cast<std::uint8_t>(number >> shift));
    }
}

void append_string(std::string& key, const std::string& value) {
    append_u64(key, value.size());
    key.append(value);
}

std::string make_cache_key(const CloudSpeechRequest& request,
                           const std::string& cacheNamespace) {
    if (request.text.empty() || request.voiceId.empty() ||
        request.voiceId.size() > 512 ||
        request.text.size() > std::numeric_limits<TextOffset>::max() ||
        !std::isfinite(request.rate) || request.rate < 0.5F ||
        request.rate > 2.0F) {
        throw ReaderError(
            ReaderErrorCode::SpeechUnavailable,
            "Cloud speech request has invalid text, voice, or rate");
    }
    if (cacheNamespace.empty()) {
        throw ReaderError(ReaderErrorCode::CacheFailed,
                          "Cloud speech cache namespace cannot be empty");
    }
    if (request.selection &&
        (request.selection->length == 0 ||
         request.selection->start > request.text.size() ||
         request.selection->length >
             request.text.size() - request.selection->start)) {
        throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                          "Cloud speech selection is outside the document");
    }

    std::string key{"DyslexiaReaderCloudSpeechV1"};
    append_string(key, cacheNamespace);
    append_u64(key, request.text.size());
    for (const char16_t codeUnit : request.text) {
        append_byte(key, static_cast<std::uint8_t>(codeUnit & 0xFF));
        append_byte(key,
                    static_cast<std::uint8_t>((codeUnit >> 8) & 0xFF));
    }
    append_string(key, request.voiceId);
    std::uint32_t rateBits = 0;
    static_assert(sizeof(rateBits) == sizeof(request.rate));
    std::memcpy(&rateBits, &request.rate, sizeof(rateBits));
    append_u32(key, rateBits);
    append_byte(key, request.selection ? 1 : 0);
    if (request.selection) {
        append_u32(key, request.selection->start);
        append_u32(key, request.selection->length);
    }
    return key;
}

}  // namespace

CachingCloudSpeechClient::CachingCloudSpeechClient(
    ICloudSpeechClient& client, IAudioCache& cache,
    std::string cacheNamespace)
    : client_(client),
      cache_(cache),
      cacheNamespace_(std::move(cacheNamespace)) {}

CloudSpeechResult CachingCloudSpeechClient::synthesize(
    const CloudSpeechRequest& request) {
    const auto key = make_cache_key(request, cacheNamespace_);
    if (auto result = cache_.find(key)) {
        return *result;
    }

    auto result = client_.synthesize(request);
    cache_.store(key, result);
    return result;
}

}  // namespace dyslexia_reader
