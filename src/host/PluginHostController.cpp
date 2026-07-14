#include "PluginHostController.hpp"
#include <chrono>
#include <iostream>
#include <cmath>

namespace ifrit {

PluginHostController::PluginHostController() : scanner(catalog) {
    loading = false;
    learnSlotIndex = -1;

    for (int i = 0; i < 16; ++i) {
        mappings[i].assigned = false;
        mappings[i].parameterId = 0;
    }
}

PluginHostController::~PluginHostController() {
    HostedPluginInstance* inst = nullptr;
    {
        std::lock_guard<std::mutex> lock(instanceMutex);
        inst = activeInstance;
        activeInstance = nullptr;
    }
    if (inst) {
        delete inst;
    }
}

void PluginHostController::startScan(const std::vector<std::string>& customDirectories) {
    scanner.startScan(customDirectories);
}

void PluginHostController::loadPluginAsync(const PluginDescriptor& desc) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
    if (loading.load()) return;
    pendingDescriptor = desc;
    pendingLifecycleOperation = PendingLifecycleOperation::Load;
    loading = true;
}

void PluginHostController::unloadPluginAsync() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
    if (loading.load()) return;
    pendingLifecycleOperation = PendingLifecycleOperation::Unload;
    loading = true;
}

void PluginHostController::retireInstance(HostedPluginInstance* instance) {
    delete instance;
}

bool PluginHostController::isLoaded() const {
    std::lock_guard<std::mutex> lock(instanceMutex);
    return activeInstance != nullptr;
}

bool PluginHostController::getActiveDescriptor(PluginDescriptor& descriptor) const {
    std::lock_guard<std::mutex> lock(instanceMutex);
    if (!activeInstance) return false;
    descriptor = activeInstance->descriptor;
    return true;
}

bool PluginHostController::processAudio(PluginProcessBlock& block) {
    std::lock_guard<std::mutex> lock(instanceMutex);
    HostedPluginInstance* inst = activeInstance;
    if (!inst || !inst->backend->isLoaded()) {
        return false; // Passthrough or silence handled by the module
    }

    return inst->backend->process(block);
}

ParameterMapping& PluginHostController::getMapping(int slotIndex) {
    return mappings[slotIndex];
}

void PluginHostController::setLearnSlot(int slotIndex) {
    learnSlotIndex.store(slotIndex);
}

void PluginHostController::cancelLearn() {
    learnSlotIndex.store(-1);
}

void PluginHostController::clearMapping(int slotIndex) {
    mappings[slotIndex].assigned = false;
    mappings[slotIndex].parameterId = 0;
    mappings[slotIndex].cachedTitle.clear();
    mappings[slotIndex].cachedShortTitle.clear();
    mappings[slotIndex].cachedUnit.clear();
}

void PluginHostController::onPluginParameterEdited(uint32_t paramId, double valueNormalized) {
    int slot = learnSlotIndex.load();
    if (slot >= 0 && slot < 16) {
            std::lock_guard<std::mutex> lock(instanceMutex);
            HostedPluginInstance* inst = activeInstance;
        if (inst) {
            auto params = inst->backend->parameters();
            for (const auto& p : params) {
                if (p.id == paramId) {
                    mappings[slot].assigned = true;
                    mappings[slot].parameterId = paramId;
                    mappings[slot].cachedTitle = p.title;
                    mappings[slot].cachedShortTitle = p.shortTitle;
                    mappings[slot].cachedUnit = p.units;
                    mappings[slot].cachedStepCount = p.stepCount;
                    mappings[slot].cachedFlags = p.flags;
                    
                    learnSlotIndex.store(-1); // finish learning
                    break;
                }
            }
        }
    }
}

void PluginHostController::updateAutomation(int slotIndex, float voltage, int32_t sampleOffset) {
    if (!mappings[slotIndex].assigned) return;

        std::lock_guard<std::mutex> lock(instanceMutex);
        HostedPluginInstance* inst = activeInstance;
    if (inst) {
        double val = (double)voltage / 10.0;
        if (val < 0.0) val = 0.0;
        if (val > 1.0) val = 1.0;

        // Discrete parameter quantization
        int32_t steps = mappings[slotIndex].cachedStepCount;
        if (steps > 0) {
            val = std::round(val * steps) / steps;
        }

        inst->backend->setParameterNormalized(mappings[slotIndex].parameterId, val, sampleOffset);
    }
}

