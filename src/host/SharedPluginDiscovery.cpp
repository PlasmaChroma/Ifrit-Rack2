#include "SharedPluginDiscovery.hpp"

#include <ctime>
#include <filesystem>
#include <jansson.h>
#include <rack.hpp>

namespace fs = std::filesystem;

namespace ifrit {

namespace {
constexpr int64_t kCacheVersion = 1;
constexpr int64_t kMaximumCacheAgeSeconds = 24 * 60 * 60;
}

SharedPluginDiscovery& SharedPluginDiscovery::instance() {
    static SharedPluginDiscovery discovery;
    return discovery;
}

SharedPluginDiscovery::SharedPluginDiscovery() : scanner(catalog) {}

PluginCatalog& SharedPluginDiscovery::getCatalog() {
    ensureInitialized();
    return catalog;
}

PluginScanner& SharedPluginDiscovery::getScanner() {
    ensureInitialized();
    return scanner;
}

void SharedPluginDiscovery::ensureInitialized() {
    std::call_once(initializeFlag, [this]() {
        const std::string directory = rack::system::join(rack::asset::user(), "Ifrit");
        rack::system::createDirectories(directory);
        cachePath = rack::system::join(directory, "plugin_cache.json");

        json_error_t error{};
        json_t* root = json_load_file(cachePath.c_str(), 0, &error);
        if (root) {
            json_t* version = json_object_get(root, "cacheVersion");
            json_t* generatedAt = json_object_get(root, "generatedAt");
            if (json_is_integer(version) && json_integer_value(version) == kCacheVersion &&
                json_is_integer(generatedAt)) {
                cacheGeneratedAt = json_integer_value(generatedAt);
                json_t* signatures = json_object_get(root, "rootSignatures");
                if (json_is_object(signatures)) {
                    const char* key = nullptr;
                    json_t* value = nullptr;
                    json_object_foreach(signatures, key, value) {
                        if (json_is_integer(value)) {
                            cachedRootSignatures[key] = json_integer_value(value);
                        }
                    }
                }
                cacheLoaded = catalog.loadFromFile(cachePath);
                if (cacheLoaded) {
                    INFO("Ifrit VST3 cache: loaded %zu entries from %s", catalog.size(), cachePath.c_str());
                }
            }
            json_decref(root);
        }

        scanner.setCompletionCallback([this]() { saveCache(); });
    });
}

int64_t SharedPluginDiscovery::directorySignature(const std::string& path) {
    std::error_code error;
    if (!fs::exists(path, error) || error) return -1;
    const auto modified = fs::last_write_time(path, error);
    if (error) return -1;
    return static_cast<int64_t>(modified.time_since_epoch().count());
}

bool SharedPluginDiscovery::cacheNeedsRefresh(const std::vector<std::string>& directories) {
    if (!cacheLoaded || catalog.size() == 0) return true;

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (cacheGeneratedAt <= 0 || now - cacheGeneratedAt >= kMaximumCacheAgeSeconds) return true;

    for (const std::string& directory : directories) {
        const auto cached = cachedRootSignatures.find(directory);
        if (cached == cachedRootSignatures.end() || cached->second != directorySignature(directory)) {
            return true;
        }
    }
    return false;
}

void SharedPluginDiscovery::requestScan(const std::vector<std::string>& customDirectories, bool force) {
    ensureInitialized();
    std::lock_guard<std::mutex> lock(stateMutex);
    if (scanner.isScanning()) return;

    std::vector<std::string> directories = PluginScanner::defaultDirectories();
    directories.insert(directories.end(), customDirectories.begin(), customDirectories.end());
    if (!force && !cacheNeedsRefresh(directories)) return;

    activeScanDirectories = directories;
    scanner.startScan(customDirectories);
}

void SharedPluginDiscovery::saveCache() {
    std::vector<std::string> directories;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        directories = activeScanDirectories;
    }

    const int64_t generatedAt = static_cast<int64_t>(std::time(nullptr));
    std::vector<std::pair<std::string, int64_t>> signatures;
    signatures.reserve(directories.size());
    for (const std::string& directory : directories) {
        signatures.emplace_back(directory, directorySignature(directory));
    }

    if (!catalog.saveToFile(cachePath, generatedAt, signatures)) {
        WARN("Ifrit VST3 cache: could not write %s", cachePath.c_str());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex);
        cacheLoaded = true;
        cacheGeneratedAt = generatedAt;
        cachedRootSignatures.clear();
        for (const auto& signature : signatures) {
            cachedRootSignatures[signature.first] = signature.second;
        }
    }
    INFO("Ifrit VST3 cache: saved %zu entries to %s", catalog.size(), cachePath.c_str());
}

} // namespace ifrit
