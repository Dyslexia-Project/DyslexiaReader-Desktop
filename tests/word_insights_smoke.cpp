#include "dyslexia_reader/windows/word_insights.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "CHECK failed: " #cond " at " << __FILE__ << ":"   \
                      << __LINE__ << std::endl;                              \
            std::exit(1);                                                    \
        }                                                                    \
    } while (false)

int main() {
    using namespace dyslexia_reader;
    using namespace dyslexia_reader::windows;

    // 1. Document -> Word Cloud extraction (stop-words, punctuation, frequency ranking).
    {
        ReadingDocument doc;
        doc.text =
            u"The focus mode helps dyslexia readers. Focus and clarity "
            u"matter because dyslexia readers need clarity, clarity!";
        // Tokenize simple words manually matching WordToken offsets.
        std::size_t i = 0;
        while (i < doc.text.size()) {
            while (i < doc.text.size() && doc.text[i] == u' ') {
                ++i;
            }
            const std::size_t start = i;
            while (i < doc.text.size() && doc.text[i] != u' ') {
                ++i;
            }
            if (i > start) {
                WordSpan tok;
                tok.text = TextRange{static_cast<TextOffset>(start),
                                     static_cast<TextOffset>(i - start)};
                doc.words.push_back(tok);
            }
        }

        const auto cloud = build_word_cloud(doc, 10);
        CHECK(!cloud.empty());
        CHECK(cloud[0].normalized == "clarity");
        CHECK(cloud[0].count == 3);
        CHECK(cloud[0].weight == 1.0F);
        // "the", "and" stop words must be absent
        for (const auto& item : cloud) {
            CHECK(item.normalized != "the");
            CHECK(item.normalized != "and");
        }
    }

    // 2. Word normalization.
    {
        CHECK(FastWordLookupClient::normalize_lookup_word("  \"Clarity!\" ") ==
              "clarity");
        CHECK(FastWordLookupClient::normalize_lookup_word("...").empty());
    }

    // 3. Dictionary API JSON parsing + synonym deduplication.
    {
        const std::string dictJson = R"([{
            "word": "clarity",
            "phonetic": "/ˈklær.ə.ti/",
            "meanings": [{
                "partOfSpeech": "noun",
                "synonyms": ["lucidity", "clearness", "Clarity"],
                "definitions": [{
                    "definition": "The quality of being coherent and intelligible.",
                    "example": "She wrote with remarkable clarity.",
                    "synonyms": ["precision", "clearness"]
                }]
            }]
        }])";

        WordLookupResult res;
        res.word = "clarity";
        FastWordLookupClient::parse_dictionary_api_json(dictJson, res);
        CHECK(res.phonetic == "/ˈklær.ə.ti/");
        CHECK(res.definitions.size() == 1);
        CHECK(res.definitions[0].partOfSpeech == "noun");
        CHECK(res.definitions[0].definition.find("coherent") !=
              std::string::npos);
        CHECK(res.synonyms.size() == 3);  // lucidity, clearness, precision
    }

    // 4. Datamuse synonyms JSON parsing.
    {
        const std::string datamuseJson = R"([
            {"word": "brilliance", "score": 900},
            {"word": "simplicity", "score": 850},
            {"word": "brilliance", "score": 800}
        ])";
        const auto syns =
            FastWordLookupClient::parse_datamuse_synonyms_json(datamuseJson);
        CHECK(syns.size() == 2);
        CHECK(syns[0] == "brilliance");
        CHECK(syns[1] == "simplicity");
    }

    std::cout << "word_insights_smoke: ALL CHECKS PASSED\n";
    return 0;
}
