#pragma once

#include "dyslexia_reader/interfaces.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace dyslexia_reader::windows {

class WindowsCloudSpeechClient final : public ICloudSpeechClient {
public:
    using AccessTokenProvider = std::function<std::string()>;

    explicit WindowsCloudSpeechClient(std::wstring serviceBaseUrl,
                                      AccessTokenProvider tokenProvider = {});
    ~WindowsCloudSpeechClient() override;

    WindowsCloudSpeechClient(const WindowsCloudSpeechClient&) = delete;
    WindowsCloudSpeechClient& operator=(const WindowsCloudSpeechClient&) = delete;

    CloudSpeechResult synthesize(const CloudSpeechRequest& request) override;

    static std::string build_synthesize_request_json(
        const CloudSpeechRequest& request, std::string_view language = {});
    static CloudSpeechResult parse_synthesize_response_json(
        const std::string& json, std::size_t inputLength,
        TextOffset baseOffset = 0);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class WindowsAudioCache final : public IAudioCache {
public:
    explicit WindowsAudioCache(std::filesystem::path directory = {});

    std::optional<CloudSpeechResult> find(const std::string& key) override;
    void store(const std::string& key,
               const CloudSpeechResult& result) override;

private:
    std::filesystem::path directory_;
};

}  // namespace dyslexia_reader::windows
