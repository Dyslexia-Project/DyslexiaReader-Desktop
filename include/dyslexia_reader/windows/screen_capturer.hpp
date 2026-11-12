#pragma once

#include "dyslexia_reader/interfaces.hpp"

#include <optional>

namespace dyslexia_reader::windows {

class WindowsScreenCapturer final : public IScreenCapturer {
public:
    WindowsScreenCapturer() = default;

    std::optional<CapturedImage> capture_selection() override;
};

}  // namespace dyslexia_reader::windows