PluginStateBlob PluginHostController::captureState() {
    std::lock_guard<std::mutex> lock(stateMutex);
    std::lock_guard<std::mutex> instanceLock(instanceMutex);
    HostedPluginInstance* inst = activeInstance;
    if (inst) {
        return inst->backend->captureState();
    }
    return PluginStateBlob();
}

void PluginHostController::restoreState(const PluginStateBlob& state) {
    std::lock_guard<std::mutex> lock(stateMutex);
    std::lock_guard<std::mutex> instanceLock(instanceMutex);
    HostedPluginInstance* inst = activeInstance;
    if (inst) {
        inst->backend->restoreState(state);
    }
}

bool PluginHostController::hasEditor() const {
    std::lock_guard<std::mutex> lock(instanceMutex);
    HostedPluginInstance* inst = activeInstance;
    return inst ? inst->backend->hasNativeEditor() : false;
}

void PluginHostController::openEditor(void* parentWindowHandle, int x, int y, int width, int height) {
    // VST editors are allowed to synchronously invoke component callbacks from
    // attached(). Do not hold instanceMutex across that call or those callbacks
    // deadlock Rack's UI thread. loading also prevents concurrent unload.
    bool expected = false;
    if (!loading.compare_exchange_strong(expected, true)) return;

    HostedPluginInstance* inst = nullptr;
    {
        std::lock_guard<std::mutex> lock(instanceMutex);
        inst = activeInstance;
    }
    if (inst) {
        inst->backend->openEditor(parentWindowHandle, x, y, width, height);
    }
    loading = false;
}

void PluginHostController::closeEditor() {
    bool expected = false;
    if (!loading.compare_exchange_strong(expected, true)) return;

    HostedPluginInstance* inst = nullptr;
    {
        std::lock_guard<std::mutex> lock(instanceMutex);
        inst = activeInstance;
    }
    if (inst) {
        inst->backend->closeEditor();
    }
    loading = false;
}

bool PluginHostController::isEditorOpen() const {
    std::lock_guard<std::mutex> lock(instanceMutex);
    HostedPluginInstance* inst = activeInstance;
    return inst ? inst->backend->isEditorVisible() : false;
}

void PluginHostController::stepUI() {
    // VST3 component/controller initialization and termination are main-thread
    // operations. Execute requests here, on Rack's UI thread, so a later
    // IPlugView::attached() runs on the same thread that owns the controller.
    PendingLifecycleOperation operation = PendingLifecycleOperation::Idle;
    PluginDescriptor descriptor;
    {
        std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
        operation = pendingLifecycleOperation;
        if (operation == PendingLifecycleOperation::Load) {
            descriptor = pendingDescriptor;
        }
        pendingLifecycleOperation = PendingLifecycleOperation::Idle;
    }

    if (operation == PendingLifecycleOperation::Load) {
        HostedPluginInstance* newInstance = new HostedPluginInstance(descriptor);
        const PluginLoadResult result = newInstance->backend->load(descriptor);
        if (result == PluginLoadResult::Ok && newInstance->backend->configure(44100.0, 128)) {
            newInstance->backend->setParameterEditCallbacks(
                nullptr,
                [this](uint32_t paramId, double value) {
                    this->onPluginParameterEdited(paramId, value);
                },
                nullptr
            );

            HostedPluginInstance* oldInstance = nullptr;
            {
                std::lock_guard<std::mutex> lock(instanceMutex);
                oldInstance = activeInstance;
                activeInstance = newInstance;
            }
            retireInstance(oldInstance);
        } else {
            delete newInstance;
        }
        loading = false;
    } else if (operation == PendingLifecycleOperation::Unload) {
        HostedPluginInstance* oldInstance = nullptr;
        {
            std::lock_guard<std::mutex> lock(instanceMutex);
            oldInstance = activeInstance;
            activeInstance = nullptr;
        }
        retireInstance(oldInstance);
        loading = false;
    }

    std::lock_guard<std::mutex> lock(instanceMutex);
    HostedPluginInstance* inst = activeInstance;
    if (inst) {
        inst->backend->stepUI();
    }
}

} // namespace ifrit
