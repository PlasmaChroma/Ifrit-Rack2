#include "host/PluginHostController.hpp"
#include "host/AudioBlockBridge.hpp"
#include <rack.hpp>
#include "ifrit_plugin.hpp"
#include "widgets/PluginIdentityDisplay.hpp"
#include "widgets/PluginEditorEyeButton.hpp"
#include "widgets/PluginParameterSlotWidget.hpp"
#include "widgets/PluginBrowserOverlay.hpp"
#include <sstream>
#include <iomanip>
#include <iostream>
#include <algorithm>

using namespace rack;

namespace ifrit {

// Hex conversion helper functions
static std::string bytesToHex(const std::vector<uint8_t>& bytes) {
    std::stringstream ss;
    for (uint8_t b : bytes) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    }
    return ss.str();
}

static std::vector<uint8_t> hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < hex.length(); i += 2) {
        if (i + 1 >= hex.length()) break;
        std::string byteString = hex.substr(i, 2);
        uint8_t byte = (uint8_t)strtol(byteString.c_str(), nullptr, 16);
        bytes.push_back(byte);
    }
    return bytes;
}

struct VstFxModule : Module {
    enum ParamId {
        BYPASS_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        AUDIO_LEFT_INPUT,
        AUDIO_RIGHT_INPUT,
        PARAM_CV_INPUT_1,
        // ... 16 inputs
        INPUTS_LEN = PARAM_CV_INPUT_1 + 16
    };
    enum OutputId {
        AUDIO_LEFT_OUTPUT,
        AUDIO_RIGHT_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        LIGHTS_LEN
    };

    PluginHostController controller;
    AudioBlockBridge bridge;

    bool bypassed = false;

    // Crossfading for bypass transitions
    float dryWetRatio = 1.0f; // 1.0 = wet, 0.0 = dry/bypassed

    VstFxModule() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(BYPASS_PARAM, 0.f, 1.f, 0.f, "Bypass");
        
