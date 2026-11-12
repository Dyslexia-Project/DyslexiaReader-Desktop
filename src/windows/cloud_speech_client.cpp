#include "dyslexia_reader/windows/cloud_speech_client.hpp"

#include <windows.h>
#include <objbase.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include <cmath>
#include <cstdint>
#include <cwctype>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace dyslexia_reader::windows {
namespace {

constexpr std::size_t kMaxResponseBytes = 128U * 1024U * 1024U;
constexpr std::size_t kMaxAudioBytes = 64U * 1024U * 1024U;
constexpr std::uint32_t kMaxAudioChunks = 1024;
constexpr std::uint32_t kMaxSpeechMarks = 1000000;

class ApartmentScope final {
public:
    ApartmentScope() {
        static std::once_flag mtaOnce;
        static CO_MTA_USAGE_COOKIE mtaCookie = nullptr;
        std::call_once(mtaOnce, []() { (void)CoIncrementMTAUsage(&mtaCookie); });
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialized_ = true;
        } catch (const winrt::hresult_error& error) {
            if (error.code() != RPC_E_CHANGED_MODE) {
                throw;
            }
        }
    }

    ~ApartmentScope() {
        if (initialized_) {
            winrt::uninit_apartment();
        }
    }

    ApartmentScope(const ApartmentScope&) = delete;
    ApartmentScope& operator=(const ApartmentScope&) = delete;

private:
    bool initialized_ = false;
};

struct WinHttpCloser {
    void operator()(std::remove_pointer_t<HINTERNET>* handle) const noexcept {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
};

using WinHttpHandle =
    std::unique_ptr<std::remove_pointer_t<HINTERNET>, WinHttpCloser>;

[[noreturn]] void throw_network_error(const char* operation,
                                      DWORD error = GetLastError()) {
    throw ReaderError(
        ReaderErrorCode::NetworkFailed,
        std::string(operation) + " failed (Win32 " +
            std::to_string(static_cast<unsigned long>(error)) + ")");
}

[[noreturn]] void throw_response_error(const std::string& message) {
    throw ReaderError(ReaderErrorCode::NetworkFailed, message);
}

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>(
                           std::numeric_limits<int>::max())) {
        throw_response_error("Cloud speech value exceeds the supported size");
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size == 0) {
        throw_response_error("Cloud speech value is not valid UTF-8");
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            size) != size) {
        throw_response_error("Cloud speech value is not valid UTF-8");
    }
    return result;
}

winrt::hstring to_hstring(const std::u16string& value) {
    return winrt::hstring(reinterpret_cast<const wchar_t*>(value.data()),
                          static_cast<std::uint32_t>(value.size()));
}

bool equals_ascii_icase(std::wstring_view lhs, std::wstring_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (std::towlower(lhs[i]) != std::towlower(rhs[i])) {
            return false;
        }
    }
    return true;
}

bool is_loopback_host(std::wstring_view host) {
    return equals_ascii_icase(host, L"localhost") ||
           host == L"127.0.0.1" || host == L"::1" || host == L"[::1]";
}

std::string infer_language_tag(std::string_view voiceId,
                               std::string_view explicitLanguage) {
    if (!explicitLanguage.empty()) {
        return std::string(explicitLanguage);
    }
    if (voiceId.size() >= 5) {
        const auto prefix = voiceId.substr(0, 5);
        if ((prefix[0] == 'e' || prefix[0] == 'E') &&
            (prefix[1] == 'n' || prefix[1] == 'N') &&
            (prefix[2] == '-' || prefix[2] == '_') &&
            (prefix[3] == 'g' || prefix[3] == 'G') &&
            (prefix[4] == 'b' || prefix[4] == 'B')) {
            return "en-GB";
        }
        if ((prefix[0] == 'r' || prefix[0] == 'R') &&
            (prefix[1] == 'o' || prefix[1] == 'O') &&
            (prefix[2] == '-' || prefix[2] == '_') &&
            (prefix[3] == 'r' || prefix[3] == 'R') &&
            (prefix[4] == 'o' || prefix[4] == 'O')) {
            return "ro-RO";
        }
    }
    return "en-US";
}

struct Endpoint {
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path;
    bool secure = true;
};

