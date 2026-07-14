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
    if (lifecycleThread.joinable()) {
        lifecycleThread.join();
    }
    std::lock_guard<std::mutex> lock(instanceMutex);
    HostedPluginInstance* inst = activeInstance;
    activeInstance = nullptr;
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
    if (lifecycleThread.joinable()) lifecycleThread.join();
    loading = true;

    lifecycleThread = std::thread([this, desc]() {
        HostedPluginInstance* newInstance = new HostedPluginInstance(desc);
        PluginLoadResult res = newInstance->backend->load(desc);
        
        if (res == PluginLoadResult::Ok) {
            // Initial configuration
            newInstance->backend->configure(44100.0, 128);

            // Hook up component handler for parameter learning
            newInstance->backend->setParameterEditCallbacks(
                nullptr,
                [this](uint32_t paramId, double value) {
                    this->onPluginParameterEdited(paramId, value);
                },
                nullptr
            );
        }

        if (res == PluginLoadResult::Ok) {
            std::lock_guard<std::mutex> lock(instanceMutex);
            HostedPluginInstance* oldInstance = activeInstance;
            activeInstance = newInstance;
            retireInstance(oldInstance);
        } else {
            delete newInstance;
        }
        loading = false;
    });
}

void PluginHostController::unloadPluginAsync() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
    if (loading.load()) return;
    if (lifecycleThread.joinable()) lifecycleThread.join();
    loading = true;

    lifecycleThread = std::thread([this]() {
        std::lock_guard<std::mutex> lock(instanceMutex);
        HostedPluginInstance* oldInstance = activeInstance;
        activeInstance = nullptr;
        retireInstance(oldInstance);
        loading = false;
    });
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
    std::lock_guard<std::mutex> lock(instanceMutex);
    HostedPluginInstance* inst = activeInstance;
    if (inst) {
        inst->backend->openEditor(parentWindowHandle, x, y, width, height);
    }
}

void PluginHostController::closeEditor() {
    std::lock_guard<std::mutex> lock(instanceMutex);
    HostedPluginInstance* inst = activeInstance;
    if (inst) {
        inst->backend->closeEditor();
    }
}

bool PluginHostController::isEditorOpen() const {
    std::lock_guard<std::mutex> lock(instanceMutex);
    HostedPluginInstance* inst = activeInstance;
    return inst ? inst->backend->isEditorVisible() : false;
}

void PluginHostController::stepUI() {
    std::lock_guard<std::mutex> lock(instanceMutex);
    HostedPluginInstance* inst = activeInstance;
    if (inst) {
        inst->backend->stepUI();
    }
}

} // namespace ifrit
