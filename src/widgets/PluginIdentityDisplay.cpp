#include "PluginIdentityDisplay.hpp"

using namespace rack;

namespace ifrit {

PluginIdentityDisplay::PluginIdentityDisplay(PluginHostController* ctrl) : controller(ctrl) {
    box.size = Vec(140, 38);
}

void PluginIdentityDisplay::draw(const DrawArgs& args) {
    bool loaded = controller->isLoaded();
    bool transitioning = controller->isTransitioning();

    std::string nameText = "NO PLUGIN";
    std::string subtitleText = "CLICK TO LOAD";

    if (transitioning) {
        nameText = "LOADING...";
        subtitleText = "PLEASE WAIT";
    } else if (loaded) {
        auto* inst = controller->getActiveInstance();
        if (inst) {
            nameText = inst->descriptor.name;
            subtitleText = inst->descriptor.vendor + " · VST3";
        }
    }

    // Limit lengths for UI rendering
    if (nameText.length() > 18) {
        nameText = nameText.substr(0, 16) + "...";
    }
    if (subtitleText.length() > 24) {
        subtitleText = subtitleText.substr(0, 22) + "...";
    }

    nvgSave(args.vg);

    // Rounded rectangle background
    nvgBeginPath(args.vg);
    nvgRoundedRect(args.vg, 0.0f, 0.0f, box.size.x, box.size.y, 4.0f);
    nvgFillColor(args.vg, nvgRGBA(15, 20, 25, 220));
    nvgFill(args.vg);

    // Neon highlight border
    nvgBeginPath(args.vg);
    nvgRoundedRect(args.vg, 0.0f, 0.0f, box.size.x, box.size.y, 4.0f);
    nvgStrokeColor(args.vg, loaded ? nvgRGBA(0, 255, 204, 80) : nvgRGBA(180, 200, 220, 40));
    nvgStrokeWidth(args.vg, 1.0f);
    nvgStroke(args.vg);

    // Text rendering
    nvgFontFaceId(args.vg, APP->window->uiFont->handle);

    // Plugin Name
    nvgFontSize(args.vg, 11.0f);
    nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    nvgFillColor(args.vg, loaded ? nvgRGBA(255, 255, 255, 240) : nvgRGBA(180, 190, 200, 160));
    nvgText(args.vg, box.size.x / 2.0f, 5.0f, nameText.c_str(), nullptr);

    // Subtitle
    nvgFontSize(args.vg, 8.5f);
    nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    nvgFillColor(args.vg, loaded ? nvgRGBA(0, 255, 204, 160) : nvgRGBA(140, 150, 160, 120));
    nvgText(args.vg, box.size.x / 2.0f, 21.0f, subtitleText.c_str(), nullptr);

    nvgRestore(args.vg);
}

void PluginIdentityDisplay::onButton(const ButtonEvent& e) {
    OpaqueWidget::onButton(e);
    if (e.action != GLFW_PRESS) {
        return;
    }

    if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
        if (onOpenBrowser) {
            onOpenBrowser();
        }
        e.consume(this);
    } else if (e.button == GLFW_MOUSE_BUTTON_RIGHT) {
        // Trigger right-click context menu with Unload option
        createMenu();
        e.consume(this);
    }
}

} // namespace ifrit
