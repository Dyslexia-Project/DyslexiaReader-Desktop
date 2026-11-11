#include "dyslexia_reader/ocr_service.hpp"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::cout << "=== Dyslexia Reader Desktop — Snipping OCR Test ===" << std::endl;

    DyslexiaReader::OcrService ocrService;

    if (argc > 1 && (std::string(argv[1]) == "--capture" || std::string(argv[1]) == "-c")) {
        std::string extractedText = ocrService.captureAndRecognizeText();
        if (extractedText.empty()) {
            std::cerr << "[ERROR] Could not capture or extract text from selection." << std::endl;
            return 1;
        }
        std::cout << "\n---------------- DETECTED TEXT ----------------" << std::endl;
        std::cout << extractedText << std::endl;
        std::cout << "-----------------------------------------------" << std::endl;
        std::cout << "[SUCCESS] OCR completed successfully!" << std::endl;
        return 0;
    }

    // Path to image (if not provided as an argument, look for test.png)
    std::string imagePath;
    if (argc > 1) {
        imagePath = argv[1];
    } else if (std::filesystem::exists("test.png")) {
        imagePath = "test.png";
    } else if (std::filesystem::exists("../test.png")) {
        imagePath = "../test.png";
    } else {
        imagePath = "test.png";
    }

    std::cout << "[INFO] Processing image: " << imagePath << std::endl;

    std::string extractedText = ocrService.recognizeTextFromImage(imagePath);
    if (extractedText.empty()) {
        std::cerr << "[ERROR] Could not open or process image: " << imagePath << std::endl;
        std::cerr << "Tip: Take a screenshot using 'Cmd + Shift + 4' and save it as 'test.png'!" << std::endl;
        return 1;
    }

    std::cout << "\n---------------- DETECTED TEXT ----------------" << std::endl;
    std::cout << extractedText << std::endl;
    std::cout << "-----------------------------------------------" << std::endl;
    std::cout << "[SUCCESS] OCR completed successfully!" << std::endl;

    return 0;
}