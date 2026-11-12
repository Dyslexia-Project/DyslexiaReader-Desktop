#include "dyslexia_reader/windows/profile_store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <combaseapi.h>
#include <shlobj.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>

namespace dyslexia_reader::windows {
namespace {

using nlohmann::json;

const char* font_family_to_string(FontFamily family) {
    switch (family) {
    case FontFamily::Lexend:
        return "Lexend";
    case FontFamily::AtkinsonHyperlegible:
        return "Atkinson Hyperlegible";
    case FontFamily::OpenDyslexic:
        return "OpenDyslexic";
    }
    return "Lexend";
}

FontFamily string_to_font_family(const std::string& value,
                                 FontFamily fallback) {
    if (value == "Lexend") {
        return FontFamily::Lexend;
    }
    if (value == "Atkinson Hyperlegible") {
        return FontFamily::AtkinsonHyperlegible;
    }
    if (value == "OpenDyslexic") {
        return FontFamily::OpenDyslexic;
    }
    return fallback;
}

const char* theme_to_string(ThemeId theme) {
    switch (theme) {
    case ThemeId::WarmCream:
        return "warm-cream";
    case ThemeId::Sepia:
        return "sepia";
    case ThemeId::DarkHighContrast:
        return "dark-high-contrast";
    }
    return "warm-cream";
}

ThemeId string_to_theme(const std::string& value, ThemeId fallback) {
    if (value == "warm-cream") {
        return ThemeId::WarmCream;
    }
    if (value == "sepia") {
        return ThemeId::Sepia;
    }
    if (value == "dark-high-contrast") {
        return ThemeId::DarkHighContrast;
    }
    return fallback;
}

const char* mode_to_string(ReaderMode mode) {
    switch (mode) {
    case ReaderMode::Floating:
        return "floating";
    case ReaderMode::Focus:
        return "focus";
    }
    return "floating";
}

ReaderMode string_to_mode(const std::string& value, ReaderMode fallback) {
    if (value == "floating") {
        return ReaderMode::Floating;
    }
    if (value == "focus") {
        return ReaderMode::Focus;
    }
    return fallback;
}

void validate_profile_for_save(const ReadingProfile& profile) {
    if (profile.schemaVersion != 1) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          "Unsupported ReadingProfile schemaVersion.");
    }
    if (!std::isfinite(profile.fontSizePt) || profile.fontSizePt < 14.0F ||
        profile.fontSizePt > 48.0F) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          "fontSizePt must be within [14, 48].");
    }
    if (!std::isfinite(profile.letterSpacingEm) ||
        profile.letterSpacingEm < 0.0F || profile.letterSpacingEm > 0.2F) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          "letterSpacingEm must be within [0, 0.2].");
    }
    if (!std::isfinite(profile.wordSpacingEm) || profile.wordSpacingEm < 0.0F ||
        profile.wordSpacingEm > 0.5F) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          "wordSpacingEm must be within [0, 0.5].");
    }
    if (!std::isfinite(profile.lineHeight) || profile.lineHeight < 1.0F ||
        profile.lineHeight > 2.5F) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          "lineHeight must be within [1, 2.5].");
    }
    if (profile.lineWidthCh < 35 || profile.lineWidthCh > 100) {
        throw ReaderError(ReaderErrorCode::InvalidProfile,
                          "lineWidthCh must be within [35, 100].");
    }
}

void write_json_atomic(const std::filesystem::path& targetPath,
                       const json& payload) {
    std::error_code ec;
    const auto parent = targetPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            throw ReaderError(ReaderErrorCode::CacheFailed,
                              "Could not create profile directory: " +
                                  ec.message());
        }
    }

    const std::filesystem::path tempPath =
        targetPath.wstring() + L".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            throw ReaderError(ReaderErrorCode::CacheFailed,
                              "Could not open profile temp file for writing.");
        }
        const std::string serialized = payload.dump(2) + "\n";
        out.write(serialized.data(),
                  static_cast<std::streamsize>(serialized.size()));
        out.flush();
        if (!out) {
            throw ReaderError(ReaderErrorCode::CacheFailed,
                              "Failed writing JSON profile contents.");
        }
    }

    if (!MoveFileExW(tempPath.c_str(), targetPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tempPath, ec);
        throw ReaderError(ReaderErrorCode::CacheFailed,
                          "Could not commit JSON profile file.");
    }
}

float read_clamped_float(const json& obj, const char* key, float minVal,
                         float maxVal, float fallback) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number()) {
        return fallback;
    }
    const double val = it->get<double>();
    if (!std::isfinite(val)) {
        return fallback;
    }
    return std::clamp(static_cast<float>(val), minVal, maxVal);
}

}  // namespace

std::filesystem::path JsonReadingProfileStore::default_profile_path() {
    PWSTR appDataRaw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                       &appDataRaw)) &&
        appDataRaw != nullptr) {
        std::filesystem::path base(appDataRaw);
        CoTaskMemFree(appDataRaw);
        return base / L"DyslexiaReader" / L"reading-profile.json";
    }
    if (appDataRaw != nullptr) {
        CoTaskMemFree(appDataRaw);
    }

    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(buffer) / L"DyslexiaReader" /
               L"reading-profile.json";
    }

    return std::filesystem::current_path() / L"DyslexiaReader" /
           L"reading-profile.json";
}

