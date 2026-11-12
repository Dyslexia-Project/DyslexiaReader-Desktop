#pragma once

#include <string>

#include "dyslexia_reader/domain.hpp"

namespace dyslexia_reader::windows {

class WindowsPlatformProfileClient {
public:
    explicit WindowsPlatformProfileClient(
        std::wstring baseUrl = default_platform_base_url());

    [[nodiscard]] ReadingProfile fetch_shared_profile(
        const std::string& shareCode) const;

    [[nodiscard]] std::string share_profile(
        const ReadingProfile& profile) const;

    [[nodiscard]] static std::wstring default_platform_base_url();
    [[nodiscard]] static std::wstring default_web_playground_url();
    [[nodiscard]] static std::string normalize_share_code(
        const std::string& rawCode);
    [[nodiscard]] static std::string serialize_profile_json(
        const ReadingProfile& profile);
    [[nodiscard]] static ReadingProfile parse_profile_json(
        const std::string& jsonText);
    [[nodiscard]] static std::string parse_share_response_code(
        const std::string& jsonText);

private:
    struct ParsedEndpoint {
        bool useTls = false;
        std::wstring host;
        unsigned short port = 0;
        std::wstring basePath;
    };

    [[nodiscard]] std::string send_json_request(
        const wchar_t* method,
        const std::wstring& pathSuffix,
        const std::string* requestBody,
        bool acceptCreatedStatus) const;

    ParsedEndpoint endpoint_;
};

}  // namespace dyslexia_reader::windows
