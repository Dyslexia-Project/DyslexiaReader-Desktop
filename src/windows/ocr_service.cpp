#include "dyslexia_reader/ocr_service.hpp"
#include <iostream>

namespace DyslexiaReader {

OcrService::OcrService() {}
OcrService::~OcrService() {}

std::string OcrService::recognizeTextFromImage(const std::string& imagePath) {
    std::cout << "[Windows OCR] Processing image on Windows: " << imagePath << std::endl;
    // Integrate Tesseract on Windows or Windows.Media.Ocr here
    return "[Mock Windows OCR Text]";
}

std::string OcrService::captureAndRecognizeText() {
    std::cout << "\n[Windows Snipping Tool] Waiting for screen capture..." << std::endl;
    // Invoke native Windows screen capture overlay here
    return recognizeTextFromImage("dummy_path.png");
}

} // namespace DyslexiaReader