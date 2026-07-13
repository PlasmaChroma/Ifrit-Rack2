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
#include <cmath>

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

struct VstInstrumentModule : Module {
    enum ParamId {
        BYPASS_PARAM,
        PANIC_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        VOCT_INPUT,
        GATE_INPUT,
        VELOCITY_INPUT,
        PRESSURE_INPUT,
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

    // Polyphonic Voice tracking
    struct ActiveNote {
        int note = -1;
        int noteId = -1;
    };
    ActiveNote activeNotes[16];
    float lastPressure[16];
    int nextNoteId = 1;

    std::vector<PluginEvent> blockEvents;
    bool bypassed = false;
    float dryWetRatio = 1.0f; // 1.0 = wet, 0.0 = bypassed (silent)

    VstInstrumentModule() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(BYPASS_PARAM, 0.f, 1.f, 0.f, "Bypass");
        configParam(PANIC_PARAM, 0.f, 1.f, 0.f, "Panic");

        bridge.configure(128, 0, 2); // 0 audio inputs, 2 audio outputs

        for (int i = 0; i < 16; ++i) {
            activeNotes[i] = {-1, -1};
            lastPressure[i] = 0.0f;
        }
    }

    ~VstInstrumentModule() override {
        controller.closeEditor();
    }

    void handlePanic() {
        for (int i = 0; i < 16; ++i) {
            if (activeNotes[i].note != -1) {
                blockEvents.push_back({EventType::NoteOff, 0, (int16_t)activeNotes[i].note, activeNotes[i].noteId, 0.0f});
                activeNotes[i] = {-1, -1};
            }
            lastPressure[i] = 0.0f;
        }
    }

