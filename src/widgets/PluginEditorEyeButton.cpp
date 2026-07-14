#include "PluginEditorEyeButton.hpp"
#include <cmath>

using namespace rack;

namespace ifrit {

PluginEditorEyeButton::PluginEditorEyeButton(PluginHostController* ctrl) : controller(ctrl) {
    box.size = Vec(24, 24);
}

void PluginEditorEyeButton::draw(const DrawArgs& args) {
    bool loaded = false;
    bool hasEd = false;
    bool open = false;
    if (controller) {
        loaded = controller->isLoaded();
        hasEd = controller->hasEditor();
        open = controller->isEditorOpen();
    }

    NVGcolor eyeColor;
    if (!loaded || !hasEd) {
        eyeColor = nvgRGBA(80, 80, 80, 255); // Dark/closed/disabled
    } else if (open) {
        eyeColor = nvgRGBA(255, 92, 28, 255); // Hot orange (open/active)
    } else {
        eyeColor = nvgRGBA(205, 132, 92, 255); // Dim ember (available)
    }

    nvgSave(args.vg);

    // Draw eye outline
    nvgBeginPath(args.vg);
    // Outer lid
    nvgMoveTo(args.vg, 2.0f, 12.0f);
    nvgBezierTo(args.vg, 8.0f, 4.0f, 16.0f, 4.0f, 22.0f, 12.0f);
    nvgBezierTo(args.vg, 16.0f, 20.0f, 8.0f, 20.0f, 2.0f, 12.0f);
    nvgStrokeColor(args.vg, eyeColor);
    nvgStrokeWidth(args.vg, 1.5f);
    nvgStroke(args.vg);

    // Pupil
    nvgBeginPath(args.vg);
    nvgCircle(args.vg, 12.0f, 12.0f, 4.0f);
    if (open) {
        nvgFillColor(args.vg, eyeColor);
        nvgFill(args.vg);
    } else {
        nvgStrokeColor(args.vg, eyeColor);
        nvgStrokeWidth(args.vg, 1.2f);
        nvgStroke(args.vg);
    }

    // Iris center reflection
    if (loaded && hasEd) {
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, 10.5f, 10.5f, 1.2f);
        nvgFillColor(args.vg, nvgRGBA(255, 255, 255, 200));
        nvgFill(args.vg);
    }

    nvgRestore(args.vg);
}

void PluginEditorEyeButton::onButton(const ButtonEvent& e) {
    OpaqueWidget::onButton(e);
    if (!controller) return;
    if (e.action != GLFW_PRESS || e.button != GLFW_MOUSE_BUTTON_LEFT) {
        return;
    }

    if (!controller->isLoaded() || !controller->hasEditor()) {
        return;
    }

    if (controller->isEditorOpen()) {
        controller->hideEditor();
    } else {
        // Open parented to 0 (which defaults to a top-level window in our implementation)
        controller->openEditor(nullptr, 100, 100, 640, 480);
    }
    e.consume(this);
}

} // namespace ifrit
