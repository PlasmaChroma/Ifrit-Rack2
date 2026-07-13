#include "PluginParameterSlotWidget.hpp"

using namespace rack;

namespace ifrit {

PluginParameterSlotWidget::PluginParameterSlotWidget(PluginHostController* ctrl, int idx) 
    : controller(ctrl), slotIndex(idx) {
    box.size = Vec(38, 28);

    mapButton = new MapButton(ctrl, idx);
    mapButton->box.pos = Vec(13, 0);
    addChild(mapButton);
}

void PluginParameterSlotWidget::step() {
    Widget::step();
}

void PluginParameterSlotWidget::draw(const DrawArgs& args) {
    ParameterMapping& map = controller->getMapping(slotIndex);
    bool learning = (controller->getLearnSlot() == slotIndex);

    std::string labelText = "---";
    NVGcolor textColor = nvgRGBA(140, 150, 160, 160);

    if (learning) {
        labelText = "LEARN";
        textColor = nvgRGBA(0, 255, 204, 255);
    } else if (map.assigned) {
        labelText = map.cachedShortTitle;
        if (labelText.empty()) {
            labelText = map.cachedTitle;
        }
        textColor = nvgRGBA(255, 255, 255, 220);
    }

    // Limit label length for UI
    if (labelText.length() > 7) {
        labelText = labelText.substr(0, 6) + ".";
    }

    nvgSave(args.vg);

    // Draw label text
    nvgFontFaceId(args.vg, APP->window->uiFont->handle);
    nvgFontSize(args.vg, 7.5f);
    nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    nvgFillColor(args.vg, textColor);
    nvgText(args.vg, box.size.x / 2.0f, 15.0f, labelText.c_str(), nullptr);

    nvgRestore(args.vg);

    Widget::draw(args);
}

// MapButton Implementation
PluginParameterSlotWidget::MapButton::MapButton(PluginHostController* ctrl, int idx) 
    : controller(ctrl), slotIndex(idx) {
    box.size = Vec(12, 12);
}

void PluginParameterSlotWidget::MapButton::draw(const DrawArgs& args) {
    ParameterMapping& map = controller->getMapping(slotIndex);
    bool learning = (controller->getLearnSlot() == slotIndex);

    NVGcolor color = nvgRGBA(60, 65, 70, 255); // unmapped dark grey
    bool drawPlus = true;

    if (learning) {
        flashCounter = (flashCounter + 1) % 60;
        if (flashCounter < 30) {
            color = nvgRGBA(0, 255, 204, 255); // flashing active cyan
        }
        drawPlus = false;
    } else if (map.assigned) {
        color = nvgRGBA(0, 255, 204, 255); // solid mapped cyan
        drawPlus = false;
    }

    nvgSave(args.vg);

    // Button circle
    nvgBeginPath(args.vg);
    nvgCircle(args.vg, box.size.x / 2.0f, box.size.y / 2.0f, 5.0f);
    nvgFillColor(args.vg, color);
    nvgFill(args.vg);

    // Subtle outline
    nvgBeginPath(args.vg);
    nvgCircle(args.vg, box.size.x / 2.0f, box.size.y / 2.0f, 5.0f);
    nvgStrokeColor(args.vg, map.assigned ? nvgRGBA(255, 255, 255, 100) : nvgRGBA(0, 0, 0, 80));
    nvgStrokeWidth(args.vg, 1.0f);
    nvgStroke(args.vg);

    // Draw tiny plus inside if unmapped
    if (drawPlus) {
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 6.0f, 4.0f);
        nvgLineTo(args.vg, 6.0f, 8.0f);
        nvgMoveTo(args.vg, 4.0f, 6.0f);
        nvgLineTo(args.vg, 8.0f, 6.0f);
        nvgStrokeColor(args.vg, nvgRGBA(140, 150, 160, 200));
        nvgStrokeWidth(args.vg, 1.0f);
        nvgStroke(args.vg);
    }

    nvgRestore(args.vg);
}

void PluginParameterSlotWidget::MapButton::onButton(const ButtonEvent& e) {
    OpaqueWidget::onButton(e);
    if (e.action != GLFW_PRESS) {
        return;
    }

    if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
        if (controller->getLearnSlot() == slotIndex) {
            controller->cancelLearn();
        } else {
            controller->setLearnSlot(slotIndex);
        }
        e.consume(this);
    } else if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
        // Clear mapping on right click
        controller->clearMapping(slotIndex);
        e.consume(this);
    }
}

} // namespace ifrit
