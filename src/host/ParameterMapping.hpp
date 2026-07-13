#pragma once

#include <string>
#include <cstdint>

namespace ifrit {

struct ParameterMapping {
    bool assigned = false;
    uint32_t parameterId = 0;

    std::string cachedTitle;
    std::string cachedShortTitle;
    std::string cachedUnit;

    int32_t cachedStepCount = 0;
    uint32_t cachedFlags = 0;
};

} // namespace ifrit
