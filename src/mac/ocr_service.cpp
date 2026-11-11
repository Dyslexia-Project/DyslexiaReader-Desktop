#include "dyslexia_reader/ocr_service.hpp"

#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <tesseract/baseapi.h>
#if __has_include(<leptonica/allheaders.h>)
#include <leptonica/allheaders.h>
#elif __has_include(<allheaders.h>)
#include <allheaders.h>
#endif

namespace DyslexiaReader {

OcrService::OcrService() {}
OcrService::~OcrService() {}

std::string OcrService::recognizeTextFromImage(const std::string& imagePath) {
    tesseract::TessBaseAPI tess;

    const char* datapath = nullptr;
    if (std::filesystem::exists("./tessdata/eng.traineddata")) {
        datapath = "./tessdata";
    } else if (std::filesystem::exists("./build/tessdata/eng.traineddata")) {
        datapath = "./build/tessdata";
    } else if (std::filesystem::exists("/opt/homebrew/share/tessdata/eng.traineddata")) {
        datapath = "/opt/homebrew/share/tessdata";
    }

    if (tess.Init(datapath, "eng") != 0) {
        if (datapath != nullptr && tess.Init(nullptr, "eng") != 0) {
            std::cerr << "[OCR ERROR] Could not initialize Tesseract engine." << std::endl;
            return "";
        } else if (datapath == nullptr) {
            std::cerr << "[OCR ERROR] Could not initialize Tesseract engine." << std::endl;
            return "";
        }
    }

    Pix* image = pixRead(imagePath.c_str());
    if (!image) {
        std::cerr << "[OCR ERROR] Could not open image: " << imagePath << std::endl;
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
    std::cout << "\n[macOS SNIPPING TOOL] Select a region on your screen..." << std::endl;
    std::string tempPath = "./temp_snippet.png";
    
    std::string captureCmd = "screencapture -i " + tempPath;
    int res = std::system(captureCmd.c_str());

    if (res != 0 || !std::filesystem::exists(tempPath)) {
        std::cerr << "[SNIPPING TOOL] Screen capture cancelled or failed." << std::endl;
        return "";
    }

    std::string text = recognizeTextFromImage(tempPath);
    std::filesystem::remove(tempPath);
    return text;
}

} // namespace DyslexiaReader