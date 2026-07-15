#include "PluginBrowserOverlay.hpp"

using namespace rack;

namespace ifrit {

static std::string fitTextToWidth(NVGcontext* vg, const std::string& text, float maxWidth) {
    if (nvgTextBounds(vg, 0.0f, 0.0f, text.c_str(), nullptr, nullptr) <= maxWidth) {
        return text;
    }

    // Find the longest UTF-8-safe ellipsized form that fits the actual pixel
    // width. This avoids wasting column space with a fixed character limit.
    size_t low = 0;
    size_t high = rack::string::UTF8Length(text);
    std::string best;
    while (low <= high) {
        const size_t middle = low + (high - low) / 2;
        const std::string candidate = middle > 0 ? rack::string::ellipsize(text, middle) : std::string();
        if (nvgTextBounds(vg, 0.0f, 0.0f, candidate.c_str(), nullptr, nullptr) <= maxWidth) {
            best = candidate;
            low = middle + 1;
        } else {
            if (middle == 0) break;
            high = middle - 1;
        }
    }
    return best;
}

static float vendorColumnX(float width) {
    return std::min(std::max(width * 0.62f, 94.0f), width - 64.0f);
}

// List Item Class inside overlay
class PluginBrowserItem : public OpaqueWidget {
public:
    PluginBrowserItem(PluginHostController* ctrl, const PluginDescriptor& desc, float width, std::function<void()> onClick)
        : controller(ctrl), descriptor(desc), callback(onClick) {
        box.size = Vec(width, 18);
    }

    void draw(const DrawArgs& args) override {
        bool hovered = (APP->event->hoveredWidget == this);
        
        nvgSave(args.vg);
        
        // Background on hover
        if (hovered) {
            nvgBeginPath(args.vg);
            nvgRoundedRect(args.vg, 0.0f, 0.0f, box.size.x, box.size.y, 2.0f);
            nvgFillColor(args.vg, nvgRGBA(255, 92, 28, 48));
            nvgFill(args.vg);
        }

        // Plugin Name Text
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFontSize(args.vg, 8.0f);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, hovered ? nvgRGBA(255, 245, 230, 245) : nvgRGBA(220, 198, 180, 205));
        
        const float vendorX = vendorColumnX(box.size.x);
        const std::string name = fitTextToWidth(args.vg, descriptor.name, vendorX - 16.0f);
        nvgText(args.vg, 8.0f, box.size.y / 2.0f, name.c_str(), nullptr);

        // Vendor/provider column
        const std::string vendor = fitTextToWidth(
            args.vg,
            descriptor.vendor.empty() ? "Unknown" : descriptor.vendor,
            box.size.x - vendorX - 56.0f);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, nvgRGBA(180, 137, 112, 195));
        nvgText(args.vg, vendorX, box.size.y / 2.0f, vendor.c_str(), nullptr);

        // VST3 badge
        nvgFontSize(args.vg, 6.5f);
        nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, nvgRGBA(255, 112, 38, 155));
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

class PluginBrowserHeader : public OpaqueWidget {
public:
    explicit PluginBrowserHeader(float width) { box.size = Vec(width, 14); }

    void draw(const DrawArgs& args) override {
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFontSize(args.vg, 6.5f);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, nvgRGBA(166, 112, 88, 200));
        const float vendorX = vendorColumnX(box.size.x);
        nvgText(args.vg, 8.0f, box.size.y / 2.0f, "PLUGIN", nullptr);
        nvgText(args.vg, vendorX, box.size.y / 2.0f, "VENDOR", nullptr);
        nvgTextAlign(args.vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, box.size.x - 6.0f, box.size.y / 2.0f, "FORMAT", nullptr);

        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 0.0f, box.size.y - 0.5f);
        nvgLineTo(args.vg, box.size.x, box.size.y - 0.5f);
        nvgStrokeColor(args.vg, nvgRGBA(255, 92, 28, 60));
        nvgStrokeWidth(args.vg, 1.0f);
        nvgStroke(args.vg);
    }
};

// Compact close control in the selector title row. Closing never changes the
// currently loaded plugin.
class ClosePluginBrowserButton : public OpaqueWidget {
public:
    explicit ClosePluginBrowserButton(std::function<void()> onClick) : callback(onClick) {
        box.size = Vec(20, 20);
    }

    void draw(const DrawArgs& args) override {
        const bool hovered = APP->event->hoveredWidget == this;
        if (hovered) {
            nvgBeginPath(args.vg);
            nvgRoundedRect(args.vg, 1.0f, 1.0f, box.size.x - 2.0f, box.size.y - 2.0f, 3.0f);
            nvgFillColor(args.vg, nvgRGBA(255, 100, 100, 45));
            nvgFill(args.vg);
        }
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFontSize(args.vg, 8.0f);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, hovered ? nvgRGBA(255, 180, 180, 245) : nvgRGBA(190, 205, 215, 210));
        nvgText(args.vg, box.size.x / 2.0f, box.size.y / 2.0f, "[x]", nullptr);
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

class RescanPluginBrowserItem : public OpaqueWidget {
public:
    RescanPluginBrowserItem(PluginHostController* ctrl, float width) : controller(ctrl) {
        box.size = Vec(width, 18);
    }

