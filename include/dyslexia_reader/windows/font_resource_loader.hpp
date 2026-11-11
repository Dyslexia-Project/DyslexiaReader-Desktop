#pragma once

#include "dyslexia_reader/interfaces.hpp"

#include <windows.h>

namespace dyslexia_reader::windows {

class FontResourceLoader final : public IFontLoader {
public:
    explicit FontResourceLoader(HINSTANCE instance) : instance_(instance) {}

    std::vector<FontAsset> load_fonts() override;

private:
    std::vector<std::uint8_t> load_resource(WORD resourceId) const;
    FontAsset load_asset(FontFamily family, WORD resourceId) const;

    HINSTANCE instance_ = nullptr;
};

}  // namespace dyslexia_reader::windows
