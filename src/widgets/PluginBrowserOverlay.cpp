#include "PluginBrowserOverlay.hpp"

using namespace rack;

namespace ifrit {

// List Item Class inside overlay
class PluginBrowserItem : public OpaqueWidget {
public:
    PluginBrowserItem(PluginHostController* ctrl, const PluginDescriptor& desc, std::function<void()> onClick) 
        : controller(ctrl), descriptor(desc), callback(onClick) {
        box.size = Vec(170, 22);
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
        if (name.length() > 25) {
            name = name.substr(0, 23) + "..";
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

// Explicitly closes the browser without changing the active plugin.
class CancelPluginBrowserItem : public OpaqueWidget {
public:
    explicit CancelPluginBrowserItem(std::function<void()> onClick) : callback(onClick) {
        box.size = Vec(170, 22);
    }

    void draw(const DrawArgs& args) override {
        const bool hovered = APP->event->hoveredWidget == this;
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFontSize(args.vg, 9.0f);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, hovered ? nvgRGBA(255, 255, 255, 245) : nvgRGBA(200, 210, 220, 200));
        nvgText(args.vg, 6.0f, box.size.y / 2.0f, "[ Cancel ]", nullptr);
    }

    void onButton(const ButtonEvent& e) override {
        OpaqueWidget::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            if (callback) callback();
            e.consume(this);
        }
    }

private:
    std::function<void()> callback;
};

// Unload Item Class
class UnloadPluginItem : public OpaqueWidget {
public:
    UnloadPluginItem(PluginHostController* ctrl, std::function<void()> onClick) 
        : controller(ctrl), callback(onClick) {
        box.size = Vec(170, 22);
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
    box.size = Vec(200, 310);

    // Search Field
    searchField = new SearchField();
    searchField->box.pos = Vec(10, 8);
    searchField->box.size = Vec(180, 20);
    searchField->onTextChange = [this](std::string text) {
        currentQuery = text;
        rebuildList();
    };
    addChild(searchField);

    // Scroll Widget
    scrollWidget = new ScrollWidget();
    scrollWidget->box.pos = Vec(10, 34);
    scrollWidget->box.size = Vec(180, 240);
    scrollWidget->container->box.size = Vec(170, 0);
    addChild(scrollWidget);

    container = scrollWidget->container;

    // Discovery runs asynchronously because loading third-party VST3 bundles
    // can be slow. The list is rebuilt when it completes in step().
    // Keep an in-flight scan alive when the browser is closed/reopened. Joining
    // it here would stall Rack's UI if a third-party plugin is slow to load.
    if (!controller->getScanner().isScanning()) {
        controller->startScan();
    }
    scanWasActive = controller->getScanner().isScanning();
    rebuildList();
}

void PluginBrowserOverlay::step() {
    OpaqueWidget::step();

    const bool scanning = controller->getScanner().isScanning();
    if (scanWasActive && !scanning) {
        rebuildList();
    }
    scanWasActive = scanning;
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

    nvgFontFaceId(args.vg, APP->window->uiFont->handle);
    nvgFontSize(args.vg, 8.0f);
    nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    const bool scanning = controller && controller->getScanner().isScanning();
    const size_t count = controller ? controller->getCatalog().size() : 0;
    const std::string status = scanning
        ? "Scanning VST3 plugins..."
        : (count == 0 ? "No VST3 plugins found" : std::to_string(count) + " plugins found");
    nvgFillColor(args.vg, scanning ? nvgRGBA(0, 255, 204, 190) : nvgRGBA(160, 175, 185, 180));
    nvgText(args.vg, box.size.x / 2.0f, box.size.y - 18.0f, status.c_str(), nullptr);

    nvgRestore(args.vg);

    OpaqueWidget::draw(args);
}

void PluginBrowserOverlay::rebuildList() {
    container->clearChildren();

    float y = 2.0f;

    // 1. Cancel closes the browser and leaves the current plugin untouched.
    CancelPluginBrowserItem* cancelItem = new CancelPluginBrowserItem([this]() {
        if (onClose) onClose();
    });
    cancelItem->box.pos = Vec(0, y);
    container->addChild(cancelItem);
    y += 24.0f;

    // Keep unload separate from cancel; it is only meaningful when something
    // is already loaded.
    if (controller->isLoaded()) {
        UnloadPluginItem* unloadItem = new UnloadPluginItem(controller, [this]() {
            if (onClose) onClose();
        });
        unloadItem->box.pos = Vec(0, y);
        container->addChild(unloadItem);
        y += 24.0f;
    }

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
