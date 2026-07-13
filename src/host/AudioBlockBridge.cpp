#include "AudioBlockBridge.hpp"
#include <algorithm>

namespace ifrit {

AudioBlockBridge::AudioBlockBridge() {
    blockSize = 128;
    numInputChannels = 2;
    numOutputChannels = 2;
    writeIndex = 0;
    readIndex = 0;
    ready = false;

    inputChannelPtrs[0] = nullptr;
    inputChannelPtrs[1] = nullptr;
    outputChannelPtrs[0] = nullptr;
    outputChannelPtrs[1] = nullptr;

    configure(blockSize, numInputChannels, numOutputChannels);
}

void AudioBlockBridge::configure(uint32_t size, uint32_t inputs, uint32_t outputs) {
    blockSize = size;
    numInputChannels = inputs;
    numOutputChannels = outputs;

    inputL.assign(blockSize, 0.0f);
    inputR.assign(blockSize, 0.0f);
    outputL.assign(blockSize, 0.0f);
    outputR.assign(blockSize, 0.0f);

    inputChannelPtrs[0] = inputL.data();
    inputChannelPtrs[1] = inputR.data();
    outputChannelPtrs[0] = outputL.data();
    outputChannelPtrs[1] = outputR.data();

    reset();
}

void AudioBlockBridge::reset() {
    std::fill(inputL.begin(), inputL.end(), 0.0f);
    std::fill(inputR.begin(), inputR.end(), 0.0f);
    std::fill(outputL.begin(), outputL.end(), 0.0f);
    std::fill(outputR.begin(), outputR.end(), 0.0f);

    writeIndex = 0;
    readIndex = 0;
    ready = false;
}

void AudioBlockBridge::pushInputSample(float left, float right) {
    if (ready) return; // Wait until the current block is processed and committed

    inputL[writeIndex] = left;
    inputR[writeIndex] = right;

    writeIndex++;
    if (writeIndex >= blockSize) {
        ready = true;
    }
}

StereoSample AudioBlockBridge::popOutputSample() {
    StereoSample sample;
    if (readIndex < blockSize) {
        sample.left = outputL[readIndex];
        sample.right = outputR[readIndex];
        readIndex++;
    }
    return sample;
}

PluginProcessBlock AudioBlockBridge::makeProcessBlock() {
    PluginProcessBlock block;
    block.inputBuffers = inputChannelPtrs;
    block.outputBuffers = outputChannelPtrs;
    block.numSamples = blockSize;
    block.numInputChannels = numInputChannels;
    block.numOutputChannels = numOutputChannels;
    return block;
}

void AudioBlockBridge::commitProcessedBlock() {
    writeIndex = 0;
    readIndex = 0;
    ready = false;
}

} // namespace ifrit
