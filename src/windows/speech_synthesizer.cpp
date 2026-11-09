#include "dyslexia_reader/windows/speech_synthesizer.hpp"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/base.h>
#include <windows.h>

#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace dyslexia_reader::windows {
namespace {

class ApartmentScope final {
public:
    ApartmentScope() {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialized_ = true;
        } catch (const winrt::hresult_error& error) {
            if (error.code() != RPC_E_CHANGED_MODE) {
                throw;
            }
        }
    }

    ~ApartmentScope() {
        if (initialized_) {
            winrt::uninit_apartment();
        }
    }

    ApartmentScope(const ApartmentScope&) = delete;
    ApartmentScope& operator=(const ApartmentScope&) = delete;

private:
    bool initialized_ = false;
};

std::string hresult_message(const winrt::hresult_error& error) {
    return "Windows speech operation failed (HRESULT " +
           std::to_string(static_cast<unsigned long>(error.code())) + "): " +
           winrt::to_string(error.message());
}

void validate_rate(float rate) {
    if (!std::isfinite(rate) || rate < 0.5F || rate > 2.0F) {
        throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                          "Speech rate must be between 0.5x and 2.0x");
    }
}

}  // namespace

class WindowsSpeechEngine::Impl final
    : public std::enable_shared_from_this<WindowsSpeechEngine::Impl> {
public:
    void shutdown() {
        stop();
        std::lock_guard<std::mutex> workerLock(workerMutex_);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void start(const SpeechRequest& request, SpeechEventHandler handler) {
        if (request.text.empty()) {
            throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                              "Cannot synthesize empty text");
        }
        if (request.text.size() >
            std::numeric_limits<TextOffset>::max()) {
            throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                              "Speech text exceeds the supported document size");
        }
        validate_rate(request.rate);

        std::u16string text = request.text;
        TextOffset baseOffset = 0;
        if (request.selection) {
            const auto selection = *request.selection;
            const auto textLength = request.text.size();
            if (selection.length == 0 || selection.start > textLength ||
                selection.length > textLength - selection.start) {
                throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                                  "Speech selection is outside the document");
            }
            baseOffset = selection.start;
            text = request.text.substr(selection.start, selection.length);
        }
        if (text.empty()) {
            throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                              "Cannot synthesize an empty text selection");
        }

        std::lock_guard<std::mutex> workerLock(workerMutex_);
        stop();
        if (worker_.joinable()) {
            worker_.join();
        }

        const auto generation =
            generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_ = true;
            rate_ = request.rate;
            handler_ = std::move(handler);
            synthesisOperation_ = nullptr;
            player_ = nullptr;
        }

        auto self = shared_from_this();
        try {
            worker_ = std::thread(
                [self = std::move(self), text = std::move(text), baseOffset,
                 voiceId = request.voiceId, generation]() mutable {
                    self->synthesize(std::move(text), baseOffset, voiceId,
                                     generation);
                });
        } catch (const std::system_error& error) {
            std::lock_guard<std::mutex> lock(mutex_);
            active_ = false;
            throw ReaderError(
                ReaderErrorCode::SpeechUnavailable,
                std::string("Could not start the Windows speech worker: ") +
                    error.what());
        }
    }

    void pause() {
        ApartmentScope apartment;
        winrt::Windows::Media::Playback::MediaPlayer player{nullptr};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            player = player_;
        }
        if (!player) {
            return;
        }
        try {
            player.Pause();
            emit_current({SpeechEventKind::Paused, std::nullopt, {}});
        } catch (const winrt::hresult_error& error) {
            throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                              hresult_message(error));
        }
    }

    void resume() {
        ApartmentScope apartment;
        winrt::Windows::Media::Playback::MediaPlayer player{nullptr};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            player = player_;
        }
        if (!player) {
            return;
        }
        try {
            player.Play();
            emit_current({SpeechEventKind::Resumed, std::nullopt, {}});
        } catch (const winrt::hresult_error& error) {
            throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                              hresult_message(error));
        }
    }

    void stop() {
        ApartmentScope apartment;
        winrt::Windows::Foundation::IAsyncOperation<
            winrt::Windows::Media::SpeechSynthesis::SpeechSynthesisStream>
            operation{nullptr};
        winrt::Windows::Media::Playback::MediaPlayer player{nullptr};
        SpeechEventHandler handler;
        bool wasActive = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wasActive = active_;
            active_ = false;
            if (wasActive) {
                generation_.fetch_add(1, std::memory_order_acq_rel);
                handler = handler_;
            }
            operation = synthesisOperation_;
            player = player_;
        }
        if (operation) {
            try {
                operation.Cancel();
            } catch (const winrt::hresult_error& error) {
                OutputDebugStringA(hresult_message(error).c_str());
                OutputDebugStringA("\n");
            }
        }
        if (player) {
            try {
                player.Pause();
                player.Source(nullptr);
            } catch (const winrt::hresult_error& error) {
                OutputDebugStringA(hresult_message(error).c_str());
                OutputDebugStringA("\n");
            }
        }
        if (wasActive && handler) {
            try {
                handler({SpeechEventKind::Stopped, std::nullopt, {}});
            } catch (const std::exception& error) {
                OutputDebugStringA("Speech event handler failed: ");
                OutputDebugStringA(error.what());
                OutputDebugStringA("\n");
            }
        }
    }

    void set_rate(float rate) {
        ApartmentScope apartment;
        validate_rate(rate);
        winrt::Windows::Media::Playback::MediaPlayer player{nullptr};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rate_ = rate;
            player = player_;
        }
        if (!player) {
            return;
        }
        try {
            player.PlaybackSession().PlaybackRate(rate);
        } catch (const winrt::hresult_error& error) {
            throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                              hresult_message(error));
        }
    }

