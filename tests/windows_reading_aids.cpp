#include "dyslexia_reader/windows/reading_aids.hpp"

#include <cassert>

int main() {
    using namespace dyslexia_reader;
    using namespace dyslexia_reader::windows;

    BionicReadingAid bionic;
    ReadingDocument document;
    document.text = u"reading focus. to a\u0301bc";
    document.words = {
        {{0, 7}, {}},
        {{8, 6}, {}},
        {{15, 2}, {}},
        {{18, 4}, {}}};
    ReadingProfile profile;
    profile.aids.bionicReading = true;
    TextLayoutSnapshot noLayout;
    ReadingAidContext bionicContext{document, profile, noLayout, {},
                                    std::nullopt};
    const AidVisuals emphasis = bionic.compose(bionicContext);
    assert(emphasis.textStyles.size() == 3);
    assert(emphasis.textStyles[0].text.start == 0);
    assert(emphasis.textStyles[0].text.length == 3);
    assert(emphasis.textStyles[1].text.start == 8);
    assert(emphasis.textStyles[1].text.length == 2);
    assert(emphasis.textStyles[2].text.start == 18);
    assert(emphasis.textStyles[2].text.length == 3);

    ReadingRulerAid ruler;
    profile.aids.readingRuler = true;
    TextLayoutSnapshot layout;
    layout.lines = {
        {{0, 5}, {24, 144, 200, 30}},
        {{6, 5}, {24, 180, 200, 30}},
        {{12, 5}, {24, 216, 200, 30}}};
    ReadingAidContext rulerContext{document, profile, layout, {},
                                   std::uint32_t{1}};
    const AidVisuals rulerVisuals = ruler.compose(rulerContext);
    assert(rulerVisuals.overlays.size() == 3);
    assert(rulerVisuals.overlays[0].opacity == 0.25F);
    assert(rulerVisuals.overlays[1].layer == OverlayLayer::UnderText);
    assert(rulerVisuals.overlays[1].rgba != 0);
    assert(rulerVisuals.overlays[2].opacity == 0.25F);

    rulerContext.activeVisualLine.reset();
    assert(ruler.compose(rulerContext).overlays.empty());

    BimodalHighlightingAid bimodal;
    profile.aids.bimodalHighlighting = true;
    ReadingDocument highlightedDocument;
    highlightedDocument.text = u"A sentence with words.";
    TextLayoutSnapshot highlightLayout;
    highlightLayout.sentences.push_back(
        {{0, 22}, {{10, 20, 120, 26}}});
    highlightLayout.words.push_back({{16, 5}, {{96, 20, 34, 26}}});
    HighlightState highlights;
    highlights.sentence = TextRange{0, 22};
    highlights.word = TextRange{16, 5};
    highlights.wordTimingReliable = true;
    highlights.sentenceTimingReliable = true;
    ReadingAidContext bimodalContext{highlightedDocument, profile,
                                     highlightLayout, highlights,
                                     std::nullopt};
    const AidVisuals bimodalVisuals = bimodal.compose(bimodalContext);
    assert(bimodalVisuals.overlays.size() == 2);
    assert(bimodalVisuals.overlays[0].kind ==
           OverlayKind::SentenceHighlight);
    assert(bimodalVisuals.overlays[0].layer == OverlayLayer::UnderText);
    assert(bimodalVisuals.overlays[0].rgba == 0xDCEBFFFF);
    assert(bimodalVisuals.overlays[0].opacity == 0.35F);
    assert(bimodalVisuals.overlays[0].bounds.x == 8);
    assert(bimodalVisuals.overlays[1].kind == OverlayKind::WordHighlight);
    assert(bimodalVisuals.overlays[1].layer == OverlayLayer::OverText);
    assert(bimodalVisuals.overlays[1].rgba == 0x155EEFFF);
    assert(bimodalVisuals.overlays[1].bounds.x == 94);

    highlights.wordTimingReliable = false;
    bimodalContext.highlights = highlights;
    assert(bimodal.compose(bimodalContext).overlays.size() == 1);
    profile.aids.bimodalHighlighting = false;
    assert(bimodal.compose(bimodalContext).overlays.empty());
}
