#include "dyslexia_reader/ocr_service.hpp"

#ifdef DYSLEXIA_HAS_TESSERACT
#include "tesseract_ocr.hpp"
#endif

#include <iostream>
#include <string>

namespace DyslexiaReader {

OcrService::OcrService() = default;
OcrService::~OcrService() = default;

std::string OcrService::recognizeTextFromImage(const std::string& imagePath) {
    std::cout << "[Windows OCR] Processing image on Windows: " << imagePath
              << std::endl;
#ifdef DYSLEXIA_HAS_TESSERACT
    return detail::recognize_text_from_image_tesseract(imagePath);
#else
    std::cerr << "[Windows OCR] Tesseract is disabled in this build."
              << std::endl;
    return "";
#endif
}

std::string OcrService::captureAndRecognizeText() {
    std::cerr
        << "[Windows OCR] Interactive screen snipping is handled through IScreenCapturer."
        << std::endl;
    return "";
}

}  // namespace DyslexiaReader
