#include "dyslexia_reader/paragraph_rebuilder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace dyslexia_reader {
namespace {

bool is_whitespace(char16_t ch) {
    return ch == u' ' || ch == u'\t' || ch == u'\r' || ch == u'\n' ||
           ch == u'\v' || ch == u'\f' || ch == u'\u00A0';
}

bool is_ascii_alpha(char16_t ch) {
    return (ch >= u'A' && ch <= u'Z') || (ch >= u'a' && ch <= u'z');
}

bool is_ascii_digit(char16_t ch) {
    return ch >= u'0' && ch <= u'9';
}

bool is_word_char(char16_t ch) {
    return is_ascii_alpha(ch) || is_ascii_digit(ch) || ch >= 0x00C0;
}

bool is_trailing_hyphen(char16_t ch) {
    return ch == u'-' || ch == u'\u2010';
}

bool is_sentence_terminator(char16_t ch) {
    return ch == u'.' || ch == u'!' || ch == u'?';
}

bool is_closing_quote_or_bracket(char16_t ch) {
    return ch == u'"' || ch == u'\'' || ch == u')' || ch == u']' ||
           ch == u'\u201D' || ch == u'\u2019';
}

OcrBoundingBox union_bounds(const OcrBoundingBox& a, const OcrBoundingBox& b) {
    if (a.width <= 0 && a.height <= 0) {
        return b;
    }
    if (b.width <= 0 && b.height <= 0) {
        return a;
    }
    const float left = std::min(a.x, b.x);
    const float top = std::min(a.y, b.y);
    const float right = std::max(a.x + a.width, b.x + b.width);
    const float bottom = std::max(a.y + a.height, b.y + b.height);
    return {left, top, right - left, bottom - top};
}

std::u16string trim_whitespace(std::u16string_view text) {
    std::size_t start = 0;
    while (start < text.size() && is_whitespace(text[start])) {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start && is_whitespace(text[end - 1])) {
        --end;
    }
    return std::u16string(text.substr(start, end - start));
}

struct LineToken {
    std::u16string text;
    OcrBoundingBox bounds;
};

std::vector<LineToken> extract_line_tokens(const OcrLine& line) {
    std::vector<LineToken> tokens;
    if (!line.words.empty()) {
        for (const auto& word : line.words) {
            std::u16string cleaned = trim_whitespace(word.text);
            if (!cleaned.empty()) {
                tokens.push_back({std::move(cleaned), word.bounds});
            }
        }
        return tokens;
    }

    std::size_t index = 0;
    while (index < line.text.size()) {
        while (index < line.text.size() && is_whitespace(line.text[index])) {
            ++index;
        }
        const std::size_t start = index;
        while (index < line.text.size() && !is_whitespace(line.text[index])) {
            ++index;
        }
        if (index > start) {
            tokens.push_back(
                {line.text.substr(start, index - start), line.bounds});
        }
    }
    return tokens;
}

bool ends_with_hyphen_wrap(const std::u16string& text,
                           const std::u16string& nextToken) {
    if (text.size() < 2 || nextToken.empty()) {
        return false;
    }
    const char16_t last = text.back();
    if (!is_trailing_hyphen(last)) {
        return false;
    }
    const char16_t beforeHyphen = text[text.size() - 2];
    return is_word_char(beforeHyphen) && is_word_char(nextToken.front());
}

bool should_start_new_paragraph(const OcrLine& prevLine,
                                const OcrLine& currLine,
                                const std::u16string& textSoFar,
                                float baselineLeftX) {
    if (prevLine.bounds.height <= 0 || currLine.bounds.height <= 0) {
        return false;
    }
    const float prevBottom = prevLine.bounds.y + prevLine.bounds.height;
    const float verticalGap = currLine.bounds.y - prevBottom;
    const float lineHeight =
        std::max(prevLine.bounds.height, currLine.bounds.height);
    if (verticalGap > lineHeight * 0.65F) {
        return true;
    }
    // Same tight vertical spacing, but indented after a completed sentence.
    if (!textSoFar.empty()) {
        char16_t last = textSoFar.back();
        if ((is_sentence_terminator(last) || is_closing_quote_or_bracket(last)) &&
            currLine.bounds.x - baselineLeftX > lineHeight * 0.85F) {
            return true;
        }
    }
    return false;
}

bool is_decimal_dot(const std::u16string& text, std::size_t index) {
    return index > 0 && index + 1 < text.size() &&
           is_ascii_digit(text[index - 1]) && is_ascii_digit(text[index + 1]);
}

bool is_initialism_dot(const std::u16string& text, std::size_t index) {
    if (text[index] != u'.' || index == 0) {
        return false;
    }
    // Match patterns like "U.S." or "e.g." where a single letter precedes '.'.
    const bool singleLetterBefore =
        is_ascii_alpha(text[index - 1]) &&
        (index == 1 || !is_ascii_alpha(text[index - 2]));
    const bool letterImmediatelyAfter =
        index + 1 < text.size() && is_ascii_alpha(text[index + 1]);
    return singleLetterBefore &&
           (letterImmediatelyAfter ||
            (index >= 3 && text[index - 2] == u'.' &&
             is_ascii_alpha(text[index - 3])));
}

void append_sentences_in_range(const std::u16string& text,
                               std::size_t rangeStart, std::size_t rangeEnd,
                               std::vector<TextRange>& out) {
    std::size_t cursor = rangeStart;
    while (cursor < rangeEnd && is_whitespace(text[cursor])) {
        ++cursor;
    }
    if (cursor >= rangeEnd) {
        return;
    }

    std::size_t sentenceStart = cursor;
    for (std::size_t i = cursor; i < rangeEnd; ++i) {
        if (!is_sentence_terminator(text[i])) {
            continue;
        }
        if (text[i] == u'.' &&
            (is_decimal_dot(text, i) || is_initialism_dot(text, i))) {
            continue;
        }

        std::size_t end = i + 1;
        while (end < rangeEnd &&
               (is_sentence_terminator(text[end]) ||
                is_closing_quote_or_bracket(text[end]))) {
            ++end;
        }
        const bool atEndOrSpace =
            end == rangeEnd || is_whitespace(text[end]);
        if (!atEndOrSpace) {
            continue;
        }

        while (sentenceStart < end && is_whitespace(text[sentenceStart])) {
            ++sentenceStart;
        }
        if (end > sentenceStart) {
            out.push_back({static_cast<TextOffset>(sentenceStart),
                           static_cast<TextOffset>(end - sentenceStart)});
        }
        sentenceStart = end;
        while (sentenceStart < rangeEnd && is_whitespace(text[sentenceStart])) {
            ++sentenceStart;
        }
        i = sentenceStart > 0 ? sentenceStart - 1 : i;
    }

    while (sentenceStart < rangeEnd && is_whitespace(text[sentenceStart])) {
        ++sentenceStart;
    }
    std::size_t tailEnd = rangeEnd;
    while (tailEnd > sentenceStart && is_whitespace(text[tailEnd - 1])) {
        --tailEnd;
    }
    if (tailEnd > sentenceStart) {
        out.push_back({static_cast<TextOffset>(sentenceStart),
                       static_cast<TextOffset>(tailEnd - sentenceStart)});
    }
}

void populate_words_from_text(ReadingDocument& doc) {
    doc.words.clear();
    std::size_t i = 0;
    while (i < doc.text.size()) {
        while (i < doc.text.size() && is_whitespace(doc.text[i])) {
            ++i;
        }
        const std::size_t start = i;
        while (i < doc.text.size() && !is_whitespace(doc.text[i])) {
            ++i;
        }
        if (i > start) {
            doc.words.push_back(
                {{static_cast<TextOffset>(start),
                  static_cast<TextOffset>(i - start)},
                 {}});
        }
    }
}

}  // namespace

