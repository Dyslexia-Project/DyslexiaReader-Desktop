#pragma once

#include "dyslexia_reader/interfaces.hpp"

#include <memory>

namespace dyslexia_reader::windows {

class WindowsSpeechEngine final : public ISpeechEngine {
public:
    WindowsSpeechEngine();
    ~WindowsSpeechEngine() override;

    WindowsSpeechEngine(const WindowsSpeechEngine&) = delete;
    WindowsSpeechEngine& operator=(const WindowsSpeechEngine&) = delete;

    std::vector<VoiceInfo> voices() const override;
    SpeechCapabilities capabilities() const override;
    void start(const SpeechRequest& request, SpeechEventHandler handler) override;
    void pause() override;
    void resume() override;
    void stop() override;
    void set_rate(float rate) override;

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace dyslexia_reader::windows
