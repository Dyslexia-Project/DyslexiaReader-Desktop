#ifndef OCR_SERVICE_HPP
#define OCR_SERVICE_HPP

#include <string>

#if __has_include(<leptonica/allheaders.h>)
#include <leptonica/allheaders.h>
#elif __has_include(<allheaders.h>)
#include <allheaders.h>
#endif

namespace DyslexiaReader {

class OcrService {
public:
    OcrService();
    ~OcrService();

    // Main capture and recognition method invoked by the application (cross-platform)
    std::string captureAndRecognizeText();

    // Recognize text from an existing image file on disk
    std::string recognizeTextFromImage(const std::string& imagePath);
};

} // namespace DyslexiaReader

#endif // OCR_SERVICE_HPP