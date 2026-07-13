#pragma once

#include <vector>
#include <atomic>
#include "pluginterfaces/vst/ivstparameterchanges.h"

namespace ifrit {

class Vst3ParamValueQueue : public Steinberg::Vst::IParamValueQueue {
public:
    Vst3ParamValueQueue(Steinberg::Vst::ParamID id) : refCount(1), paramId(id) {}

    void clear() {
        points.clear();
    }

    void addPointInternal(Steinberg::int32 sampleOffset, Steinberg::Vst::ParamValue value) {
        points.push_back({sampleOffset, value});
    }

    // FUnknown
    virtual Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IParamValueQueue::iid.toTUID()) ||
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

    // IParamValueQueue
    virtual Steinberg::Vst::ParamID PLUGIN_API getParameterId() override {
        return paramId;
    }

    virtual Steinberg::int32 PLUGIN_API getPointCount() override {
        return (Steinberg::int32)points.size();
    }

    virtual Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index, Steinberg::int32& sampleOffset, Steinberg::Vst::ParamValue& value) override {
        if (index < 0 || index >= (Steinberg::int32)points.size()) {
            return Steinberg::kInvalidArgument;
        }
        sampleOffset = points[index].sampleOffset;
        value = points[index].value;
        return Steinberg::kResultOk;
    }

    virtual Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 sampleOffset, Steinberg::Vst::ParamValue value, Steinberg::int32& index) override {
        points.push_back({sampleOffset, value});
        index = (Steinberg::int32)points.size() - 1;
        return Steinberg::kResultOk;
    }

private:
    virtual ~Vst3ParamValueQueue() = default;

    std::atomic<Steinberg::uint32> refCount;
    Steinberg::Vst::ParamID paramId;

    struct Point {
        Steinberg::int32 sampleOffset;
        Steinberg::Vst::ParamValue value;
    };
    std::vector<Point> points;
};

class Vst3ParameterChanges : public Steinberg::Vst::IParameterChanges {
public:
    Vst3ParameterChanges() : refCount(1) {}

    virtual ~Vst3ParameterChanges() {
        clear();
    }

    void clear() {
        for (auto queue : queues) {
            if (queue) queue->release();
        }
        queues.clear();
    }

    void addChange(Steinberg::Vst::ParamID id, Steinberg::int32 sampleOffset, Steinberg::Vst::ParamValue value) {
        for (auto queue : queues) {
            if (queue->getParameterId() == id) {
                queue->addPointInternal(sampleOffset, value);
                return;
            }
        }
        
        Vst3ParamValueQueue* queue = new Vst3ParamValueQueue(id);
        queue->addPointInternal(sampleOffset, value);
        queues.push_back(queue);
    }

    // FUnknown
    virtual Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IParameterChanges::iid.toTUID()) ||
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

    // IParameterChanges
    virtual Steinberg::int32 PLUGIN_API getParameterCount() override {
        return (Steinberg::int32)queues.size();
    }

    virtual Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 index) override {
        if (index < 0 || index >= (Steinberg::int32)queues.size()) {
            return nullptr;
        }
        return queues[index];
    }

    virtual Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID& id, Steinberg::int32& index) override {
        for (size_t i = 0; i < queues.size(); ++i) {
            if (queues[i]->getParameterId() == id) {
                index = (Steinberg::int32)i;
                return queues[i];
            }
        }
        
        Vst3ParamValueQueue* queue = new Vst3ParamValueQueue(id);
        queues.push_back(queue);
        index = (Steinberg::int32)queues.size() - 1;
        return queue;
    }

private:
    std::atomic<Steinberg::uint32> refCount;
    std::vector<Vst3ParamValueQueue*> queues;
};

} // namespace ifrit
