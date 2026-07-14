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
#include <logger.hpp>
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/gui/iplugview.h"

namespace ifrit {

#if defined(_WIN32)
namespace {
constexpr char kVst3EditorWindowClass[] = "IfritVst3EditorWindow";

LRESULT CALLBACK vst3EditorWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CLOSE) {
        auto* backend = reinterpret_cast<Vst3Backend*>(GetWindowLongPtrA(window, GWLP_USERDATA));
        if (backend) {
            backend->closeEditor();
            return 0;
        }
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

ATOM ensureVst3EditorWindowClass() {
    static const ATOM windowClass = []() {
        WNDCLASSA wc{};
        wc.lpfnWndProc = vst3EditorWindowProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kVst3EditorWindowClass;
        return RegisterClassA(&wc);
    }();
    return windowClass;
}

class Vst3EditorFrame final : public Steinberg::IPlugFrame {
public:
    explicit Vst3EditorFrame(HWND window) : window(window) {}

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::IPlugFrame::iid.toTUID()) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid.toTUID())) {
            *obj = this;
            addRef();
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    Steinberg::uint32 PLUGIN_API addRef() override { return ++refCount; }
    Steinberg::uint32 PLUGIN_API release() override {
        const auto count = --refCount;
        if (count == 0) delete this;
        return count;
    }

    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* plugView, Steinberg::ViewRect* size) override {
        if (!window || !plugView || !size) return Steinberg::kInvalidArgument;

        RECT outer{};
        RECT client{};
        GetWindowRect(window, &outer);
        GetClientRect(window, &client);
        const int frameWidth = (outer.right - outer.left) - (client.right - client.left);
        const int frameHeight = (outer.bottom - outer.top) - (client.bottom - client.top);
        SetWindowPos(window, nullptr, 0, 0, size->getWidth() + frameWidth,
                     size->getHeight() + frameHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return plugView->onSize(size);
    }

private:
    std::atomic<Steinberg::uint32> refCount{1};
    HWND window = nullptr;
};
} // namespace
#endif

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
    controllerIsComponent = false;
    componentHandler = nullptr;
    view = nullptr;
    editorFrame = nullptr;
    editorAvailable = false;
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
            } else {
                controller->release();
                controller = nullptr;
            }
        }
    }

    // Some VST3s implement IEditController directly on the component instead
    // of exposing a separate controller class. Support that standard form so
    // their editor is available to the Rack module as well.
    if (!controller) {
        component->queryInterface(Steinberg::Vst::IEditController::iid, (void**)&controller);
        if (controller) {
            controllerIsComponent = true;
            componentHandler = new Vst3ComponentHandler();
            controller->setComponentHandler(componentHandler);
        }
    }

    // 7. Enumerate Buses
    // MVP: activate main input and output bus (usually index 0)
    component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, true);
    component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, true);

    // Do not probe createView() here or from the UI draw loop. Several VST3
    // implementations allocate editor state in createView() and are unstable
    // when the host repeatedly creates/releases it just to test availability.
    editorAvailable = controller != nullptr;
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
        if (!controllerIsComponent) {
            controller->terminate();
        }
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
    return loaded && controller && editorAvailable;
}