JsonReadingProfileStore::JsonReadingProfileStore()
    : profilePath_(default_profile_path()) {}

JsonReadingProfileStore::JsonReadingProfileStore(
    std::filesystem::path profilePath)
    : profilePath_(std::move(profilePath)) {}

std::filesystem::path JsonReadingProfileStore::ui_state_path() const {
    return profilePath_.parent_path() / L"ui-state.json";
}

std::optional<ReadingProfile> JsonReadingProfileStore::load() {
    std::error_code ec;
    if (!std::filesystem::exists(profilePath_, ec) || ec) {
        return std::nullopt;
    }

    std::ifstream in(profilePath_, std::ios::binary);
    if (!in.is_open()) {
        return std::nullopt;
    }

    json doc;
    try {
        in >> doc;
    } catch (const std::exception&) {
        OutputDebugStringA(
            "Ignored corrupted reading-profile.json; falling back to defaults.\n");
        return std::nullopt;
    }

    if (!doc.is_object()) {
        return std::nullopt;
    }

    ReadingProfile profile;
    if (const auto it = doc.find("schemaVersion");
        it != doc.end() && it->is_number_integer()) {
        const auto ver = it->get<std::int64_t>();
        if (ver != 1) {
            return std::nullopt;
        }
        profile.schemaVersion = 1;
    }

    if (const auto it = doc.find("fontFamily");
        it != doc.end() && it->is_string()) {
        profile.fontFamily = string_to_font_family(it->get<std::string>(),
                                                   profile.fontFamily);
    }
    profile.fontSizePt =
        read_clamped_float(doc, "fontSizePt", 14.0F, 48.0F, profile.fontSizePt);
    profile.letterSpacingEm = read_clamped_float(
        doc, "letterSpacingEm", 0.0F, 0.2F, profile.letterSpacingEm);
    profile.wordSpacingEm = read_clamped_float(
        doc, "wordSpacingEm", 0.0F, 0.5F, profile.wordSpacingEm);
    profile.lineHeight =
        read_clamped_float(doc, "lineHeight", 1.0F, 2.5F, profile.lineHeight);

    if (const auto it = doc.find("lineWidthCh");
        it != doc.end() && it->is_number_integer()) {
        const auto widthCh = it->get<std::int64_t>();
        profile.lineWidthCh = static_cast<std::uint32_t>(
            std::clamp<std::int64_t>(widthCh, 35, 100));
    }

    if (const auto it = doc.find("theme");
        it != doc.end() && it->is_string()) {
        profile.theme = string_to_theme(it->get<std::string>(), profile.theme);
    }

    if (const auto it = doc.find("aids"); it != doc.end() && it->is_object()) {
        const json& aids = *it;
        if (const auto b = aids.find("bionicReading");
            b != aids.end() && b->is_boolean()) {
            profile.aids.bionicReading = b->get<bool>();
        }
        if (const auto r = aids.find("readingRuler");
            r != aids.end() && r->is_boolean()) {
            profile.aids.readingRuler = r->get<bool>();
        }
        if (const auto m = aids.find("bimodalHighlighting");
            m != aids.end() && m->is_boolean()) {
            profile.aids.bimodalHighlighting = m->get<bool>();
        }
    }

    return profile;
}

void JsonReadingProfileStore::save(const ReadingProfile& profile) {
    validate_profile_for_save(profile);

    const json payload = {
        {"schemaVersion", profile.schemaVersion},
        {"fontFamily", font_family_to_string(profile.fontFamily)},
        {"fontSizePt", profile.fontSizePt},
        {"letterSpacingEm", profile.letterSpacingEm},
        {"wordSpacingEm", profile.wordSpacingEm},
        {"lineHeight", profile.lineHeight},
        {"lineWidthCh", profile.lineWidthCh},
        {"theme", theme_to_string(profile.theme)},
        {"aids",
         {
             {"bionicReading", profile.aids.bionicReading},
             {"readingRuler", profile.aids.readingRuler},
             {"bimodalHighlighting", profile.aids.bimodalHighlighting},
         }},
    };

    write_json_atomic(profilePath_, payload);
}

ReaderMode JsonReadingProfileStore::load_reader_mode() const {
    const auto path = ui_state_path();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return ReaderMode::Floating;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return ReaderMode::Floating;
    }

    try {
        json doc;
        in >> doc;
        if (doc.is_object()) {
            if (const auto it = doc.find("mode");
                it != doc.end() && it->is_string()) {
                return string_to_mode(it->get<std::string>(),
                                      ReaderMode::Floating);
            }
        }
    } catch (const std::exception&) {
        OutputDebugStringA("Ignored corrupted ui-state.json.\n");
    }
    return ReaderMode::Floating;
}

void JsonReadingProfileStore::save_reader_mode(ReaderMode mode) const {
    const json payload = {
        {"schemaVersion", 1},
        {"mode", mode_to_string(mode)},
    };
    write_json_atomic(ui_state_path(), payload);
}

}  // namespace dyslexia_reader::windows
