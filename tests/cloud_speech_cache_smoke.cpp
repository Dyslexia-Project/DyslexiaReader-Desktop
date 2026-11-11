#include "dyslexia_reader/caching_cloud_speech_client.hpp"

#include <cassert>
#include <map>
#include <string>

namespace {

class FakeCloudClient final : public dyslexia_reader::ICloudSpeechClient {
public:
    dyslexia_reader::CloudSpeechResult synthesize(
        const dyslexia_reader::CloudSpeechRequest& request) override {
        ++calls;
        lastRequest = request;
        dyslexia_reader::CloudSpeechResult result;
        result.chunks.push_back({"audio/test", {1, 2, 3}});
        result.marks.push_back(
            {dyslexia_reader::SpeechMarkKind::Word, {0, 4}, 12});
        result.wordTimingReliable = true;
        return result;
    }

    std::uint32_t calls = 0;
    dyslexia_reader::CloudSpeechRequest lastRequest;
};

class MemoryCache final : public dyslexia_reader::IAudioCache {
public:
    std::optional<dyslexia_reader::CloudSpeechResult> find(
        const std::string& key) override {
        ++findCalls;
        const auto found = values.find(key);
        if (found == values.end()) {
            return std::nullopt;
        }
        return found->second;
    }

    void store(const std::string& key,
               const dyslexia_reader::CloudSpeechResult& result) override {
        ++storeCalls;
        if (failStore) {
            throw dyslexia_reader::ReaderError(
                dyslexia_reader::ReaderErrorCode::CacheFailed,
                "simulated cache write failure");
        }
        values.insert_or_assign(key, result);
    }

    std::uint32_t findCalls = 0;
    std::uint32_t storeCalls = 0;
    bool failStore = false;
    std::map<std::string, dyslexia_reader::CloudSpeechResult> values;
};

}  // namespace

int main() {
    using namespace dyslexia_reader;

    FakeCloudClient upstream;
    MemoryCache cache;
    CachingCloudSpeechClient client(upstream, cache, "https://speech.test");

    CloudSpeechRequest request;
    request.text = u"Read this sentence.";
    request.voiceId = "en-US-test";
    request.rate = 1.0F;

    const auto first = client.synthesize(request);
    assert(upstream.calls == 1);
    assert(cache.findCalls == 1 && cache.storeCalls == 1);
    assert(first.chunks.size() == 1 && first.chunks[0].bytes.size() == 3);
    assert(first.marks.size() == 1 && first.wordTimingReliable);

    const auto second = client.synthesize(request);
    assert(upstream.calls == 1);
    assert(cache.findCalls == 2 && cache.storeCalls == 1);
    assert(second.chunks[0].bytes == first.chunks[0].bytes);

    auto differentRate = request;
    differentRate.rate = 1.25F;
    client.synthesize(differentRate);
    assert(upstream.calls == 2);

    auto differentSelection = request;
    differentSelection.selection = TextRange{5, 4};
    client.synthesize(differentSelection);
    assert(upstream.calls == 3);
    assert(upstream.lastRequest.selection.has_value());
    assert(upstream.lastRequest.selection->start == 5);
    assert(upstream.lastRequest.selection->length == 4);

    auto differentVoice = request;
    differentVoice.voiceId = "en-GB-test";
    client.synthesize(differentVoice);
    assert(upstream.calls == 4);

    auto otherService = CachingCloudSpeechClient(
        upstream, cache, "https://another-speech.test");
    otherService.synthesize(request);
    assert(upstream.calls == 5);

    auto invalidRate = request;
    invalidRate.rate = 2.1F;
    bool rejectedInvalidRate = false;
    try {
        client.synthesize(invalidRate);
    } catch (const ReaderError& error) {
        rejectedInvalidRate =
            error.code() == ReaderErrorCode::SpeechUnavailable;
    }
    assert(rejectedInvalidRate && upstream.calls == 5);

    cache.failStore = true;
    auto uncachedRequest = request;
    uncachedRequest.text = u"A new uncached request.";
    bool surfacedStoreFailure = false;
    try {
        client.synthesize(uncachedRequest);
    } catch (const ReaderError& error) {
        surfacedStoreFailure = error.code() == ReaderErrorCode::CacheFailed;
    }
    assert(surfacedStoreFailure && upstream.calls == 6);

    return 0;
}
