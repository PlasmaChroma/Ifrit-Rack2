#pragma once

#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include "PluginCatalog.hpp"

namespace ifrit {

class PluginScanner {
public:
    PluginScanner(PluginCatalog& catalog);
    ~PluginScanner();

    // Start scanning in a background thread
    void startScan(const std::vector<std::string>& customDirectories = {});
    void setCompletionCallback(std::function<void()> callback) { completionCallback = std::move(callback); }
    static std::vector<std::string> defaultDirectories();
    
    // Check if currently scanning
    bool isScanning() const { return scanning; }
    
    // Get progress percentage (0.0 to 1.0)
    float getProgress() const { return progress; }

private:
    void scanLoop(const std::vector<std::string>& customDirectories);
    void scanDirectory(const std::string& path, PluginCatalog& destination);
    void scanPluginFile(const std::string& path, PluginCatalog& destination);

    PluginCatalog& catalog;
    std::thread* scanThread;
    std::atomic<bool> scanning;
    std::atomic<float> progress;
    std::atomic<bool> cancelRequested;
    std::function<void()> completionCallback;
};

} // namespace ifrit
