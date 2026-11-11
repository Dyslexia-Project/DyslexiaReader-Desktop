#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dyslexia_reader {

using TextOffset = std::uint32_t;

struct TextRange {
    TextOffset start = 0;
    TextOffset length = 0;
};

struct RectF {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

struct SizeF {
    float width = 0;
    float height = 0;
};

enum class FontFamily {
    Lexend,
    AtkinsonHyperlegible,
    OpenDyslexic
};

enum class ThemeId {
    WarmCream,
    Sepia,
    DarkHighContrast
};

enum class ReaderMode {
    Floating,
    Focus
};

enum class PlaybackState {
    Idle,
    Loading,
    Playing,
    Paused,
    Stopped,
    Completed,
    Failed
};

enum class SpeechMarkKind {
    Word,
    Sentence
};

enum class SpeechEventKind {
    Started,
    Paused,
    Resumed,
    Stopped,
    Completed,
    MarkReached,
    Failed
};

enum class PixelFormat {
    Bgra8,
    Rgba8
};

enum class OverlayKind {
    ReadingRuler,
    SentenceHighlight,
    WordHighlight
};

enum class OverlayLayer {
    UnderText,
    OverText
};

enum class ReaderErrorCode {
    CaptureFailed,
    OcrFailed,
    OcrLanguageUnavailable,
    FontUnavailable,
    SpeechUnavailable,
    NetworkFailed,
    CacheFailed,
    InvalidProfile,
    WindowCreationFailed,
    RenderingFailed
};

class ReaderError final : public std::runtime_error {
public:
    ReaderError(ReaderErrorCode code, const std::string& message)
        : std::runtime_error(message), code_(code) {}

    ReaderErrorCode code() const noexcept { return code_; }

private:
    ReaderErrorCode code_;
};

struct AidSettings {
    bool bionicReading = false;
    bool readingRuler = false;
    bool bimodalHighlighting = true;
};

struct ReadingProfile {
    std::uint32_t schemaVersion = 1;
    FontFamily fontFamily = FontFamily::Lexend;
    float fontSizePt = 20;
    float letterSpacingEm = 0.02F;
    float wordSpacingEm = 0.1F;
    float lineHeight = 1.6F;
    std::uint32_t lineWidthCh = 65;
    ThemeId theme = ThemeId::WarmCream;
    AidSettings aids;
};

struct FontAsset {
    FontFamily family = FontFamily::Lexend;
    std::vector<std::uint8_t> bytes;
};

struct OcrBoundingBox {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

struct OcrWord {
    std::u16string text;
    OcrBoundingBox bounds;
    float confidence = 0;
};

struct OcrLine {
    std::u16string text;
    OcrBoundingBox bounds;
    std::vector<OcrWord> words;
};

struct OcrResult {
    std::vector<OcrLine> lines;
    std::string languageTag;
};

struct WordSpan {
    TextRange text;
    OcrBoundingBox sourceBounds;
};

struct ReadingDocument {
    std::u16string text;
    std::vector<TextRange> paragraphs;
    std::vector<TextRange> sentences;
    std::vector<WordSpan> words;
};

struct ScreenRect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

struct CapturedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t strideBytes = 0;
    PixelFormat pixelFormat = PixelFormat::Bgra8;
    float scaleX = 1;
    float scaleY = 1;
    ScreenRect desktopBounds;
    std::vector<std::uint8_t> pixels;
};

struct Viewport {
    SizeF size;
    float scrollY = 0;
};

struct VisualLine {
    TextRange text;
    RectF bounds;
};

struct VisualWord {
    TextRange text;
    std::vector<RectF> fragments;
};

struct VisualSentence {
    TextRange text;
    std::vector<RectF> fragments;
};

struct TextLayoutSnapshot {
    std::vector<VisualLine> lines;
    std::vector<VisualWord> words;
    std::vector<VisualSentence> sentences;
};

struct HighlightState {
    std::optional<TextRange> sentence;
    std::optional<TextRange> word;
    bool wordTimingReliable = false;
    bool sentenceTimingReliable = false;
};

struct TextStyleRange {
    TextRange text;
    std::uint16_t fontWeight = 400;
};

struct AidOverlay {
    OverlayKind kind = OverlayKind::ReadingRuler;
    RectF bounds;
    std::uint32_t rgba = 0;  // 0xRRGGBBAA
    float opacity = 1;
    OverlayLayer layer = OverlayLayer::OverText;
};

struct AidVisuals {
    std::vector<TextStyleRange> textStyles;
    std::vector<AidOverlay> overlays;
};

struct ReadingSessionData {
    ReadingDocument document;
    ReadingProfile profile;
    ReaderMode mode = ReaderMode::Floating;
    PlaybackState playbackState = PlaybackState::Idle;
    float scrollY = 0;
    std::string selectedVoiceId;
    std::optional<TextRange> activeSelection;
};

struct SpeechMark {
    SpeechMarkKind kind = SpeechMarkKind::Word;
    TextRange text;
    std::uint64_t timeMilliseconds = 0;
};

struct SpeechRequest {
    std::u16string text;
    std::optional<TextRange> selection;
    std::string voiceId;
    float rate = 1;
};

struct SpeechEvent {
    SpeechEventKind kind = SpeechEventKind::Failed;
    std::optional<SpeechMark> mark;
    std::string message;
};

struct VoiceInfo {
    std::string id;
    std::string displayName;
    std::string locale;
};

struct SpeechCapabilities {
    bool pauseResume = false;
    bool wordTiming = false;
    bool sentenceTiming = false;
};

struct AudioChunk {
    std::string mediaType;
    std::vector<std::uint8_t> bytes;
};

struct CloudSpeechRequest {
    std::u16string text;
    std::optional<TextRange> selection;
    std::string voiceId;
    float rate = 1;
};

struct CloudSpeechResult {
    std::vector<AudioChunk> chunks;
    std::vector<SpeechMark> marks;
    bool wordTimingReliable = false;
    bool sentenceTimingReliable = false;
};

}  // namespace dyslexia_reader
