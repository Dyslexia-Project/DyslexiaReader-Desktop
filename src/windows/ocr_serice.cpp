#include "ocr_service.hpp"
#include <iostream>

namespace DyslexiaReader {

OcrService::OcrService() {}
OcrService::~OcrService() {}

std::string OcrService::recognizeTextFromImage(const std::string& imagePath) {
    std::cout << "[Windows OCR] Prelucrare imagine pe Windows: " << imagePath << std::endl;
    // Aici colegii tăi pot integra Tesseract pe Windows (sau Windows.Media.Ocr)
    return "[Mock Windows OCR Text]";
}

std::string OcrService::captureAndRecognizeText() {
    std::cout << "\n[Windows Snipping Tool] Asteptare captura ecran..." << std::endl;
    // Aici colegii tăi vor apela captura nativă de Windows
    return recognizeTextFromImage("dummy_path.png");
}

} // namespace DyslexiaReader