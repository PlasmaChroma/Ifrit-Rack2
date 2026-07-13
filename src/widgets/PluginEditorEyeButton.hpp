#pragma once

#include <rack.hpp>
#include "host/PluginHostController.hpp"

namespace ifrit {

class PluginEditorEyeButton : public rack::widget::OpaqueWidget {
public:
    PluginEditorEyeButton(PluginHostController* controller);
    ~PluginEditorEyeButton() = default;

    void draw(const rack::widget::Widget::DrawArgs& args) override;
    void onButton(const rack::widget::Widget::ButtonEvent& e) override;

private:
    PluginHostController* controller;
};

} // namespace ifrit