private:
    using SpeechSynthesizer =
        winrt::Windows::Media::SpeechSynthesis::SpeechSynthesizer;
    using SpeechSynthesisStream =
        winrt::Windows::Media::SpeechSynthesis::SpeechSynthesisStream;
    using MediaPlayer = winrt::Windows::Media::Playback::MediaPlayer;
    using MediaPlaybackItem =
        winrt::Windows::Media::Playback::MediaPlaybackItem;
    using TimedMetadataTrack =
        winrt::Windows::Media::Core::TimedMetadataTrack;
    using MediaCueEventArgs =
        winrt::Windows::Media::Core::MediaCueEventArgs;
    using SpeechCue = winrt::Windows::Media::Core::SpeechCue;

    void synthesize(std::u16string text, TextOffset baseOffset,
                    const std::string& voiceId, std::uint64_t generation) {
        try {
            ApartmentScope apartment;
            SpeechSynthesizer synthesizer;
            synthesizer.Options().IncludeWordBoundaryMetadata(true);
            synthesizer.Options().IncludeSentenceBoundaryMetadata(true);

            if (!voiceId.empty()) {
                const auto requestedVoice = winrt::to_hstring(voiceId);
                const auto availableVoices = SpeechSynthesizer::AllVoices();
                winrt::Windows::Media::SpeechSynthesis::VoiceInformation
                    selectedVoice{nullptr};
                for (const auto& voice : availableVoices) {
                    if (voice.Id() == requestedVoice) {
                        selectedVoice = voice;
                        break;
                    }
                }
                if (!selectedVoice) {
                    throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                                      "The selected Windows speech voice is "
                                      "no longer available");
                }
                synthesizer.Voice(selectedVoice);
            }

            const auto input = winrt::hstring(
                reinterpret_cast<const wchar_t*>(text.data()), text.size());
            auto operation = synthesizer.SynthesizeTextToStreamAsync(input);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!is_current(generation)) {
                    operation.Cancel();
                    return;
                }
                synthesisOperation_ = operation;
            }
            SpeechSynthesisStream stream = operation.get();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                synthesisOperation_ = nullptr;
                if (!is_current(generation)) {
                    return;
                }
            }

            auto source =
                winrt::Windows::Media::Core::MediaSource::CreateFromStream(
                    stream, stream.ContentType());
            MediaPlaybackItem item(source);
            register_timing_tracks(
                item, baseOffset, static_cast<TextOffset>(text.size()),
                generation);

            MediaPlayer player;
            player.AutoPlay(false);
            const auto weak = weak_from_this();
            player.MediaEnded(
                [weak, generation](const MediaPlayer&, const auto&) {
                    if (const auto self = weak.lock()) {
                        self->complete(generation);
                    }
                });
            player.MediaFailed(
                [weak, generation](const MediaPlayer&, const auto& args) {
                    if (const auto self = weak.lock()) {
                        self->fail(generation,
                                   winrt::to_string(args.ErrorMessage()));
                    }
                });

            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!is_current(generation)) {
                    return;
                }
                player_ = player;
                player.Source(item);
                player.PlaybackSession().PlaybackRate(rate_);
                player.Play();
            }
            emit(generation, {SpeechEventKind::Started, std::nullopt, {}});
        } catch (const ReaderError& error) {
            fail(generation, error.what());
        } catch (const winrt::hresult_error& error) {
            if (error.code() != HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
                is_current(generation)) {
                fail(generation, hresult_message(error));
            }
        } catch (const std::exception& error) {
            fail(generation, error.what());
        }
    }

    void register_timing_tracks(
        const MediaPlaybackItem& item, TextOffset baseOffset,
        TextOffset inputLength,
        std::uint64_t generation) {
        auto registeredTracks = std::make_shared<std::set<std::uint32_t>>();
        auto registeredTracksMutex = std::make_shared<std::mutex>();
        const auto weak = weak_from_this();
        const auto registerTracks =
            [weak, registeredTracks, registeredTracksMutex, baseOffset,
             inputLength, generation](const MediaPlaybackItem& playbackItem) {
                const auto tracks = playbackItem.TimedMetadataTracks();
                for (std::uint32_t index = 0; index < tracks.Size(); ++index) {
                    const auto track = tracks.GetAt(index);
                    const auto trackId = track.Id();
                    SpeechMarkKind kind;
                    if (trackId == L"SpeechWord") {
                        kind = SpeechMarkKind::Word;
                    } else if (trackId == L"SpeechSentence") {
                        kind = SpeechMarkKind::Sentence;
                    } else {
                        continue;
                    }

                    {
                        std::lock_guard<std::mutex> lock(
                            *registeredTracksMutex);
                        if (!registeredTracks->insert(index).second) {
                            continue;
                        }
                    }
                    track.CueEntered(
                        [weak, baseOffset, inputLength, generation, kind](
                            const TimedMetadataTrack&, const MediaCueEventArgs& args) {
                            const auto self = weak.lock();
                            if (!self || !self->is_current(generation)) {
                                return;
                            }
                            const auto cue = args.Cue().try_as<SpeechCue>();
                            if (!cue) {
                                return;
                            }

                            const auto start = cue.StartPositionInInput();
                            const auto end = cue.EndPositionInInput();
                            if (end < start || end > inputLength ||
                                end > std::numeric_limits<TextOffset>::max() -
                                          baseOffset) {
                                return;
                            }
                            const auto ticks = cue.StartTime().count();
                            const auto milliseconds =
                                ticks <= 0
                                    ? 0
                                    : static_cast<std::uint64_t>(
                                          ticks / 10000);
                            self->emit(
                                generation,
                                {SpeechEventKind::MarkReached,
                                 SpeechMark{
                                     kind,
                                     {baseOffset + start, end - start},
                                     milliseconds},
                                 {}});
                        });
                    playbackItem.TimedMetadataTracks().SetPresentationMode(
                        index,
                        winrt::Windows::Media::Core::
                            TimedMetadataTrackPresentationMode::
                                ApplicationPresented);
                }
            };

        registerTracks(item);
        item.TimedMetadataTracksChanged(
            [registerTracks](const MediaPlaybackItem& playbackItem,
                             const auto&) { registerTracks(playbackItem); });
    }

    bool is_current(std::uint64_t generation) const {
        return generation_.load(std::memory_order_acquire) == generation;
    }

    void emit(std::uint64_t generation, const SpeechEvent& event) {
        SpeechEventHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!is_current(generation)) {
                return;
            }
            handler = handler_;
        }
        if (handler) {
            try {
                handler(event);
            } catch (const std::exception& error) {
                OutputDebugStringA("Speech event handler failed: ");
                OutputDebugStringA(error.what());
                OutputDebugStringA("\n");
            }
        }
    }

    void emit_current(const SpeechEvent& event) {
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!active_) {
                return;
            }
            generation =
                generation_.load(std::memory_order_acquire);
        }
        emit(generation, event);
    }

    void complete(std::uint64_t generation) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!is_current(generation)) {
                return;
            }
            active_ = false;
        }
        emit(generation, {SpeechEventKind::Completed, std::nullopt, {}});
    }

    void fail(std::uint64_t generation, const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!is_current(generation)) {
                return;
            }
            active_ = false;
        }
        emit(generation, {SpeechEventKind::Failed, std::nullopt, message});
    }

    mutable std::mutex mutex_;
    std::mutex workerMutex_;
    std::thread worker_;
    std::atomic<std::uint64_t> generation_{0};
    bool active_ = false;
    float rate_ = 1.0F;
    SpeechEventHandler handler_;
    winrt::Windows::Foundation::IAsyncOperation<
        winrt::Windows::Media::SpeechSynthesis::SpeechSynthesisStream>
        synthesisOperation_{nullptr};
    winrt::Windows::Media::Playback::MediaPlayer player_{nullptr};
};

