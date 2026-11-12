#pragma once

#include "dyslexia_reader/interfaces.hpp"

#include <string>

namespace dyslexia_reader {

class TesseractOcrEngine final : public IOcrEngine {
public:
    explicit TesseractOcrEngine(std::string languageTag = "en-US");

    OcrResult recognize(const CapturedImage& image) override;
    OcrResult recognize_file(const std::string& imagePath);

    const std::string& language_tag() const noexcept { return languageTag_; }

private:
    std::string languageTag_;
};

}  // namespace dyslexia_reader
