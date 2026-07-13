#pragma once

#include <string>

namespace ifrit {

struct PluginDescriptor {
    std::string format = "VST3";
    std::string modulePath;
    std::string classId;
    std::string name;
    std::string vendor;
    std::string category;
    std::string subcategories;
    std::string version;

    bool appearsInstrument = false;
    bool appearsEffect = false;
    bool hasAudioInput = false;
    bool hasAudioOutput = false;
    bool hasEventInput = false;
};

} // namespace ifrit