        bridge.configure(128, 2, 2);
    }

    ~VstFxModule() override {
        controller.closeEditor();
    }

    void process(const ProcessArgs& args) override {
        // Handle bypass state
        bypassed = (params[BYPASS_PARAM].getValue() > 0.5f);

        // Slow crossfade to avoid hard discontinuities
        if (bypassed) {
            dryWetRatio = std::max(0.0f, dryWetRatio - (1.0f / 32.f));
        } else {
            dryWetRatio = std::min(1.0f, dryWetRatio + (1.0f / 32.f));
        }

        float inL = inputs[AUDIO_LEFT_INPUT].getVoltage();
        float inR = inputs[AUDIO_RIGHT_INPUT].isConnected() ? inputs[AUDIO_RIGHT_INPUT].getVoltage() : inL;

        // If no plugin loaded, copy input to output directly
        if (!controller.isLoaded()) {
            outputs[AUDIO_LEFT_OUTPUT].setVoltage(inL);
            outputs[AUDIO_RIGHT_OUTPUT].setVoltage(inR);
            return;
        }

        // Push input sample scaled to +/- 1.0
        bridge.pushInputSample(inL / 5.0f, inR / 5.0f);

        if (bridge.blockReady()) {
            // Update CV automation from mapped slots
            for (int i = 0; i < 16; ++i) {
                if (inputs[PARAM_CV_INPUT_1 + i].isConnected()) {
                    float cv = inputs[PARAM_CV_INPUT_1 + i].getVoltage();
                    controller.updateAutomation(i, cv, 0);
                }
            }

            PluginProcessBlock block = bridge.makeProcessBlock();
            controller.processAudio(block);
            bridge.commitProcessedBlock();
        }

        StereoSample out = bridge.popOutputSample();
        
        // Scale back to VCV Rack standard voltage (+/- 5V nominal, clamped at +/- 10V)
        float wetL = clamp(out.left * 5.0f, -10.f, 10.f);
        float wetR = clamp(out.right * 5.0f, -10.f, 10.f);

        // Mix dry/wet for smooth bypass crossfading
        float finalL = (dryWetRatio * wetL) + ((1.0f - dryWetRatio) * inL);
        float finalR = (dryWetRatio * wetR) + ((1.0f - dryWetRatio) * inR);

        outputs[AUDIO_LEFT_OUTPUT].setVoltage(finalL);
        outputs[AUDIO_RIGHT_OUTPUT].setVoltage(finalR);
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();

        // Save mappings
        json_t* mappingsJ = json_array();
        for (int i = 0; i < 16; ++i) {
            json_t* slotJ = json_object();
            const auto& map = controller.getMapping(i);
            json_object_set_new(slotJ, "assigned", json_boolean(map.assigned));
            json_object_set_new(slotJ, "parameterId", json_integer(map.parameterId));
            json_object_set_new(slotJ, "title", json_string(map.cachedTitle.c_str()));
            json_object_set_new(slotJ, "shortTitle", json_string(map.cachedShortTitle.c_str()));
            json_object_set_new(slotJ, "unit", json_string(map.cachedUnit.c_str()));
            json_object_set_new(slotJ, "stepCount", json_integer(map.cachedStepCount));
            json_object_set_new(slotJ, "flags", json_integer(map.cachedFlags));
            json_array_append_new(mappingsJ, slotJ);
        }
        json_object_set_new(rootJ, "mappings", mappingsJ);

        // Save active plugin metadata
        json_t* pluginJ = json_object();
        auto* inst = controller.getActiveInstance();
        if (inst) {
            json_object_set_new(pluginJ, "loaded", json_boolean(true));
            json_object_set_new(pluginJ, "name", json_string(inst->descriptor.name.c_str()));
            json_object_set_new(pluginJ, "vendor", json_string(inst->descriptor.vendor.c_str()));
            json_object_set_new(pluginJ, "modulePath", json_string(inst->descriptor.modulePath.c_str()));
            json_object_set_new(pluginJ, "classId", json_string(inst->descriptor.classId.c_str()));
        } else {
            json_object_set_new(pluginJ, "loaded", json_boolean(false));
        }
        json_object_set_new(rootJ, "plugin", pluginJ);

        // Save state blobs as hex strings
        PluginStateBlob blob = controller.captureState();
        json_object_set_new(rootJ, "componentStateHex", json_string(bytesToHex(blob.componentState).c_str()));
        json_object_set_new(rootJ, "controllerStateHex", json_string(bytesToHex(blob.controllerState).c_str()));

        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        // Load mappings
        json_t* mappingsJ = json_object_get(rootJ, "mappings");
        if (json_is_array(mappingsJ)) {
            size_t idx;
            json_t* slotJ;
            json_array_foreach(mappingsJ, idx, slotJ) {
                if (idx < 16) {
                    auto& map = controller.getMapping((int)idx);
                    json_t* assignedJ = json_object_get(slotJ, "assigned");
                    if (json_is_boolean(assignedJ)) map.assigned = json_is_true(assignedJ);

                    json_t* paramIdJ = json_object_get(slotJ, "parameterId");
                    if (json_is_integer(paramIdJ)) map.parameterId = (uint32_t)json_integer_value(paramIdJ);

                    json_t* titleJ = json_object_get(slotJ, "title");
                    if (json_is_string(titleJ)) map.cachedTitle = json_string_value(titleJ);

                    json_t* shortTitleJ = json_object_get(slotJ, "shortTitle");
                    if (json_is_string(shortTitleJ)) map.cachedShortTitle = json_string_value(shortTitleJ);

                    json_t* unitJ = json_object_get(slotJ, "unit");
                    if (json_is_string(unitJ)) map.cachedUnit = json_string_value(unitJ);

                    json_t* stepCountJ = json_object_get(slotJ, "stepCount");
                    if (json_is_integer(stepCountJ)) map.cachedStepCount = (int32_t)json_integer_value(stepCountJ);

                    json_t* flagsJ = json_object_get(slotJ, "flags");
                    if (json_is_integer(flagsJ)) map.cachedFlags = (uint32_t)json_integer_value(flagsJ);
                }
            }
        }

        // Load plugin
        json_t* pluginJ = json_object_get(rootJ, "plugin");
        if (json_is_object(pluginJ)) {
            json_t* loadedJ = json_object_get(pluginJ, "loaded");
            if (json_is_true(loadedJ)) {
                PluginDescriptor desc;
                desc.format = "VST3";
                desc.name = json_string_value(json_object_get(pluginJ, "name"));
                desc.vendor = json_string_value(json_object_get(pluginJ, "vendor"));
                desc.modulePath = json_string_value(json_object_get(pluginJ, "modulePath"));
                desc.classId = json_string_value(json_object_get(pluginJ, "classId"));
                desc.appearsEffect = true;
                desc.hasAudioInput = true;
                desc.hasAudioOutput = true;

                controller.loadPluginAsync(desc);
            }
        }

        // Load state blobs
        json_t* compHexJ = json_object_get(rootJ, "componentStateHex");
        json_t* ctrlHexJ = json_object_get(rootJ, "controllerStateHex");
        PluginStateBlob blob;
        if (json_is_string(compHexJ)) blob.componentState = hexToBytes(json_string_value(compHexJ));
        if (json_is_string(ctrlHexJ)) blob.controllerState = hexToBytes(json_string_value(ctrlHexJ));

        if (!blob.componentState.empty() || !blob.controllerState.empty()) {
            std::thread([this, blob]() {
                int retries = 100;
                while (retries-- > 0 && !controller.isLoaded()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (controller.isLoaded()) {
                    controller.restoreState(blob);
                }
            }).detach();
        }
    }
};

