#pragma once

#include "dyslexia_reader/domain.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dyslexia_reader {

class IScreenCapturer {
public:
    virtual ~IScreenCapturer() = default;
    virtual std::optional<CapturedImage> capture_selection() = 0;
};

class IOcrEngine {
public:
    virtual ~IOcrEngine() = default;
    virtual OcrResult recognize(const CapturedImage& image) = 0;
};

class IParagraphRebuilder {
public:
    virtual ~IParagraphRebuilder() = default;
    virtual ReadingDocument rebuild(const OcrResult& ocr) = 0;
};

class IFontLoader {
public:
    virtual ~IFontLoader() = default;
    virtual std::vector<FontAsset> load_fonts() = 0;
};

class ITextRenderer {
public:
    virtual ~ITextRenderer() = default;
    virtual void set_document(const ReadingDocument& document) = 0;
    virtual void set_profile(const ReadingProfile& profile) = 0;
    virtual void set_viewport(const Viewport& viewport) = 0;
    virtual TextLayoutSnapshot layout_snapshot() const = 0;
    virtual void set_aid_visuals(const AidVisuals& visuals) = 0;
    virtual void set_highlights(const HighlightState& highlights) = 0;
    virtual void invalidate() = 0;
};

struct ReadingAidContext {
    const ReadingDocument& document;
    const ReadingProfile& profile;
    const TextLayoutSnapshot& layout;
    HighlightState highlights;
    std::optional<std::uint32_t> activeVisualLine;
};

class IReadingAid {
public:
    virtual ~IReadingAid() = default;
    virtual std::string id() const = 0;
    virtual AidVisuals compose(const ReadingAidContext& context) const = 0;
};

using SpeechEventHandler = std::function<void(const SpeechEvent&)>;

class ISpeechEngine {
public:
    virtual ~ISpeechEngine() = default;
    virtual std::vector<VoiceInfo> voices() const = 0;
    virtual SpeechCapabilities capabilities() const = 0;
    virtual void start(const SpeechRequest& request, SpeechEventHandler handler) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    virtual void set_rate(float rate) = 0;
};

class ICloudSpeechClient {
public:
    virtual ~ICloudSpeechClient() = default;
    virtual CloudSpeechResult synthesize(const CloudSpeechRequest& request) = 0;
};

class IAudioPlayer {
public:
    virtual ~IAudioPlayer() = default;
    virtual void play(const std::vector<AudioChunk>& chunks, SpeechEventHandler handler) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    virtual void set_rate(float rate) = 0;
};

class IAudioCache {
public:
    virtual ~IAudioCache() = default;
    virtual std::optional<CloudSpeechResult> find(const std::string& key) = 0;
    virtual void store(const std::string& key, const CloudSpeechResult& result) = 0;
};

class IReadingProfileStore {
public:
    virtual ~IReadingProfileStore() = default;
    virtual std::optional<ReadingProfile> load() = 0;
    virtual void save(const ReadingProfile& profile) = 0;
};

}  // namespace dyslexia_reader
