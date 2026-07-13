#pragma once

#include "PluginDescriptor.hpp"
#include "vst3/Vst3Backend.hpp"

namespace ifrit {

class HostedPluginInstance {
public:
    HostedPluginInstance(const PluginDescriptor& desc) : descriptor(desc) {
        backend = new Vst3Backend();
    }
    
    ~HostedPluginInstance() {
        backend->unload();
        delete backend;
    }

    PluginDescriptor descriptor;
    Vst3Backend* backend;
};

} // namespace ifrit
