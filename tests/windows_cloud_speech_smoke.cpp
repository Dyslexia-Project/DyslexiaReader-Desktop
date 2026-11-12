#include "dyslexia_reader/caching_cloud_speech_client.hpp"
#include "dyslexia_reader/windows/cloud_speech_client.hpp"

#include <cstdlib>
#include <filesystem>
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

    // 1. Endpoint validation: HTTPS & localhost HTTP allowed; plain remote HTTP rejected.
    {
        WindowsCloudSpeechClient httpsClient(L"https://speech.dyslexia.example.com");
        WindowsCloudSpeechClient localhostClient(L"http://localhost:8080");
        WindowsCloudSpeechClient loopbackIpClient(L"http://127.0.0.1:8080");
        (void)httpsClient;
        (void)localhostClient;
        (void)loopbackIpClient;

        bool rejectedPlainHttp = false;
        try {
            WindowsCloudSpeechClient insecure(L"http://speech.dyslexia.example.com");
        } catch (const ReaderError& err) {
            rejectedPlainHttp = (err.code() == ReaderErrorCode::NetworkFailed);
        }
        CHECK(rejectedPlainHttp);
    }

    // 2. Request JSON matches Platform SynthesizeRequest contract (speed + language + selection slice).
    {
        CloudSpeechRequest req;
        req.text = u"Hello world!";
        req.selection = TextRange{6, 5};
        req.voiceId = "en-GB-Neural2-A";
        req.rate = 1.25F;

        const std::string json =
            WindowsCloudSpeechClient::build_synthesize_request_json(req);
        CHECK(json.find("\"text\":\"world\"") != std::string::npos);
        CHECK(json.find("\"voiceId\":\"en-GB-Neural2-A\"") != std::string::npos);
        CHECK(json.find("\"speed\":1.25") != std::string::npos);
        CHECK(json.find("\"language\":\"en-GB\"") != std::string::npos);

        req.voiceId = "ro-RO-Wavenet-A";
        const std::string roJson =
            WindowsCloudSpeechClient::build_synthesize_request_json(req);
        CHECK(roJson.find("\"language\":\"ro-RO\"") != std::string::npos);
    }

    // 3. Parse Platform speech-synthesis.schema.json response with start/end & timingGranularity.
    {
        const std::string platformResponse = R"({
            "schemaVersion": 1,
            "provider": "cloud",
            "voiceId": "en-US-Neural2-F",
            "language": "en-US",
            "speed": 1.0,
            "cacheHit": false,
            "characterCount": 13,
            "audioMimeType": "audio/mpeg",
            "audioBase64": "AQIDBA==",
            "timingGranularity": "WORD_AND_SENTENCE",
            "speechMarks": [
                { "time": 0, "duration": 900, "type": "sentence", "start": 0, "end": 13, "value": "Hello reader." },
                { "time": 0, "duration": 400, "type": "word", "start": 0, "end": 5, "value": "Hello" },
                { "time": 420, "duration": 480, "type": "word", "start": 6, "end": 13, "value": "reader." }
            ]
        })";

        const CloudSpeechResult parsed =
            WindowsCloudSpeechClient::parse_synthesize_response_json(
                platformResponse, 13, 10);
        CHECK(parsed.wordTimingReliable);
        CHECK(parsed.sentenceTimingReliable);
        CHECK(parsed.chunks.size() == 1);
        CHECK(parsed.chunks[0].mediaType == "audio/mpeg");
        CHECK(parsed.chunks[0].bytes.size() == 4);
        CHECK(parsed.chunks[0].bytes[0] == 1 && parsed.chunks[0].bytes[3] == 4);
        CHECK(parsed.marks.size() == 3);
        CHECK(parsed.marks[0].kind == SpeechMarkKind::Sentence);
        CHECK(parsed.marks[0].text.start == 10);
        CHECK(parsed.marks[0].text.length == 13);
        CHECK(parsed.marks[2].kind == SpeechMarkKind::Word);
        CHECK(parsed.marks[2].text.start == 16);
        CHECK(parsed.marks[2].text.length == 7);
        CHECK(parsed.marks[2].timeMilliseconds == 420);

        const std::string sentenceOnly = R"({
            "audioMimeType": "audio/mpeg",
            "audioBase64": "AQID",
            "timingGranularity": "SENTENCE_ONLY",
            "speechMarks": [
                { "time": 50, "type": "sentence", "start": 0, "end": 5 }
            ]
        })";
        const CloudSpeechResult sentenceParsed =
            WindowsCloudSpeechClient::parse_synthesize_response_json(
                sentenceOnly, 5, 0);
        CHECK(!sentenceParsed.wordTimingReliable);
        CHECK(sentenceParsed.sentenceTimingReliable);
        CHECK(sentenceParsed.marks.size() == 1);
        CHECK(sentenceParsed.marks[0].timeMilliseconds == 50);
    }

    // 4. Parse legacy cloud-speech-v1 response format too (backward compatibility).
    {
        const std::string legacyResponse = R"({
            "audioChunks": [{ "mediaType": "audio/wav", "dataBase64": "BQYH" }],
            "speechMarks": [{ "type": "word", "start": 0, "length": 4, "timeMilliseconds": 25 }],
            "wordTimingReliable": true,
            "sentenceTimingReliable": false
        })";
        const CloudSpeechResult legacy =
            WindowsCloudSpeechClient::parse_synthesize_response_json(
                legacyResponse, 4, 0);
        CHECK(legacy.wordTimingReliable);
        CHECK(!legacy.sentenceTimingReliable);
        CHECK(legacy.chunks.size() == 1 && legacy.chunks[0].mediaType == "audio/wav");
        CHECK(legacy.chunks[0].bytes.size() == 3);
        CHECK(legacy.marks.size() == 1 && legacy.marks[0].text.length == 4);

        bool caughtOutOfRange = false;
        try {
            const std::string badRange = R"({
                "audioMimeType": "audio/mpeg",
                "audioBase64": "AQID",
                "timingGranularity": "WORD_AND_SENTENCE",
                "speechMarks": [{ "time": 0, "type": "word", "start": 2, "end": 2 }]
            })";
            WindowsCloudSpeechClient::parse_synthesize_response_json(
                badRange, 4, 0);
        } catch (const ReaderError& err) {
            caughtOutOfRange = (err.code() == ReaderErrorCode::NetworkFailed);
        }
        CHECK(caughtOutOfRange);
    }

    // 5. WindowsAudioCache persists & loads results using SHA-256 hashed filenames.
    {
        const fs::path cacheDir =
            fs::temp_directory_path() / "dyslexia_reader_audio_cache_smoke";
        TempDirGuard guard{cacheDir};
        std::error_code ec;
        fs::remove_all(cacheDir, ec);

        WindowsAudioCache cache(cacheDir);
        const std::string secretTextKey = "SensitiveUserSentence|voice|1.0";
        CHECK(!cache.find(secretTextKey).has_value());

        CloudSpeechResult stored;
        stored.wordTimingReliable = true;
        stored.sentenceTimingReliable = true;
        stored.chunks.push_back({"audio/mpeg", {10, 20, 30, 40}});
        stored.marks.push_back({SpeechMarkKind::Word, {0, 9}, 125});

        cache.store(secretTextKey, stored);

        const auto loaded = cache.find(secretTextKey);
        CHECK(loaded.has_value());
        CHECK(loaded->wordTimingReliable);
        CHECK(loaded->chunks.size() == 1);
        CHECK(loaded->chunks[0].bytes == stored.chunks[0].bytes);
        CHECK(loaded->marks.size() == 1);
        CHECK(loaded->marks[0].timeMilliseconds == 125);

        std::size_t binFiles = 0;
        for (const auto& entry : fs::directory_iterator(cacheDir)) {
            const std::string name = entry.path().filename().string();
            CHECK(name.find("SensitiveUserSentence") == std::string::npos);
            if (entry.path().extension() == ".bin") {
                CHECK(name.size() == 68);  // 64 hex chars + ".bin"
                ++binFiles;
            }
        }
        CHECK(binFiles == 1);
    }

    return 0;
}
