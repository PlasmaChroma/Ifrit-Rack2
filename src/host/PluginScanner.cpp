#include "PluginScanner.hpp"
#include <filesystem>
#if defined(_WIN32)
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"

namespace fs = std::filesystem;

namespace ifrit {

// Helper to format TUID as 32-char hex string
static std::string tuidToString(const Steinberg::TUID tuid) {
    std::stringstream ss;
    for (int i = 0; i < 16; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)(uint8_t)tuid[i];
    }
    return ss.str();
}

static std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}

PluginScanner::PluginScanner(PluginCatalog& cat) 
    : catalog(cat), scanThread(nullptr), scanning(false), progress(0.0f), cancelRequested(false) {}

PluginScanner::~PluginScanner() {
    cancelRequested = true;
    if (scanThread) {
        if (scanThread->joinable()) {
            scanThread->join();
        }
        delete scanThread;
    }
}

void PluginScanner::startScan(const std::vector<std::string>& customDirectories) {
    cancelRequested = true;
    if (scanThread) {
        if (scanThread->joinable()) {
            scanThread->join();
        }
        delete scanThread;
        scanThread = nullptr;
    }

    cancelRequested = false;
    scanning = true;
    progress = 0.0f;
    
    scanThread = new std::thread(&PluginScanner::scanLoop, this, customDirectories);
}

void PluginScanner::scanLoop(const std::vector<std::string>& customDirectories) {
    // 1. Gather all directories to scan
    std::vector<std::string> dirs;
    
#if defined(_WIN32)
    char* commonFiles = std::getenv("COMMONPROGRAMFILES");
    if (commonFiles) {
        dirs.push_back(std::string(commonFiles) + "\\VST3");
    } else {
        dirs.push_back("C:\\Program Files\\Common Files\\VST3");
    }
    
    char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData) {
        dirs.push_back(std::string(localAppData) + "\\Programs\\Common\\VST3");
    }
#else
    // Standard Linux paths
    dirs.push_back("/usr/lib/vst3");
    dirs.push_back("/usr/local/lib/vst3");
    
    char* home = std::getenv("HOME");
    if (home) {
        std::string homeVst3 = std::string(home) + "/.vst3";
        dirs.push_back(homeVst3);
    }
#endif

    // Add user custom paths
    for (const auto& d : customDirectories) {
        if (!d.empty()) {
            dirs.push_back(d);
        }
    }

    // 2. Scan each directory
    int totalDirs = (int)dirs.size();
    for (int i = 0; i < totalDirs; ++i) {
        if (cancelRequested) break;
        scanDirectory(dirs[i]);
        progress = (float)(i + 1) / (float)totalDirs;
    }

    scanning = false;
    progress = 1.0f;
}

void PluginScanner::scanDirectory(const std::string& path) {
    if (!fs::exists(path) || !fs::is_directory(path)) {
        return;
    }

    try {
        for (const auto& entry : fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied)) {
            if (cancelRequested) break;

            const auto& p = entry.path();
            if (p.extension() == ".vst3") {
                if (fs::is_directory(p)) {
                    // Standard VST3 bundle directory
#if defined(_WIN32)
                    std::string binPath = p.string() + "/Contents/x86_64-win";
#else
                    std::string binPath = p.string() + "/Contents/x86_64-linux";
#endif
                    if (fs::exists(binPath) && fs::is_directory(binPath)) {
                        for (const auto& subEntry : fs::directory_iterator(binPath)) {
#if defined(_WIN32)
                            if (subEntry.path().extension() == ".vst3" || subEntry.path().extension() == ".dll") {
                                scanPluginFile(subEntry.path().string());
                            }
#else
                            if (subEntry.path().extension() == ".so") {
                                scanPluginFile(subEntry.path().string());
                            }
#endif
                        }
                    }
                } else if (fs::is_regular_file(p)) {
                    // Direct file VST3 (often symlinks or flat setups)
                    scanPluginFile(p.string());
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[PluginScanner] Exception during directory traversal: " << e.what() << std::endl;
    }
}

void PluginScanner::scanPluginFile(const std::string& path) {
    // 1. Dynamic Load
#if defined(_WIN32)
    HMODULE handle = LoadLibraryA(path.c_str());
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!handle) {
        return; // skip unloadable files silently
    }

    // 2. Get Factory
    using GetFactoryFunc = Steinberg::IPluginFactory* (*)();
#if defined(_WIN32)
    GetFactoryFunc getFactory = (GetFactoryFunc)GetProcAddress(handle, "GetPluginFactory");
#else
    GetFactoryFunc getFactory = (GetFactoryFunc)dlsym(handle, "GetPluginFactory");
#endif
    if (!getFactory) {
#if defined(_WIN32)
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return;
    }

    Steinberg::IPluginFactory* factory = getFactory();
    if (!factory) {
#if defined(_WIN32)
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return;
    }
    factory->addRef();

    // Query Vendor from Factory Info
    std::string factoryVendor = "Unknown Vendor";
    Steinberg::PFactoryInfo factoryInfo;
    if (factory->getFactoryInfo(&factoryInfo) == Steinberg::kResultOk) {
        factoryVendor = factoryInfo.vendor;
    }

    // Query IPluginFactory2 for enriched info
    Steinberg::IPluginFactory2* factory2 = nullptr;
    factory->queryInterface(Steinberg::IPluginFactory2::iid, (void**)&factory2);

    // 3. Loop classes
    Steinberg::int32 count = factory->countClasses();
    for (Steinberg::int32 i = 0; i < count; ++i) {
        Steinberg::PClassInfo classInfo;
        if (factory->getClassInfo(i, &classInfo) == Steinberg::kResultOk) {
            // We only host audio effects or instruments (kVstAudioEffectClass)
            if (std::strcmp(classInfo.category, kVstAudioEffectClass) != 0) {
                continue;
            }

            PluginDescriptor desc;
            desc.format = "VST3";
            desc.modulePath = path;
            desc.classId = tuidToString(classInfo.cid);
            desc.name = classInfo.name;
            desc.vendor = factoryVendor;
            desc.category = classInfo.category;

            // Load subcategory and other attributes if Factory2 is supported
            if (factory2) {
                Steinberg::PClassInfo2 classInfo2;
                if (factory2->getClassInfo2(i, &classInfo2) == Steinberg::kResultOk) {
                    desc.subcategories = classInfo2.subCategories;
                    desc.version = classInfo2.version;
                    if (std::strlen(classInfo2.vendor) > 0) {
                        desc.vendor = classInfo2.vendor;
                    }
                }
            }

            // Categorize as effect vs instrument
            std::string subcats = lowercase(desc.subcategories);
            if (subcats.find("instrument") != std::string::npos ||
                subcats.find("synth") != std::string::npos ||
                subcats.find("sampler") != std::string::npos ||
                subcats.find("generator") != std::string::npos) {
                desc.appearsInstrument = true;
                desc.hasEventInput = true;
                desc.hasAudioOutput = true;
            } else {
                desc.appearsEffect = true;
                desc.hasAudioInput = true;
                desc.hasAudioOutput = true;
            }

            // Add to catalog
            catalog.addDescriptor(desc);
        }
    }

    if (factory2) factory2->release();
    factory->release();
#if defined(_WIN32)
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
}

} // namespace ifrit
