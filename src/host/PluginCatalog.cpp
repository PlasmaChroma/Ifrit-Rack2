#include "PluginCatalog.hpp"
#include <jansson.h>
#include <algorithm>
#include <iostream>
#include <fstream>

namespace ifrit {

static std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}

PluginCatalog::PluginCatalog() {}

void PluginCatalog::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    descriptors.clear();
}

void PluginCatalog::addDescriptor(const PluginDescriptor& desc) {
    std::lock_guard<std::mutex> lock(mutex);
    // Prevent duplicates
    auto it = std::find_if(descriptors.begin(), descriptors.end(), [&](const PluginDescriptor& d) {
        return d.modulePath == desc.modulePath && d.classId == desc.classId;
    });
    if (it == descriptors.end()) {
        descriptors.push_back(desc);
    } else {
        *it = desc; // update metadata
    }
}

size_t PluginCatalog::size() const {
    std::lock_guard<std::mutex> lock(mutex);
    return descriptors.size();
}

std::vector<PluginDescriptor> PluginCatalog::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return descriptors;
}

void PluginCatalog::replace(std::vector<PluginDescriptor> newDescriptors) {
    std::lock_guard<std::mutex> lock(mutex);
    descriptors = std::move(newDescriptors);
}

bool PluginCatalog::loadFromFile(const std::string& path) {
    clear();
    json_error_t error;
    json_t* root = json_load_file(path.c_str(), 0, &error);
    if (!root) {
        return false;
    }

    json_t* array = json_object_get(root, "plugins");
    if (json_is_array(array)) {
        size_t index;
        json_t* val;
        json_array_foreach(array, index, val) {
            PluginDescriptor desc;
            
            json_t* format = json_object_get(val, "format");
            if (json_is_string(format)) desc.format = json_string_value(format);

            json_t* modulePath = json_object_get(val, "modulePath");
            if (json_is_string(modulePath)) desc.modulePath = json_string_value(modulePath);

            json_t* classId = json_object_get(val, "classId");
            if (json_is_string(classId)) desc.classId = json_string_value(classId);

            json_t* name = json_object_get(val, "name");
            if (json_is_string(name)) desc.name = json_string_value(name);

            json_t* vendor = json_object_get(val, "vendor");
            if (json_is_string(vendor)) desc.vendor = json_string_value(vendor);

            json_t* category = json_object_get(val, "category");
            if (json_is_string(category)) desc.category = json_string_value(category);

            json_t* subcategories = json_object_get(val, "subcategories");
            if (json_is_string(subcategories)) desc.subcategories = json_string_value(subcategories);

            json_t* version = json_object_get(val, "version");
            if (json_is_string(version)) desc.version = json_string_value(version);

            json_t* appearsInst = json_object_get(val, "appearsInstrument");
            if (json_is_boolean(appearsInst)) desc.appearsInstrument = json_is_true(appearsInst);

            json_t* appearsEff = json_object_get(val, "appearsEffect");
            if (json_is_boolean(appearsEff)) desc.appearsEffect = json_is_true(appearsEff);

            json_t* hasAudioIn = json_object_get(val, "hasAudioInput");
            if (json_is_boolean(hasAudioIn)) desc.hasAudioInput = json_is_true(hasAudioIn);

            json_t* hasAudioOut = json_object_get(val, "hasAudioOutput");
            if (json_is_boolean(hasAudioOut)) desc.hasAudioOutput = json_is_true(hasAudioOut);

            json_t* hasEventIn = json_object_get(val, "hasEventInput");
            if (json_is_boolean(hasEventIn)) desc.hasEventInput = json_is_true(hasEventIn);

            addDescriptor(desc);
        }
    }

    json_decref(root);
    return true;
}

bool PluginCatalog::saveToFile(const std::string& path) {
    return saveToFile(path, 0, {});
}

bool PluginCatalog::saveToFile(
    const std::string& path,
    int64_t generatedAt,
    const std::vector<std::pair<std::string, int64_t>>& rootSignatures
) {
    std::lock_guard<std::mutex> lock(mutex);
    json_t* root = json_object();
    json_object_set_new(root, "cacheVersion", json_integer(1));
    json_object_set_new(root, "generatedAt", json_integer(generatedAt));

    json_t* roots = json_object();
    for (const auto& signature : rootSignatures) {
        json_object_set_new(roots, signature.first.c_str(), json_integer(signature.second));
    }
    json_object_set_new(root, "rootSignatures", roots);

    json_t* array = json_array();

    for (const auto& desc : descriptors) {
        json_t* val = json_object();
        json_object_set_new(val, "format", json_string(desc.format.c_str()));
        json_object_set_new(val, "modulePath", json_string(desc.modulePath.c_str()));
        json_object_set_new(val, "classId", json_string(desc.classId.c_str()));
        json_object_set_new(val, "name", json_string(desc.name.c_str()));
        json_object_set_new(val, "vendor", json_string(desc.vendor.c_str()));
        json_object_set_new(val, "category", json_string(desc.category.c_str()));
        json_object_set_new(val, "subcategories", json_string(desc.subcategories.c_str()));
        json_object_set_new(val, "version", json_string(desc.version.c_str()));
        json_object_set_new(val, "appearsInstrument", json_boolean(desc.appearsInstrument));
        json_object_set_new(val, "appearsEffect", json_boolean(desc.appearsEffect));
        json_object_set_new(val, "hasAudioInput", json_boolean(desc.hasAudioInput));
        json_object_set_new(val, "hasAudioOutput", json_boolean(desc.hasAudioOutput));
        json_object_set_new(val, "hasEventInput", json_boolean(desc.hasEventInput));

        json_array_append_new(array, val);
    }

    json_object_set_new(root, "plugins", array);
    int res = json_dump_file(root, path.c_str(), JSON_INDENT(2));
    json_decref(root);
    return (res == 0);
}

std::vector<PluginDescriptor> PluginCatalog::search(const std::string& query, bool effectsOnly, bool instrumentsOnly) const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<PluginDescriptor> results;
    std::string lq = lowercase(query);

    for (const auto& desc : descriptors) {
        if (effectsOnly && !desc.appearsEffect) continue;
        if (instrumentsOnly && !desc.appearsInstrument) continue;

        if (lq.empty()) {
            results.push_back(desc);
            continue;
        }

        std::string name = lowercase(desc.name);
        std::string vendor = lowercase(desc.vendor);
        std::string category = lowercase(desc.category);

        if (name.find(lq) != std::string::npos ||
            vendor.find(lq) != std::string::npos ||
            category.find(lq) != std::string::npos) {
            results.push_back(desc);
        }
    }
    return results;
}

} // namespace ifrit
