#pragma once

#include <atomic>
#include <functional>
#include "pluginterfaces/vst/ivsteditcontroller.h"

namespace ifrit {

class Vst3ComponentHandler : public Steinberg::Vst::IComponentHandler {
public:
    using EditCallback = std::function<void(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value)>;
    using RestartCallback = std::function<void(Steinberg::int32 flags)>;

    Vst3ComponentHandler() : refCount(1) {}
    virtual ~Vst3ComponentHandler() = default;

    void setEditCallbacks(EditCallback onBegin, EditCallback onPerform, EditCallback onEnd) {
        beginEditCallback = onBegin;
        performEditCallback = onPerform;
        endEditCallback = onEnd;
    }

    void setRestartCallback(RestartCallback onRestart) {
        restartCallback = onRestart;
    }

    // FUnknown
    virtual Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IComponentHandler::iid.toTUID()) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid.toTUID())) {
            addRef();
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }

    virtual Steinberg::uint32 PLUGIN_API addRef() override {
        return ++refCount;
    }

    virtual Steinberg::uint32 PLUGIN_API release() override {
        Steinberg::uint32 r = --refCount;
        if (r == 0) delete this;
        return r;
    }

    // IComponentHandler
    virtual Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID tag) override {
        if (beginEditCallback) {
            beginEditCallback(tag, 0.0);
        }
        return Steinberg::kResultOk;
    }

    virtual Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized) override {
        if (performEditCallback) {
            performEditCallback(tag, valueNormalized);
        }
        return Steinberg::kResultOk;
    }

    virtual Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID tag) override {
        if (endEditCallback) {
            endEditCallback(tag, 0.0);
        }
        return Steinberg::kResultOk;
    }

    virtual Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override {
        if (restartCallback) {
            restartCallback(flags);
        }
        return Steinberg::kResultOk;
    }

private:
    std::atomic<Steinberg::uint32> refCount;

    EditCallback beginEditCallback;
    EditCallback performEditCallback;
    EditCallback endEditCallback;
    RestartCallback restartCallback;
};

} // namespace ifrit
