#pragma once

#include <vector>
#include <string>
#include <mutex>
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

    // Serialization
    bool loadFromFile(const std::string& path);
    bool saveToFile(const std::string& path);

    // Search and filtering
    std::vector<PluginDescriptor> search(const std::string& query, bool effectsOnly, bool instrumentsOnly) const;

private:
    mutable std::mutex mutex;
    std::vector<PluginDescriptor> descriptors;
};

} // namespace ifrit
