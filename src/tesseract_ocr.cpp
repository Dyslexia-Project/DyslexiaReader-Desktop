#include "tesseract_ocr.hpp"

#include "dyslexia_reader/interfaces.hpp"
#include "dyslexia_reader/paragraph_rebuilder.hpp"
#include "ocr_build_paths.hpp"

#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>
#include <tesseract/resultiterator.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace DyslexiaReader::detail {
namespace {

std::vector<std::string> tessdata_candidates() {
    namespace fs = std::filesystem;
    std::vector<std::string> candidates;
    const auto addIfValid = [&candidates](const fs::path& candidate) {
        std::error_code ec;
        if (!fs::exists(candidate / "eng.traineddata", ec)) {
            return;
        }
        const std::string normalized = fs::weakly_canonical(candidate, ec).string();
        const std::string value = ec ? candidate.string() : normalized;
        if (std::find(candidates.begin(), candidates.end(), value) == candidates.end()) {
            candidates.push_back(value);
        }
    };

    addIfValid(fs::path(".") / "tessdata");
    addIfValid(fs::path("..") / "tessdata");
    addIfValid(fs::path(kBuildTessdataDir));
    return candidates;
}

std::string to_tesseract_language(std::string_view languageTag) {
    if (languageTag.empty() || languageTag == "en" || languageTag == "en-US" ||
        languageTag == "en_US" || languageTag == "en-GB" ||
        languageTag == "en_GB" || languageTag == "eng") {
        return "eng";
    }
    return std::string(languageTag);
}

std::string normalize_bcp47_tag(std::string_view languageTag) {
    if (languageTag.empty() || languageTag == "eng") {
        return "en-US";
    }
    return std::string(languageTag);
}

bool init_tesseract(tesseract::TessBaseAPI& tess, const std::string& tesseractLang) {
    for (const auto& candidate : tessdata_candidates()) {
        if (tess.Init(candidate.c_str(), tesseractLang.c_str()) == 0) {
            return true;
        }
        tess.End();
    }
    return tess.Init(nullptr, tesseractLang.c_str()) == 0;
}

using PixPtr = std::unique_ptr<Pix, void (*)(Pix*)>;

PixPtr read_pix(const std::string& imagePath) {
    auto destroyPix = [](Pix* pix) {
        if (pix != nullptr) {
            pixDestroy(&pix);
        }
    };
    return PixPtr(pixRead(imagePath.c_str()), destroyPix);
}

std::vector<unsigned char> bgra8_to_rgb24(
    const dyslexia_reader::CapturedImage& image) {
    if (image.pixelFormat != dyslexia_reader::PixelFormat::Bgra8 &&
        image.pixelFormat != dyslexia_reader::PixelFormat::Rgba8) {
        throw dyslexia_reader::ReaderError(
            dyslexia_reader::ReaderErrorCode::OcrFailed,
            "Unsupported pixel format passed to Tesseract OCR engine.");
    }
    if (image.width == 0 || image.height == 0 ||
        image.strideBytes < image.width * 4) {
        throw dyslexia_reader::ReaderError(
            dyslexia_reader::ReaderErrorCode::OcrFailed,
            "Captured image has invalid dimensions or stride.");
    }
    const std::size_t requiredBytes =
        static_cast<std::size_t>(image.strideBytes) *
        static_cast<std::size_t>(image.height);
    if (image.pixels.size() < requiredBytes) {
        throw dyslexia_reader::ReaderError(
            dyslexia_reader::ReaderErrorCode::OcrFailed,
            "Captured image buffer is smaller than width * height * stride.");
    }

    const std::size_t width = static_cast<std::size_t>(image.width);
    const std::size_t height = static_cast<std::size_t>(image.height);
    const std::size_t srcStride = static_cast<std::size_t>(image.strideBytes);
    const bool isBgra =
        image.pixelFormat == dyslexia_reader::PixelFormat::Bgra8;
    std::vector<unsigned char> rgb(width * height * 3);

    for (std::size_t y = 0; y < height; ++y) {
        const std::uint8_t* srcRow = image.pixels.data() + y * srcStride;
        unsigned char* dstRow = rgb.data() + y * width * 3;
        for (std::size_t x = 0; x < width; ++x) {
            if (isBgra) {
                dstRow[x * 3 + 0] = srcRow[x * 4 + 2];
                dstRow[x * 3 + 1] = srcRow[x * 4 + 1];
                dstRow[x * 3 + 2] = srcRow[x * 4 + 0];
            } else {
                dstRow[x * 3 + 0] = srcRow[x * 4 + 0];
                dstRow[x * 3 + 1] = srcRow[x * 4 + 1];
                dstRow[x * 3 + 2] = srcRow[x * 4 + 2];
            }
        }
    }
    return rgb;
}

std::u16string trim_word_text(std::u16string_view text) {
    const auto isSpace = [](char16_t ch) {
        return ch == u' ' || ch == u'\t' || ch == u'\r' || ch == u'\n' ||
               ch == u'\u00A0';
    };
    std::size_t start = 0;
    while (start < text.size() && isSpace(text[start])) {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start && isSpace(text[end - 1])) {
        --end;
    }
    return std::u16string(text.substr(start, end - start));
}

dyslexia_reader::OcrBoundingBox expand_bounds(
    const dyslexia_reader::OcrBoundingBox& lhs,
    const dyslexia_reader::OcrBoundingBox& rhs) {
    if (lhs.width <= 0.0F || lhs.height <= 0.0F) {
        return rhs;
    }
    if (rhs.width <= 0.0F || rhs.height <= 0.0F) {
        return lhs;
    }
    const float left = std::min(lhs.x, rhs.x);
    const float top = std::min(lhs.y, rhs.y);
    const float right = std::max(lhs.x + lhs.width, rhs.x + rhs.width);
    const float bottom = std::max(lhs.y + lhs.height, rhs.y + rhs.height);
    return {left, top, right - left, bottom - top};
}

dyslexia_reader::OcrResult extract_ocr_result(tesseract::TessBaseAPI& tess,
                                                std::string_view languageTag) {
    if (tess.Recognize(nullptr) != 0) {
        throw dyslexia_reader::ReaderError(
            dyslexia_reader::ReaderErrorCode::OcrFailed,
            "Tesseract recognition failed.");
    }

    dyslexia_reader::OcrResult result;
    result.languageTag = normalize_bcp47_tag(languageTag);

    std::unique_ptr<tesseract::ResultIterator> it(tess.GetIterator());
    if (!it) {
        return result;
    }

    dyslexia_reader::OcrLine currentLine;
    bool hasOpenLine = false;
    const auto flushLine = [&]() {
        if (hasOpenLine && !currentLine.words.empty()) {
            result.lines.push_back(std::move(currentLine));
        }
        currentLine = dyslexia_reader::OcrLine{};
        hasOpenLine = false;
    };

    do {
        if (it->IsAtBeginningOf(tesseract::RIL_TEXTLINE)) {
            flushLine();
            int lx1 = 0;
            int ly1 = 0;
            int lx2 = 0;
            int ly2 = 0;
            if (it->BoundingBox(tesseract::RIL_TEXTLINE, &lx1, &ly1, &lx2,
                                &ly2)) {
                currentLine.bounds = {
                    static_cast<float>(lx1),
                    static_cast<float>(ly1),
                    static_cast<float>(std::max(0, lx2 - lx1)),
                    static_cast<float>(std::max(0, ly2 - ly1)),
                };
            }
            hasOpenLine = true;
        }

        if (!it->Empty(tesseract::RIL_WORD)) {
            hasOpenLine = true;
            std::unique_ptr<char[]> utf8Word(
                it->GetUTF8Text(tesseract::RIL_WORD));
            std::u16string trimmed = utf8Word
                                         ? trim_word_text(
                                               dyslexia_reader::utf8_to_utf16(
                                                   utf8Word.get()))
                                         : std::u16string{};
            if (!trimmed.empty()) {
                int wx1 = 0;
                int wy1 = 0;
                int wx2 = 0;
                int wy2 = 0;
                dyslexia_reader::OcrBoundingBox wordBounds{};
                if (it->BoundingBox(tesseract::RIL_WORD, &wx1, &wy1, &wx2,
                                    &wy2)) {
                    wordBounds = {
                        static_cast<float>(wx1),
                        static_cast<float>(wy1),
                        static_cast<float>(std::max(0, wx2 - wx1)),
                        static_cast<float>(std::max(0, wy2 - wy1)),
                    };
                }
                if (currentLine.bounds.width <= 0.0F ||
                    currentLine.bounds.height <= 0.0F) {
                    currentLine.bounds =
                        expand_bounds(currentLine.bounds, wordBounds);
                }
                if (!currentLine.text.empty()) {
                    currentLine.text.push_back(u' ');
                }
                currentLine.text += trimmed;

                dyslexia_reader::OcrWord word;
                word.text = std::move(trimmed);
                word.bounds = wordBounds;
                word.confidence = std::clamp(
                    it->Confidence(tesseract::RIL_WORD) / 100.0F, 0.0F, 1.0F);
                currentLine.words.push_back(std::move(word));
            }
        }

        if (it->IsAtFinalElement(tesseract::RIL_TEXTLINE, tesseract::RIL_WORD)) {
            flushLine();
        }
    } while (it->Next(tesseract::RIL_WORD));

    flushLine();
    return result;
}

}  // namespace

