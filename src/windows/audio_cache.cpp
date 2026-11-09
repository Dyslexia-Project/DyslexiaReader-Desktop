#include "dyslexia_reader/windows/cloud_speech_client.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

namespace dyslexia_reader::windows {
namespace {

constexpr std::array<char, 8> kCacheMagic{
    {'D', 'R', 'C', 'A', 'C', 'H', 'E', '1'}};
constexpr std::uint32_t kCacheVersion = 1;
constexpr std::size_t kMaxCacheFileBytes = 80U * 1024U * 1024U;
constexpr std::size_t kMaxAudioBytes = 64U * 1024U * 1024U;
constexpr std::uint32_t kMaxAudioChunks = 1024;
constexpr std::uint32_t kMaxSpeechMarks = 1000000;

struct AlgorithmHandle {
    BCRYPT_ALG_HANDLE value = nullptr;

    ~AlgorithmHandle() {
        if (value != nullptr) {
            BCryptCloseAlgorithmProvider(value, 0);
        }
    }
};

struct HashHandle {
    BCRYPT_HASH_HANDLE value = nullptr;

    ~HashHandle() {
        if (value != nullptr) {
            BCryptDestroyHash(value);
        }
    }
};

[[noreturn]] void throw_cache_error(const std::string& message) {
    throw ReaderError(ReaderErrorCode::CacheFailed, message);
}

[[noreturn]] void throw_crypto_error(const char* operation, NTSTATUS status) {
    throw_cache_error(std::string(operation) + " failed (NTSTATUS " +
                      std::to_string(static_cast<long>(status)) + ")");
}

std::filesystem::path default_cache_directory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD written =
        GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (written == 0 || written >= buffer.size()) {
        throw_cache_error("Could not locate the Windows temporary directory");
    }
    return std::filesystem::path(
               std::wstring(buffer.data(), written)) /
           L"DyslexiaReader" / L"CloudSpeechCache";
}

std::string cache_file_name(const std::string& key) {
    AlgorithmHandle algorithm;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status < 0) {
        throw_crypto_error("BCryptOpenAlgorithmProvider", status);
    }

    HashHandle hash;
    std::vector<UCHAR> hashObject;
    ULONG objectLength = 0;
    ULONG bytesWritten = 0;
    status = BCryptGetProperty(
        algorithm.value, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
        &bytesWritten, 0);
    if (status < 0) {
        throw_crypto_error("BCryptGetProperty", status);
    }
    hashObject.resize(objectLength);
    status = BCryptCreateHash(algorithm.value, &hash.value, hashObject.data(),
                              objectLength, nullptr, 0, 0);
    if (status < 0) {
        throw_crypto_error("BCryptCreateHash", status);
    }

    std::size_t offset = 0;
    while (offset < key.size()) {
        const auto count = static_cast<ULONG>(
            std::min<std::size_t>(key.size() - offset,
                                  std::numeric_limits<ULONG>::max()));
        status = BCryptHashData(
            hash.value,
            reinterpret_cast<PUCHAR>(
                const_cast<char*>(key.data() + offset)),
            count, 0);
        if (status < 0) {
            throw_crypto_error("BCryptHashData", status);
        }
        offset += count;
    }

    std::array<UCHAR, 32> digest{};
    status = BCryptFinishHash(hash.value, digest.data(),
                              static_cast<ULONG>(digest.size()), 0);
    if (status < 0) {
        throw_crypto_error("BCryptFinishHash", status);
    }
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (const UCHAR byte : digest) {
        hex << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return hex.str();
}

template <typename Integer>
void write_integer(std::ostream& output, Integer value) {
    static_assert(std::is_unsigned<Integer>::value);
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if (!output) {
        throw_cache_error("Could not write a cloud speech cache record");
    }
}

template <typename Integer>
Integer read_integer(std::istream& input) {
    static_assert(std::is_unsigned<Integer>::value);
    Integer value = 0;
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) {
        throw_cache_error("Cloud speech cache record is truncated");
    }
    return value;
}