EditorOpenResult Vst3Backend::openEditor(void* parentWindowHandle, int x, int y, int width, int height) {
    if (!loaded || !controller) return EditorOpenResult::NoEditor;
    if (editorOpen) return EditorOpenResult::AlreadyOpen;

    INFO("Ifrit VST3 editor: createView begin (%s)", pluginName.c_str());
    view = controller->createView(Steinberg::Vst::ViewType::kEditor);
    INFO("Ifrit VST3 editor: createView end (%s, view=%p)", pluginName.c_str(), static_cast<void*>(view));
    if (!view) {
        return EditorOpenResult::NoEditor;
    }

#if defined(_WIN32)
    INFO("Ifrit VST3 editor: isPlatformTypeSupported begin (%s)", pluginName.c_str());
    if (view->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != Steinberg::kResultOk) {
        WARN("Ifrit VST3 editor: HWND platform rejected (%s)", pluginName.c_str());
        view->release();
        view = nullptr;
        return EditorOpenResult::PlatformError;
    }
    INFO("Ifrit VST3 editor: isPlatformTypeSupported end (%s)", pluginName.c_str());

    HWND parent = static_cast<HWND>(parentWindowHandle);
    bool ownsWindow = false;
    if (!parent) {
        if (!ensureVst3EditorWindowClass()) {
            view->release();
            view = nullptr;
            return EditorOpenResult::PlatformError;
        }

        Steinberg::ViewRect rect{};
        INFO("Ifrit VST3 editor: getSize begin (%s)", pluginName.c_str());
        if (view->getSize(&rect) == Steinberg::kResultOk) {
            width = std::max(1, static_cast<int>(rect.right - rect.left));
            height = std::max(1, static_cast<int>(rect.bottom - rect.top));
        }
        INFO("Ifrit VST3 editor: getSize end (%s, %dx%d)", pluginName.c_str(), width, height);

        RECT windowRect{0, 0, width, height};
        AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);
        INFO("Ifrit VST3 editor: CreateWindowEx begin (%s)", pluginName.c_str());
        parent = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kVst3EditorWindowClass, pluginName.c_str(), WS_OVERLAPPEDWINDOW,
            x, y, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
            nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
        if (!parent) {
            WARN("Ifrit VST3 editor: CreateWindowEx failed (%s, error=%lu)", pluginName.c_str(), GetLastError());
            view->release();
            view = nullptr;
            return EditorOpenResult::PlatformError;
        }
        INFO("Ifrit VST3 editor: CreateWindowEx end (%s, hwnd=%p)", pluginName.c_str(), static_cast<void*>(parent));
        SetWindowLongPtrA(parent, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        xWindow = parent;
        ownsWindow = true;

        // Match Steinberg's editorhost: the native parent is visible before
        // setFrame()/attached() are invoked.
        INFO("Ifrit VST3 editor: ShowWindow begin (%s)", pluginName.c_str());
        ShowWindow(xWindow, SW_SHOW);
        SetWindowPos(xWindow, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        UpdateWindow(xWindow);
        INFO("Ifrit VST3 editor: ShowWindow end (%s)", pluginName.c_str());
    }

    editorFrame = new Vst3EditorFrame(parent);
    INFO("Ifrit VST3 editor: setFrame begin (%s)", pluginName.c_str());
    if (view->setFrame(editorFrame) != Steinberg::kResultOk) {
        WARN("Ifrit VST3 editor: setFrame failed (%s)", pluginName.c_str());
        editorFrame->release();
        editorFrame = nullptr;
        if (ownsWindow) {
            DestroyWindow(xWindow);
            xWindow = nullptr;
        }
        view->release();
        view = nullptr;
        return EditorOpenResult::PlatformError;
    }
    INFO("Ifrit VST3 editor: setFrame end (%s)", pluginName.c_str());

    // A VST3 editor needs a real HWND parent. Passing nullptr attaches nowhere,
    // which is why the editor button previously opened an invisible window.
    INFO("Ifrit VST3 editor: attached begin (%s)", pluginName.c_str());
    if (view->attached(parent, Steinberg::kPlatformTypeHWND) != Steinberg::kResultOk) {
        WARN("Ifrit VST3 editor: attached failed (%s)", pluginName.c_str());
        view->setFrame(nullptr);
        editorFrame->release();
        editorFrame = nullptr;
        if (ownsWindow) {
            DestroyWindow(xWindow);
            xWindow = nullptr;
        }
        view->release();
        view = nullptr;
        return EditorOpenResult::PlatformError;
    }
    INFO("Ifrit VST3 editor: attached end (%s)", pluginName.c_str());
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

    // Steinberg's editorhost clears the frame before removing the view.
    view->setFrame(nullptr);
    view->removed();
    view->release();
    view = nullptr;

    if (editorFrame) {
        editorFrame->release();
        editorFrame = nullptr;
    }

#if defined(_WIN32)
    if (xWindow) {
        SetWindowLongPtrA(xWindow, GWLP_USERDATA, 0);
        DestroyWindow(xWindow);
        xWindow = nullptr;
    }
#else
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
