#include "dyslexia_reader/windows/platform_profile_client.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace dyslexia_reader::windows {
namespace {

using Json = nlohmann::json;

constexpr wchar_t kDefaultPlatformUrl[] = L"http://localhost:8080";
constexpr wchar_t kDefaultWebUrl[] = L"http://localhost:5173";
constexpr wchar_t kUserAgent[] = L"DyslexiaReader-Desktop/1.0";
constexpr int kHttpTimeoutMs = 10000;
constexpr std::size_t kMaxResponseBytes = 512 * 1024;
constexpr std::size_t kMinShareCodeLength = 4;
constexpr std::size_t kMaxShareCodeLength = 16;

struct WinHttpHandle {
    HINTERNET handle = nullptr;

    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) noexcept : handle(h) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    ~WinHttpHandle() { reset(); }

    void reset(HINTERNET next = nullptr) noexcept {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
        handle = next;
    }
};

std::string narrow_ascii_or_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
        nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                        static_cast<int>(wide.size()), out.data(), needed,
                        nullptr, nullptr);
    return out;
}

std::wstring widen_ascii(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

std::wstring trim_wide(const std::wstring& value) {
    std::size_t start = 0;
    while (start < value.size() && iswspace(value[start]) != 0) {
        ++start;
    }
    std::size_t end = value.size();
    while (end > start && iswspace(value[end - 1]) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

std::wstring read_env_url(const wchar_t* name, const wchar_t* fallback) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return fallback;
    }
    std::wstring buffer(static_cast<std::size_t>(needed), L'\0');
    const DWORD written = GetEnvironmentVariableW(name, buffer.data(), needed);
    if (written == 0 || written >= needed) {
        return fallback;
    }
    buffer.resize(static_cast<std::size_t>(written));
    buffer = trim_wide(buffer);
    return buffer.empty() ? std::wstring(fallback) : buffer;
}

bool iequals_wide(const std::wstring& lhs, const wchar_t* rhs) {
    return _wcsicmp(lhs.c_str(), rhs) == 0;
}

bool is_loopback_host(const std::wstring& host) {
    return iequals_wide(host, L"localhost") || host == L"127.0.0.1" ||
           host == L"::1" || host == L"[::1]";
}

double round_thousandths(float value) {
    return std::round(static_cast<double>(value) * 1000.0) / 1000.0;
}

const char* font_family_to_string(FontFamily family) {
    switch (family) {
    case FontFamily::Lexend:
        return "Lexend";
    case FontFamily::AtkinsonHyperlegible:
        return "Atkinson Hyperlegible";
    case FontFamily::OpenDyslexic:
        return "OpenDyslexic";
    }
    throw ReaderError(ReaderErrorCode::InvalidProfile,
                      "Unsupported fontFamily value");
}

FontFamily parse_font_family(const std::string& name) {
    if (name == "Lexend") {
        return FontFamily::Lexend;
    }
    if (name == "Atkinson Hyperlegible") {
        return FontFamily::AtkinsonHyperlegible;
    }
    if (name == "OpenDyslexic") {
        return FontFamily::OpenDyslexic;
    }
    throw ReaderError(ReaderErrorCode::InvalidProfile,
                      "Unsupported fontFamily in shared profile: " + name);
}

const char* theme_to_string(ThemeId theme) {
    switch (theme) {
    case ThemeId::WarmCream:
        return "warm-cream";
    case ThemeId::Sepia:
        return "sepia";
    case ThemeId::DarkHighContrast:
        return "dark-high-contrast";
    }
    throw ReaderError(ReaderErrorCode::InvalidProfile,
                      "Unsupported theme value");
}

ThemeId parse_theme(const std::string& name) {
    if (name == "warm-cream") {
        return ThemeId::WarmCream;
    }
    if (name == "sepia") {
        return ThemeId::Sepia;
    }
    if (name == "dark-high-contrast") {
        return ThemeId::DarkHighContrast;
    }
    throw ReaderError(ReaderErrorCode::InvalidProfile,
                      "Unsupported theme in shared profile: " + name);
}

void validate_profile_bounds(const ReadingProfile& profile) {
    if (profile.schemaVersion != 1 || !std::isfinite(profile.fontSizePt) ||
        profile.fontSizePt < 14.0F || profile.fontSizePt > 48.0F ||
        !std::isfinite(profile.letterSpacingEm) ||
        profile.letterSpacingEm < 0.0F || profile.letterSpacingEm > 0.2F ||
        !std::isfinite(profile.wordSpacingEm) ||
        profile.wordSpacingEm < 0.0F || profile.wordSpacingEm > 0.5F ||
        !std::isfinite(profile.lineHeight) || profile.lineHeight < 1.0F ||
        profile.lineHeight > 2.5F || profile.lineWidthCh < 35 ||
        profile.lineWidthCh > 100) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          "Reading profile is outside schema version 1 bounds");
    }
}

