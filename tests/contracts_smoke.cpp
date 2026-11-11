#include "dyslexia_reader/mocks.hpp"

#include <cassert>

int main() {
    dyslexia_reader::MockScreenCapturer capturer;
    const auto image = capturer.capture_selection();
    assert(image.has_value());

    dyslexia_reader::MockOcrEngine ocrEngine;
    const auto ocr = ocrEngine.recognize(*image);
    assert(ocr.lines.size() == 2);
    assert(ocr.lines.front().words.front().bounds.x == 10);

    dyslexia_reader::MockParagraphRebuilder rebuilder;
    const auto document = rebuilder.rebuild(ocr);
    assert(document.text == u"comprehension improves focus.");
    assert(document.words.size() == 3);
    assert(document.words.front().text.length == 13);

    dyslexia_reader::MockTextRenderer renderer;
    renderer.set_document(document);
    renderer.invalidate();
    assert(renderer.document().text == document.text);
    assert(renderer.invalidation_count() == 1);

    dyslexia_reader::MockFontLoader fontLoader;
    assert(fontLoader.load_fonts().size() == 3);
}
