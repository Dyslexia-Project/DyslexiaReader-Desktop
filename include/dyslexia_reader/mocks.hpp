#pragma once

#include "dyslexia_reader/interfaces.hpp"

namespace dyslexia_reader {

class MockScreenCapturer final : public IScreenCapturer {
public:
    std::optional<CapturedImage> capture_selection() override {
        CapturedImage image;
        image.width = 2;
        image.height = 2;
        image.strideBytes = 8;
        image.pixels = {0, 0, 0, 255, 0, 0, 0, 255,
                        0, 0, 0, 255, 0, 0, 0, 255};
        return image;
    }
};

class MockOcrEngine final : public IOcrEngine {
public:
    OcrResult recognize(const CapturedImage&) override {
        OcrResult result;
        result.languageTag = "en-US";
        result.lines = {
            {u"compre-",
             {10, 10, 70, 18},
             {{u"compre-", {10, 10, 70, 18}, 0.98F}}},
            {u"hension improves focus.",
             {10, 30, 210, 18},
             {{u"hension", {10, 30, 75, 18}, 0.97F},
              {u"improves", {92, 30, 78, 18}, 0.99F},
              {u"focus.", {180, 30, 40, 18}, 0.98F}}}};
        return result;
    }
};

class MockParagraphRebuilder final : public IParagraphRebuilder {
public:
    ReadingDocument rebuild(const OcrResult&) override {
        ReadingDocument document;
        document.text = u"comprehension improves focus.";
        document.paragraphs = {{0, 28}};
        document.sentences = {{0, 28}};
        document.words = {
            {{0, 13}, {10, 10, 75, 38}},
            {{14, 8}, {92, 30, 78, 18}},
            {{23, 5}, {180, 30, 40, 18}}};
        return document;
    }
};

class MockFontLoader final : public IFontLoader {
public:
    std::vector<FontAsset> load_fonts() override {
        return {
            {FontFamily::Lexend, {0}},
            {FontFamily::AtkinsonHyperlegible, {0}},
            {FontFamily::OpenDyslexic, {0}}};
    }
};

class MockTextRenderer final : public ITextRenderer {
public:
    void set_document(const ReadingDocument& document) override {
        document_ = document;
    }

    void set_profile(const ReadingProfile& profile) override {
        profile_ = profile;
    }

    void set_viewport(const Viewport& viewport) override {
        viewport_ = viewport;
    }

    TextLayoutSnapshot layout_snapshot() const override {
        return layout_;
    }

    void set_aid_visuals(const AidVisuals& visuals) override {
        visuals_ = visuals;
    }

    void set_highlights(const HighlightState& highlights) override {
        highlights_ = highlights;
    }

    void invalidate() override {
        ++invalidationCount_;
    }

    const ReadingDocument& document() const { return document_; }
    const ReadingProfile& profile() const { return profile_; }
    const Viewport& viewport() const { return viewport_; }
    const HighlightState& highlights() const { return highlights_; }
    std::uint32_t invalidation_count() const { return invalidationCount_; }

private:
    ReadingDocument document_;
    ReadingProfile profile_;
    Viewport viewport_;
    TextLayoutSnapshot layout_;
    AidVisuals visuals_;
    HighlightState highlights_;
    std::uint32_t invalidationCount_ = 0;
};

class MockSpeechEngine final : public ISpeechEngine {
public:
    std::vector<VoiceInfo> voices() const override {
        return {{"mock-en-us", "Mock English", "en-US"}};
    }

    SpeechCapabilities capabilities() const override {
        return {true, true, true};
    }

    void start(const SpeechRequest&, SpeechEventHandler handler) override {
        if (handler) {
            handler({SpeechEventKind::Started, std::nullopt, {}});
            handler({SpeechEventKind::MarkReached,
                     SpeechMark{SpeechMarkKind::Word, {0, 13}, 0}, {}});
        }
    }

    void pause() override {}
    void resume() override {}
    void stop() override {}
    void set_rate(float) override {}
};

class MockCloudSpeechClient final : public ICloudSpeechClient {
public:
    CloudSpeechResult synthesize(const CloudSpeechRequest&) override {
        CloudSpeechResult result;
        result.chunks.push_back({"audio/mock", {0}});
        result.marks = {{SpeechMarkKind::Word, {0, 13}, 0}};
        result.wordTimingReliable = true;
        return result;
    }
};

}  // namespace dyslexia_reader
