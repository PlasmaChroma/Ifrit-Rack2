#include "PluginBrowserOverlay.hpp"

using namespace rack;

namespace ifrit {

// List Item Class inside overlay
class PluginBrowserItem : public OpaqueWidget {
public:
    PluginBrowserItem(PluginHostController* ctrl, const PluginDescriptor& desc, std::function<void()> onClick) 
        : controller(ctrl), descriptor(desc), callback(onClick) {
        box.size = Vec(110, 22);
    }

    void draw(const DrawArgs& args) override {
        bool hovered = (APP->event->hoveredWidget == this);
        
        nvgSave(args.vg);
        
        // Background on hover
        if (hovered) {
            nvgBeginPath(args.vg);
            nvgRoundedRect(args.vg, 0.0f, 0.0f, box.size.x, box.size.y, 2.0f);
            nvgFillColor(args.vg, nvgRGBA(0, 255, 204, 40));
            nvgFill(args.vg);
        }

        // Plugin Name Text
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFontSize(args.vg, 9.0f);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, hovered ? nvgRGBA(255, 255, 255, 245) : nvgRGBA(200, 210, 220, 200));
        
        std::string name = descriptor.name;
        if (name.length() > 15) {
            name = name.substr(0, 13) + "..";
        }
        nvgText(args.vg, 6.0f, box.size.y / 2.0f, name.c_str(), nullptr);

        // VST3 badge
        nvgFontSize(args.vg, 7.0f);
        nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, nvgRGBA(0, 255, 204, 120));
        nvgText(args.vg, box.size.x - 6.0f, box.size.y / 2.0f, "VST3", nullptr);

        nvgRestore(args.vg);
    }

    void onButton(const ButtonEvent& e) override {
        OpaqueWidget::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            controller->loadPluginAsync(descriptor);
            if (callback) {
                callback();
            }
            e.consume(this);
        }
    }

private:
    PluginHostController* controller;
    PluginDescriptor descriptor;
    std::function<void()> callback;
};

// Unload Item Class
class UnloadPluginItem : public OpaqueWidget {
public:
    UnloadPluginItem(PluginHostController* ctrl, std::function<void()> onClick) 
        : controller(ctrl), callback(onClick) {
        box.size = Vec(110, 22);
    }

    void draw(const DrawArgs& args) override {
        bool hovered = (APP->event->hoveredWidget == this);
        nvgSave(args.vg);
        
        if (hovered) {
            nvgBeginPath(args.vg);
            nvgRoundedRect(args.vg, 0.0f, 0.0f, box.size.x, box.size.y, 2.0f);
            nvgFillColor(args.vg, nvgRGBA(255, 100, 100, 40));
            nvgFill(args.vg);
        }

        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFontSize(args.vg, 9.0f);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, hovered ? nvgRGBA(255, 150, 150, 245) : nvgRGBA(230, 160, 160, 180));
        nvgText(args.vg, 6.0f, box.size.y / 2.0f, "[ Unload Plugin ]", nullptr);

        nvgRestore(args.vg);
    }

    void onButton(const ButtonEvent& e) override {
        OpaqueWidget::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            controller->unloadPluginAsync();
            if (callback) {
                callback();
            }
            e.consume(this);
        }
    }

private:
    PluginHostController* controller;
    std::function<void()> callback;
};

// Main Browser Overlay Implementation
PluginBrowserOverlay::PluginBrowserOverlay(PluginHostController* ctrl, bool instOnly) 
    : controller(ctrl), instrumentsOnly(instOnly) {
    // Keep the browser inside the 150 px-wide module panel.
    box.size = Vec(140, 310);

    // Search Field
    searchField = new SearchField();
    searchField->box.pos = Vec(10, 8);
    searchField->box.size = Vec(120, 20);
    searchField->onTextChange = [this](std::string text) {
        currentQuery = text;
        rebuildList();
    };
    addChild(searchField);

    // Scroll Widget
    scrollWidget = new ScrollWidget();
    scrollWidget->box.pos = Vec(10, 34);
    scrollWidget->box.size = Vec(120, 240);
    scrollWidget->container->box.size = Vec(110, 0);
    addChild(scrollWidget);

    container = scrollWidget->container;

    rebuildList();
}

void PluginBrowserOverlay::step() {
    OpaqueWidget::step();
}

void PluginBrowserOverlay::draw(const DrawArgs& args) {
    nvgSave(args.vg);

    // Rounded rectangle panel background
    nvgBeginPath(args.vg);
    nvgRoundedRect(args.vg, 0.0f, 0.0f, box.size.x, box.size.y, 6.0f);
    nvgFillColor(args.vg, nvgRGBA(20, 25, 30, 245));
    nvgFill(args.vg);

    // Outer cyan highlight border
    nvgBeginPath(args.vg);
    nvgRoundedRect(args.vg, 0.0f, 0.0f, box.size.x, box.size.y, 6.0f);
    nvgStrokeColor(args.vg, nvgRGBA(0, 255, 204, 100));
    nvgStrokeWidth(args.vg, 1.2f);
    nvgStroke(args.vg);

    nvgRestore(args.vg);

    OpaqueWidget::draw(args);
}

void PluginBrowserOverlay::rebuildList() {
    container->clearChildren();

    float y = 2.0f;

    // 1. Add Unload item at top
    UnloadPluginItem* unloadItem = new UnloadPluginItem(controller, [this]() {
        if (onClose) onClose();
    });
    unloadItem->box.pos = Vec(0, y);
    container->addChild(unloadItem);
    y += 24.0f;

    // 2. Add filtered plugins from catalog
    // VST-FX scans effects; VST-Instrument scans instruments
    bool effects = !instrumentsOnly;
    bool instruments = instrumentsOnly;

    auto list = controller->getCatalog().search(currentQuery, effects, instruments);
    for (const auto& desc : list) {
        PluginBrowserItem* item = new PluginBrowserItem(controller, desc, [this]() {
            if (onClose) onClose();
        });
        item->box.pos = Vec(0, y);
        container->addChild(item);
        y += 24.0f;
    }

    container->box.size.y = y;
}

void PluginBrowserOverlay::onButton(const ButtonEvent& e) {
    OpaqueWidget::onButton(e);
    if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
        // If clicked outside the overlay boundaries, close it
        if (!searchField->box.contains(e.pos) && !scrollWidget->box.contains(e.pos)) {
            if (onClose) {
                onClose();
            }
            e.consume(this);
        }
    }
}

void PluginBrowserOverlay::onHoverKey(const HoverKeyEvent& e) {
    OpaqueWidget::onHoverKey(e);
}

} // namespace ifrit
