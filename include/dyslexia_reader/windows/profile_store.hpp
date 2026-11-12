#pragma once

#include "dyslexia_reader/interfaces.hpp"

#include <filesystem>
#include <optional>

namespace dyslexia_reader::windows {

class JsonReadingProfileStore final : public IReadingProfileStore {
public:
    JsonReadingProfileStore();
    explicit JsonReadingProfileStore(std::filesystem::path profilePath);

    std::optional<ReadingProfile> load() override;
    void save(const ReadingProfile& profile) override;

    ReaderMode load_reader_mode() const;
    void save_reader_mode(ReaderMode mode) const;

    const std::filesystem::path& profile_path() const noexcept {
        return profilePath_;
    }

    static std::filesystem::path default_profile_path();

private:
    std::filesystem::path ui_state_path() const;

    std::filesystem::path profilePath_;
};

}  // namespace dyslexia_reader::windows
