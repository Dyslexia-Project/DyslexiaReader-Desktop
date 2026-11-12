#include "dyslexia_reader/windows/word_insights.hpp"
#include "theme_palette.hpp"

#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dyslexia_reader::windows {
namespace {

using Json = nlohmann::json;

constexpr wchar_t kUserAgent[] = L"DyslexiaReader-Desktop/1.0";
constexpr int kLookupTimeoutMs = 3500;
constexpr std::size_t kMaxLookupResponseBytes = 256 * 1024;
constexpr std::size_t kMaxDefinitions = 4;
constexpr std::size_t kMaxSynonyms = 14;
constexpr UINT kMsgLookupReady = WM_APP + 0x61;

constexpr int kQueryEditId = 2001;
constexpr int kSearchBtnId = 2002;
constexpr int kSpeakBtnId = 2003;
constexpr int kCopyBtnId = 2004;
constexpr int kTopBarHeight = 46;

struct WinHttpHandle {
    HINTERNET handle = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) noexcept : handle(h) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    ~WinHttpHandle() {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
};

struct GdiObjectGuard {
    HGDIOBJ obj = nullptr;
    explicit GdiObjectGuard(HGDIOBJ o = nullptr) noexcept : obj(o) {}
    GdiObjectGuard(const GdiObjectGuard&) = delete;
    GdiObjectGuard& operator=(const GdiObjectGuard&) = delete;
    ~GdiObjectGuard() {
        if (obj != nullptr) {
            DeleteObject(obj);
        }
    }
};

std::string utf8_from_utf16(std::u16string_view u16) {
    if (u16.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, reinterpret_cast<const wchar_t*>(u16.data()),
        static_cast<int>(u16.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(u16.data()),
                        static_cast<int>(u16.size()), out.data(), needed,
                        nullptr, nullptr);
    return out;
}

std::wstring wide_from_utf8(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                        static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

std::string utf8_from_wide(std::wstring_view wide) {
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
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

COLORREF colorref_from_hex(std::string_view hex) {
    if (hex.size() == 7 && hex[0] == '#') {
        auto nibble = [](char ch) -> unsigned int {
            if (ch >= '0' && ch <= '9') return static_cast<unsigned int>(ch - '0');
            if (ch >= 'a' && ch <= 'f') return static_cast<unsigned int>(ch - 'a' + 10);
            if (ch >= 'A' && ch <= 'F') return static_cast<unsigned int>(ch - 'A' + 10);
            return 0U;
        };
        const unsigned int r = (nibble(hex[1]) << 4U) | nibble(hex[2]);
        const unsigned int g = (nibble(hex[3]) << 4U) | nibble(hex[4]);
        const unsigned int b = (nibble(hex[5]) << 4U) | nibble(hex[6]);
        return RGB(r, g, b);
    }
    return RGB(17, 17, 17);
}

const theme_palette::ThemeHex& palette_for_theme(ThemeId theme) {
    switch (theme) {
    case ThemeId::WarmCream:
        return theme_palette::warmCream;
    case ThemeId::Sepia:
        return theme_palette::sepia;
    case ThemeId::DarkHighContrast:
        return theme_palette::darkHighContrast;
    }
    return theme_palette::warmCream;
}

COLORREF blend_color(COLORREF a, COLORREF b, float t) {
    const float clamped = std::clamp(t, 0.0F, 1.0F);
    const auto mix = [clamped](int c1, int c2) -> BYTE {
        return static_cast<BYTE>(std::lround(
            static_cast<float>(c1) * (1.0F - clamped) +
            static_cast<float>(c2) * clamped));
    };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
               mix(GetBValue(a), GetBValue(b)));
}

HFONT create_ui_font(int pixelHeight, int weight, const wchar_t* face = L"Segoe UI") {
    return CreateFontW(-pixelHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

bool is_stop_word(std::string_view lowerWord) {
    static const std::unordered_set<std::string_view> kStopWords = {
        "the",   "and",   "for",   "with",  "that",  "this",  "from",  "are",
        "was",   "were",  "not",   "but",   "you",   "your",  "can",   "will",
        "have",  "has",   "had",   "into",  "onto",  "about", "more",  "some",
        "such",  "than",  "then",  "they",  "them",  "their", "there", "these",
        "those", "what",  "when",  "where", "which", "while", "who",   "whom",
        "whose", "why",   "how",   "all",   "any",   "both",  "each",  "few",
        "most",  "other", "over",  "under", "again", "very",  "just",  "only",
        "also",  "been",  "being", "could", "would", "should","might", "must",
        "does",  "did",   "doing", "done",  "she",   "her",   "hers",  "him",
        "his",   "its",   "our",   "ours",  "out",   "off",   "too",   "use",
        "used",  "using", "one",   "two",   "may",   "can",   "now",   "way",
        // Common Romanian stop words as well
        "sau",   "dar",   "din",   "prin",  "pentru","care",  "este",  "sunt",
        "fost",  "mai",   "cum",   "cel",   "cea",   "cei",   "cele",  "lui",
        "lor",   "una",   "unui",  "unei",  "fara",  "peste", "sub",   "intre",
        "doar",  "daca",  "cand",  "unde",  "atunci","acest", "acesta","aceasta",
    };
    return kStopWords.find(lowerWord) != kStopWords.end();
}

std::string trim_word_punctuation(std::string_view raw) {
    auto isEdgeGarbage = [](unsigned char ch) {
        return std::isspace(ch) != 0 ||
               (ch < 0x80 && std::isalpha(ch) == 0);
    };
    std::size_t start = 0;
    while (start < raw.size() &&
           isEdgeGarbage(static_cast<unsigned char>(raw[start]))) {
        ++start;
    }
    std::size_t end = raw.size();
    while (end > start &&
           isEdgeGarbage(static_cast<unsigned char>(raw[end - 1]))) {
        --end;
    }
    return std::string(raw.substr(start, end - start));
}

std::string lowercase_ascii(std::string s) {
    for (char& ch : s) {
        if (static_cast<unsigned char>(ch) < 0x80) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
    }
    return s;
}

std::string percent_encode_utf8(std::string_view input) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 2);
    for (unsigned char ch : input) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[(ch >> 4U) & 0x0FU]);
            out.push_back(kHex[ch & 0x0FU]);
        }
    }
    return out;
}