float require_float_field(const Json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          std::string("Missing or non-numeric field: ") + key);
    }
    const double val = it->get<double>();
    if (!std::isfinite(val)) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          std::string("Non-finite field: ") + key);
    }
    return static_cast<float>(val);
}

int require_int_field(const Json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          std::string("Missing or non-numeric field: ") + key);
    }
    const double val = it->get<double>();
    if (!std::isfinite(val) || std::floor(val) != val ||
        val < static_cast<double>(std::numeric_limits<int>::min()) ||
        val > static_cast<double>(std::numeric_limits<int>::max())) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          std::string("Expected integer field: ") + key);
    }
    return static_cast<int>(val);
}

std::string require_string_field(const Json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          std::string("Missing or non-string field: ") + key);
    }
    return it->get<std::string>();
}

bool require_bool_field(const Json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          std::string("Missing or non-boolean field: ") + key);
    }
    return it->get<bool>();
}

[[noreturn]] void throw_winhttp_error(const char* step) {
    const DWORD err = GetLastError();
    throw ReaderError(ReaderErrorCode::NetworkFailed,
                      std::string("Platform HTTP request failed at ") + step +
                          " (Win32 error " + std::to_string(err) + ")");
}

}  // namespace

std::wstring WindowsPlatformProfileClient::default_platform_base_url() {
    return read_env_url(L"DYSLEXIA_PLATFORM_URL", kDefaultPlatformUrl);
}

std::wstring WindowsPlatformProfileClient::default_web_playground_url() {
    return read_env_url(L"DYSLEXIA_WEB_URL", kDefaultWebUrl);
}

std::string WindowsPlatformProfileClient::normalize_share_code(
    const std::string& rawCode) {
    std::string trimmed;
    trimmed.reserve(rawCode.size());
    for (unsigned char ch : rawCode) {
        if (std::isspace(ch) != 0) {
            continue;
        }
        if (std::isalnum(ch) == 0) {
            throw ReaderError(ReaderErrorCode::InvalidProfile,
                              "Share code must contain only letters and digits");
        }
        trimmed.push_back(static_cast<char>(std::toupper(ch)));
    }
    if (trimmed.size() < kMinShareCodeLength ||
        trimmed.size() > kMaxShareCodeLength) {
        throw ReaderError(
            ReaderErrorCode::InvalidProfile,
            "Share code must be between 4 and 16 alphanumeric characters");
    }
    return trimmed;
}

std::string WindowsPlatformProfileClient::serialize_profile_json(
    const ReadingProfile& profile) {
    validate_profile_bounds(profile);
    const Json payload = {
        {"schemaVersion", profile.schemaVersion},
        {"fontFamily", font_family_to_string(profile.fontFamily)},
        {"fontSizePt", round_thousandths(profile.fontSizePt)},
        {"letterSpacingEm", round_thousandths(profile.letterSpacingEm)},
        {"wordSpacingEm", round_thousandths(profile.wordSpacingEm)},
        {"lineHeight", round_thousandths(profile.lineHeight)},
        {"lineWidthCh", profile.lineWidthCh},
        {"theme", theme_to_string(profile.theme)},
        {"aids",
         {{"bionicReading", profile.aids.bionicReading},
          {"readingRuler", profile.aids.readingRuler},
          {"bimodalHighlighting", profile.aids.bimodalHighlighting}}},
    };
    return payload.dump();
}

