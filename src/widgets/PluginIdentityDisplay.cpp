#include "PluginIdentityDisplay.hpp"

using namespace rack;

namespace ifrit {

PluginIdentityDisplay::PluginIdentityDisplay(PluginHostController* ctrl) : controller(ctrl) {
    box.size = Vec(160, 38);
}

void PluginIdentityDisplay::draw(const DrawArgs& args) {
    bool loaded = false;
    bool transitioning = false;
    if (controller) {
        loaded = controller->isLoaded();
        transitioning = controller->isTransitioning();
    }

    std::string nameText = "NO PLUGIN";
    std::string subtitleText = "CLICK TO LOAD";

    if (transitioning) {
        nameText = "LOADING...";
        subtitleText = "PLEASE WAIT";
    } else if (loaded && controller) {
        PluginDescriptor descriptor;
        if (controller->getActiveDescriptor(descriptor)) {
            nameText = descriptor.name;
            subtitleText = descriptor.vendor + " · VST3";
        }
    }

    // Limit lengths for UI rendering
    if (nameText.length() > 22) {
        nameText = nameText.substr(0, 20) + "...";
    }
    if (subtitleText.length() > 27) {
        subtitleText = subtitleText.substr(0, 25) + "...";
    }

    nvgSave(args.vg);

    // Rounded rectangle background
    nvgBeginPath(args.vg);
    nvgRoundedRect(args.vg, 0.0f, 0.0f, box.size.x, box.size.y, 4.0f);
    nvgFillColor(args.vg, nvgRGBA(25, 13, 10, 225));
    nvgFill(args.vg);

    // Ember highlight border
    nvgBeginPath(args.vg);
    nvgRoundedRect(args.vg, 0.0f, 0.0f, box.size.x, box.size.y, 4.0f);
    nvgStrokeColor(args.vg, loaded ? nvgRGBA(255, 92, 28, 100) : nvgRGBA(205, 145, 110, 45));
    nvgStrokeWidth(args.vg, 1.0f);
    nvgStroke(args.vg);

    // Text rendering
    nvgFontFaceId(args.vg, APP->window->uiFont->handle);

    // Plugin Name
    nvgFontSize(args.vg, 11.0f);
    nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    nvgFillColor(args.vg, loaded ? nvgRGBA(255, 242, 224, 240) : nvgRGBA(195, 165, 145, 160));
    nvgText(args.vg, box.size.x / 2.0f, 5.0f, nameText.c_str(), nullptr);

    // Subtitle
    nvgFontSize(args.vg, 8.5f);
    nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_TOP);
    nvgFillColor(args.vg, loaded ? nvgRGBA(255, 116, 45, 190) : nvgRGBA(155, 115, 95, 130));
    nvgText(args.vg, box.size.x / 2.0f, 21.0f, subtitleText.c_str(), nullptr);

    nvgRestore(args.vg);
}

void PluginIdentityDisplay::onButton(const ButtonEvent& e) {
    OpaqueWidget::onButton(e);
    if (!controller) return;
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
