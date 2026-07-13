#pragma once

#include <rack.hpp>
#include "host/PluginHostController.hpp"

namespace ifrit {

class PluginIdentityDisplay : public rack::widget::OpaqueWidget {
public:
    PluginIdentityDisplay(PluginHostController* controller);
    ~PluginIdentityDisplay() = default;

    void draw(const rack::widget::Widget::DrawArgs& args) override;
    void onButton(const rack::widget::Widget::ButtonEvent& e) override;

    // Callback when user wants to open the browser
    std::function<void()> onOpenBrowser;

private:
    PluginHostController* controller;
};

} // namespace ifrit