Endpoint parse_endpoint(const std::wstring& baseUrl) {
    if (baseUrl.empty() ||
        baseUrl.size() > std::numeric_limits<DWORD>::max()) {
        throw ReaderError(ReaderErrorCode::NetworkFailed,
                          "Cloud speech service URL is empty or too long");
    }

    wchar_t host[256]{};
    wchar_t path[2048]{};
    wchar_t extra[512]{};
    wchar_t username[128]{};
    wchar_t password[128]{};
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = path;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    components.lpszExtraInfo = extra;
    components.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    components.lpszUserName = username;
    components.dwUserNameLength = static_cast<DWORD>(std::size(username));
    components.lpszPassword = password;
    components.dwPasswordLength = static_cast<DWORD>(std::size(password));

    if (!WinHttpCrackUrl(baseUrl.c_str(),
                         static_cast<DWORD>(baseUrl.size()), 0,
                         &components)) {
        throw_network_error("WinHttpCrackUrl");
    }
    if (components.dwHostNameLength == 0 ||
        components.dwHostNameLength >= std::size(host) ||
        components.dwUrlPathLength >= std::size(path) ||
        components.dwExtraInfoLength != 0 ||
        components.dwUserNameLength != 0 ||
        components.dwPasswordLength != 0) {
        throw ReaderError(
            ReaderErrorCode::NetworkFailed,
            "Cloud speech service URL has unsupported components");
    }

    const std::wstring_view hostView(host, components.dwHostNameLength);
    const bool isHttps = (components.nScheme == INTERNET_SCHEME_HTTPS);
    const bool isLoopbackHttp =
        (components.nScheme == INTERNET_SCHEME_HTTP) &&
        is_loopback_host(hostView);
    if (!isHttps && !isLoopbackHttp) {
        throw ReaderError(
            ReaderErrorCode::NetworkFailed,
            "Cloud speech service URL must use HTTPS (except localhost)");
    }

    Endpoint endpoint;
    endpoint.host.assign(hostView);
    endpoint.port = components.nPort;
    endpoint.secure = isHttps;
    endpoint.path.assign(path, components.dwUrlPathLength);
    while (!endpoint.path.empty() && endpoint.path.back() == L'/') {
        endpoint.path.pop_back();
    }
    endpoint.path += L"/api/v1/speech/synthesize";
    return endpoint;
}

std::string request_json(const std::u16string& text,
                         const std::string& voiceId, float rate,
                         std::string_view language) {
    using namespace winrt::Windows::Data::Json;

    ApartmentScope apartment;
    const double roundedSpeed =
        std::round(static_cast<double>(rate) * 100.0) / 100.0;
    const std::string resolvedLanguage =
        infer_language_tag(voiceId, language);

    JsonObject body;
    body.Insert(L"text", JsonValue::CreateStringValue(to_hstring(text)));
    body.Insert(L"voiceId",
                JsonValue::CreateStringValue(winrt::to_hstring(voiceId)));
    body.Insert(L"speed", JsonValue::CreateNumberValue(roundedSpeed));
    body.Insert(
        L"language",
        JsonValue::CreateStringValue(winrt::to_hstring(resolvedLanguage)));
    return winrt::to_string(body.Stringify());
}

std::vector<std::uint8_t> decode_base64(const winrt::hstring& encoded) {
    if (encoded.empty() ||
        encoded.size() > std::numeric_limits<DWORD>::max() ||
        encoded.size() > (kMaxAudioBytes * 4U / 3U + 4U)) {
        throw_response_error("Cloud speech audio chunk has invalid base64 data");
    }
    DWORD size = 0;
    if (!CryptStringToBinaryW(encoded.c_str(), 0, CRYPT_STRING_BASE64, nullptr,
                              &size, nullptr, nullptr)) {
        throw_response_error("Cloud speech audio chunk is not valid base64");
    }
    std::vector<std::uint8_t> bytes(size);
    if (!CryptStringToBinaryW(encoded.c_str(), 0, CRYPT_STRING_BASE64,
                              bytes.data(), &size, nullptr, nullptr)) {
        throw_response_error("Cloud speech audio chunk is not valid base64");
    }
    bytes.resize(size);
    return bytes;
}

