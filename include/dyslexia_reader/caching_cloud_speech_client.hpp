#pragma once

#include "dyslexia_reader/interfaces.hpp"

#include <string>

namespace dyslexia_reader {

class CachingCloudSpeechClient final : public ICloudSpeechClient {
public:
    CachingCloudSpeechClient(ICloudSpeechClient& client, IAudioCache& cache,
                             std::string cacheNamespace);

    CloudSpeechResult synthesize(const CloudSpeechRequest& request) override;

private:
    ICloudSpeechClient& client_;
    IAudioCache& cache_;
    std::string cacheNamespace_;
};

}  // namespace dyslexia_reader