bool https_get_json(const wchar_t* host, const std::wstring& path,
                    std::string& responseBody, DWORD& httpStatus) {
    httpStatus = 0;
    responseBody.clear();

    WinHttpHandle session(WinHttpOpen(kUserAgent,
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.handle == nullptr) {
        return false;
    }
    WinHttpSetTimeouts(session.handle, kLookupTimeoutMs, kLookupTimeoutMs,
                       kLookupTimeoutMs, kLookupTimeoutMs);

    WinHttpHandle connection(
        WinHttpConnect(session.handle, host, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (connection.handle == nullptr) {
        return false;
    }

    WinHttpHandle request(WinHttpOpenRequest(
        connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (request.handle == nullptr) {
        return false;
    }

    constexpr wchar_t kHeaders[] = L"Accept: application/json\r\n";
    if (!WinHttpSendRequest(request.handle, kHeaders,
                            static_cast<DWORD>(wcslen(kHeaders)),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return false;
    }
    if (!WinHttpReceiveResponse(request.handle, nullptr)) {
        return false;
    }

    DWORD statusSize = sizeof(httpStatus);
    if (!WinHttpQueryHeaders(
            request.handle,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &httpStatus, &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available)) {
            return false;
        }
        if (available == 0) {
            break;
        }
        if (responseBody.size() + static_cast<std::size_t>(available) >
            kMaxLookupResponseBytes) {
            return false;
        }
        const std::size_t offset = responseBody.size();
        responseBody.resize(offset + static_cast<std::size_t>(available));
        DWORD readBytes = 0;
        if (!WinHttpReadData(request.handle, responseBody.data() + offset,
                             available, &readBytes)) {
            return false;
        }
        responseBody.resize(offset + static_cast<std::size_t>(readBytes));
    }
    return httpStatus == 200;
}

void append_unique_synonym(std::vector<std::string>& dst,
                           std::unordered_set<std::string>& seen,
                           const std::string& candidate,
                           const std::string& rootWord) {
    if (dst.size() >= kMaxSynonyms) {
        return;
    }
    std::string trimmed = trim_word_punctuation(candidate);
    if (trimmed.empty() && !candidate.empty()) {
        // Multi-word synonym expression fallback
        std::size_t s = 0;
        while (s < candidate.size() &&
               std::isspace(static_cast<unsigned char>(candidate[s])) != 0) {
            ++s;
        }
        std::size_t e = candidate.size();
        while (e > s &&
               std::isspace(static_cast<unsigned char>(candidate[e - 1])) != 0) {
            --e;
        }
        trimmed = candidate.substr(s, e - s);
    }
    if (trimmed.empty()) {
        return;
    }
    const std::string key = lowercase_ascii(trimmed);
    if (key == rootWord || !seen.insert(key).second) {
        return;
    }
    dst.push_back(key);
}

bool copy_text_to_clipboard(HWND owner, const std::string& utf8) {
    const std::wstring wide = wide_from_utf8(utf8);
    if (!OpenClipboard(owner)) {
        return false;
    }
    struct Guard {
        ~Guard() { CloseClipboard(); }
    } guard;
    if (!EmptyClipboard()) {
        return false;
    }
    const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem == nullptr) {
        return false;
    }
    void* ptr = GlobalLock(mem);
    if (ptr == nullptr) {
        GlobalFree(mem);
        return false;
    }
    std::memcpy(ptr, wide.c_str(), bytes);
    GlobalUnlock(mem);
    if (SetClipboardData(CF_UNICODETEXT, mem) == nullptr) {
        GlobalFree(mem);
        return false;
    }
    return true;
}

}  // namespace

std::vector<WordCloudItem> build_word_cloud(const ReadingDocument& document,
                                            std::size_t maxWords) {
    if (maxWords == 0 || document.words.empty() || document.text.empty()) {
        return {};
    }

    struct Acc {
        std::string display;
        std::size_t count = 0;
        std::size_t firstIndex = 0;
    };

    std::unordered_map<std::string, Acc> counts;
    counts.reserve(document.words.size());

    for (std::size_t i = 0; i < document.words.size(); ++i) {
        const TextRange range = document.words[i].text;
        if (range.length == 0 || range.start >= document.text.size() ||
            range.length > document.text.size() - range.start) {
            continue;
        }
        const std::u16string_view slice(document.text.data() + range.start,
                                        range.length);
        const std::string utf8 = utf8_from_utf16(slice);
        std::string cleaned = trim_word_punctuation(utf8);
        if (cleaned.size() < 3) {
            continue;
        }
        std::string norm = lowercase_ascii(cleaned);
        if (is_stop_word(norm)) {
            continue;
        }
        auto it = counts.find(norm);
        if (it == counts.end()) {
            counts.emplace(std::move(norm), Acc{norm, 1, i});
        } else {
            ++it->second.count;
        }
    }

    std::vector<WordCloudItem> items;
    items.reserve(counts.size());
    for (auto& [norm, acc] : counts) {
        WordCloudItem item;
        item.word = std::move(acc.display);
        item.normalized = norm;
        item.count = acc.count;
        item.firstWordIndex = acc.firstIndex;
        items.push_back(std::move(item));
    }

    std::sort(items.begin(), items.end(),
              [](const WordCloudItem& a, const WordCloudItem& b) {
                  if (a.count != b.count) {
                      return a.count > b.count;
                  }
                  return a.firstWordIndex < b.firstWordIndex;
              });

    if (items.size() > maxWords) {
        items.resize(maxWords);
    }
    if (items.empty()) {
        return items;
    }

    const std::size_t maxCount = items.front().count;
    const std::size_t minCount = items.back().count;
    for (auto& item : items) {
        if (maxCount == minCount) {
            item.weight = 1.0F;
        } else {
            item.weight = static_cast<float>(item.count - minCount) /
                          static_cast<float>(maxCount - minCount);
        }
    }
    return items;
}

std::string FastWordLookupClient::normalize_lookup_word(
    const std::string& rawWord) {
    std::string cleaned = trim_word_punctuation(rawWord);
    if (cleaned.empty() || cleaned.size() > 64) {
        return {};
    }
    return lowercase_ascii(std::move(cleaned));
}

void FastWordLookupClient::parse_dictionary_api_json(
    const std::string& jsonText, WordLookupResult& out) {
    Json root;
    try {
        root = Json::parse(jsonText);
    } catch (const Json::exception&) {
        return;
    }
    if (!root.is_array() || root.empty()) {
        return;
    }

    std::unordered_set<std::string> seenSynonyms(out.synonyms.begin(),
                                                 out.synonyms.end());
    const std::string rootWord = lowercase_ascii(out.word);

    for (const auto& entry : root) {
        if (!entry.is_object()) {
            continue;
        }
        if (out.phonetic.empty()) {
            const auto phIt = entry.find("phonetic");
            if (phIt != entry.end() && phIt->is_string()) {
                out.phonetic = phIt->get<std::string>();
            } else {
                const auto phArr = entry.find("phonetics");
                if (phArr != entry.end() && phArr->is_array()) {
                    for (const auto& item : *phArr) {
                        if (item.is_object() && item.contains("text") &&
                            item["text"].is_string() &&
                            !item["text"].get_ref<const std::string&>().empty()) {
                            out.phonetic = item["text"].get<std::string>();
                            break;
                        }
                    }
                }
            }
        }

        const auto meaningsIt = entry.find("meanings");
        if (meaningsIt == entry.end() || !meaningsIt->is_array()) {
            continue;
        }
        for (const auto& meaning : *meaningsIt) {
            if (!meaning.is_object()) {
                continue;
            }
            std::string pos;
            if (meaning.contains("partOfSpeech") &&
                meaning["partOfSpeech"].is_string()) {
                pos = meaning["partOfSpeech"].get<std::string>();
            }
            if (meaning.contains("synonyms") && meaning["synonyms"].is_array()) {
                for (const auto& syn : meaning["synonyms"]) {
                    if (syn.is_string()) {
                        append_unique_synonym(out.synonyms, seenSynonyms,
                                              syn.get<std::string>(), rootWord);
                    }
                }
            }
            if (meaning.contains("definitions") &&
                meaning["definitions"].is_array()) {
                for (const auto& def : meaning["definitions"]) {
                    if (!def.is_object()) {
                        continue;
                    }
                    if (def.contains("synonyms") && def["synonyms"].is_array()) {
                        for (const auto& syn : def["synonyms"]) {
                            if (syn.is_string()) {
                                append_unique_synonym(out.synonyms, seenSynonyms,
                                                      syn.get<std::string>(),
                                                      rootWord);
                            }
                        }
                    }
                    if (out.definitions.size() < kMaxDefinitions &&
                        def.contains("definition") &&
                        def["definition"].is_string()) {
                        WordDefinitionEntry d;
                        d.partOfSpeech = pos;
                        d.definition = def["definition"].get<std::string>();
                        if (def.contains("example") &&
                            def["example"].is_string()) {
                            d.example = def["example"].get<std::string>();
                        }
                        if (!d.definition.empty()) {
                            out.definitions.push_back(std::move(d));
                        }
                    }
                }
            }
        }
    }
}

std::vector<std::string> FastWordLookupClient::parse_datamuse_synonyms_json(
    const std::string& jsonText) {
    Json root;
    try {
        root = Json::parse(jsonText);
    } catch (const Json::exception&) {
        return {};
    }
    if (!root.is_array()) {
        return {};
    }
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    for (const auto& item : root) {
        if (!item.is_object() || !item.contains("word") ||
            !item["word"].is_string()) {
            continue;
        }
        std::string w = lowercase_ascii(item["word"].get<std::string>());
        if (!w.empty() && seen.insert(w).second) {
            result.push_back(std::move(w));
            if (result.size() >= kMaxSynonyms) {
                break;
            }
        }
    }
    return result;
}

WordLookupResult FastWordLookupClient::lookup(const std::string& rawWord) {
    const std::string normalized = normalize_lookup_word(rawWord);
    if (normalized.empty()) {
        WordLookupResult emptyErr;
        emptyErr.word = rawWord;
        emptyErr.errorMessage = "Select or enter an alphabetic word first.";
        return emptyErr;
    }

    {
        std::scoped_lock lock(cacheMutex_);
        const auto it = cache_.find(normalized);
        if (it != cache_.end()) {
            WordLookupResult cached = it->second;
            cached.fromCache = true;
            return cached;
        }
    }

    const std::wstring encoded =
        wide_from_utf8(percent_encode_utf8(normalized));

    struct FetchOutcome {
        bool ok = false;
        DWORD status = 0;
        std::string body;
    };

    auto dictFuture = std::async(std::launch::async, [encoded]() {
        FetchOutcome o;
        o.ok = https_get_json(L"api.dictionaryapi.dev",
                              L"/api/v2/entries/en/" + encoded, o.body,
                              o.status);
        return o;
    });

    auto synFuture = std::async(std::launch::async, [encoded]() {
        FetchOutcome o;
        o.ok = https_get_json(L"api.datamuse.com",
                              L"/words?rel_syn=" + encoded + L"&max=12", o.body,
                              o.status);
        return o;
    });

    const FetchOutcome dictRes = dictFuture.get();
    const FetchOutcome synRes = synFuture.get();

    WordLookupResult result;
    result.word = normalized;
    if (dictRes.ok) {
        parse_dictionary_api_json(dictRes.body, result);
    }

    if (synRes.ok) {
        std::unordered_set<std::string> seen(result.synonyms.begin(),
                                             result.synonyms.end());
        for (const auto& s : parse_datamuse_synonyms_json(synRes.body)) {
            append_unique_synonym(result.synonyms, seen, s, normalized);
        }
    }

    if (result.definitions.empty() && result.synonyms.empty()) {
        if (dictRes.status == 404 && (synRes.ok || synRes.status == 200)) {
            result.errorMessage =
                "No dictionary entry or synonyms found for \"" + normalized +
                "\".";
        } else if (dictRes.status == 0 && synRes.status == 0) {
            result.errorMessage =
                "Dictionary lookup requires an internet connection.";
        } else {
            result.errorMessage =
                "No definition or synonyms available for \"" + normalized + "\".";
        }
        return result;
    }

    {
        std::scoped_lock lock(cacheMutex_);
        cache_.emplace(normalized, result);
    }
    return result;
}

// ------------------------- WordCloudWindow -------------------------

WordCloudWindow::~WordCloudWindow() { close(); }

void WordCloudWindow::close() {
    if (hwnd_ != nullptr && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
}

void WordCloudWindow::show(HWND owner, ThemeId theme,
                           const std::vector<WordCloudItem>& items,
                           WordActivatedCallback onWordActivated) {
    theme_ = theme;
    items_ = items;
    onWordActivated_ = std::move(onWordActivated);
    hoveredIndex_ = -1;

    constexpr wchar_t kClassName[] = L"DyslexiaReader.WordCloudWindow";
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &WordCloudWindow::wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    if (hwnd_ == nullptr || !IsWindow(hwnd_)) {
        int x = CW_USEDEFAULT;
        int y = CW_USEDEFAULT;
        constexpr int kW = 700;
        constexpr int kH = 480;
        RECT ownerRect{};
        if (owner != nullptr && GetWindowRect(owner, &ownerRect)) {
            x = ownerRect.left +
                ((ownerRect.right - ownerRect.left - kW) / 2);
            y = ownerRect.top +
                ((ownerRect.bottom - ownerRect.top - kH) / 2);
        }
        hwnd_ = CreateWindowExW(
            0, kClassName, L"Document Word Cloud — Click any word to define & jump",
            WS_OVERLAPPEDWINDOW, x, y, kW, kH, owner, nullptr, instance, this);
    }

    if (hwnd_ != nullptr) {
        recompute_layout();
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void WordCloudWindow::update_theme(ThemeId theme) {
    theme_ = theme;
    if (hwnd_ != nullptr && IsWindow(hwnd_)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void WordCloudWindow::update_items(const std::vector<WordCloudItem>& items) {
    items_ = items;
    hoveredIndex_ = -1;
    if (hwnd_ != nullptr && IsWindow(hwnd_)) {
        recompute_layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

LRESULT CALLBACK WordCloudWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                           LPARAM lParam) {
    auto* self = reinterpret_cast<WordCloudWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<WordCloudWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->hwnd_ = hwnd;
        }
    }
    if (self != nullptr) {
        return self->handle_message(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT WordCloudWindow::handle_message(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        recompute_layout();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        paint(hdc, rc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT pt{static_cast<LONG>(static_cast<SHORT>(LOWORD(lParam))),
                       static_cast<LONG>(static_cast<SHORT>(HIWORD(lParam)))};
        int found = -1;
        for (int i = 0; i < static_cast<int>(layout_.size()); ++i) {
            if (PtInRect(&layout_[static_cast<std::size_t>(i)].bounds, pt)) {
                found = i;
                break;
            }
        }
        if (found != hoveredIndex_) {
            hoveredIndex_ = found;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT pt{static_cast<LONG>(static_cast<SHORT>(LOWORD(lParam))),
                       static_cast<LONG>(static_cast<SHORT>(HIWORD(lParam)))};
        for (const auto& item : layout_) {
            if (PtInRect(&item.bounds, pt)) {
                if (onWordActivated_) {
                    onWordActivated_(item.data.firstWordIndex, item.data.word);
                }
                return 0;
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            ShowWindow(hwnd_, SW_HIDE);
            return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(hwnd_, SW_HIDE);
        return 0;
    case WM_NCDESTROY:
        hwnd_ = nullptr;
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void WordCloudWindow::recompute_layout() {
    layout_.clear();
    if (hwnd_ == nullptr || items_.empty()) {
        return;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int clientW = std::max(280, static_cast<int>(rc.right - rc.left));
    const int marginX = 24;
    const int usableW = std::max(200, clientW - marginX * 2);

    const auto& tokens = palette_for_theme(theme_);
    const COLORREF textCol = colorref_from_hex(tokens.text);
    const COLORREF focusCol = colorref_from_hex(tokens.focus);
    const COLORREF accent2 =
        theme_ == ThemeId::DarkHighContrast ? RGB(125, 211, 252)
                                            : RGB(14, 116, 144);
    const COLORREF accent3 =
        theme_ == ThemeId::DarkHighContrast ? RGB(167, 243, 208)
                                            : RGB(124, 58, 237);

    // Deterministic visual interleaving so big words are centered near middle,
    // surrounded smoothly by medium/small terms without colliding.
    std::vector<std::size_t> visualOrder(items_.size());
    std::size_t left = 0;
    std::size_t right = items_.size();
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (i % 2 == 0) {
            visualOrder[left++] = items_.size() - 1 - i;
        } else {
            visualOrder[--right] = items_.size() - 1 - i;
        }
    }

    HDC screenDc = GetDC(hwnd_);
    struct RowEntry {
        LayoutItem item;
        int w = 0;
        int h = 0;
    };
    std::vector<std::vector<RowEntry>> rows(1);
    int curRowW = 0;
    constexpr int kGapX = 14;
    constexpr int kGapY = 12;

    for (const std::size_t idx : visualOrder) {
        const auto& src = items_[idx];
        const int fontPx =
            16 + static_cast<int>(std::lround(src.weight * 20.0F));
        const int fontWeight =
            src.weight >= 0.55F ? FW_BOLD : FW_SEMIBOLD;
        GdiObjectGuard font(create_ui_font(fontPx, fontWeight));
        HGDIOBJ oldFont = SelectObject(screenDc, font.obj);

        const std::wstring wide = wide_from_utf8(src.word);
        SIZE sz{};
        GetTextExtentPoint32W(screenDc, wide.c_str(),
                              static_cast<int>(wide.size()), &sz);
        SelectObject(screenDc, oldFont);

        const int pillW = static_cast<int>(sz.cx) + 18;
        const int pillH = static_cast<int>(sz.cy) + 10;
        if (!rows.back().empty() && curRowW + kGapX + pillW > usableW) {
            rows.emplace_back();
            curRowW = 0;
        }
        if (!rows.back().empty()) {
            curRowW += kGapX;
        }
        curRowW += pillW;

        COLORREF col = textCol;
        if (src.weight >= 0.72F) {
            col = focusCol;
        } else if (idx % 3 == 1) {
            col = accent2;
        } else if (idx % 3 == 2) {
            col = accent3;
        }
        RowEntry entry;
        entry.item.data = src;
        entry.item.fontPx = fontPx;
        entry.item.color = col;
        entry.w = pillW;
        entry.h = pillH;
        rows.back().push_back(std::move(entry));
    }
    ReleaseDC(hwnd_, screenDc);

    int totalH = 0;
    std::vector<int> rowHeights;
    rowHeights.reserve(rows.size());
    for (const auto& row : rows) {
        int mh = 0;
        for (const auto& e : row) {
            mh = std::max(mh, e.h);
        }
        rowHeights.push_back(mh);
        totalH += mh + kGapY;
    }

    const int clientH = std::max(200, static_cast<int>(rc.bottom - rc.top) - 36);
    int curY = std::max(20, (clientH - totalH) / 2);

    for (std::size_t r = 0; r < rows.size(); ++r) {
        int rowW = 0;
        for (std::size_t c = 0; c < rows[r].size(); ++c) {
            if (c > 0) {
                rowW += kGapX;
            }
            rowW += rows[r][c].w;
        }
        int curX = std::max(marginX, (clientW - rowW) / 2);
        const int rh = rowHeights[r];
        for (auto& e : rows[r]) {
            const int y = curY + (rh - e.h) / 2;
            e.item.bounds = RECT{curX, y, curX + e.w, y + e.h};
            layout_.push_back(std::move(e.item));
            curX += e.w + kGapX;
        }
        curY += rh + kGapY;
    }
}

void WordCloudWindow::paint(HDC hdc, const RECT& clientRect) {
    const int width = std::max(1, static_cast<int>(clientRect.right - clientRect.left));
    const int height = std::max(1, static_cast<int>(clientRect.bottom - clientRect.top));

    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, width, height);
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    const auto& tokens = palette_for_theme(theme_);
    const COLORREF bgCol = colorref_from_hex(tokens.background);
    const COLORREF surfaceCol = colorref_from_hex(tokens.surface);
    const COLORREF borderCol = colorref_from_hex(tokens.border);
    const COLORREF textCol = colorref_from_hex(tokens.text);
    const COLORREF focusCol = colorref_from_hex(tokens.focus);
    const COLORREF hoverFill = colorref_from_hex(tokens.sentenceHighlight);

    {
        GdiObjectGuard bgBrush(CreateSolidBrush(bgCol));
        FillRect(memDc, &clientRect, static_cast<HBRUSH>(bgBrush.obj));
    }

    SetBkMode(memDc, TRANSPARENT);
    if (layout_.empty()) {
        GdiObjectGuard emptyFont(create_ui_font(18, FW_NORMAL));
        HGDIOBJ oldFont = SelectObject(memDc, emptyFont.obj);
        SetTextColor(memDc, textCol);
        RECT textRc = clientRect;
        DrawTextW(memDc,
                  L"Capture or paste text first to build a Word Cloud.", -1,
                  &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(memDc, oldFont);
    } else {
        for (int i = 0; i < static_cast<int>(layout_.size()); ++i) {
            const auto& item = layout_[static_cast<std::size_t>(i)];
            const bool hovered = (i == hoveredIndex_);
            if (hovered) {
                GdiObjectGuard fill(CreateSolidBrush(hoverFill));
                GdiObjectGuard pen(CreatePen(PS_SOLID, 2, focusCol));
                HGDIOBJ oldB = SelectObject(memDc, fill.obj);
                HGDIOBJ oldP = SelectObject(memDc, pen.obj);
                RoundRect(memDc, item.bounds.left, item.bounds.top,
                          item.bounds.right, item.bounds.bottom, 10, 10);
                SelectObject(memDc, oldP);
                SelectObject(memDc, oldB);
            }
            const int fontWeight =
                item.data.weight >= 0.55F ? FW_BOLD : FW_SEMIBOLD;
            GdiObjectGuard font(create_ui_font(item.fontPx, fontWeight));
            HGDIOBJ oldFont = SelectObject(memDc, font.obj);
            SetTextColor(memDc, item.color);
            RECT txtRc = item.bounds;
            const std::wstring wide = wide_from_utf8(item.data.word);
            DrawTextW(memDc, wide.c_str(), static_cast<int>(wide.size()),
                      &txtRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(memDc, oldFont);
        }
    }

    // Bottom status strip
    const RECT footerRc{0, height - 34, width, height};
    {
        GdiObjectGuard surfaceBrush(CreateSolidBrush(surfaceCol));
        FillRect(memDc, &footerRc, static_cast<HBRUSH>(surfaceBrush.obj));
        GdiObjectGuard pen(CreatePen(PS_SOLID, 1, borderCol));
        HGDIOBJ oldP = SelectObject(memDc, pen.obj);
        MoveToEx(memDc, 0, height - 34, nullptr);
        LineTo(memDc, width, height - 34);
        SelectObject(memDc, oldP);
    }

    std::wstring footerText = L"Click any word to jump in the reader & load fast definition + synonyms";
    if (hoveredIndex_ >= 0 &&
        hoveredIndex_ < static_cast<int>(layout_.size())) {
        const auto& h = layout_[static_cast<std::size_t>(hoveredIndex_)].data;
        footerText = wide_from_utf8(h.word) + L"  (" +
                     std::to_wstring(h.count) +
                     (h.count == 1 ? L" occurrence" : L" occurrences") +
                     L")  — Click to jump & define";
    }
    GdiObjectGuard footerFont(create_ui_font(14, FW_NORMAL));
    HGDIOBJ oldFooterFont = SelectObject(memDc, footerFont.obj);
    SetTextColor(memDc, textCol);
    RECT paddedFooter{14, height - 34, width - 14, height};
    DrawTextW(memDc, footerText.c_str(), static_cast<int>(footerText.size()),
              &paddedFooter, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(memDc, oldFooterFont);

    BitBlt(hdc, 0, 0, width, height, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);
}

// ------------------------- WordDefinitionWindow -------------------------

WordDefinitionWindow::WordDefinitionWindow(
    std::shared_ptr<FastWordLookupClient> lookupClient)
    : lookupClient_(std::move(lookupClient)),
      pendingState_(std::make_shared<PendingState>()) {}

WordDefinitionWindow::~WordDefinitionWindow() {
    if (pendingState_) {
        std::scoped_lock lock(pendingState_->mutex);
        pendingState_->alive = false;
    }
    close();
}

void WordDefinitionWindow::close() {
    if (hwnd_ != nullptr && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    if (bgBrush_ != nullptr) {
        DeleteObject(bgBrush_);
        bgBrush_ = nullptr;
    }
}

void WordDefinitionWindow::show_word(HWND owner, ThemeId theme,
                                     const std::string& rawWord,
                                     SpeakWordCallback onSpeakWord) {
    theme_ = theme;
    onSpeakWord_ = std::move(onSpeakWord);

    constexpr wchar_t kClassName[] = L"DyslexiaReader.WordDefinitionWindow";
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &WordDefinitionWindow::wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    if (hwnd_ == nullptr || !IsWindow(hwnd_)) {
        int x = CW_USEDEFAULT;
        int y = CW_USEDEFAULT;
        constexpr int kW = 520;
        constexpr int kH = 460;
        RECT ownerRect{};
        if (owner != nullptr && GetWindowRect(owner, &ownerRect)) {
            x = ownerRect.right - kW - 24;
            y = ownerRect.top + 88;
        }
        hwnd_ = CreateWindowExW(
            WS_EX_TOOLWINDOW, kClassName, L"Quick Definition & Synonyms",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VSCROLL,
            x, y, kW, kH, owner, nullptr, instance, this);
    }

    if (hwnd_ != nullptr) {
        update_theme(theme);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        start_lookup(rawWord);
    }
}

void WordDefinitionWindow::update_theme(ThemeId theme) {
    theme_ = theme;
    if (bgBrush_ != nullptr) {
        DeleteObject(bgBrush_);
        bgBrush_ = nullptr;
    }
    bgBrush_ = CreateSolidBrush(colorref_from_hex(palette_for_theme(theme_).surface));
    if (hwnd_ != nullptr && IsWindow(hwnd_)) {
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

void WordDefinitionWindow::start_lookup(const std::string& rawWord) {
    std::string cleaned = FastWordLookupClient::normalize_lookup_word(rawWord);
    if (cleaned.empty()) {
        cleaned = rawWord;
    }
    activeWord_ = cleaned;
    scrollY_ = 0;
    hoveredPill_ = -1;
    synonymPills_.clear();

    if (queryEdit_ != nullptr) {
        SetWindowTextW(queryEdit_, wide_from_utf8(activeWord_).c_str());
    }

    loading_ = true;
    result_ = WordLookupResult{};
    result_.word = activeWord_;
    update_scroll_bar();
    if (hwnd_ != nullptr) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    const std::uint64_t seq = ++requestSeq_;
    HWND target = hwnd_;
    std::shared_ptr<FastWordLookupClient> client = lookupClient_;
    std::shared_ptr<PendingState> shared = pendingState_;
    std::thread([target, client, shared, seq, active = activeWord_]() {
        WordLookupResult res = client ? client->lookup(active) : WordLookupResult{};
        bool shouldPost = false;
        if (shared) {
            std::scoped_lock lock(shared->mutex);
            if (shared->alive) {
                shared->seq = seq;
                shared->result = std::move(res);
                shouldPost = true;
            }
        }
        if (shouldPost && IsWindow(target)) {
            PostMessageW(target, kMsgLookupReady, static_cast<WPARAM>(seq), 0);
        }
    }).detach();
}

LRESULT CALLBACK WordDefinitionWindow::wnd_proc(HWND hwnd, UINT msg,
                                                WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<WordDefinitionWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<WordDefinitionWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->hwnd_ = hwnd;
        }
    }
    if (self != nullptr) {
        return self->handle_message(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void WordDefinitionWindow::layout_controls() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int width = std::max(360, static_cast<int>(rc.right - rc.left));
    const int copyW = 104;
    const int speakW = 68;
    const int searchW = 68;
    const int editW = std::max(100, width - 12 - 6 - searchW - 6 - speakW - 6 - copyW - 12);

    int x = 12;
    if (queryEdit_ != nullptr) {
        MoveWindow(queryEdit_, x, 10, editW, 26, TRUE);
    }
    x += editW + 6;
    if (searchButton_ != nullptr) {
        MoveWindow(searchButton_, x, 9, searchW, 28, TRUE);
    }
    x += searchW + 6;
    if (speakButton_ != nullptr) {
        MoveWindow(speakButton_, x, 9, speakW, 28, TRUE);
    }
    x += speakW + 6;
    if (copyButton_ != nullptr) {
        MoveWindow(copyButton_, x, 9, copyW, 28, TRUE);
    }
}

void WordDefinitionWindow::update_scroll_bar() {
    if (hwnd_ == nullptr) {
        return;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int viewH = std::max(1, static_cast<int>(rc.bottom - rc.top) - kTopBarHeight);
    const int maxScroll = std::max(0, contentHeight_ - viewH);
    scrollY_ = std::clamp(scrollY_, 0, maxScroll);

    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, contentHeight_ - 1);
    si.nPage = static_cast<UINT>(viewH);
    si.nPos = scrollY_;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

LRESULT WordDefinitionWindow::handle_message(UINT msg, WPARAM wParam,
                                             LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        auto guiFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        queryEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12, 10, 180,
            26, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kQueryEditId)),
            inst, nullptr);
        searchButton_ = CreateWindowExW(
            0, L"BUTTON", L"Lookup",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 200, 9, 68,
            28, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchBtnId)),
            inst, nullptr);
        speakButton_ = CreateWindowExW(
            0, L"BUTTON", L"Speak",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 274, 9, 68, 28,
            hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSpeakBtnId)),
            inst, nullptr);
        copyButton_ = CreateWindowExW(
            0, L"BUTTON", L"Copy Syns",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 348, 9, 104,
            28, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCopyBtnId)),
            inst, nullptr);
        for (HWND ctl : {queryEdit_, searchButton_, speakButton_, copyButton_}) {
            if (ctl != nullptr && guiFont != nullptr) {
                SendMessageW(ctl, WM_SETFONT, reinterpret_cast<WPARAM>(guiFont),
                             FALSE);
            }
        }
        layout_controls();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        layout_controls();
        update_scroll_bar();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (id == kSearchBtnId && code == BN_CLICKED && queryEdit_ != nullptr) {
            wchar_t buf[128]{};
            GetWindowTextW(queryEdit_, buf, 128);
            start_lookup(utf8_from_wide(buf));
            return 0;
        }
        if (id == kSpeakBtnId && code == BN_CLICKED) {
            if (onSpeakWord_ && !activeWord_.empty()) {
                onSpeakWord_(activeWord_);
            }
            return 0;
        }
        if (id == kCopyBtnId && code == BN_CLICKED && !result_.synonyms.empty()) {
            std::string joined;
            for (std::size_t i = 0; i < result_.synonyms.size(); ++i) {
                if (i > 0) {
                    joined += ", ";
                }
                joined += result_.synonyms[i];
            }
            copy_text_to_clipboard(hwnd_, joined);
            return 0;
        }
        break;
    }
    case kMsgLookupReady: {
        const std::uint64_t seq = static_cast<std::uint64_t>(wParam);
        if (seq != requestSeq_) {
            return 0;
        }
        if (pendingState_) {
            std::scoped_lock lock(pendingState_->mutex);
            if (pendingState_->seq == requestSeq_) {
                result_ = std::move(pendingState_->result);
            }
        }
        loading_ = false;
        if (copyButton_ != nullptr) {
            EnableWindow(copyButton_, result_.synonyms.empty() ? FALSE : TRUE);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT pt{static_cast<LONG>(static_cast<SHORT>(LOWORD(lParam))),
                       static_cast<LONG>(static_cast<SHORT>(HIWORD(lParam)))};
        int found = -1;
        for (int i = 0; i < static_cast<int>(synonymPills_.size()); ++i) {
            if (PtInRect(&synonymPills_[static_cast<std::size_t>(i)].bounds, pt)) {
                found = i;
                break;
            }
        }
        if (found != hoveredPill_) {
            hoveredPill_ = found;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT pt{static_cast<LONG>(static_cast<SHORT>(LOWORD(lParam))),
                       static_cast<LONG>(static_cast<SHORT>(HIWORD(lParam)))};
        for (const auto& pill : synonymPills_) {
            if (PtInRect(&pill.bounds, pt)) {
                start_lookup(pill.word);
                return 0;
            }
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        scrollY_ -= (delta / WHEEL_DELTA) * 36;
        update_scroll_bar();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd_, SB_VERT, &si);
        switch (LOWORD(wParam)) {
        case SB_LINEUP:
            scrollY_ -= 24;
            break;
        case SB_LINEDOWN:
            scrollY_ += 24;
            break;
        case SB_PAGEUP:
            scrollY_ -= static_cast<int>(si.nPage);
            break;
        case SB_PAGEDOWN:
            scrollY_ += static_cast<int>(si.nPage);
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION:
            scrollY_ = si.nTrackPos;
            break;
        default:
            break;
        }
        update_scroll_bar();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        paint_canvas(hdc, rc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            ShowWindow(hwnd_, SW_HIDE);
            return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(hwnd_, SW_HIDE);
        return 0;
    case WM_NCDESTROY:
        hwnd_ = nullptr;
        queryEdit_ = nullptr;
        searchButton_ = nullptr;
        speakButton_ = nullptr;
        copyButton_ = nullptr;
        return DefWindowProcW(hwnd_, msg, wParam, lParam);
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void WordDefinitionWindow::paint_canvas(HDC hdc, const RECT& clientRect) {
    const int width = std::max(1, static_cast<int>(clientRect.right - clientRect.left));
    const int height = std::max(1, static_cast<int>(clientRect.bottom - clientRect.top));

    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, width, height);
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    const auto& tokens = palette_for_theme(theme_);
    const COLORREF bgCol = colorref_from_hex(tokens.background);
    const COLORREF surfaceCol = colorref_from_hex(tokens.surface);
    const COLORREF borderCol = colorref_from_hex(tokens.border);
    const COLORREF textCol = colorref_from_hex(tokens.text);
    const COLORREF focusCol = colorref_from_hex(tokens.focus);
    const COLORREF pillHoverCol = colorref_from_hex(tokens.sentenceHighlight);
    const COLORREF mutedCol = blend_color(textCol, bgCol, 0.35F);

    {
        GdiObjectGuard bgBrush(CreateSolidBrush(bgCol));
        FillRect(memDc, &clientRect, static_cast<HBRUSH>(bgBrush.obj));
        const RECT topRc{0, 0, width, kTopBarHeight};
        GdiObjectGuard topBrush(CreateSolidBrush(surfaceCol));
        FillRect(memDc, &topRc, static_cast<HBRUSH>(topBrush.obj));
        GdiObjectGuard pen(CreatePen(PS_SOLID, 1, borderCol));
        HGDIOBJ oldP = SelectObject(memDc, pen.obj);
        MoveToEx(memDc, 0, kTopBarHeight - 1, nullptr);
        LineTo(memDc, width, kTopBarHeight - 1);
        SelectObject(memDc, oldP);
    }

    synonymPills_.clear();
    SetBkMode(memDc, TRANSPARENT);

    // Clip below toolbar bar
    const int saveState = SaveDC(memDc);
    IntersectClipRect(memDc, 0, kTopBarHeight, width, height);

    const int padX = 18;
    const int contentW = std::max(180, width - padX * 2);
    int y = kTopBarHeight + 14 - scrollY_;

    // Word header + phonetic
    {
        GdiObjectGuard headFont(create_ui_font(26, FW_BOLD));
        HGDIOBJ oldFont = SelectObject(memDc, headFont.obj);
        SetTextColor(memDc, textCol);
        std::wstring header = wide_from_utf8(activeWord_);
        if (!result_.phonetic.empty()) {
            header += L"   " + wide_from_utf8(result_.phonetic);
        }
        RECT hRc{padX, y, padX + contentW, y + 40};
        const int used =
            DrawTextW(memDc, header.c_str(), static_cast<int>(header.size()),
                      &hRc, DT_LEFT | DT_WORDBREAK);
        y += std::max(28, used) + 8;
        SelectObject(memDc, oldFont);
    }

    if (loading_) {
        GdiObjectGuard infoFont(create_ui_font(16, FW_NORMAL));
        HGDIOBJ oldFont = SelectObject(memDc, infoFont.obj);
        SetTextColor(memDc, mutedCol);
        RECT msgRc{padX, y, padX + contentW, y + 40};
        DrawTextW(memDc, L"Fetching definition and synonyms...", -1, &msgRc,
                  DT_LEFT | DT_SINGLELINE);
        SelectObject(memDc, oldFont);
        contentHeight_ = y + scrollY_ + 60;
    } else if (!result_.errorMessage.empty()) {
        GdiObjectGuard errFont(create_ui_font(16, FW_NORMAL));
        HGDIOBJ oldFont = SelectObject(memDc, errFont.obj);
        SetTextColor(memDc, textCol);
        const std::wstring msg = wide_from_utf8(result_.errorMessage);
        RECT msgRc{padX, y, padX + contentW, y + 120};
        const int used = DrawTextW(memDc, msg.c_str(),
                                   static_cast<int>(msg.size()), &msgRc,
                                   DT_LEFT | DT_WORDBREAK);
        SelectObject(memDc, oldFont);
        contentHeight_ = y + scrollY_ + used + 24;
    } else {
        // Synonyms section
        if (!result_.synonyms.empty()) {
            GdiObjectGuard secFont(create_ui_font(13, FW_BOLD));
            HGDIOBJ oldSec = SelectObject(memDc, secFont.obj);
            SetTextColor(memDc, focusCol);
            RECT secRc{padX, y, padX + contentW, y + 22};
            DrawTextW(memDc, L"SYNONYMS (click any to explore)", -1, &secRc,
                      DT_LEFT | DT_SINGLELINE);
            SelectObject(memDc, oldSec);
            y += 24;

            GdiObjectGuard chipFont(create_ui_font(15, FW_SEMIBOLD));
            HGDIOBJ oldChip = SelectObject(memDc, chipFont.obj);
            int px = padX;
            const int pillH = 28;
            for (int i = 0; i < static_cast<int>(result_.synonyms.size()); ++i) {
                const std::string& syn = result_.synonyms[static_cast<std::size_t>(i)];
                const std::wstring ws = wide_from_utf8(syn);
                SIZE sz{};
                GetTextExtentPoint32W(memDc, ws.c_str(),
                                      static_cast<int>(ws.size()), &sz);
                const int pillW = static_cast<int>(sz.cx) + 20;
                if (px > padX && px + pillW > padX + contentW) {
                    px = padX;
                    y += pillH + 8;
                }
                RECT pillRc{px, y, px + pillW, y + pillH};
                const bool hovered = (i == hoveredPill_);
                GdiObjectGuard pBrush(
                    CreateSolidBrush(hovered ? pillHoverCol : surfaceCol));
                GdiObjectGuard pPen(
                    CreatePen(PS_SOLID, hovered ? 2 : 1,
                              hovered ? focusCol : borderCol));
                HGDIOBJ ob = SelectObject(memDc, pBrush.obj);
                HGDIOBJ op = SelectObject(memDc, pPen.obj);
                RoundRect(memDc, pillRc.left, pillRc.top, pillRc.right,
                          pillRc.bottom, 14, 14);
                SelectObject(memDc, op);
                SelectObject(memDc, ob);

                SetTextColor(memDc, textCol);
                DrawTextW(memDc, ws.c_str(), static_cast<int>(ws.size()),
                          &pillRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (pillRc.bottom > kTopBarHeight && pillRc.top < height) {
                    synonymPills_.push_back(SynonymPill{syn, pillRc});
                }
                px += pillW + 8;
            }
            SelectObject(memDc, oldChip);
            y += pillH + 16;
        }

        // Definitions section
        if (!result_.definitions.empty()) {
            GdiObjectGuard secFont(create_ui_font(13, FW_BOLD));
            HGDIOBJ oldSec = SelectObject(memDc, secFont.obj);
            SetTextColor(memDc, focusCol);
            RECT secRc{padX, y, padX + contentW, y + 22};
            DrawTextW(memDc, L"DEFINITIONS", -1, &secRc, DT_LEFT | DT_SINGLELINE);
            SelectObject(memDc, oldSec);
            y += 26;

            for (std::size_t i = 0; i < result_.definitions.size(); ++i) {
                const auto& def = result_.definitions[i];
                if (!def.partOfSpeech.empty()) {
                    GdiObjectGuard posFont(create_ui_font(13, FW_BOLD));
                    HGDIOBJ oldPos = SelectObject(memDc, posFont.obj);
                    SetTextColor(memDc, focusCol);
                    const std::wstring pos =
                        wide_from_utf8(def.partOfSpeech);
                    RECT posRc{padX, y, padX + contentW, y + 20};
                    DrawTextW(memDc, pos.c_str(), static_cast<int>(pos.size()),
                              &posRc, DT_LEFT | DT_SINGLELINE);
                    SelectObject(memDc, oldPos);
                    y += 20;
                }

                GdiObjectGuard defFont(create_ui_font(16, FW_NORMAL));
                HGDIOBJ oldDef = SelectObject(memDc, defFont.obj);
                SetTextColor(memDc, textCol);
                const std::wstring text =
                    std::to_wstring(i + 1) + L". " +
                    wide_from_utf8(def.definition);
                RECT calcRc{padX, y, padX + contentW, y + 1000};
                const int defH = DrawTextW(memDc, text.c_str(),
                                           static_cast<int>(text.size()),
                                           &calcRc, DT_LEFT | DT_WORDBREAK);
                y += defH + 4;
                SelectObject(memDc, oldDef);

                if (!def.example.empty()) {
                    GdiObjectGuard exFont(create_ui_font(14, FW_NORMAL));
                    HGDIOBJ oldEx = SelectObject(memDc, exFont.obj);
                    SetTextColor(memDc, mutedCol);
                    const std::wstring exText =
                        L"Example: \"" + wide_from_utf8(def.example) + L"\"";
                    RECT exRc{padX + 12, y, padX + contentW, y + 400};
                    const int exH = DrawTextW(memDc, exText.c_str(),
                                              static_cast<int>(exText.size()),
                                              &exRc, DT_LEFT | DT_WORDBREAK);
                    y += exH + 4;
                    SelectObject(memDc, oldEx);
                }
                y += 10;
            }
        }
        contentHeight_ = (y + scrollY_) - kTopBarHeight + 16;
    }

    RestoreDC(memDc, saveState);
    update_scroll_bar();

    BitBlt(hdc, 0, 0, width, height, memDc, 0, 0, SRCCOPY);
    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);
}

}  // namespace dyslexia_reader::windows
