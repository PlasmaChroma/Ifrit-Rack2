#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "PluginCatalog.hpp"
#include "PluginScanner.hpp"

namespace ifrit {

// One discovery catalog and scanner shared by every Ifrit host module.
class SharedPluginDiscovery {
public:
    static SharedPluginDiscovery& instance();

    PluginCatalog& getCatalog();
    PluginScanner& getScanner();
    void requestScan(const std::vector<std::string>& customDirectories = {}, bool force = false);

private:
    SharedPluginDiscovery();
    void ensureInitialized();
    bool cacheNeedsRefresh(const std::vector<std::string>& directories);
    void saveCache();
    static int64_t directorySignature(const std::string& path);

    PluginCatalog catalog;
    std::once_flag initializeFlag;
    std::mutex stateMutex;
    std::string cachePath;
    bool cacheLoaded = false;
    int64_t cacheGeneratedAt = 0;
    std::unordered_map<std::string, int64_t> cachedRootSignatures;
    std::vector<std::string> activeScanDirectories;
    // Declared last so it is destroyed first and joins its worker while all
    // callback state above is still alive.
    PluginScanner scanner;
};

} // namespace ifrit
