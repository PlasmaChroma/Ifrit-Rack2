#pragma once

#include <vector>
#include <cstdint>
#include "PluginHostBackend.hpp"

namespace ifrit {

struct StereoSample {
    float left = 0.0f;
    float right = 0.0f;
};

class AudioBlockBridge {
public:
    AudioBlockBridge();
    ~AudioBlockBridge() = default;

    void configure(uint32_t blockSize, uint32_t inputChannels, uint32_t outputChannels);
    void reset();

    // Push a sample from the Rack engine (sample-by-sample)
    void pushInputSample(float left, float right);
    
    // Pop a processed sample to send to the Rack output
    StereoSample popOutputSample();

    // Check if we have gathered enough samples to run a block process
    bool blockReady() const { return ready; }

    uint32_t getWriteIndex() const { return writeIndex; }

    // Prepare the block structure to pass to the backend
    PluginProcessBlock makeProcessBlock();
    
    // Reset the ready flag and prepare for the next block
    void commitProcessedBlock();

private:
    uint32_t blockSize;
    uint32_t numInputChannels;
    uint32_t numOutputChannels;

    std::vector<float> inputL;
    std::vector<float> inputR;
    std::vector<float> outputL;
    std::vector<float> outputR;

    float* inputChannelPtrs[2];
    float* outputChannelPtrs[2];

    uint32_t writeIndex;
    uint32_t readIndex;
    bool ready;
};

} // namespace ifrit