ReadingProfile WindowsPlatformProfileClient::parse_profile_json(
    const std::string& jsonText) {
    Json root;
    try {
        root = Json::parse(jsonText);
    } catch (const Json::exception& ex) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          std::string("Invalid platform profile JSON: ") +
                              ex.what());
    }
    if (!root.is_object()) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          "Platform profile JSON must be an object");
    }

    ReadingProfile parsed;
    parsed.schemaVersion = require_int_field(root, "schemaVersion");
    parsed.fontFamily =
        parse_font_family(require_string_field(root, "fontFamily"));
    parsed.fontSizePt = require_float_field(root, "fontSizePt");
    parsed.letterSpacingEm = require_float_field(root, "letterSpacingEm");
    parsed.wordSpacingEm = require_float_field(root, "wordSpacingEm");
    parsed.lineHeight = require_float_field(root, "lineHeight");
    parsed.lineWidthCh = require_int_field(root, "lineWidthCh");
    parsed.theme = parse_theme(require_string_field(root, "theme"));

    const auto aidsIt = root.find("aids");
    if (aidsIt == root.end() || !aidsIt->is_object()) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          "Missing or invalid aids object in platform profile");
    }
    parsed.aids.bionicReading = require_bool_field(*aidsIt, "bionicReading");
    parsed.aids.readingRuler = require_bool_field(*aidsIt, "readingRuler");
    parsed.aids.bimodalHighlighting =
        require_bool_field(*aidsIt, "bimodalHighlighting");

    validate_profile_bounds(parsed);
    return parsed;
}

std::string WindowsPlatformProfileClient::parse_share_response_code(
    const std::string& jsonText) {
    Json root;
    try {
        root = Json::parse(jsonText);
    } catch (const Json::exception& ex) {
        throw ReaderError(ReaderErrorCode::NetworkFailed,
                          std::string("Invalid share response JSON: ") +
                              ex.what());
    }
    if (!root.is_object()) {
        throw ReaderError(ReaderErrorCode::NetworkFailed,
                          "Share response must be a JSON object");
    }
    return normalize_share_code(require_string_field(root, "code"));
}

WindowsPlatformProfileClient::WindowsPlatformProfileClient(std::wstring baseUrl) {
    baseUrl = trim_wide(baseUrl);
    if (baseUrl.empty()) {
        throw ReaderError(ReaderErrorCode::NetworkFailed,
                          "Platform base URL cannot be empty");
    }

    const std::size_t bufLen = baseUrl.size() + 1;
    std::wstring scheme(bufLen, L'\0');
    std::wstring host(bufLen, L'\0');
    std::wstring urlPath(bufLen, L'\0');
    std::wstring extraInfo(bufLen, L'\0');

    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.lpszScheme = scheme.data();
    components.dwSchemeLength = static_cast<DWORD>(scheme.size());
    components.lpszHostName = host.data();
    components.dwHostNameLength = static_cast<DWORD>(host.size());
    components.lpszUrlPath = urlPath.data();
    components.dwUrlPathLength = static_cast<DWORD>(urlPath.size());
    components.lpszExtraInfo = extraInfo.data();
    components.dwExtraInfoLength = static_cast<DWORD>(extraInfo.size());

    if (!WinHttpCrackUrl(baseUrl.c_str(), static_cast<DWORD>(baseUrl.size()),
                         0, &components)) {
        throw ReaderError(ReaderErrorCode::NetworkFailed,
                          "Invalid Platform base URL");
    }

    scheme.resize(components.dwSchemeLength);
    host.resize(components.dwHostNameLength);
    urlPath.resize(components.dwUrlPathLength);
    extraInfo.resize(components.dwExtraInfoLength);

    if (host.empty()) {
        throw ReaderError(ReaderErrorCode::NetworkFailed,
                          "Platform base URL must include a hostname");
    }
    if (!extraInfo.empty()) {
        throw ReaderError(ReaderErrorCode::NetworkFailed,
                          "Platform base URL must not contain query/fragment");
    }

    if (iequals_wide(scheme, L"https")) {
        endpoint_.useTls = true;
    } else if (iequals_wide(scheme, L"http")) {
        if (!is_loopback_host(host)) {
            throw ReaderError(
                ReaderErrorCode::NetworkFailed,
                "Plain HTTP Platform URL is only permitted for loopback hosts (" +
                    narrow_ascii_or_utf8(host) + ")");
        }
        endpoint_.useTls = false;
    } else {
        throw ReaderError(ReaderErrorCode::NetworkFailed,
                          "Platform URL scheme must be https or localhost http");
    }

    endpoint_.host = std::move(host);
    endpoint_.port = static_cast<unsigned short>(components.nPort);
    while (!urlPath.empty() && urlPath.back() == L'/') {
        urlPath.pop_back();
    }
    endpoint_.basePath = std::move(urlPath);
}