std::string recognize_text_from_image_tesseract(const std::string& imagePath) {
    tesseract::TessBaseAPI tess;
    if (!init_tesseract(tess, "eng")) {
        std::cerr << "[Tesseract] Could not initialize tessdata for 'eng'."
                  << std::endl;
        return "";
    }

    PixPtr image = read_pix(imagePath);
    if (!image) {
        std::cerr << "[Tesseract] Could not read image: " << imagePath
                  << std::endl;
        return "";
    }

    tess.SetImage(image.get());
    std::unique_ptr<char[]> outText(tess.GetUTF8Text());
    if (!outText) {
        return "";
    }
    return std::string(outText.get());
}

dyslexia_reader::OcrResult recognize_ocr_result_tesseract(
    const dyslexia_reader::CapturedImage& image, std::string_view languageTag) {
    const std::vector<unsigned char> rgb = bgra8_to_rgb24(image);

    tesseract::TessBaseAPI tess;
    const std::string tessLang = to_tesseract_language(languageTag);
    if (!init_tesseract(tess, tessLang)) {
        throw dyslexia_reader::ReaderError(
            dyslexia_reader::ReaderErrorCode::OcrLanguageUnavailable,
            "Could not initialize Tesseract language model '" + tessLang + "'.");
    }

    tess.SetImage(rgb.data(), static_cast<int>(image.width),
                  static_cast<int>(image.height), 3,
                  static_cast<int>(image.width * 3));
    const float scale = image.scaleX > 0.25F ? image.scaleX : 1.0F;
    const int dpi = std::clamp(static_cast<int>(std::lround(96.0F * scale)), 70,
                               2400);
    tess.SetSourceResolution(dpi);

    return extract_ocr_result(tess, languageTag);
}

dyslexia_reader::OcrResult recognize_file_ocr_result_tesseract(
    const std::string& imagePath, std::string_view languageTag) {
    tesseract::TessBaseAPI tess;
    const std::string tessLang = to_tesseract_language(languageTag);
    if (!init_tesseract(tess, tessLang)) {
        throw dyslexia_reader::ReaderError(
            dyslexia_reader::ReaderErrorCode::OcrLanguageUnavailable,
            "Could not initialize Tesseract language model '" + tessLang + "'.");
    }

    PixPtr image = read_pix(imagePath);
    if (!image) {
        throw dyslexia_reader::ReaderError(
            dyslexia_reader::ReaderErrorCode::OcrFailed,
            "Could not load image file '" + imagePath + "' for OCR.");
    }

    tess.SetImage(image.get());
    return extract_ocr_result(tess, languageTag);
}

}  // namespace DyslexiaReader::detail