std::uint32_t required_uint32(
    const winrt::Windows::Data::Json::JsonObject& object,
    const wchar_t* name) {
    const double value = object.GetNamedNumber(name);
    if (!std::isfinite(value) || value < 0 ||
        value > std::numeric_limits<std::uint32_t>::max() ||
        std::floor(value) != value) {
        throw_response_error("Cloud speech response contains an invalid range");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint64_t required_milliseconds(
    const winrt::Windows::Data::Json::JsonObject& object,
    const wchar_t* name) {
    constexpr double kMaxExactJsonInteger = 9007199254740991.0;
    const double value = object.GetNamedNumber(name);
    if (!std::isfinite(value) || value < 0 ||
        value > kMaxExactJsonInteger || std::floor(value) != value) {
        throw_response_error(
            "Cloud speech response contains an invalid timestamp");
    }
    return static_cast<std::uint64_t>(value);
}

CloudSpeechResult parse_response(const std::string& json,
                                 std::size_t inputLength,
                                 TextOffset baseOffset) {
    using namespace winrt::Windows::Data::Json;

    try {
        ApartmentScope apartment;
        const auto wideJson = utf8_to_wide(json);
        const auto object = JsonObject::Parse(winrt::hstring(wideJson));
        if (!object.HasKey(L"speechMarks")) {
            throw_response_error(
                "Cloud speech response is missing required fields");
        }

        CloudSpeechResult result;

        // Reliability flags: Platform v1 uses timingGranularity; provisional v1 uses booleans.
        if (object.HasKey(L"timingGranularity")) {
            const auto granularity = object.GetNamedString(L"timingGranularity");
            const std::wstring_view granularityView(granularity);
            if (granularityView == L"WORD_AND_SENTENCE") {
                result.wordTimingReliable = true;
                result.sentenceTimingReliable = true;
            } else if (granularityView == L"SENTENCE_ONLY") {
                result.wordTimingReliable = false;
                result.sentenceTimingReliable = true;
            } else if (granularityView == L"NONE") {
                result.wordTimingReliable = false;
                result.sentenceTimingReliable = false;
            } else {
                throw_response_error(
                    "Cloud speech response has an unknown timingGranularity");
            }
        } else if (object.HasKey(L"wordTimingReliable") &&
                   object.HasKey(L"sentenceTimingReliable")) {
            result.wordTimingReliable =
                object.GetNamedBoolean(L"wordTimingReliable");
            result.sentenceTimingReliable =
                object.GetNamedBoolean(L"sentenceTimingReliable");
        } else {
            throw_response_error(
                "Cloud speech response is missing timing reliability metadata");
        }

        // Audio payload: Platform v1 uses audioMimeType + audioBase64; provisional v1 uses audioChunks[].
        if (object.HasKey(L"audioMimeType") && object.HasKey(L"audioBase64")) {
            const auto mediaType =
                winrt::to_string(object.GetNamedString(L"audioMimeType"));
            if (mediaType.empty() || mediaType.size() > 1024) {
                throw_response_error(
                    "Cloud speech response has an invalid audio media type");
            }
            auto bytes = decode_base64(object.GetNamedString(L"audioBase64"));
            if (bytes.empty() || bytes.size() > kMaxAudioBytes) {
                throw_response_error(
                    "Cloud speech response exceeds the audio size limit");
            }
            result.chunks.push_back({mediaType, std::move(bytes)});
        } else if (object.HasKey(L"audioChunks")) {
            const auto chunks = object.GetNamedArray(L"audioChunks");
            if (chunks.Size() == 0 || chunks.Size() > kMaxAudioChunks) {
                throw_response_error(
                    "Cloud speech response has an invalid audio chunk count");
            }
            std::size_t totalAudioBytes = 0;
            result.chunks.reserve(chunks.Size());
            for (const auto& value : chunks) {
                const auto chunk = value.GetObject();
                const auto mediaType =
                    winrt::to_string(chunk.GetNamedString(L"mediaType"));
                if (mediaType.empty() || mediaType.size() > 1024) {
                    throw_response_error(
                        "Cloud speech response has an invalid audio media type");
                }
                auto bytes = decode_base64(chunk.GetNamedString(L"dataBase64"));
                if (bytes.empty() ||
                    bytes.size() > kMaxAudioBytes - totalAudioBytes) {
                    throw_response_error(
                        "Cloud speech response exceeds the audio size limit");
                }
                totalAudioBytes += bytes.size();
                result.chunks.push_back({mediaType, std::move(bytes)});
            }
        } else {
            throw_response_error(
                "Cloud speech response is missing audio payload");
        }

        const auto marks = object.GetNamedArray(L"speechMarks");
        if (marks.Size() > kMaxSpeechMarks) {
            throw_response_error(
                "Cloud speech response contains too many speech marks");
        }
        result.marks.reserve(marks.Size());
        for (const auto& value : marks) {
            const auto mark = value.GetObject();
            const auto type = mark.GetNamedString(L"type");
            const std::wstring_view typeView(type);
            SpeechMarkKind kind;
            if (typeView == L"word") {
                kind = SpeechMarkKind::Word;
            } else if (typeView == L"sentence") {
                kind = SpeechMarkKind::Sentence;
            } else {
                throw_response_error(
                    "Cloud speech response has an unknown mark type");
            }

            const auto start = required_uint32(mark, L"start");
            std::uint32_t length = 0;
            if (mark.HasKey(L"length")) {
                length = required_uint32(mark, L"length");
            } else if (mark.HasKey(L"end")) {
                const auto end = required_uint32(mark, L"end");
                if (end <= start) {
                    throw_response_error(
                        "Cloud speech response mark has end <= start");
                }
                length = end - start;
            } else {
                throw_response_error(
                    "Cloud speech response mark is missing length/end");
            }

            std::uint64_t timeMs = 0;
            if (mark.HasKey(L"timeMilliseconds")) {
                timeMs = required_milliseconds(mark, L"timeMilliseconds");
            } else if (mark.HasKey(L"time")) {
                timeMs = required_milliseconds(mark, L"time");
            } else {
                throw_response_error(
                    "Cloud speech response mark is missing timestamp");
            }

            if (length == 0 ||
                static_cast<std::size_t>(start) > inputLength ||
                static_cast<std::size_t>(length) >
                    inputLength - static_cast<std::size_t>(start) ||
                start > std::numeric_limits<TextOffset>::max() - baseOffset) {
                throw_response_error(
                    "Cloud speech response contains a mark outside the text");
            }
            result.marks.push_back(
                {kind, {baseOffset + start, length}, timeMs});
        }
        return result;
    } catch (const ReaderError&) {
        throw;
    } catch (const winrt::hresult_error&) {
        throw_response_error("Cloud speech response is malformed JSON");
    }
}

std::string read_response(HINTERNET request) {
    std::string response;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            throw_network_error("WinHttpQueryDataAvailable");
        }
        if (available == 0) {
            break;
        }
        if (available > kMaxResponseBytes - response.size()) {
            throw_response_error("Cloud speech response exceeds the size limit");
        }
        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read)) {
            throw_network_error("WinHttpReadData");
        }
        if (read == 0) {
            break;
        }
        response.append(buffer.data(), read);
    }
    return response;
}

