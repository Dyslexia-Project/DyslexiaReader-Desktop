#include "dyslexia_reader/tesseract_ocr_engine.hpp"

#ifdef DYSLEXIA_HAS_TESSERACT
#include "tesseract_ocr.hpp"
#endif

#include <utility>

namespace dyslexia_reader {

TesseractOcrEngine::TesseractOcrEngine(std::string languageTag)
    : languageTag_(std::move(languageTag)) {}

OcrResult TesseractOcrEngine::recognize(const CapturedImage& image) {
#ifdef DYSLEXIA_HAS_TESSERACT
    return DyslexiaReader::detail::recognize_ocr_result_tesseract(image,
                                                                  languageTag_);
#else
    (void)image;
    throw ReaderError(ReaderErrorCode::OcrLanguageUnavailable,
                      "Tesseract support is disabled in this build.");
#endif
}

OcrResult TesseractOcrEngine::recognize_file(const std::string& imagePath) {
#ifdef DYSLEXIA_HAS_TESSERACT
    return DyslexiaReader::detail::recognize_file_ocr_result_tesseract(
        imagePath, languageTag_);
#else
    (void)imagePath;
    throw ReaderError(ReaderErrorCode::OcrLanguageUnavailable,
                      "Tesseract support is disabled in this build.");
#endif
}

}  // namespace dyslexia_reader
