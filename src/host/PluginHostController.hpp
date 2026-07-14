#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include "PluginDescriptor.hpp"
#include "HostedPluginInstance.hpp"
#include "PluginCatalog.hpp"
#include "PluginScanner.hpp"
#include "ParameterMapping.hpp"

namespace ifrit {

class PluginHostController {
public:
    PluginHostController();
    ~PluginHostController();

    // Catalog & Scanning
    PluginCatalog& getCatalog() { return catalog; }
    PluginScanner& getScanner() { return scanner; }
    void startScan(const std::vector<std::string>& customDirectories = {});

    // Asynchronous Loading/Unloading
    void loadPluginAsync(const PluginDescriptor& desc);
    void unloadPluginAsync();
    bool isLoaded() const;
    bool isTransitioning() const { return loading.load(); }
    bool getActiveDescriptor(PluginDescriptor& descriptor) const;

    // Audio thread block processing (realtime safe)
    bool processAudio(PluginProcessBlock& block);

    // Mappings bank
    ParameterMapping& getMapping(int slotIndex);
    void setLearnSlot(int slotIndex);
    int getLearnSlot() const { return learnSlotIndex.load(); }
    void cancelLearn();
    void clearMapping(int slotIndex);

    // Automation (realtime safe)
    void updateAutomation(int slotIndex, float voltage, int32_t sampleOffset);

    // State persistence
    PluginStateBlob captureState();
    void restoreState(const PluginStateBlob& state);

    // Editor Window
    bool hasEditor() const;
    void openEditor(void* parentWindowHandle, int x, int y, int width, int height);
    void closeEditor();
    bool isEditorOpen() const;

    // UI Tick (called on Rack main thread)
    void stepUI();

private:
    void retireInstance(HostedPluginInstance* instance);
    void onPluginParameterEdited(uint32_t paramId, double valueNormalized);

    HostedPluginInstance* activeInstance = nullptr;
    mutable std::mutex instanceMutex;
    std::atomic<bool> loading;
    std::thread lifecycleThread;
    std::mutex lifecycleMutex;

    PluginCatalog catalog;
    PluginScanner scanner;

    ParameterMapping mappings[16];
    std::atomic<int> learnSlotIndex;

    std::mutex stateMutex;
};

} // namespace ifrit