    void process(const ProcessArgs& args) override {
        // Panic trigger
        if (params[PANIC_PARAM].getValue() > 0.5f) {
            handlePanic();
            params[PANIC_PARAM].setValue(0.0f);
        }

        // Bypass crossfading
        bypassed = (params[BYPASS_PARAM].getValue() > 0.5f);
        if (bypassed) {
            dryWetRatio = std::max(0.0f, dryWetRatio - (1.0f / 32.f));
        } else {
            dryWetRatio = std::min(1.0f, dryWetRatio + (1.0f / 32.f));
        }

        // Determine active voice lanes
        int maxChannels = 1;
        if (inputs[GATE_INPUT].isConnected()) {
            maxChannels = std::max(inputs[GATE_INPUT].getChannels(), inputs[VOCT_INPUT].getChannels());
        }
        maxChannels = clamp(maxChannels, 1, 16);

        // Process polyphonic note events from Rack inputs
        if (inputs[GATE_INPUT].isConnected() && !bypassed) {
            for (int i = 0; i < maxChannels; ++i) {
                float gateVal = inputs[GATE_INPUT].getPolyVoltage(i);
                bool isHigh = (gateVal > 1.0f);
                bool wasHigh = (activeNotes[i].note != -1);

                if (isHigh && !wasHigh) {
                    // Note On
                    int note = std::lround(inputs[VOCT_INPUT].getPolyVoltage(i) * 12.0f) + 60;
                    note = clamp(note, 0, 127);
                    float vel = inputs[VELOCITY_INPUT].isConnected() ? clamp(inputs[VELOCITY_INPUT].getPolyVoltage(i) / 10.0f, 0.0f, 1.0f) : 0.7874f;
                    int noteId = nextNoteId++;

                    activeNotes[i] = {note, noteId};
                    blockEvents.push_back({EventType::NoteOn, (int32_t)bridge.getWriteIndex(), (int16_t)note, noteId, vel});
                } 
                else if (!isHigh && wasHigh) {
                    // Note Off
                    blockEvents.push_back({EventType::NoteOff, (int32_t)bridge.getWriteIndex(), (int16_t)activeNotes[i].note, activeNotes[i].noteId, 0.0f});
                    activeNotes[i] = {-1, -1};
                } 
                else if (isHigh && wasHigh) {
                    // Quantized pitch change while gate is high (chromatic retrigger)
                    int newNote = std::lround(inputs[VOCT_INPUT].getPolyVoltage(i) * 12.0f) + 60;
                    newNote = clamp(newNote, 0, 127);
                    if (newNote != activeNotes[i].note) {
                        blockEvents.push_back({EventType::NoteOff, (int32_t)bridge.getWriteIndex(), (int16_t)activeNotes[i].note, activeNotes[i].noteId, 0.0f});
                        
                        int noteId = nextNoteId++;
                        float vel = inputs[VELOCITY_INPUT].isConnected() ? clamp(inputs[VELOCITY_INPUT].getPolyVoltage(i) / 10.0f, 0.0f, 1.0f) : 0.7874f;
                        activeNotes[i] = {newNote, noteId};
                        blockEvents.push_back({EventType::NoteOn, (int32_t)bridge.getWriteIndex(), (int16_t)newNote, noteId, vel});
                    }

                    // Polyphonic Pressure (aftertouch)
                    if (inputs[PRESSURE_INPUT].isConnected()) {
                        float press = clamp(inputs[PRESSURE_INPUT].getPolyVoltage(i) / 10.0f, 0.0f, 1.0f);
                        if (std::abs(press - lastPressure[i]) > 0.02f) {
                            blockEvents.push_back({EventType::PolyPressure, (int32_t)bridge.getWriteIndex(), (int16_t)activeNotes[i].note, activeNotes[i].noteId, press});
                            lastPressure[i] = press;
                        }
                    }
                }
            }
        }

        // If no plugin loaded, output silence
        if (!controller.isLoaded()) {
            outputs[AUDIO_LEFT_OUTPUT].setVoltage(0.0f);
            outputs[AUDIO_RIGHT_OUTPUT].setVoltage(0.0f);
            return;
        }

        // Push silence input scaled (instruments consume notes, not inputs)
        bridge.pushInputSample(0.0f, 0.0f);

        if (bridge.blockReady()) {
            // Update CV automation from mapped slots
            for (int i = 0; i < 16; ++i) {
                if (inputs[PARAM_CV_INPUT_1 + i].isConnected()) {
                    float cv = inputs[PARAM_CV_INPUT_1 + i].getVoltage();
                    controller.updateAutomation(i, cv, 0);
                }
            }

            PluginProcessBlock block = bridge.makeProcessBlock();
            block.inputEvents = blockEvents;
            
            controller.processAudio(block);
            
            blockEvents.clear();
            bridge.commitProcessedBlock();
        }

        StereoSample out = bridge.popOutputSample();
        
        // Scale and output to Rack L/R ports (dryWetRatio handles smooth bypass mute)
        float wetL = clamp(out.left * 5.0f, -10.f, 10.f);
        float wetR = clamp(out.right * 5.0f, -10.f, 10.f);

        outputs[AUDIO_LEFT_OUTPUT].setVoltage(dryWetRatio * wetL);
        outputs[AUDIO_RIGHT_OUTPUT].setVoltage(dryWetRatio * wetR);
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
                desc.appearsInstrument = true;
                desc.hasEventInput = true;
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

struct VstInstrumentWidget : ModuleWidget {
    PluginBrowserOverlay* browserOverlay = nullptr;

    VstInstrumentWidget(VstInstrumentModule* module) {
        setModule(module);
        
        // 10 HP Width
        setPanel(createPanel(asset::plugin(pluginInstance, "res/VstInstrument.svg")));

        // Identity display & Editor eye
        PluginIdentityDisplay* display = new PluginIdentityDisplay(&module->controller);
        display->box.pos = Vec(15, 20);
        display->onOpenBrowser = [this]() {
            this->openBrowser();
        };
        addChild(display);

        PluginEditorEyeButton* eye = new PluginEditorEyeButton(&module->controller);
        eye->box.pos = Vec(112, 27);
        addChild(eye);

        // Bypass & Panic controls
        addParam(createParamCentered<SvgSwitch>(Vec(45, 75), module, VstInstrumentModule::BYPASS_PARAM));
        addParam(createParamCentered<SvgSwitch>(Vec(105, 75), module, VstInstrumentModule::PANIC_PARAM));

        // Audio & Poly CV ports (18-step spacing centered)
        addInput(createInputCentered<PJ301MPort>(Vec(18, 345), module, VstInstrumentModule::VOCT_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(42, 345), module, VstInstrumentModule::GATE_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(66, 345), module, VstInstrumentModule::VELOCITY_INPUT));
        addInput(createInputCentered<PJ301MPort>(Vec(90, 345), module, VstInstrumentModule::PRESSURE_INPUT));
        
        addOutput(createOutputCentered<PJ301MPort>(Vec(114, 345), module, VstInstrumentModule::AUDIO_LEFT_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(Vec(138, 345), module, VstInstrumentModule::AUDIO_RIGHT_OUTPUT));

        // Mapping slots grid (2 columns of 8 rows)
        for (int i = 0; i < 8; ++i) {
            // Column 1 (Left)
            addInput(createInputCentered<PJ301MPort>(Vec(25, 105 + i * 28), module, VstInstrumentModule::PARAM_CV_INPUT_1 + i));
            PluginParameterSlotWidget* slot1 = new PluginParameterSlotWidget(&module->controller, i);
            slot1->box.pos = Vec(15, 115 + i * 28);
            addChild(slot1);

            // Column 2 (Right)
            addInput(createInputCentered<PJ301MPort>(Vec(95, 105 + i * 28), module, VstInstrumentModule::PARAM_CV_INPUT_1 + 8 + i));
            PluginParameterSlotWidget* slot2 = new PluginParameterSlotWidget(&module->controller, 8 + i);
            slot2->box.pos = Vec(85, 115 + i * 28);
            addChild(slot2);
        }
    }

    void step() override {
        ModuleWidget::step();
        VstInstrumentModule* module = dynamic_cast<VstInstrumentModule*>(this->module);
        if (module) {
            module->controller.stepUI();
        }
    }

    void openBrowser() {
        VstInstrumentModule* module = dynamic_cast<VstInstrumentModule*>(this->module);
        if (!module) return;

        if (browserOverlay) {
            removeChild(browserOverlay);
            delete browserOverlay;
            browserOverlay = nullptr;
        }

        browserOverlay = new PluginBrowserOverlay(&module->controller, true); // true = instruments only
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

Model* modelVstInstrument = createModel<VstInstrumentModule, VstInstrumentWidget>("VST-Instrument");

} // namespace ifrit
