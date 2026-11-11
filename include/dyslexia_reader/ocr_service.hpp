#ifndef OCR_SERVICE_HPP
#define OCR_SERVICE_HPP

#include <string>

namespace DyslexiaReader {

class OcrService {
public:
    OcrService();
    ~OcrService();

    // Metodă unică apelată de aplicație (indiferent de OS)
    std::string captureAndRecognizeText();

    // Recunoaștere dintr-o imagine existentă pe disc
    std::string recognizeTextFromImage(const std::string& imagePath);
};

} // namespace DyslexiaReader

#endif // OCR_SERVICE_HPP