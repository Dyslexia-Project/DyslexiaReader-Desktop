#include "dyslexia_reader/windows/reading_aids.hpp"
#include "theme_palette.hpp"

#include <algorithm>
#include <climits>
#include <iterator>
#include <utility>
#include <windows.h>

namespace dyslexia_reader::windows {
namespace {

struct Grapheme {
    std::uint32_t start;
    std::uint32_t end;
    std::uint32_t firstCodePoint;
    std::uint32_t lastCodePoint;
    std::uint8_t regionalIndicators;
    bool alphabetic;
};

std::pair<std::uint32_t, std::uint32_t> decode_utf16(
    const std::u16string& text, std::uint32_t offset) {
    const std::uint32_t first = text[offset];
    if (first >= 0xD800 && first <= 0xDBFF &&
        offset + 1 < text.size()) {
        const std::uint32_t second = text[offset + 1];
        if (second >= 0xDC00 && second <= 0xDFFF) {
            return {0x10000 + ((first - 0xD800) << 10) + second - 0xDC00, 2};
        }
    }
    return {first, 1};
}

bool is_variation_selector(std::uint32_t codePoint) {
    return (codePoint >= 0xFE00 && codePoint <= 0xFE0F) ||
           (codePoint >= 0xE0100 && codePoint <= 0xE01EF);
}

bool is_emoji_modifier_or_tag(std::uint32_t codePoint) {
    return (codePoint >= 0x1F3FB && codePoint <= 0x1F3FF) ||
           (codePoint >= 0xE0020 && codePoint <= 0xE007F);
}

bool is_regional_indicator(std::uint32_t codePoint) {
    return codePoint >= 0x1F1E6 && codePoint <= 0x1F1FF;
}

bool is_hangul_l(std::uint32_t codePoint) {
    return (codePoint >= 0x1100 && codePoint <= 0x115F) ||
           (codePoint >= 0xA960 && codePoint <= 0xA97C);
}

bool is_hangul_v(std::uint32_t codePoint) {
    return (codePoint >= 0x1160 && codePoint <= 0x11A7) ||
           (codePoint >= 0xD7B0 && codePoint <= 0xD7C6);
}

bool is_hangul_t(std::uint32_t codePoint) {
    return (codePoint >= 0x11A8 && codePoint <= 0x11FF) ||
           (codePoint >= 0xD7CB && codePoint <= 0xD7FB);
}

bool is_hangul_lv(std::uint32_t codePoint) {
    return codePoint >= 0xAC00 && codePoint <= 0xD7A3 &&
           (codePoint - 0xAC00) % 28 == 0;
}

bool is_hangul_lvt(std::uint32_t codePoint) {
    return codePoint >= 0xAC00 && codePoint <= 0xD7A3 &&
           (codePoint - 0xAC00) % 28 != 0;
}

bool hangul_continues_cluster(std::uint32_t previous,
                              std::uint32_t current) {
    return (is_hangul_l(previous) &&
            (is_hangul_l(current) || is_hangul_v(current) ||
             is_hangul_lv(current) || is_hangul_lvt(current))) ||
           ((is_hangul_lv(previous) || is_hangul_v(previous)) &&
            (is_hangul_v(current) || is_hangul_t(current))) ||
           ((is_hangul_lvt(previous) || is_hangul_t(previous)) &&
            is_hangul_t(current));
}

std::vector<Grapheme> graphemes(const std::u16string& text,
                                std::uint32_t start, std::uint32_t length) {
    if (length > static_cast<std::uint32_t>(INT_MAX)) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "A reading-aid word exceeds Windows text limits");
    }
    std::vector<WORD> characterTypes(length);
    std::vector<WORD> categoryTypes(length);
    static_assert(sizeof(char16_t) == sizeof(WCHAR),
                  "Windows Unicode classification requires UTF-16 code units");
    const auto* word = reinterpret_cast<LPCWCH>(text.data() + start);
    if (!GetStringTypeW(CT_CTYPE3, word, static_cast<int>(length),
                        characterTypes.data()) ||
        !GetStringTypeW(CT_CTYPE1, word, static_cast<int>(length),
                        categoryTypes.data())) {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Windows could not classify a reading-aid word");
    }

