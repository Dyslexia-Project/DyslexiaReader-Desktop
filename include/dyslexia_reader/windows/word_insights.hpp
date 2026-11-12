#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dyslexia_reader/domain.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace dyslexia_reader::windows {

struct WordCloudItem {
    std::string word;
    std::string normalized;
    std::size_t count = 0;
    std::size_t firstWordIndex = 0;
    float weight = 0.0F;
};

struct WordDefinitionEntry {
    std::string partOfSpeech;
    std::string definition;
    std::string example;
};

struct WordLookupResult {
    std::string word;
    std::string phonetic;
    std::vector<WordDefinitionEntry> definitions;
    std::vector<std::string> synonyms;
    std::string errorMessage;
    bool fromCache = false;
};

[[nodiscard]] std::vector<WordCloudItem> build_word_cloud(
    const ReadingDocument& document, std::size_t maxWords = 45);

class FastWordLookupClient {
public:
    FastWordLookupClient() = default;

    [[nodiscard]] WordLookupResult lookup(const std::string& rawWord);

    [[nodiscard]] static std::string normalize_lookup_word(
        const std::string& rawWord);
    static void parse_dictionary_api_json(const std::string& jsonText,
                                          WordLookupResult& out);
    [[nodiscard]] static std::vector<std::string> parse_datamuse_synonyms_json(
        const std::string& jsonText);

private:
    std::mutex cacheMutex_;
    std::unordered_map<std::string, WordLookupResult> cache_;
};

class WordCloudWindow {
public:
    using WordActivatedCallback =
        std::function<void(std::size_t firstWordIndex, const std::string& word)>;

    WordCloudWindow() = default;
    ~WordCloudWindow();

    WordCloudWindow(const WordCloudWindow&) = delete;
    WordCloudWindow& operator=(const WordCloudWindow&) = delete;

    void show(HWND owner, ThemeId theme, const std::vector<WordCloudItem>& items,
              WordActivatedCallback onWordActivated);
    void update_theme(ThemeId theme);
    void update_items(const std::vector<WordCloudItem>& items);
    void close();

private:
    struct LayoutItem {
        WordCloudItem data;
        RECT bounds{};
        int fontPx = 16;
        COLORREF color = RGB(20, 20, 20);
    };

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam);
    LRESULT handle_message(UINT msg, WPARAM wParam, LPARAM lParam);
    void recompute_layout();
    void paint(HDC hdc, const RECT& clientRect);

    HWND hwnd_ = nullptr;
    ThemeId theme_ = ThemeId::WarmCream;
    std::vector<WordCloudItem> items_;
    std::vector<LayoutItem> layout_;
    int hoveredIndex_ = -1;
    WordActivatedCallback onWordActivated_;
};

class WordDefinitionWindow {
public:
    using SpeakWordCallback = std::function<void(const std::string& word)>;

    explicit WordDefinitionWindow(
        std::shared_ptr<FastWordLookupClient> lookupClient);
    ~WordDefinitionWindow();

    WordDefinitionWindow(const WordDefinitionWindow&) = delete;
    WordDefinitionWindow& operator=(const WordDefinitionWindow&) = delete;

    void show_word(HWND owner, ThemeId theme, const std::string& rawWord,
                   SpeakWordCallback onSpeakWord);
    void update_theme(ThemeId theme);
    void close();

private:
    struct SynonymPill {
        std::string word;
        RECT bounds{};
    };

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                     LPARAM lParam);
    LRESULT handle_message(UINT msg, WPARAM wParam, LPARAM lParam);
    void start_lookup(const std::string& rawWord);
    void layout_controls();
    void update_scroll_bar();
    void paint_canvas(HDC hdc, const RECT& clientRect);

    struct PendingState {
        std::mutex mutex;
        bool alive = true;
        std::uint64_t seq = 0;
        WordLookupResult result;
    };

    std::shared_ptr<FastWordLookupClient> lookupClient_;
    std::shared_ptr<PendingState> pendingState_;
    HWND hwnd_ = nullptr;
    HWND queryEdit_ = nullptr;
    HWND searchButton_ = nullptr;
    HWND speakButton_ = nullptr;
    HWND copyButton_ = nullptr;
    HBRUSH bgBrush_ = nullptr;

    ThemeId theme_ = ThemeId::WarmCream;
    SpeakWordCallback onSpeakWord_;
    std::string activeWord_;
    bool loading_ = false;
    std::uint64_t requestSeq_ = 0;
    WordLookupResult result_;
    std::vector<SynonymPill> synonymPills_;
    int hoveredPill_ = -1;
    int scrollY_ = 0;
    int contentHeight_ = 0;
};

}  // namespace dyslexia_reader::windows
