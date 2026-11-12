#pragma once

#include "dyslexia_reader/interfaces.hpp"

#include <string>
#include <string_view>

namespace dyslexia_reader {

ReadingDocument build_reading_document_from_utf16(std::u16string_view rawText);
ReadingDocument build_reading_document_from_utf8(std::string_view utf8Text);
std::u16string utf8_to_utf16(std::string_view utf8);

class ParagraphRebuilder final : public IParagraphRebuilder {
public:
    ReadingDocument rebuild(const OcrResult& ocr) override;
};

}  // namespace dyslexia_reader
