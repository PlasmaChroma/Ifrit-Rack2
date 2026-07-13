#pragma once

#include <rack.hpp>
#include "host/PluginHostController.hpp"

namespace ifrit {

class PluginParameterSlotWidget : public rack::widget::Widget {
public:
    PluginParameterSlotWidget(PluginHostController* controller, int slotIndex);
    ~PluginParameterSlotWidget() = default;

    void draw(const rack::widget::Widget::DrawArgs& args) override;
    void step() override;

private:
    PluginHostController* controller;
    int slotIndex;
    
    // Child widgets
    class MapButton : public rack::widget::OpaqueWidget {
    public:
        MapButton(PluginHostController* controller, int slotIndex);
        void draw(const rack::widget::Widget::DrawArgs& args) override;
        void onButton(const rack::widget::Widget::ButtonEvent& e) override;
    private:
        PluginHostController* controller;
        int slotIndex;
        int flashCounter = 0;
    };

    MapButton* mapButton;
};

} // namespace ifrit