void write_data(std::ostream& output, const void* data, std::size_t size) {
    output.write(static_cast<const char*>(data),
                 static_cast<std::streamsize>(size));
    if (!output) {
        throw_cache_error("Could not write a cloud speech cache record");
    }
}

void read_data(std::istream& input, void* data, std::size_t size) {
    input.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    if (!input) {
        throw_cache_error("Cloud speech cache record is truncated");
    }
}

void write_result(std::ostream& output, const CloudSpeechResult& result) {
    if (result.chunks.empty() ||
        result.chunks.size() > kMaxAudioChunks ||
        result.marks.size() > kMaxSpeechMarks) {
        throw_cache_error("Cloud speech result exceeds cache limits");
    }

    std::size_t totalAudioBytes = 0;
    for (const auto& chunk : result.chunks) {
        if (chunk.mediaType.empty() || chunk.mediaType.size() > 1024 ||
            chunk.bytes.empty() ||
            chunk.bytes.size() > kMaxAudioBytes - totalAudioBytes) {
            throw_cache_error("Cloud speech audio chunk exceeds cache limits");
        }
        totalAudioBytes += chunk.bytes.size();
    }

    write_data(output, kCacheMagic.data(), kCacheMagic.size());
    write_integer(output, kCacheVersion);
    write_integer(output,
                  static_cast<std::uint8_t>(result.wordTimingReliable ? 1 : 0));
    write_integer(
        output,
        static_cast<std::uint8_t>(result.sentenceTimingReliable ? 1 : 0));
    write_integer(output, static_cast<std::uint32_t>(result.chunks.size()));
    for (const auto& chunk : result.chunks) {
        write_integer(output,
                      static_cast<std::uint32_t>(chunk.mediaType.size()));
        write_data(output, chunk.mediaType.data(), chunk.mediaType.size());
        write_integer(output,
                      static_cast<std::uint64_t>(chunk.bytes.size()));
        write_data(output, chunk.bytes.data(), chunk.bytes.size());
    }
    write_integer(output, static_cast<std::uint32_t>(result.marks.size()));
    for (const auto& mark : result.marks) {
        if (mark.kind != SpeechMarkKind::Word &&
            mark.kind != SpeechMarkKind::Sentence) {
            throw_cache_error("Cloud speech result has an unknown mark type");
        }
        if (mark.text.length == 0 ||
            mark.text.start >
                std::numeric_limits<TextOffset>::max() - mark.text.length) {
            throw_cache_error("Cloud speech result has an invalid mark range");
        }
        write_integer(
            output,
            static_cast<std::uint8_t>(mark.kind == SpeechMarkKind::Word ? 0
                                                                          : 1));
        write_integer(output, mark.text.start);
        write_integer(output, mark.text.length);
        write_integer(output, mark.timeMilliseconds);
    }
}

