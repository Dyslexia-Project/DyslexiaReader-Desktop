#include "dyslexia_reader/paragraph_rebuilder.hpp"

#ifdef DYSLEXIA_HAS_TESSERACT
#include "dyslexia_reader/tesseract_ocr_engine.hpp"
#include "ocr_build_paths.hpp"
#endif

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "CHECK failed: " #cond " at " << __FILE__ << ":"   \
                      << __LINE__ << std::endl;                              \
            std::exit(1);                                                    \
        }                                                                    \
    } while (false)

namespace {

std::u16string slice(std::u16string_view text, dyslexia_reader::TextRange range) {
    CHECK(range.start <= text.size());
    CHECK(range.start + range.length <= text.size());
    return std::u16string(text.substr(range.start, range.length));
}

void test_hyphen_wrap_and_paragraph_breaks() {
    using namespace dyslexia_reader;

    OcrResult ocr;
    ocr.languageTag = "en-US";

    // Line 1 ends with a hyphenated wrap "dys-"
    OcrLine line1;
    line1.text = u"Reading multi-";
    line1.bounds = {20.0F, 10.0F, 200.0F, 20.0F};
    line1.words = {
        {u"Reading", {20.0F, 10.0F, 90.0F, 20.0F}, 0.98F},
        {u"multi-", {120.0F, 10.0F, 70.0F, 20.0F}, 0.95F},
    };

    // Line 2 continues the wrapped word "syllable" and ends sentence with decimal 3.14.
    OcrLine line2;
    line2.text = u"syllable text like 3.14 works.";
    line2.bounds = {20.0F, 34.0F, 300.0F, 20.0F};
    line2.words = {
        {u"syllable", {20.0F, 34.0F, 80.0F, 20.0F}, 0.97F},
        {u"text", {105.0F, 34.0F, 45.0F, 20.0F}, 0.99F},
        {u"like", {155.0F, 34.0F, 40.0F, 20.0F}, 0.98F},
        {u"3.14", {200.0F, 34.0F, 45.0F, 20.0F}, 0.96F},
        {u"works.", {250.0F, 34.0F, 65.0F, 20.0F}, 0.97F},
    };

    // Line 3 starts a new paragraph (large vertical gap + indent).
    OcrLine line3;
    line3.text = u"Second paragraph! Is U.S. ready?";
    line3.bounds = {60.0F, 92.0F, 320.0F, 20.0F};
    line3.words = {
        {u"Second", {60.0F, 92.0F, 70.0F, 20.0F}, 0.98F},
        {u"paragraph!", {135.0F, 92.0F, 100.0F, 20.0F}, 0.98F},
        {u"Is", {240.0F, 92.0F, 20.0F, 20.0F}, 0.99F},
        {u"U.S.", {265.0F, 92.0F, 40.0F, 20.0F}, 0.95F},
        {u"ready?", {310.0F, 92.0F, 65.0F, 20.0F}, 0.97F},
    };

    ocr.lines = {line1, line2, line3};

    ParagraphRebuilder rebuilder;
    const ReadingDocument doc = rebuilder.rebuild(ocr);

    CHECK(doc.text ==
          u"Reading multisyllable text like 3.14 works.\n\n"
          u"Second paragraph! Is U.S. ready?");
    CHECK(doc.paragraphs.size() == 2);
    CHECK(slice(doc.text, doc.paragraphs[0]) ==
          u"Reading multisyllable text like 3.14 works.");
    CHECK(slice(doc.text, doc.paragraphs[1]) ==
          u"Second paragraph! Is U.S. ready?");

    CHECK(doc.sentences.size() == 3);
    CHECK(slice(doc.text, doc.sentences[0]) ==
          u"Reading multisyllable text like 3.14 works.");
    CHECK(slice(doc.text, doc.sentences[1]) == u"Second paragraph!");
    CHECK(slice(doc.text, doc.sentences[2]) == u"Is U.S. ready?");

    CHECK(doc.words.size() == 11);
    CHECK(slice(doc.text, doc.words[1].text) == u"multisyllable");
    CHECK(doc.words[1].sourceBounds.x == 20.0F);
    CHECK(doc.words[1].sourceBounds.width == 170.0F);
}

void test_utf8_clipboard_rebuilder() {
    using namespace dyslexia_reader;

    const ReadingDocument doc = build_reading_document_from_utf8(
        "Hello re-\r\nwrapped world.\r\n\r\nNext block! Done?");
    CHECK(doc.text == u"Hello rewrapped world.\n\nNext block! Done?");
    CHECK(doc.paragraphs.size() == 2);
    CHECK(doc.sentences.size() == 3);
    CHECK(slice(doc.text, doc.sentences[0]) == u"Hello rewrapped world.");
    CHECK(slice(doc.text, doc.sentences[1]) == u"Next block!");
    CHECK(slice(doc.text, doc.sentences[2]) == u"Done?");
}

#ifdef DYSLEXIA_HAS_TESSERACT
void test_tesseract_engine_on_sample_image() {
    using namespace dyslexia_reader;
    namespace fs = std::filesystem;

    const fs::path sample = fs::path(kProjectSourceDir) / "test.png";
    if (!fs::exists(sample)) {
        std::cout << "[Skip] Sample image missing: " << sample.string()
                  << std::endl;
        return;
    }

    TesseractOcrEngine engine("en-US");
    const OcrResult ocr = engine.recognize_file(sample.string());
    CHECK(!ocr.lines.empty());
    CHECK(ocr.languageTag == "en-US");

    ParagraphRebuilder rebuilder;
    const ReadingDocument doc = rebuilder.rebuild(ocr);
    CHECK(!doc.text.empty());
    CHECK(!doc.words.empty());
    CHECK(!doc.paragraphs.empty());
    CHECK(!doc.sentences.empty());
}
#endif

}  // namespace

int main() {
    test_hyphen_wrap_and_paragraph_breaks();
    test_utf8_clipboard_rebuilder();
#ifdef DYSLEXIA_HAS_TESSERACT
    test_tesseract_engine_on_sample_image();
#endif
    return 0;
}
