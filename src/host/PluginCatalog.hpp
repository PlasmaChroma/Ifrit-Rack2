#pragma once

#include <vector>
#include <string>
#include <mutex>
#include <cstdint>
#include <utility>
#include "PluginDescriptor.hpp"

namespace ifrit {

class PluginCatalog {
public:
    PluginCatalog();
    ~PluginCatalog() = default;

    void clear();
    void addDescriptor(const PluginDescriptor& desc);
    const std::vector<PluginDescriptor>& getDescriptors() const { return descriptors; }
    size_t size() const;
    std::vector<PluginDescriptor> snapshot() const;
    void replace(std::vector<PluginDescriptor> newDescriptors);

    // Serialization
    bool loadFromFile(const std::string& path);
    bool saveToFile(const std::string& path);
    bool saveToFile(
        const std::string& path,
        int64_t generatedAt,
        const std::vector<std::pair<std::string, int64_t>>& rootSignatures
    );

    // Search and filtering
    std::vector<PluginDescriptor> search(const std::string& query, bool effectsOnly, bool instrumentsOnly) const;

private:
    mutable std::mutex mutex;
    std::vector<PluginDescriptor> descriptors;
};

} // namespace ifrit
