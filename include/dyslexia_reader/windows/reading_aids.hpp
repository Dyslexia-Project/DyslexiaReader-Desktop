#pragma once

#include "dyslexia_reader/interfaces.hpp"

namespace dyslexia_reader::windows {

class BionicReadingAid final : public IReadingAid {
public:
    std::string id() const override;
    AidVisuals compose(const ReadingAidContext& context) const override;
};

class ReadingRulerAid final : public IReadingAid {
public:
    std::string id() const override;
    AidVisuals compose(const ReadingAidContext& context) const override;
};

class BimodalHighlightingAid final : public IReadingAid {
public:
    std::string id() const override;
    AidVisuals compose(const ReadingAidContext& context) const override;
};

}  // namespace dyslexia_reader::windows
