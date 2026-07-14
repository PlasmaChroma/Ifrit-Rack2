#include "Vst3Backend.hpp"
#include "Vst3StateStream.hpp"
#if defined(_WIN32)
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/gui/iplugview.h"

namespace ifrit {

// Helper to parse hex string into TUID
static bool stringToTUID(const std::string& str, Steinberg::TUID tuid) {
    if (str.length() != 32) return false;
    for (int i = 0; i < 16; ++i) {
        std::string byteStr = str.substr(i * 2, 2);
        tuid[i] = (uint8_t)std::strtol(byteStr.c_str(), nullptr, 16);
    }
    return true;
}

Vst3Backend::Vst3Backend() {
    clearPointers();
    loaded = false;
    currentSampleRate = 44100.0;
    currentBlockSize = 128;
    
    paramChanges = new Vst3ParameterChanges();
    eventList = new Vst3EventList();

#if !defined(_WIN32)
    xDisplay = nullptr;
#endif
    xWindow = 0;
    editorOpen = false;
}

Vst3Backend::~Vst3Backend() {
    unload();
    if (paramChanges) paramChanges->release();
    if (eventList) eventList->release();
}

void Vst3Backend::clearPointers() {
    moduleHandle = nullptr;
    factory = nullptr;
    component = nullptr;
    processor = nullptr;
    controller = nullptr;
    componentHandler = nullptr;
    view = nullptr;
}

PluginLoadResult Vst3Backend::load(const PluginDescriptor& descriptor) {
    unload();

    modulePath = descriptor.modulePath;
    classIdStr = descriptor.classId;

    // 1. Dynamic Load
#if defined(_WIN32)
    moduleHandle = LoadLibraryA(modulePath.c_str());
    if (!moduleHandle) {
        std::cerr << "[Vst3Backend] Failed to LoadLibrary: " << GetLastError() << std::endl;
        return PluginLoadResult::FileNotFound;
    }
#else
    moduleHandle = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!moduleHandle) {
        std::cerr << "[Vst3Backend] Failed to dlopen: " << dlerror() << std::endl;
        return PluginLoadResult::FileNotFound;
    }
#endif

    // 2. Get Factory Function
    using GetFactoryFunc = Steinberg::IPluginFactory* (PLUGIN_API*)();
#if defined(_WIN32)
    // GetProcAddress necessarily returns FARPROC. The VST3 SDK declares this
    // exported function with PLUGIN_API, which preserves the Windows ABI.
    #if defined(__GNUC__)
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wcast-function-type"
    #endif
    GetFactoryFunc getFactory = reinterpret_cast<GetFactoryFunc>(GetProcAddress(moduleHandle, "GetPluginFactory"));
    #if defined(__GNUC__)
        #pragma GCC diagnostic pop
    #endif
#else
    GetFactoryFunc getFactory = reinterpret_cast<GetFactoryFunc>(dlsym(moduleHandle, "GetPluginFactory"));
#endif
    if (!getFactory) {
        std::cerr << "[Vst3Backend] GetPluginFactory symbol not found." << std::endl;
        unload();
        return PluginLoadResult::InitializationFailed;
    }

    factory = getFactory();
    if (!factory) {
        unload();
        return PluginLoadResult::InitializationFailed;
    }
    factory->addRef();

    // 3. Resolve Class ID
    Steinberg::TUID cid;
    if (!stringToTUID(classIdStr, cid)) {
        unload();
        return PluginLoadResult::InvalidClassId;
    }

    // 4. Instantiate Component
    if (factory->createInstance(cid, Steinberg::Vst::IComponent::iid, (void**)&component) != Steinberg::kResultOk) {
        unload();
        return PluginLoadResult::InstantiationFailed;
    }

    if (component->initialize(nullptr) != Steinberg::kResultOk) {
        unload();
        return PluginLoadResult::InitializationFailed;
    }

    // 5. Query Audio Processor
    if (component->queryInterface(Steinberg::Vst::IAudioProcessor::iid, (void**)&processor) != Steinberg::kResultOk) {
        unload();
        return PluginLoadResult::InstantiationFailed;
    }

    // Get basic descriptor info
    pluginName = descriptor.name;
    pluginVendor = descriptor.vendor;

