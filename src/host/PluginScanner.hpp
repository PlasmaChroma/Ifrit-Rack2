#pragma once

#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include "PluginCatalog.hpp"

namespace ifrit {

class PluginScanner {
public:
    PluginScanner(PluginCatalog& catalog);
    ~PluginScanner();

    // Start scanning in a background thread
    void startScan(const std::vector<std::string>& customDirectories = {});
    
    // Check if currently scanning
    bool isScanning() const { return scanning; }
    
    // Get progress percentage (0.0 to 1.0)
    float getProgress() const { return progress; }

private:
    void scanLoop(const std::vector<std::string>& customDirectories);
    void scanDirectory(const std::string& path);
    void scanPluginFile(const std::string& path);

    PluginCatalog& catalog;
    std::thread* scanThread;
    std::atomic<bool> scanning;
    std::atomic<float> progress;
    std::atomic<bool> cancelRequested;
};

} // namespace ifrit