std::string extract_error_detail(HINTERNET request) {
    try {
        const std::string body = read_response(request);
        if (body.empty()) {
            return {};
        }
        using namespace winrt::Windows::Data::Json;
        const auto wide = utf8_to_wide(body);
        const auto obj = JsonObject::Parse(winrt::hstring(wide));
        if (obj.HasKey(L"message")) {
            return winrt::to_string(obj.GetNamedString(L"message"));
        }
    } catch (...) {
    }
    return {};
}

void validate_request(const CloudSpeechRequest& request) {
    if (request.text.empty() || request.voiceId.empty() ||
        request.voiceId.size() > 512 ||
        request.text.size() > std::numeric_limits<TextOffset>::max() ||
        !std::isfinite(request.rate) || request.rate < 0.5F ||
        request.rate > 2.0F) {
        throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                          "Cloud speech request has invalid text, voice, or rate");
    }
    if (request.selection &&
        (request.selection->length == 0 ||
         request.selection->start > request.text.size() ||
         request.selection->length >
             request.text.size() - request.selection->start)) {
        throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                          "Cloud speech selection is outside the document");
    }
}

}  // namespace

class WindowsCloudSpeechClient::Impl final {
public:
    Impl(std::wstring serviceBaseUrl, AccessTokenProvider tokenProvider)
        : endpoint_(parse_endpoint(serviceBaseUrl)),
          tokenProvider_(std::move(tokenProvider)) {}

    CloudSpeechResult synthesize(const CloudSpeechRequest& request) const {
        validate_request(request);
        ApartmentScope apartment;

        std::u16string text = request.text;
        TextOffset baseOffset = 0;
        if (request.selection) {
            baseOffset = request.selection->start;
            text = request.text.substr(request.selection->start,
                                       request.selection->length);
        }
        std::string body;
        try {
            body = request_json(text, request.voiceId, request.rate, {});
        } catch (const winrt::hresult_error&) {
            throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                              "Could not serialize the cloud speech request");
        }
        if (body.size() > std::numeric_limits<DWORD>::max()) {
            throw ReaderError(ReaderErrorCode::NetworkFailed,
                              "Cloud speech request exceeds the HTTP size limit");
        }

        std::string accessToken;
        if (tokenProvider_) {
            try {
                accessToken = tokenProvider_();
            } catch (const std::exception&) {
                throw ReaderError(
                    ReaderErrorCode::NetworkFailed,
                    "Could not obtain an access token for cloud speech");
            }
        }
        if (accessToken.size() > 8192) {
            throw ReaderError(ReaderErrorCode::NetworkFailed,
                              "Cloud speech access token is too large");
        }