    // 6. Create Associated Edit Controller
    Steinberg::TUID controllerCID;
    if (component->getControllerClassId(controllerCID) == Steinberg::kResultOk) {
        if (factory->createInstance(controllerCID, Steinberg::Vst::IEditController::iid, (void**)&controller) == Steinberg::kResultOk) {
            if (controller->initialize(nullptr) == Steinberg::kResultOk) {
                // Establish connection between component and controller
                Steinberg::Vst::IConnectionPoint* componentCP = nullptr;
                Steinberg::Vst::IConnectionPoint* controllerCP = nullptr;
                
                component->queryInterface(Steinberg::Vst::IConnectionPoint::iid, (void**)&componentCP);
                controller->queryInterface(Steinberg::Vst::IConnectionPoint::iid, (void**)&controllerCP);
                
                if (componentCP && controllerCP) {
                    componentCP->connect(controllerCP);
                    controllerCP->connect(componentCP);
                }
                
                if (componentCP) componentCP->release();
                if (controllerCP) controllerCP->release();

                // Setup Component Handler for parameter changes
                componentHandler = new Vst3ComponentHandler();
                controller->setComponentHandler(componentHandler);
            }
        }
    }

    // 7. Enumerate Buses
    // MVP: activate main input and output bus (usually index 0)
    component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, true);
    component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, true);

    loaded = true;
    return PluginLoadResult::Ok;
}

void Vst3Backend::unload() {
    if (!loaded) return;

    closeEditor();

    if (processor) {
        processor->setProcessing(false);
        processor->release();
        processor = nullptr;
    }

    if (component) {
        component->setActive(false);
        component->terminate();
        component->release();
        component = nullptr;
    }

    if (controller) {
        if (componentHandler) {
            controller->setComponentHandler(nullptr);
            componentHandler->release();
            componentHandler = nullptr;
        }
        controller->terminate();
        controller->release();
        controller = nullptr;
    }

    if (factory) {
        factory->release();
        factory = nullptr;
    }

    if (moduleHandle) {
#if defined(_WIN32)
        FreeLibrary(moduleHandle);
#else
        dlclose(moduleHandle);
#endif
        moduleHandle = nullptr;
    }

    loaded = false;
    clearPointers();
}

PluginRuntimeInfo Vst3Backend::runtimeInfo() const {
    PluginRuntimeInfo info;
    info.name = pluginName;
    info.vendor = pluginVendor;
    info.sampleRate = currentSampleRate;
    info.blockSizing = currentBlockSize;
    if (processor) {
        info.latencySamples = processor->getLatencySamples();
    }
    return info;
}

bool Vst3Backend::configure(double sampleRate, uint32_t maxBlockSize) {
    if (!loaded || !processor || !component) return false;

    currentSampleRate = sampleRate;
    currentBlockSize = maxBlockSize;

    // Terminate active processing state if active
    processor->setProcessing(false);
    component->setActive(false);

    // Set Bus arrangements
    Steinberg::Vst::SpeakerArrangement inArr = Steinberg::Vst::SpeakerArr::kStereo;
    Steinberg::Vst::SpeakerArrangement outArr = Steinberg::Vst::SpeakerArr::kStereo;
    
    // Attempt stereo, fallback is handled inside plug
    processor->setBusArrangements(&inArr, 1, &outArr, 1);

    Steinberg::Vst::ProcessSetup setup;
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = maxBlockSize;
    setup.sampleRate = sampleRate;

    if (processor->setupProcessing(setup) != Steinberg::kResultOk) {
        return false;
    }

    if (component->setActive(true) != Steinberg::kResultOk) {
        return false;
    }

    if (processor->setProcessing(true) != Steinberg::kResultOk) {
        return false;
    }

    return true;
}