CloudSpeechResult read_result(std::istream& input) {
    std::array<char, kCacheMagic.size()> magic{};
    read_data(input, magic.data(), magic.size());
    if (magic != kCacheMagic ||
        read_integer<std::uint32_t>(input) != kCacheVersion) {
        throw_cache_error("Cloud speech cache record has an unknown format");
    }

    const auto wordReliable = read_integer<std::uint8_t>(input);
    const auto sentenceReliable = read_integer<std::uint8_t>(input);
    if (wordReliable > 1 || sentenceReliable > 1) {
        throw_cache_error("Cloud speech cache record has invalid flags");
    }

    CloudSpeechResult result;
    result.wordTimingReliable = wordReliable != 0;
    result.sentenceTimingReliable = sentenceReliable != 0;
    const auto chunkCount = read_integer<std::uint32_t>(input);
    if (chunkCount == 0 || chunkCount > kMaxAudioChunks) {
        throw_cache_error("Cloud speech cache record has invalid audio chunks");
    }
    result.chunks.reserve(chunkCount);
    std::size_t totalAudioBytes = 0;
    for (std::uint32_t index = 0; index < chunkCount; ++index) {
        const auto mediaTypeLength = read_integer<std::uint32_t>(input);
        if (mediaTypeLength == 0 || mediaTypeLength > 1024) {
            throw_cache_error("Cloud speech cache has an invalid media type");
        }
        std::string mediaType(mediaTypeLength, '\0');
        read_data(input, mediaType.data(), mediaType.size());
        const auto bytesLength = read_integer<std::uint64_t>(input);
        if (bytesLength == 0 || bytesLength > kMaxAudioBytes - totalAudioBytes) {
            throw_cache_error("Cloud speech cache audio exceeds its size limit");
        }
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(bytesLength));
        read_data(input, bytes.data(), bytes.size());
        totalAudioBytes += bytes.size();
        result.chunks.push_back({std::move(mediaType), std::move(bytes)});
    }

    const auto markCount = read_integer<std::uint32_t>(input);
    if (markCount > kMaxSpeechMarks) {
        throw_cache_error("Cloud speech cache has too many speech marks");
    }
    result.marks.reserve(markCount);
    for (std::uint32_t index = 0; index < markCount; ++index) {
        const auto kindValue = read_integer<std::uint8_t>(input);
        if (kindValue > 1) {
            throw_cache_error("Cloud speech cache has an unknown mark type");
        }
        const auto kind = kindValue == 0 ? SpeechMarkKind::Word
                                         : SpeechMarkKind::Sentence;
        const auto start = read_integer<TextOffset>(input);
        const auto length = read_integer<std::uint32_t>(input);
        if (length == 0 ||
            start > std::numeric_limits<TextOffset>::max() - length) {
            throw_cache_error("Cloud speech cache has an invalid mark range");
        }
        result.marks.push_back(
            {kind, {start, length},
             read_integer<std::uint64_t>(input)});
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        throw_cache_error("Cloud speech cache record has trailing data");
    }
    return result;
}

std::atomic<std::uint64_t> temporaryFileCounter{0};

class TemporaryFileCleanup final {
public:
    explicit TemporaryFileCleanup(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~TemporaryFileCleanup() {
        if (active_) {
            DeleteFileW(path_.c_str());
        }
    }

    void release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_ = true;
};

}  // namespace

WindowsAudioCache::WindowsAudioCache(std::filesystem::path directory)
    : directory_(directory.empty() ? default_cache_directory()
                                   : std::move(directory)) {}

std::optional<CloudSpeechResult> WindowsAudioCache::find(
    const std::string& key) {
    const auto path =
        directory_ / (cache_file_name(key) + ".bin");
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        throw_cache_error("Could not inspect the cloud speech cache: " +
                          error.message());
    }
    if (!exists) {
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw_cache_error("Could not read cloud speech cache size: " +
                          error.message());
    }
    if (size > kMaxCacheFileBytes) {
        throw_cache_error("Cloud speech cache file is too large");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw_cache_error("Could not open a cloud speech cache file");
    }
    return read_result(input);
}

void WindowsAudioCache::store(const std::string& key,
                              const CloudSpeechResult& result) {
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
        throw_cache_error(
            "Could not create the cloud speech cache directory: " +
            error.message());
    }

    const auto target = directory_ / (cache_file_name(key) + ".bin");
    const std::filesystem::path temporary =
        target.wstring() + L".tmp." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(
            temporaryFileCounter.fetch_add(1, std::memory_order_relaxed));
    TemporaryFileCleanup cleanup(temporary);

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw_cache_error("Could not create a cloud speech cache file");
        }
        write_result(output, result);
        output.flush();
        if (!output) {
            throw_cache_error("Could not flush a cloud speech cache file");
        }
    }
    const auto size = std::filesystem::file_size(temporary, error);
    if (error) {
        throw_cache_error("Could not read temporary cache size: " +
                          error.message());
    }
    if (size > kMaxCacheFileBytes) {
        throw_cache_error("Cloud speech cache record exceeds its size limit");
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw_cache_error("Could not atomically store cloud speech audio "
                          "(Win32 " +
                          std::to_string(GetLastError()) + ")");
    }
    cleanup.release();
}

}  // namespace dyslexia_reader::windows