    std::vector<Grapheme> result;
    std::uint32_t offset = start;
    const std::uint32_t end = start + length;
    bool joinNext = false;
    while (offset < end) {
        const auto [codePoint, codeUnitLength] = decode_utf16(text, offset);
        const auto alphabetic = [&categoryTypes, start, offset,
                                 codeUnitLength]() {
            for (std::uint32_t index = 0; index < codeUnitLength; ++index) {
                if ((categoryTypes[offset - start + index] & C1_ALPHA) != 0) {
                    return true;
                }
            }
            return false;
        }();
        const bool extender =
            ((characterTypes[offset - start] &
              (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK)) != 0) ||
            is_variation_selector(codePoint) ||
            is_emoji_modifier_or_tag(codePoint) || codePoint == 0x200C;
        const bool regionalPair =
            is_regional_indicator(codePoint) && !result.empty() &&
            result.back().regionalIndicators == 1;
        const bool continuesHangul =
            !result.empty() &&
            hangul_continues_cluster(result.back().lastCodePoint, codePoint);

        if (codePoint == 0x200D || joinNext || extender || regionalPair ||
            continuesHangul ||
            (codePoint == 0x0A && !result.empty() &&
             result.back().lastCodePoint == 0x0D)) {
            if (result.empty()) {
                result.push_back({offset, offset + codeUnitLength, codePoint,
                                  codePoint, 0, alphabetic});
            } else {
                auto& previous = result.back();
                previous.end = offset + codeUnitLength;
                previous.lastCodePoint = codePoint;
                if (is_regional_indicator(codePoint)) {
                    ++previous.regionalIndicators;
                }
                previous.alphabetic = previous.alphabetic || alphabetic;
            }
            joinNext = codePoint == 0x200D;
        } else {
            result.push_back(
                {offset, offset + codeUnitLength, codePoint, codePoint,
                 static_cast<std::uint8_t>(
                     is_regional_indicator(codePoint) ? 1 : 0),
                 alphabetic});
            joinNext = false;
        }
        offset += codeUnitLength;
    }
    return result;
}

unsigned int hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned int>(value - '0');
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<unsigned int>(value - 'A' + 10);
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned int>(value - 'a' + 10);
    }
    throw ReaderError(ReaderErrorCode::RenderingFailed,
                      "Theme contract contains an invalid ruler color");
}

std::uint32_t ruler_color(ThemeId theme) {
    const std::string_view color = [&theme]() -> std::string_view {
        switch (theme) {
        case ThemeId::WarmCream:
            return theme_palette::warmCream.rulerHighlight;
        case ThemeId::Sepia:
            return theme_palette::sepia.rulerHighlight;
        case ThemeId::DarkHighContrast:
            return theme_palette::darkHighContrast.rulerHighlight;
        }
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          "Reading profile uses an unknown color theme");
    }();
    if (color.size() != 7 || color.front() != '#') {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Theme contract ruler colors must use #RRGGBB");
    }
    return (hex_digit(color[1]) << 28) | (hex_digit(color[2]) << 24) |
           (hex_digit(color[3]) << 20) | (hex_digit(color[4]) << 16) |
           (hex_digit(color[5]) << 12) | (hex_digit(color[6]) << 8) | 0xFF;
}

std::uint32_t semantic_color(ThemeId theme, bool wordBorder) {
    const std::string_view color = [&theme, wordBorder]() -> std::string_view {
        switch (theme) {
        case ThemeId::WarmCream:
            return wordBorder ? theme_palette::warmCream.wordHighlightBorder
                              : theme_palette::warmCream.sentenceHighlight;
        case ThemeId::Sepia:
            return wordBorder ? theme_palette::sepia.wordHighlightBorder
                              : theme_palette::sepia.sentenceHighlight;
        case ThemeId::DarkHighContrast:
            return wordBorder
                       ? theme_palette::darkHighContrast.wordHighlightBorder
                       : theme_palette::darkHighContrast.sentenceHighlight;
        }
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          "Reading profile uses an unknown color theme");
    }();
    if (color.size() != 7 || color.front() != '#') {
        throw ReaderError(ReaderErrorCode::RenderingFailed,
                          "Theme contract contains an invalid highlight color");
    }
    return (hex_digit(color[1]) << 28) | (hex_digit(color[2]) << 24) |
           (hex_digit(color[3]) << 20) | (hex_digit(color[4]) << 16) |
           (hex_digit(color[5]) << 12) | (hex_digit(color[6]) << 8) | 0xFF;
}

}  // namespace

std::string BionicReadingAid::id() const {
    return "bionic-reading";
}

