#pragma once

#include <atomic>
#include <thread>
#include <mutex>
#include <X11/Xlib.h>
#include "host/PluginHostBackend.hpp"
#include "host/vst3/Vst3ComponentHandler.hpp"
#include "host/vst3/Vst3ParameterChanges.hpp"
#include "host/vst3/Vst3EventList.hpp"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"

namespace ifrit {

class Vst3Backend final : public PluginHostBackend {
public:
    Vst3Backend();
    virtual ~Vst3Backend();

    // PluginHostBackend
    virtual PluginLoadResult load(const PluginDescriptor& descriptor) override;
    virtual void unload() override;

    virtual bool isLoaded() const override { return loaded; }
    virtual PluginRuntimeInfo runtimeInfo() const override;

    virtual bool configure(double sampleRate, uint32_t maxBlockSize) override;
    virtual bool process(PluginProcessBlock& block) override;

    virtual std::vector<PluginParameterInfo> parameters() const override;
    virtual bool setParameterNormalized(
        uint32_t stableParameterId,
        double normalizedValue,
        int32_t sampleOffset
    ) override;

    virtual PluginStateBlob captureState() override;
    virtual bool restoreState(const PluginStateBlob& state) override;

    virtual bool hasNativeEditor() const override;
    virtual EditorOpenResult openEditor(void* parentWindowHandle, int x, int y, int width, int height) override;
    virtual void closeEditor() override;
    virtual bool isEditorVisible() const override { return editorOpen; }

    // UI event loop polling (called on Rack main thread)
    void stepUI();

    using ParameterEditCallback = std::function<void(uint32_t paramId, double valueNormalized)>;
    void setParameterEditCallbacks(ParameterEditCallback onBegin, ParameterEditCallback onPerform, ParameterEditCallback onEnd);

private:
    void clearPointers();

    std::atomic<bool> loaded;
    std::string modulePath;
    std::string classIdStr;
    std::string pluginName;
    std::string pluginVendor;

    void* moduleHandle;
    Steinberg::IPluginFactory* factory;
    Steinberg::Vst::IComponent* component;
    Steinberg::Vst::IAudioProcessor* processor;
    Steinberg::Vst::IEditController* controller;
    Vst3ComponentHandler* componentHandler;
    Steinberg::IPlugView* view;

    double currentSampleRate;
    uint32_t currentBlockSize;

    // Parameter changes queue from host to plugin
    Vst3ParameterChanges* paramChanges;
    Vst3EventList* eventList;

    // Mutex for parameter change thread safety
    mutable std::mutex paramMutex;

    // Native X11 Editor window details
    ::Display* xDisplay;
    ::Window xWindow;
    std::atomic<bool> editorOpen;
};

} // namespace ifrit
