#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "PluginDescriptor.hpp"

namespace ifrit {

enum class PluginLoadResult {
    Ok,
    FileNotFound,
    InvalidClassId,
    InstantiationFailed,
    InitializationFailed,
    UnsupportedLayout
};

enum class EditorOpenResult {
    Ok,
    NoEditor,
    PlatformError,
    AlreadyOpen
};

struct PluginParameterInfo {
    uint32_t id = 0;
    std::string title;
    std::string shortTitle;
    std::string units;

    int32_t stepCount = 0;
    double defaultNormalized = 0.0;
    uint32_t flags = 0;
    int32_t unitId = 0;

    bool canAutomate = true;
    bool isReadOnly = false;
    bool isBypass = false;
    bool isProgramChange = false;
};

struct PluginStateBlob {
    std::vector<uint8_t> componentState;
    std::vector<uint8_t> controllerState;
};

enum class EventType {
    NoteOn,
    NoteOff,
    PolyPressure
};

struct PluginEvent {
    EventType type;
    int32_t sampleOffset;
    int16_t note;
    int32_t noteId;
    float value; // velocity or pressure [0.0, 1.0]
};

struct PluginProcessBlock {
    // Array of channel buffers: inputs[busIndex][channelIndex][sampleIndex]
    // For MVP we only have 1 main input/output bus, so it can be float** inputBuffers
    float** inputBuffers = nullptr;
    float** outputBuffers = nullptr;
    uint32_t numSamples = 0;
    uint32_t numInputChannels = 0;
    uint32_t numOutputChannels = 0;

    // MIDI-style note events
    std::vector<PluginEvent> inputEvents;
};

struct PluginRuntimeInfo {
    std::string name;
    std::string vendor;
    double sampleRate = 44100.0;
    uint32_t blockSizing = 128;
    uint32_t latencySamples = 0;
};

class PluginHostBackend {
public:
    virtual ~PluginHostBackend() = default;

    virtual PluginLoadResult load(const PluginDescriptor& descriptor) = 0;
    virtual void unload() = 0;

    virtual bool isLoaded() const = 0;
    virtual PluginRuntimeInfo runtimeInfo() const = 0;

    virtual bool configure(double sampleRate, uint32_t maxBlockSize) = 0;
    virtual bool process(PluginProcessBlock& block) = 0;

    virtual std::vector<PluginParameterInfo> parameters() const = 0;
    virtual bool setParameterNormalized(
        uint32_t stableParameterId,
        double normalizedValue,
        int32_t sampleOffset
    ) = 0;

    virtual PluginStateBlob captureState() = 0;
    virtual bool restoreState(const PluginStateBlob& state) = 0;

    virtual bool hasNativeEditor() const = 0;
    virtual EditorOpenResult openEditor(void* parentWindowHandle, int x, int y, int width, int height) = 0;
    virtual void closeEditor() = 0;
    virtual bool isEditorVisible() const = 0;
};

} // namespace ifrit
