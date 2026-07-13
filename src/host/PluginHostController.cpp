#include "PluginHostController.hpp"
#include <chrono>
#include <iostream>
#include <cmath>

namespace ifrit {

PluginHostController::PluginHostController() : scanner(catalog) {
    activeInstance = nullptr;
    loading = false;
    learnSlotIndex = -1;

    for (int i = 0; i < 16; ++i) {
        mappings[i].assigned = false;
        mappings[i].parameterId = 0;
    }
}

PluginHostController::~PluginHostController() {
    HostedPluginInstance* inst = activeInstance.exchange(nullptr);
    if (inst) {
        delete inst;
    }
}

void PluginHostController::startScan(const std::vector<std::string>& customDirectories) {
    scanner.startScan(customDirectories);
}

void PluginHostController::loadPluginAsync(const PluginDescriptor& desc) {
    if (loading.load()) return;
    loading = true;

    std::thread([this, desc]() {
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

        // We can write Vst3Backend changes later. For now, let's write the swap logic:
        HostedPluginInstance* oldInstance = activeInstance.exchange(newInstance, std::memory_order_release);
        if (oldInstance) {
            retireInstance(oldInstance);
        }
        loading = false;
    }).detach();
}

void PluginHostController::unloadPluginAsync() {
    if (loading.load()) return;
    loading = true;

    std::thread([this]() {
        HostedPluginInstance* oldInstance = activeInstance.exchange(nullptr, std::memory_order_release);
        if (oldInstance) {
            retireInstance(oldInstance);
        }
        loading = false;
    }).detach();
}

void PluginHostController::retireInstance(HostedPluginInstance* instance) {
    // Sleep for 50ms to allow audio thread to finish its active block
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    delete instance;
}

bool PluginHostController::processAudio(PluginProcessBlock& block) {
    HostedPluginInstance* inst = activeInstance.load(std::memory_order_acquire);
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
        HostedPluginInstance* inst = activeInstance.load(std::memory_order_acquire);
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

    HostedPluginInstance* inst = activeInstance.load(std::memory_order_acquire);
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
    HostedPluginInstance* inst = activeInstance.load(std::memory_order_acquire);
    if (inst) {
        return inst->backend->captureState();
    }
    return PluginStateBlob();
}

void PluginHostController::restoreState(const PluginStateBlob& state) {
    std::lock_guard<std::mutex> lock(stateMutex);
    HostedPluginInstance* inst = activeInstance.load(std::memory_order_acquire);
    if (inst) {
        inst->backend->restoreState(state);
    }
}

bool PluginHostController::hasEditor() const {
    HostedPluginInstance* inst = activeInstance.load(std::memory_order_acquire);
    return inst ? inst->backend->hasNativeEditor() : false;
}

void PluginHostController::openEditor(void* parentWindowHandle, int x, int y, int width, int height) {
    HostedPluginInstance* inst = activeInstance.load(std::memory_order_acquire);
    if (inst) {
        inst->backend->openEditor(parentWindowHandle, x, y, width, height);
    }
}

void PluginHostController::closeEditor() {
    HostedPluginInstance* inst = activeInstance.load(std::memory_order_acquire);
    if (inst) {
        inst->backend->closeEditor();
    }
}

bool PluginHostController::isEditorOpen() const {
    HostedPluginInstance* inst = activeInstance.load(std::memory_order_acquire);
    return inst ? inst->backend->isEditorVisible() : false;
}

void PluginHostController::stepUI() {
    HostedPluginInstance* inst = activeInstance.load(std::memory_order_acquire);
    if (inst) {
        inst->backend->stepUI();
    }
}

} // namespace ifrit