    void draw(const DrawArgs& args) override {
        const bool hovered = APP->event->hoveredWidget == this;
        nvgFontFaceId(args.vg, APP->window->uiFont->handle);
        nvgFontSize(args.vg, 8.0f);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg, hovered ? nvgRGBA(255, 174, 55, 245) : nvgRGBA(220, 125, 70, 210));
        nvgText(args.vg, 6.0f, box.size.y / 2.0f, "[ Rescan Plugins ]", nullptr);
    }

    void onButton(const ButtonEvent& e) override {
        OpaqueWidget::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            controller->startScan({}, true);
            e.consume(this);
        }
    }

private:
    PluginHostController* controller;
};

// Unload Item Class
class UnloadPluginItem : public OpaqueWidget {
public:
    UnloadPluginItem(PluginHostController* ctrl, float width, std::function<void()> onClick)
        : controller(ctrl), callback(onClick) {
        box.size = Vec(width, 18);
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
        nvgFontSize(args.vg, 8.0f);
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
PluginBrowserOverlay::PluginBrowserOverlay(
    PluginHostController* ctrl,
    bool instOnly,
    Vec moduleSize)
    : controller(ctrl), instrumentsOnly(instOnly) {
    box.size = moduleSize;

    // Search Field
    searchField = new SearchField();
    searchField->box.pos = Vec(14, 12);
    searchField->box.size = Vec(140, 22);
    searchField->onTextChange = [this](std::string text) {
        currentQuery = text;
        rebuildList();
    };
    addChild(searchField);

    closeButton = new ClosePluginBrowserButton([this]() {
        if (onClose) onClose();
    });
    addChild(closeButton);

    // Extend the viewport almost to the panel edge so Rack's vertical
    // scrollbar sits against the selector's far-right side.
    scrollWidget = new ScrollWidget();
    scrollWidget->box.pos = Vec(12, 42);
    scrollWidget->box.size = Vec(176, 230);
    scrollWidget->container->box.size = Vec(164, 0);
    addChild(scrollWidget);

    container = scrollWidget->container;
    updateLayout();

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

void PluginBrowserOverlay::updateLayout() {
    searchField->box.pos = Vec(10.0f, 10.0f);
    searchField->box.size = Vec(std::max(120.0f, box.size.x - 42.0f), 22.0f);
    closeButton->box.pos = Vec(box.size.x - 26.0f, 11.0f);

    scrollWidget->box.pos = Vec(10.0f, 42.0f);
    scrollWidget->box.size = Vec(
        std::max(176.0f, box.size.x - 12.0f),
        std::max(220.0f, box.size.y - 82.0f));
    container->box.size.x = std::max(164.0f, scrollWidget->box.size.x - 12.0f);
}

void PluginBrowserOverlay::draw(const DrawArgs& args) {
    nvgSave(args.vg);

    // Rounded rectangle panel background
    nvgBeginPath(args.vg);
    nvgRoundedRect(args.vg, 0.0f, 0.0f, box.size.x, box.size.y, 6.0f);
    nvgFillColor(args.vg, nvgRGBA(27, 14, 10, 248));
    nvgFill(args.vg);

    // Outer ember highlight border
    nvgBeginPath(args.vg);
    nvgRoundedRect(args.vg, 0.0f, 0.0f, box.size.x, box.size.y, 6.0f);
    nvgStrokeColor(args.vg, nvgRGBA(255, 76, 22, 125));
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
    nvgFillColor(args.vg, scanning ? nvgRGBA(255, 145, 38, 215) : nvgRGBA(190, 150, 125, 185));
    nvgText(args.vg, box.size.x / 2.0f, box.size.y - 18.0f, status.c_str(), nullptr);

    nvgRestore(args.vg);

    OpaqueWidget::draw(args);
}

void PluginBrowserOverlay::rebuildList() {
    container->clearChildren();

    const float contentWidth = container->box.size.x;

    float y = 2.0f;

    RescanPluginBrowserItem* rescanItem = new RescanPluginBrowserItem(controller, contentWidth);
    rescanItem->box.pos = Vec(0, y);
    container->addChild(rescanItem);
    y += 19.0f;

    // Unload is only meaningful when something is already loaded.
    if (controller->isLoaded()) {
        UnloadPluginItem* unloadItem = new UnloadPluginItem(controller, contentWidth, [this]() {
            if (onClose) onClose();
        });
        unloadItem->box.pos = Vec(0, y);
        container->addChild(unloadItem);
        y += 19.0f;
    }

    PluginBrowserHeader* header = new PluginBrowserHeader(contentWidth);
    header->box.pos = Vec(0, y);
    container->addChild(header);
    y += 15.0f;

    // Add filtered plugins from catalog
    // VST-FX scans effects; VST-Instrument scans instruments
    bool effects = !instrumentsOnly;
    bool instruments = instrumentsOnly;

    auto list = controller->getCatalog().search(currentQuery, effects, instruments);
    for (const auto& desc : list) {
        PluginBrowserItem* item = new PluginBrowserItem(controller, desc, contentWidth, [this]() {
            if (onClose) onClose();
        });
        item->box.pos = Vec(0, y);
        container->addChild(item);
        y += 19.0f;
    }

    container->box.size.y = y;
}

void PluginBrowserOverlay::onButton(const ButtonEvent& e) {
    OpaqueWidget::onButton(e);
    if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
        // The selector is modal over its module. Consume unused panel clicks so
        // modules and cables underneath it cannot be changed accidentally.
        e.consume(this);
    }
}

void PluginBrowserOverlay::onHoverKey(const HoverKeyEvent& e) {
    OpaqueWidget::onHoverKey(e);
}

} // namespace ifrit