struct VstFxWidget : ModuleWidget {
    PluginBrowserOverlay* browserOverlay = nullptr;

    VstFxWidget(VstFxModule* module) {
        setModule(module);
        
        // 10 HP Width
        setPanel(createPanel(asset::plugin(pluginInstance, "res/VstFx.svg")));

        // Identity display & Editor eye
        PluginIdentityDisplay* display = new PluginIdentityDisplay(module ? &module->controller : nullptr);
        display->box.pos = Vec(15, 20);
        display->onOpenBrowser = [this]() {
            this->openBrowser();
        };
        addChild(display);

        PluginEditorEyeButton* eye = new PluginEditorEyeButton(module ? &module->controller : nullptr);
        eye->box.pos = Vec(112, 27);
        addChild(eye);

        // Bypass switch
        addParam(createParamCentered<SvgSwitch>(Vec(75, 75), module, VstFxModule::BYPASS_PARAM));

        // Audio ports (using PJ301MPort)
        addInput(createInputCentered<PJ301MPort>(Vec(25, 345), module, VstFxModule::AUDIO_LEFT_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(55, 345), module, VstFxModule::AUDIO_RIGHT_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(Vec(95, 345), module, VstFxModule::AUDIO_LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(Vec(125, 345), module, VstFxModule::AUDIO_RIGHT_OUTPUT));

        // Mapping slots grid (2 columns of 8 rows)
        for (int i = 0; i < 8; ++i) {
            // Column 1 (Left)
            addInput(createInputCentered<PJ301MPort>(Vec(25, 105 + i * 28), module, VstFxModule::PARAM_CV_INPUT_1 + i));
            PluginParameterSlotWidget* slot1 = new PluginParameterSlotWidget(module ? &module->controller : nullptr, i);
            slot1->box.pos = Vec(15, 115 + i * 28);
            addChild(slot1);

            // Column 2 (Right)
            addInput(createInputCentered<PJ301MPort>(Vec(95, 105 + i * 28), module, VstFxModule::PARAM_CV_INPUT_1 + 8 + i));
            PluginParameterSlotWidget* slot2 = new PluginParameterSlotWidget(module ? &module->controller : nullptr, 8 + i);
            slot2->box.pos = Vec(85, 115 + i * 28);
            addChild(slot2);
        }
    }

    void step() override {
        ModuleWidget::step();
        VstFxModule* module = dynamic_cast<VstFxModule*>(this->module);
        if (module) {
            module->controller.stepUI();
        }
    }

    void openBrowser() {
        VstFxModule* module = dynamic_cast<VstFxModule*>(this->module);
        if (!module) return;

        if (browserOverlay) {
            removeChild(browserOverlay);
            delete browserOverlay;
            browserOverlay = nullptr;
        }

        browserOverlay = new PluginBrowserOverlay(&module->controller, false);
        browserOverlay->box.pos = Vec(10, 30);
        browserOverlay->onClose = [this]() {
            if (this->browserOverlay) {
                this->removeChild(this->browserOverlay);
                delete this->browserOverlay;
                this->browserOverlay = nullptr;
            }
        };
        addChild(browserOverlay);
    }
};

} // namespace ifrit

Model* modelVstFx = createModel<ifrit::VstFxModule, ifrit::VstFxWidget>("VST-FX");
