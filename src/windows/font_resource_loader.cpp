#include "dyslexia_reader/windows/font_resource_loader.hpp"
#include "../../resources/font_resource_ids.h"

namespace dyslexia_reader::windows {

std::vector<std::uint8_t> FontResourceLoader::load_resource(
    WORD resourceId) const {
    if (instance_ == nullptr) {
        throw ReaderError(ReaderErrorCode::FontUnavailable,
                          "Font resource module is not available");
    }

    const HRSRC resource =
        FindResourceW(instance_, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (resource == nullptr) {
        throw ReaderError(ReaderErrorCode::FontUnavailable,
                          "A bundled font resource could not be found");
    }

    const DWORD size = SizeofResource(instance_, resource);
    if (size == 0) {
        throw ReaderError(ReaderErrorCode::FontUnavailable,
                          "A bundled font resource has an invalid size");
    }

    const HGLOBAL loadedResource = LoadResource(instance_, resource);
    if (loadedResource == nullptr) {
        throw ReaderError(ReaderErrorCode::FontUnavailable,
                          "A bundled font resource could not be loaded");
    }
    const auto* data = static_cast<const std::uint8_t*>(
        LockResource(loadedResource));
    if (data == nullptr) {
        throw ReaderError(ReaderErrorCode::FontUnavailable,
                          "A bundled font resource could not be loaded");
    }

    return std::vector<std::uint8_t>(data, data + size);
}

std::vector<FontAsset> FontResourceLoader::load_fonts() {
    return {
        load_asset(FontFamily::Lexend, IDR_FONT_LEXEND),
        load_asset(FontFamily::AtkinsonHyperlegible,
                   IDR_FONT_ATKINSON_REGULAR),
        load_asset(FontFamily::AtkinsonHyperlegible, IDR_FONT_ATKINSON_BOLD),
        load_asset(FontFamily::OpenDyslexic, IDR_FONT_OPENDYSLEXIC_REGULAR),
        load_asset(FontFamily::OpenDyslexic, IDR_FONT_OPENDYSLEXIC_BOLD)};
}

FontAsset FontResourceLoader::load_asset(FontFamily family,
                                         WORD resourceId) const {
    return {family, load_resource(resourceId)};
}

}  // namespace dyslexia_reader::windows