WindowsSpeechEngine::WindowsSpeechEngine() : impl_(std::make_shared<Impl>()) {}

WindowsSpeechEngine::~WindowsSpeechEngine() {
    impl_->shutdown();
}

std::vector<VoiceInfo> WindowsSpeechEngine::voices() const {
    try {
        ApartmentScope apartment;
        std::vector<VoiceInfo> result;
        const auto allVoices =
            winrt::Windows::Media::SpeechSynthesis::SpeechSynthesizer::
                AllVoices();
        result.reserve(allVoices.Size());
        for (const auto& voice : allVoices) {
            result.push_back({winrt::to_string(voice.Id()),
                              winrt::to_string(voice.DisplayName()),
                              winrt::to_string(voice.Language())});
        }
        if (result.empty()) {
            throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                              "No local Windows speech voices are installed");
        }
        return result;
    } catch (const ReaderError&) {
        throw;
    } catch (const winrt::hresult_error& error) {
        throw ReaderError(ReaderErrorCode::SpeechUnavailable,
                          hresult_message(error));
    }
}

SpeechCapabilities WindowsSpeechEngine::capabilities() const {
    return {true, true, true};
}

void WindowsSpeechEngine::start(const SpeechRequest& request,
                                SpeechEventHandler handler) {
    impl_->start(request, std::move(handler));
}

void WindowsSpeechEngine::pause() { impl_->pause(); }

void WindowsSpeechEngine::resume() { impl_->resume(); }

void WindowsSpeechEngine::stop() { impl_->stop(); }

void WindowsSpeechEngine::set_rate(float rate) { impl_->set_rate(rate); }

}  // namespace dyslexia_reader::windows