std::u16string utf8_to_utf16(std::string_view utf8) {
    std::u16string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto byte0 = static_cast<std::uint8_t>(utf8[i]);
        std::uint32_t codePoint = 0xFFFD;
        std::size_t advance = 1;

        if (byte0 < 0x80) {
            codePoint = byte0;
        } else if ((byte0 & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            const auto byte1 = static_cast<std::uint8_t>(utf8[i + 1]);
            if ((byte1 & 0xC0) == 0x80) {
                codePoint = ((byte0 & 0x1F) << 6) | (byte1 & 0x3F);
                if (codePoint >= 0x80) {
                    advance = 2;
                } else {
                    codePoint = 0xFFFD;
                }
            }
        } else if ((byte0 & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            const auto byte1 = static_cast<std::uint8_t>(utf8[i + 1]);
            const auto byte2 = static_cast<std::uint8_t>(utf8[i + 2]);
            if ((byte1 & 0xC0) == 0x80 && (byte2 & 0xC0) == 0x80) {
                codePoint = ((byte0 & 0x0F) << 12) | ((byte1 & 0x3F) << 6) |
                            (byte2 & 0x3F);
                if (codePoint >= 0x800 &&
                    (codePoint < 0xD800 || codePoint > 0xDFFF)) {
                    advance = 3;
                } else {
                    codePoint = 0xFFFD;
                }
            }
        } else if ((byte0 & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
            const auto byte1 = static_cast<std::uint8_t>(utf8[i + 1]);
            const auto byte2 = static_cast<std::uint8_t>(utf8[i + 2]);
            const auto byte3 = static_cast<std::uint8_t>(utf8[i + 3]);
            if ((byte1 & 0xC0) == 0x80 && (byte2 & 0xC0) == 0x80 &&
                (byte3 & 0xC0) == 0x80) {
                codePoint = ((byte0 & 0x07) << 18) | ((byte1 & 0x3F) << 12) |
                            ((byte2 & 0x3F) << 6) | (byte3 & 0x3F);
                if (codePoint >= 0x10000 && codePoint <= 0x10FFFF) {
                    advance = 4;
                } else {
                    codePoint = 0xFFFD;
                }
            }
        }

        if (codePoint <= 0xFFFF) {
            out.push_back(static_cast<char16_t>(codePoint));
        } else {
            const std::uint32_t adjusted = codePoint - 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (adjusted >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (adjusted & 0x3FF)));
        }
        i += advance;
    }
    return out;
}

ReadingDocument build_reading_document_from_utf16(std::u16string_view rawText) {
    ReadingDocument document;

    // Normalize line endings and split paragraphs on blank lines.
    std::vector<std::u16string> paragraphs;
    std::u16string currentLine;
    std::u16string currentParagraph;
    const auto flushLine = [&currentLine, &currentParagraph]() {
        const std::u16string trimmed = trim_whitespace(currentLine);
        currentLine.clear();
        if (trimmed.empty()) {
            return false;
        }
        if (!currentParagraph.empty()) {
            if (ends_with_hyphen_wrap(currentParagraph, trimmed)) {
                currentParagraph.pop_back();
                currentParagraph += trimmed;
            } else {
                currentParagraph.push_back(u' ');
                currentParagraph += trimmed;
            }
        } else {
            currentParagraph = trimmed;
        }
        return true;
    };

    for (std::size_t i = 0; i < rawText.size(); ++i) {
        const char16_t ch = rawText[i];
        if (ch == u'\r') {
            if (i + 1 < rawText.size() && rawText[i + 1] == u'\n') {
                ++i;
            }
            if (!flushLine() && !currentParagraph.empty()) {
                paragraphs.push_back(std::move(currentParagraph));
                currentParagraph.clear();
            }
        } else if (ch == u'\n') {
            if (!flushLine() && !currentParagraph.empty()) {
                paragraphs.push_back(std::move(currentParagraph));
                currentParagraph.clear();
            }
        } else {
            currentLine.push_back(ch);
        }
    }
    flushLine();
    if (!currentParagraph.empty()) {
        paragraphs.push_back(std::move(currentParagraph));
    }

    for (const auto& para : paragraphs) {
        if (!document.text.empty()) {
            document.text += u"\n\n";
        }
        const std::size_t start = document.text.size();
        document.text += para;
        const std::size_t end = document.text.size();
        document.paragraphs.push_back(
            {static_cast<TextOffset>(start),
             static_cast<TextOffset>(end - start)});
        append_sentences_in_range(document.text, start, end,
                                  document.sentences);
    }

    populate_words_from_text(document);
    return document;
}

ReadingDocument build_reading_document_from_utf8(std::string_view utf8Text) {
    return build_reading_document_from_utf16(utf8_to_utf16(utf8Text));
}

ReadingDocument ParagraphRebuilder::rebuild(const OcrResult& ocr) {
    ReadingDocument document;

    struct NonEmptyLine {
        const OcrLine* source = nullptr;
        std::vector<LineToken> tokens;
    };
    std::vector<NonEmptyLine> lines;
    lines.reserve(ocr.lines.size());
    float baselineLeftX = 0.0F;
    bool hasBaseline = false;

    for (const auto& line : ocr.lines) {
        auto tokens = extract_line_tokens(line);
        if (tokens.empty()) {
            continue;
        }
        if (line.bounds.height > 0) {
            if (!hasBaseline || line.bounds.x < baselineLeftX) {
                baselineLeftX = line.bounds.x;
                hasBaseline = true;
            }
        }
        lines.push_back({&line, std::move(tokens)});
    }

    if (lines.empty()) {
        return document;
    }

    std::size_t paragraphStart = 0;
    const OcrLine* prevSource = nullptr;

    for (const auto& line : lines) {
        const bool newParagraph =
            prevSource != nullptr &&
            should_start_new_paragraph(*prevSource, *line.source, document.text,
                                       baselineLeftX);
        if (newParagraph && !document.text.empty()) {
            const std::size_t paragraphEnd = document.text.size();
            document.paragraphs.push_back(
                {static_cast<TextOffset>(paragraphStart),
                 static_cast<TextOffset>(paragraphEnd - paragraphStart)});
            append_sentences_in_range(document.text, paragraphStart,
                                      paragraphEnd, document.sentences);
            document.text += u"\n\n";
            paragraphStart = document.text.size();
        }

        for (std::size_t tokenIndex = 0; tokenIndex < line.tokens.size();
             ++tokenIndex) {
            const auto& token = line.tokens[tokenIndex];
            const bool joinHyphenWrap =
                !newParagraph && tokenIndex == 0 && !document.words.empty() &&
                !document.text.empty() &&
                ends_with_hyphen_wrap(document.text, token.text);

            if (joinHyphenWrap) {
                document.text.pop_back();
                auto& prevWord = document.words.back();
                document.text += token.text;
                prevWord.text.length = static_cast<TextOffset>(
                    document.text.size() - prevWord.text.start);
                prevWord.sourceBounds =
                    union_bounds(prevWord.sourceBounds, token.bounds);
            } else {
                if (!document.text.empty() && !newParagraph &&
                    document.text.back() != u'\n') {
                    document.text.push_back(u' ');
                } else if (tokenIndex > 0 && !document.text.empty() &&
                           document.text.back() != u'\n') {
                    document.text.push_back(u' ');
                }
                const std::size_t wordStart = document.text.size();
                document.text += token.text;
                document.words.push_back(
                    {{static_cast<TextOffset>(wordStart),
                      static_cast<TextOffset>(token.text.size())},
                     token.bounds});
            }
        }
        prevSource = line.source;
    }

    if (document.text.size() > paragraphStart) {
        const std::size_t paragraphEnd = document.text.size();
        document.paragraphs.push_back(
            {static_cast<TextOffset>(paragraphStart),
             static_cast<TextOffset>(paragraphEnd - paragraphStart)});
        append_sentences_in_range(document.text, paragraphStart, paragraphEnd,
                                  document.sentences);
    }

    return document;
}

}  // namespace dyslexia_reader
