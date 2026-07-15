#pragma once

#include <rack.hpp>
#include "host/PluginHostController.hpp"

namespace ifrit {

class PluginBrowserOverlay : public rack::widget::OpaqueWidget {
public:
    PluginBrowserOverlay(
        PluginHostController* controller,
        bool instrumentsOnly,
        rack::math::Vec moduleSize);
    ~PluginBrowserOverlay() = default;

    void draw(const rack::widget::Widget::DrawArgs& args) override;
    void step() override;
    void onButton(const rack::widget::Widget::ButtonEvent& e) override;
    void onHoverKey(const rack::widget::Widget::HoverKeyEvent& e) override;

    // Callback when closed
    std::function<void()> onClose;

private:
    void updateLayout();
    void rebuildList();

    PluginHostController* controller;
    bool instrumentsOnly;

    // Text field for search
    class SearchField : public rack::ui::TextField {
    public:
        std::function<void(std::string)> onTextChange;
        void onSelectText(const rack::widget::Widget::SelectTextEvent& e) override {
            rack::ui::TextField::onSelectText(e);
            if (onTextChange) {
                onTextChange(text);
            }
        }
        void onSelectKey(const rack::widget::Widget::SelectKeyEvent& e) override {
            rack::ui::TextField::onSelectKey(e);
            if (onTextChange) {
                onTextChange(text);
            }
        }
    };

    SearchField* searchField;
    rack::widget::Widget* closeButton;
    rack::ui::ScrollWidget* scrollWidget;
    rack::widget::Widget* container;
    std::string currentQuery;
    bool scanWasActive = false;
};

} // namespace ifrit
