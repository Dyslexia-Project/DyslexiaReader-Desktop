#include "dyslexia_reader/mocks.hpp"
#include "dyslexia_reader/windows/direct2d_text_renderer.hpp"
#include "dyslexia_reader/windows/font_resource_loader.hpp"
#include "dyslexia_reader/windows/speech_synthesizer.hpp"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    try {
        dyslexia_reader::MockScreenCapturer capturer;
        dyslexia_reader::MockOcrEngine ocrEngine;
        dyslexia_reader::MockParagraphRebuilder paragraphRebuilder;

        const auto image = capturer.capture_selection();
        if (!image) {
            return 1;
        }

        const auto ocr = ocrEngine.recognize(*image);
        const auto document = paragraphRebuilder.rebuild(ocr);

        dyslexia_reader::windows::FontResourceLoader fontLoader(instance);
        dyslexia_reader::windows::WindowsSpeechEngine speechEngine;
        dyslexia_reader::windows::Direct2DTextRenderer renderer(fontLoader,
                                                               &speechEngine);
        renderer.create_reader_window(instance);
        renderer.set_document(document);
        renderer.show(showCommand);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    } catch (const dyslexia_reader::ReaderError& error) {
        MessageBoxA(nullptr, error.what(), "Dyslexia Reader",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
}