ReadingProfile WindowsPlatformProfileClient::fetch_shared_profile(
    const std::string& shareCode) const {
    const std::string normalized = normalize_share_code(shareCode);
    const std::wstring path =
        L"/api/v1/profiles/share/" + widen_ascii(normalized);
    const std::string body =
        send_json_request(L"GET", path, nullptr, false);
    return parse_profile_json(body);
}

std::string WindowsPlatformProfileClient::share_profile(
    const ReadingProfile& profile) const {
    const std::string requestJson = serialize_profile_json(profile);
    const std::string body = send_json_request(
        L"POST", L"/api/v1/profiles/share", &requestJson, true);
    return parse_share_response_code(body);
}

std::string WindowsPlatformProfileClient::send_json_request(
    const wchar_t* method,
    const std::wstring& pathSuffix,
    const std::string* requestBody,
    bool acceptCreatedStatus) const {
    WinHttpHandle session(WinHttpOpen(kUserAgent,
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.handle == nullptr) {
        throw_winhttp_error("WinHttpOpen");
    }

    WinHttpSetTimeouts(session.handle, kHttpTimeoutMs, kHttpTimeoutMs,
                       kHttpTimeoutMs, kHttpTimeoutMs);

    WinHttpHandle connection(WinHttpConnect(
        session.handle, endpoint_.host.c_str(), endpoint_.port, 0));
    if (connection.handle == nullptr) {
        throw_winhttp_error("WinHttpConnect");
    }

    const std::wstring fullPath = endpoint_.basePath + pathSuffix;
    const DWORD flags = endpoint_.useTls ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(
        connection.handle, method, fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (request.handle == nullptr) {
        throw_winhttp_error("WinHttpOpenRequest");
    }

    const std::wstring headers = requestBody != nullptr
        ? L"Accept: application/json\r\nContent-Type: application/json\r\n"
        : L"Accept: application/json\r\n";

    LPVOID payloadPtr = WINHTTP_NO_REQUEST_DATA;
    DWORD payloadSize = 0;
    if (requestBody != nullptr && !requestBody->empty()) {
        if (requestBody->size() >
            static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
            throw ReaderError(ReaderErrorCode::InvalidProfile,
                              "Profile JSON payload exceeds WinHTTP limit");
        }
        payloadPtr = const_cast<char*>(requestBody->data());
        payloadSize = static_cast<DWORD>(requestBody->size());
    }

    if (!WinHttpSendRequest(request.handle, headers.c_str(),
                            static_cast<DWORD>(headers.size()), payloadPtr,
                            payloadSize, payloadSize, 0)) {
        throw_winhttp_error("WinHttpSendRequest");
    }

    if (!WinHttpReceiveResponse(request.handle, nullptr)) {
        throw_winhttp_error("WinHttpReceiveResponse");
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(
            request.handle,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        throw_winhttp_error("WinHttpQueryHeaders");
    }

    std::string responseBody;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available)) {
            throw_winhttp_error("WinHttpQueryDataAvailable");
        }
        if (available == 0) {
            break;
        }
        if (responseBody.size() + static_cast<std::size_t>(available) >
            kMaxResponseBytes) {
            throw ReaderError(ReaderErrorCode::NetworkFailed,
                              "Platform HTTP response exceeded 512 KB limit");
        }
        const std::size_t offset = responseBody.size();
        responseBody.resize(offset + static_cast<std::size_t>(available));
        DWORD readBytes = 0;
        if (!WinHttpReadData(request.handle, responseBody.data() + offset,
                             available, &readBytes)) {
            throw_winhttp_error("WinHttpReadData");
        }
        responseBody.resize(offset + static_cast<std::size_t>(readBytes));
    }

    const bool okStatus =
        statusCode == 200 || (acceptCreatedStatus && statusCode == 201);
    if (!okStatus) {
        std::string message =
            "Platform HTTP " + std::to_string(statusCode);
        if (statusCode == 404) {
            message += " (share code not found)";
        } else if (!responseBody.empty()) {
            message += ": " + responseBody.substr(0, 200);
        }
        throw ReaderError(ReaderErrorCode::NetworkFailed, message);
    }

    return responseBody;
}

}  // namespace dyslexia_reader::windows
