#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <tesseract/baseapi.h>
#include "ocr_service.hpp"
namespace DyslexiaReader {

OcrService::OcrService() {}
OcrService::~OcrService() {}

std::string OcrService::recognizeTextFromImage(const std::string& imagePath) {
    tesseract::TessBaseAPI tess;

    if (tess.Init("./tessdata", "eng")) {
        if (tess.Init(nullptr, "eng")) {
            std::cerr << "[EROARE OCR] Nu s-a putut inițializa motorul Tesseract." << std::endl;
            return "";
        }
    }

    Pix* image = pixRead(imagePath.c_str());
    if (!image) {
        std::cerr << "[EROARE OCR] Nu s-a putut deschide imaginea: " << imagePath << std::endl;
        tess.End();
        return "";
    }

    tess.SetImage(image);
    char* outText = tess.GetUTF8Text();
    std::string extractedText = outText ? outText : "";

    delete[] outText;
    pixDestroy(&image);
    tess.End();

    return extractedText;
}

std::string OcrService::captureAndRecognizeText() {
    std::cout << "\n[macOS SNIPPING TOOL] Selecteaza zona de pe ecran..." << std::endl;
    std::string tempPath = "./temp_snippet.png";
    
    std::string captureCmd = "screencapture -i " + tempPath;
    int res = std::system(captureCmd.c_str());

    if (res != 0) {
        std::cerr << "[SNIPPING TOOL] Captura anulata." << std::endl;
        return "";
    }

    std::string text = recognizeTextFromImage(tempPath);
    std::remove(tempPath.c_str());
    return text;
}

} // namespace DyslexiaReader