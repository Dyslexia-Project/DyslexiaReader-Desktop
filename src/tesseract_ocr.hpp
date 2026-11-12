#pragma once

#include "dyslexia_reader/domain.hpp"

#include <string>
#include <string_view>

namespace DyslexiaReader::detail {

std::string recognize_text_from_image_tesseract(const std::string& imagePath);

dyslexia_reader::OcrResult recognize_ocr_result_tesseract(
    const dyslexia_reader::CapturedImage& image, std::string_view languageTag);

dyslexia_reader::OcrResult recognize_file_ocr_result_tesseract(
    const std::string& imagePath, std::string_view languageTag);

}  // namespace DyslexiaReader::detail