bool Vst3Backend::process(PluginProcessBlock& block) {
    if (!loaded || !processor) return false;

    Steinberg::Vst::ProcessData data{};

    data.processMode = Steinberg::Vst::kRealtime;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = block.numSamples;

    // Create process context
    Steinberg::Vst::ProcessContext context{};
    context.sampleRate = currentSampleRate;
    context.projectTimeSamples = 0;
    context.tempo = 120.0;
    context.timeSigNumerator = 4;
    context.timeSigDenominator = 4;
    context.state = Steinberg::Vst::ProcessContext::kPlaying;
    data.processContext = &context;

    // Audio Inputs
    Steinberg::Vst::AudioBusBuffers inBus{};
    if (block.numInputChannels > 0 && block.inputBuffers) {
        inBus.numChannels = block.numInputChannels;
        inBus.silenceFlags = 0;
        inBus.channelBuffers32 = block.inputBuffers;
        data.numInputs = 1;
        data.inputs = &inBus;
    }

    // Audio Outputs
    Steinberg::Vst::AudioBusBuffers outBus{};
    if (block.numOutputChannels > 0 && block.outputBuffers) {
        outBus.numChannels = block.numOutputChannels;
        outBus.silenceFlags = 0;
        outBus.channelBuffers32 = block.outputBuffers;
        data.numOutputs = 1;
        data.outputs = &outBus;
    }

    // Map events
    eventList->clear();
    for (const auto& ev : block.inputEvents) {
        if (ev.type == EventType::NoteOn) {
            eventList->addNoteOn(ev.sampleOffset, ev.note, ev.noteId, ev.value);
        } else if (ev.type == EventType::NoteOff) {
            eventList->addNoteOff(ev.sampleOffset, ev.note, ev.noteId, ev.value);
        } else if (ev.type == EventType::PolyPressure) {
            eventList->addPolyPressure(ev.sampleOffset, ev.note, ev.noteId, ev.value);
        }
    }
    data.inputEvents = eventList;

    // Assign parameter changes
    {
        std::lock_guard<std::mutex> lock(paramMutex);
        data.inputParameterChanges = paramChanges;
    }

    // Run DSP Process
    Steinberg::tresult result = processor->process(data);

    // Clear parameter changes after processing
    {
        std::lock_guard<std::mutex> lock(paramMutex);
        paramChanges->clear();
    }

    return (result == Steinberg::kResultOk);
}

std::vector<PluginParameterInfo> Vst3Backend::parameters() const {
    std::vector<PluginParameterInfo> list;
    if (!loaded || !controller) return list;

    Steinberg::int32 count = controller->getParameterCount();
    for (Steinberg::int32 i = 0; i < count; ++i) {
        Steinberg::Vst::ParameterInfo info;
        if (controller->getParameterInfo(i, info) == Steinberg::kResultOk) {
            PluginParameterInfo p;
            p.id = info.id;
            
            // Convert title and shortTitle from UTF-16 to UTF-8
            // (Simple casting since parameters are mostly ASCII/basic chars in VST)
            std::stringstream titleStream;
            for (int k = 0; info.title[k] && k < 128; ++k) {
                titleStream << (char)info.title[k];
            }
            p.title = titleStream.str();

            std::stringstream shortStream;
            for (int k = 0; info.shortTitle[k] && k < 128; ++k) {
                shortStream << (char)info.shortTitle[k];
            }
            p.shortTitle = shortStream.str();

            std::stringstream unitStream;
            for (int k = 0; info.units[k] && k < 128; ++k) {
                unitStream << (char)info.units[k];
            }
            p.units = unitStream.str();

            p.stepCount = info.stepCount;
            p.defaultNormalized = info.defaultNormalizedValue;
            p.flags = info.flags;
            p.unitId = info.unitId;

            p.canAutomate = (info.flags & Steinberg::Vst::ParameterInfo::kCanAutomate) != 0;
            p.isReadOnly = (info.flags & Steinberg::Vst::ParameterInfo::kIsReadOnly) != 0;
            p.isBypass = (info.flags & Steinberg::Vst::ParameterInfo::kIsBypass) != 0;
            p.isProgramChange = (info.flags & Steinberg::Vst::ParameterInfo::kIsProgramChange) != 0;

            list.push_back(p);
        }
    }
    return list;
}

bool Vst3Backend::setParameterNormalized(uint32_t stableParameterId, double normalizedValue, int32_t sampleOffset) {
    if (!loaded || !controller) return false;

    // Update Controller
    controller->setParamNormalized(stableParameterId, normalizedValue);

    // Enqueue for DSP Processor
    {
        std::lock_guard<std::mutex> lock(paramMutex);
        paramChanges->addChange(stableParameterId, sampleOffset, normalizedValue);
    }
    return true;
}

PluginStateBlob Vst3Backend::captureState() {
    PluginStateBlob blob;
    if (!loaded) return blob;

    // 1. Capture component state
    if (component) {
        Vst3StateStream* stream = new Vst3StateStream();
        if (component->getState(stream) == Steinberg::kResultOk) {
            blob.componentState = stream->getData();
        }
        stream->release();
    }

    // 2. Capture controller state
    if (controller) {
        Vst3StateStream* stream = new Vst3StateStream();
        if (controller->getState(stream) == Steinberg::kResultOk) {
            blob.controllerState = stream->getData();
        }
        stream->release();
    }

    return blob;
}

