#include "dyslexia_reader/windows/profile_store.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "CHECK failed: " #cond " at " << __FILE__ << ":"   \
                      << __LINE__ << std::endl;                              \
            std::exit(1);                                                    \
        }                                                                    \
    } while (false)

namespace {

struct TempDirGuard {
    std::filesystem::path dir;
    ~TempDirGuard() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

}  // namespace

int main() {
    using namespace dyslexia_reader;
    using namespace dyslexia_reader::windows;
    namespace fs = std::filesystem;

    const fs::path baseDir =
        fs::temp_directory_path() / "dyslexia_reader_profile_store_smoke";
    TempDirGuard guard{baseDir};
    std::error_code ec;
    fs::remove_all(baseDir, ec);

    const fs::path profilePath = baseDir / "reading-profile.json";
    JsonReadingProfileStore store(profilePath);

    // Missing file returns std::nullopt.
    CHECK(!store.load().has_value());
    CHECK(store.load_reader_mode() == ReaderMode::Floating);

    // Save non-default profile and verify exact round-trip.
    ReadingProfile saved;
    saved.fontFamily = FontFamily::OpenDyslexic;
    saved.fontSizePt = 28.0F;
    saved.letterSpacingEm = 0.08F;
    saved.wordSpacingEm = 0.25F;
    saved.lineHeight = 1.9F;
    saved.lineWidthCh = 52;
    saved.theme = ThemeId::DarkHighContrast;
    saved.aids.bionicReading = true;
    saved.aids.readingRuler = true;
    saved.aids.bimodalHighlighting = false;

    store.save(saved);
    store.save_reader_mode(ReaderMode::Focus);

    const auto loadedOpt = store.load();
    CHECK(loadedOpt.has_value());
    const ReadingProfile& loaded = *loadedOpt;
    CHECK(loaded.schemaVersion == 1);
    CHECK(loaded.fontFamily == FontFamily::OpenDyslexic);
    CHECK(loaded.fontSizePt == 28.0F);
    CHECK(loaded.letterSpacingEm > 0.079F && loaded.letterSpacingEm < 0.081F);
    CHECK(loaded.wordSpacingEm > 0.249F && loaded.wordSpacingEm < 0.251F);
    CHECK(loaded.lineHeight > 1.899F && loaded.lineHeight < 1.901F);
    CHECK(loaded.lineWidthCh == 52);
    CHECK(loaded.theme == ThemeId::DarkHighContrast);
    CHECK(loaded.aids.bionicReading);
    CHECK(loaded.aids.readingRuler);
    CHECK(!loaded.aids.bimodalHighlighting);
    CHECK(store.load_reader_mode() == ReaderMode::Focus);

    // Invalid profile throws ReaderError::InvalidProfile.
    ReadingProfile bad = saved;
    bad.fontSizePt = 99.0F;
    bool threw = false;
    try {
        store.save(bad);
    } catch (const ReaderError& err) {
        threw = (err.code() == ReaderErrorCode::InvalidProfile);
    }
    CHECK(threw);

    // Corrupted JSON recovers cleanly as std::nullopt.
    {
        std::ofstream corrupt(profilePath, std::ios::binary | std::ios::trunc);
        corrupt << "{ not valid json";
    }
    CHECK(!store.load().has_value());

    // Default path points inside DyslexiaReader/reading-profile.json.
    const fs::path defPath = JsonReadingProfileStore::default_profile_path();
    CHECK(defPath.filename() == L"reading-profile.json");
    CHECK(defPath.parent_path().filename() == L"DyslexiaReader");

    return 0;
}