        WinHttpHandle session(
            WinHttpOpen(L"DyslexiaReaderDesktop/0.1",
                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session) {
            throw_network_error("WinHttpOpen");
        }
        if (!WinHttpSetTimeouts(session.get(), 10000, 10000, 30000, 120000)) {
            throw_network_error("WinHttpSetTimeouts");
        }
        WinHttpHandle connection(WinHttpConnect(
            session.get(), endpoint_.host.c_str(), endpoint_.port, 0));
        if (!connection) {
            throw_network_error("WinHttpConnect");
        }
        WinHttpHandle httpRequest(WinHttpOpenRequest(
            connection.get(), L"POST", endpoint_.path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            endpoint_.secure ? WINHTTP_FLAG_SECURE : 0));
        if (!httpRequest) {
            throw_network_error("WinHttpOpenRequest");
        }

        DWORD disableRedirects = WINHTTP_DISABLE_REDIRECTS;
        if (!WinHttpSetOption(httpRequest.get(), WINHTTP_OPTION_DISABLE_FEATURE,
                              &disableRedirects, sizeof(disableRedirects))) {
            throw_network_error("WinHttpSetOption");
        }

        std::wstring headers =
            L"Content-Type: application/json; charset=utf-8\r\n"
            L"Accept: application/json\r\n";
        if (!accessToken.empty()) {
            for (const unsigned char character : accessToken) {
                if (character < 0x21 || character > 0x7E) {
                    throw ReaderError(
                        ReaderErrorCode::NetworkFailed,
                        "Cloud speech access token contains invalid characters");
                }
            }
            headers += L"Authorization: Bearer ";
            headers += utf8_to_wide(accessToken);
            headers += L"\r\n";
        }
        if (headers.size() > std::numeric_limits<DWORD>::max()) {
            throw ReaderError(ReaderErrorCode::NetworkFailed,
                              "Cloud speech request headers are too large");
        }
        if (!WinHttpAddRequestHeaders(
                httpRequest.get(), headers.c_str(),
                static_cast<DWORD>(headers.size()),
                WINHTTP_ADDREQ_FLAG_ADD)) {
            throw_network_error("WinHttpAddRequestHeaders");
        }
        if (!WinHttpSendRequest(
                httpRequest.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                const_cast<char*>(body.data()),
                static_cast<DWORD>(body.size()),
                static_cast<DWORD>(body.size()), 0)) {
            throw_network_error("WinHttpSendRequest");
        }
        if (!WinHttpReceiveResponse(httpRequest.get(), nullptr)) {
            throw_network_error("WinHttpReceiveResponse");
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(
                httpRequest.get(),
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                WINHTTP_NO_HEADER_INDEX)) {
            throw_network_error("WinHttpQueryHeaders");
        }
        if (status < 200 || status >= 300) {
            const std::string detail = extract_error_detail(httpRequest.get());
            std::string message =
                "Cloud speech service returned HTTP status " +
                std::to_string(status);
            if (!detail.empty()) {
                message += ": " + detail;
            }
            throw ReaderError(ReaderErrorCode::NetworkFailed, message);
        }

        const auto response = read_response(httpRequest.get());
        return parse_response(response, text.size(), baseOffset);
    }

private:
    Endpoint endpoint_;
    AccessTokenProvider tokenProvider_;
};

WindowsCloudSpeechClient::WindowsCloudSpeechClient(
    std::wstring serviceBaseUrl, AccessTokenProvider tokenProvider)
    : impl_(std::make_unique<Impl>(std::move(serviceBaseUrl),
                                   std::move(tokenProvider))) {}

WindowsCloudSpeechClient::~WindowsCloudSpeechClient() = default;

CloudSpeechResult WindowsCloudSpeechClient::synthesize(
    const CloudSpeechRequest& request) {
    return impl_->synthesize(request);
}

std::string WindowsCloudSpeechClient::build_synthesize_request_json(
    const CloudSpeechRequest& request, std::string_view language) {
    validate_request(request);
    std::u16string text = request.text;
    if (request.selection) {
        text = request.text.substr(request.selection->start,
                                   request.selection->length);
    }
    try {
        return request_json(text, request.voiceId, request.rate, language);
    } catch (const winrt::hresult_error&) {
        throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                          "Could not serialize the cloud speech request");
    }
}

CloudSpeechResult WindowsCloudSpeechClient::parse_synthesize_response_json(
    const std::string& json, std::size_t inputLength,
    TextOffset baseOffset) {
    return parse_response(json, inputLength, baseOffset);
}

}  // namespace dyslexia_reader::windows