AidVisuals BionicReadingAid::compose(
    const ReadingAidContext& context) const {
    AidVisuals visuals;
    if (!context.profile.aids.bionicReading) {
        return visuals;
    }

    for (const auto& word : context.document.words) {
        const auto start = word.text.start;
        const auto length = word.text.length;
        if (length == 0 || start >= context.document.text.size() ||
            length > context.document.text.size() - start) {
            continue;
        }

        const auto units =
            graphemes(context.document.text, start, length);
        auto first = std::find_if(units.begin(), units.end(),
                                  [](const Grapheme& unit) {
                                      return unit.alphabetic;
                                  });
        if (first == units.end()) {
            continue;
        }
        auto last = std::find_if(units.rbegin(), units.rend(),
                                 [](const Grapheme& unit) {
                                     return unit.alphabetic;
                                 })
                        .base();
        const auto alphabeticCount =
            static_cast<std::uint32_t>(std::distance(first, last));
        if (alphabeticCount < 3 ||
            !std::all_of(first, last, [](const Grapheme& unit) {
                return unit.alphabetic;
            })) {
            continue;
        }

        const std::uint32_t emphasizedCount = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(alphabeticCount) * 40 + 99) / 100);
        const std::uint32_t emphasizedEnd =
            first[emphasizedCount - 1].end;
        visuals.textStyles.push_back(
            {{first->start, emphasizedEnd - first->start}, 700});
    }
    return visuals;
}

std::string ReadingRulerAid::id() const {
    return "reading-ruler";
}

AidVisuals ReadingRulerAid::compose(
    const ReadingAidContext& context) const {
    AidVisuals visuals;
    if (!context.profile.aids.readingRuler ||
        !context.activeVisualLine.has_value() ||
        context.layout.lines.empty() ||
        *context.activeVisualLine >= context.layout.lines.size()) {
        return visuals;
    }

    const auto& lines = context.layout.lines;
    const auto& active = lines[*context.activeVisualLine].bounds;
    const float left = std::max(0.0F, active.x - 8.0F);
    const float right = active.x + active.width + 8.0F;
    const float firstTop = lines.front().bounds.y;
    const auto& lastLine = lines.back().bounds;
    const float lastBottom = lastLine.y + lastLine.height;
    const float activeTop = active.y - 4.0F;
    const float activeBottom = active.y + active.height + 4.0F;

    if (activeTop > firstTop) {
        visuals.overlays.push_back(
            {OverlayKind::ReadingRuler,
             {left, firstTop, right - left, activeTop - firstTop},
             0x000000FF, 0.25F, OverlayLayer::OverText});
    }
    visuals.overlays.push_back(
        {OverlayKind::ReadingRuler,
         {left, activeTop, right - left, activeBottom - activeTop},
         ruler_color(context.profile.theme), 0.2F, OverlayLayer::UnderText});
    if (activeBottom < lastBottom) {
        visuals.overlays.push_back(
            {OverlayKind::ReadingRuler,
             {left, activeBottom, right - left, lastBottom - activeBottom},
             0x000000FF, 0.25F, OverlayLayer::OverText});
    }
    return visuals;
}

std::string BimodalHighlightingAid::id() const {
    return "bimodal-highlighting";
}

AidVisuals BimodalHighlightingAid::compose(
    const ReadingAidContext& context) const {
    AidVisuals visuals;
    if (!context.profile.aids.bimodalHighlighting) {
        return visuals;
    }

    if (context.highlights.sentenceTimingReliable &&
        context.highlights.sentence.has_value()) {
        for (const auto& sentence : context.layout.sentences) {
            if (sentence.text.start != context.highlights.sentence->start ||
                sentence.text.length != context.highlights.sentence->length) {
                continue;
            }
            for (const auto& fragment : sentence.fragments) {
                if (fragment.width <= 0 || fragment.height <= 0) {
                    continue;
                }
                constexpr float padding = 2.0F;
                visuals.overlays.push_back(
                    {OverlayKind::SentenceHighlight,
                     {fragment.x - padding, fragment.y - padding,
                      fragment.width + 2 * padding,
                      fragment.height + 2 * padding},
                     semantic_color(context.profile.theme, false), 0.35F,
                     OverlayLayer::UnderText});
            }
        }
    }

    if (context.highlights.wordTimingReliable &&
        context.highlights.word.has_value()) {
        for (const auto& word : context.layout.words) {
            if (word.text.start != context.highlights.word->start ||
                word.text.length != context.highlights.word->length) {
                continue;
            }
            for (const auto& fragment : word.fragments) {
                if (fragment.width <= 0 || fragment.height <= 0) {
                    continue;
                }
                constexpr float padding = 2.0F;
                visuals.overlays.push_back(
                    {OverlayKind::WordHighlight,
                     {fragment.x - padding, fragment.y - padding,
                      fragment.width + 2 * padding,
                      fragment.height + 2 * padding},
                     semantic_color(context.profile.theme, true), 1.0F,
                     OverlayLayer::OverText});
            }
        }
    }
    return visuals;
}

}  // namespace dyslexia_reader::windows