bool Vst3Backend::restoreState(const PluginStateBlob& blob) {
    if (!loaded) return false;

    bool success = true;

    // 1. Restore component state
    if (component && !blob.componentState.empty()) {
        Vst3StateStream* stream = new Vst3StateStream(blob.componentState);
        if (component->setState(stream) != Steinberg::kResultOk) {
            success = false;
        }
        // Apply component state to controller as well (standard VST3 recommendation)
        if (controller) {
            stream->seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            controller->setComponentState(stream);
        }
        stream->release();
    }

    // 2. Restore controller state
    if (controller && !blob.controllerState.empty()) {
        Vst3StateStream* stream = new Vst3StateStream(blob.controllerState);
        if (controller->setState(stream) != Steinberg::kResultOk) {
            success = false;
        }
        stream->release();
    }

    return success;
}

bool Vst3Backend::hasNativeEditor() const {
    if (!loaded || !controller) return false;
    Steinberg::IPlugView* editorView = controller->createView(Steinberg::Vst::ViewType::kEditor);
    if (editorView) {
        editorView->release();
        return true;
    }
    return false;
}

EditorOpenResult Vst3Backend::openEditor(void* parentWindowHandle, int x, int y, int width, int height) {
    if (!loaded || !controller) return EditorOpenResult::NoEditor;
    if (editorOpen) return EditorOpenResult::AlreadyOpen;

    view = controller->createView(Steinberg::Vst::ViewType::kEditor);
    if (!view) {
        return EditorOpenResult::NoEditor;
    }

#if defined(_WIN32)
    if (view->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != Steinberg::kResultOk) {
        view->release();
        view = nullptr;
        return EditorOpenResult::PlatformError;
    }

    HWND parent = (HWND)parentWindowHandle;
    // Attach View
    if (view->attached((void*)parent, Steinberg::kPlatformTypeHWND) != Steinberg::kResultOk) {
        view->release();
        view = nullptr;
        return EditorOpenResult::PlatformError;
    }
#else
    if (view->isPlatformTypeSupported(Steinberg::kPlatformTypeX11EmbedWindowID) != Steinberg::kResultOk) {
        view->release();
        view = nullptr;
        return EditorOpenResult::PlatformError;
    }

    // Initialize X11 Window
    xDisplay = XOpenDisplay(NULL);
    if (!xDisplay) {
        view->release();
        view = nullptr;
        return EditorOpenResult::PlatformError;
    }

    Window parent = (Window)parentWindowHandle;
    if (parent == 0) {
        // Create a standalone decorated top-level window if no parent provided
        int screen = DefaultScreen(xDisplay);
        parent = XCreateSimpleWindow(xDisplay, DefaultRootWindow(xDisplay), x, y, width, height, 1,
                                     BlackPixel(xDisplay, screen),
                                     WhitePixel(xDisplay, screen));
        XSelectInput(xDisplay, parent, StructureNotifyMask | ExposureMask);
        XMapWindow(xDisplay, parent);
        xWindow = parent;
    }

    // Attach View
    if (view->attached((void*)parent, Steinberg::kPlatformTypeX11EmbedWindowID) != Steinberg::kResultOk) {
        if (xWindow) {
            XDestroyWindow(xDisplay, xWindow);
            xWindow = 0;
        }
        XCloseDisplay(xDisplay);
        xDisplay = nullptr;
        view->release();
        view = nullptr;
        return EditorOpenResult::PlatformError;
    }
#endif

    editorOpen = true;
    return EditorOpenResult::Ok;
}

void Vst3Backend::closeEditor() {
    if (!editorOpen || !view) return;

    view->removed();
    view->release();
    view = nullptr;

#if !defined(_WIN32)
    if (xWindow) {
        XDestroyWindow(xDisplay, xWindow);
        xWindow = 0;
    }

    if (xDisplay) {
        XCloseDisplay(xDisplay);
        xDisplay = nullptr;
    }
#endif

    editorOpen = false;
}

void Vst3Backend::stepUI() {
#if !defined(_WIN32)
    if (!editorOpen || !xDisplay) return;

    // Process pending X11 events for the plugin window
    XEvent event;
    while (XPending(xDisplay)) {
        XNextEvent(xDisplay, &event);
        // Handle exposures or configuration updates if necessary
    }
#endif
}

void Vst3Backend::setParameterEditCallbacks(ParameterEditCallback onBegin, ParameterEditCallback onPerform, ParameterEditCallback onEnd) {
    if (componentHandler) {
        componentHandler->setEditCallbacks(onBegin, onPerform, onEnd);
    }
}

} // namespace ifrit
