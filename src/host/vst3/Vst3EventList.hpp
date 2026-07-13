#pragma once

#include <vector>
#include <atomic>
#include <cstring>
#include "pluginterfaces/vst/ivstevents.h"

namespace ifrit {

class Vst3EventList : public Steinberg::Vst::IEventList {
public:
    Vst3EventList() : refCount(1) {}

    virtual ~Vst3EventList() = default;

    void clear() {
        events.clear();
    }

    void addNoteOn(Steinberg::int32 sampleOffset, Steinberg::int16 note, Steinberg::int32 noteId, float velocity) {
        Steinberg::Vst::Event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.busIndex = 0;
        ev.sampleOffset = sampleOffset;
        ev.ppqPosition = 0.0;
        ev.flags = Steinberg::Vst::Event::kIsLive;
        ev.type = Steinberg::Vst::Event::kNoteOnEvent;
        ev.noteOn.channel = 0;
        ev.noteOn.pitch = note;
        ev.noteOn.tuning = 0.0;
        ev.noteOn.velocity = velocity;
        ev.noteOn.length = 0;
        ev.noteOn.noteId = noteId;
        
        events.push_back(ev);
    }

    void addNoteOff(Steinberg::int32 sampleOffset, Steinberg::int16 note, Steinberg::int32 noteId, float velocity) {
        Steinberg::Vst::Event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.busIndex = 0;
        ev.sampleOffset = sampleOffset;
        ev.ppqPosition = 0.0;
        ev.flags = Steinberg::Vst::Event::kIsLive;
        ev.type = Steinberg::Vst::Event::kNoteOffEvent;
        ev.noteOff.channel = 0;
        ev.noteOff.pitch = note;
        ev.noteOff.velocity = velocity;
        ev.noteOff.noteId = noteId;
        ev.noteOff.tuning = 0.0;
        
        events.push_back(ev);
    }

    void addPolyPressure(Steinberg::int32 sampleOffset, Steinberg::int16 note, Steinberg::int32 noteId, float pressure) {
        Steinberg::Vst::Event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.busIndex = 0;
        ev.sampleOffset = sampleOffset;
        ev.ppqPosition = 0.0;
        ev.flags = Steinberg::Vst::Event::kIsLive;
        ev.type = Steinberg::Vst::Event::kPolyPressureEvent;
        ev.polyPressure.channel = 0;
        ev.polyPressure.pitch = note;
        ev.polyPressure.pressure = pressure;
        ev.polyPressure.noteId = noteId;
        
        events.push_back(ev);
    }

    // FUnknown
    virtual Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IEventList::iid.toTUID()) ||
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

    // IEventList
    virtual Steinberg::int32 PLUGIN_API getEventCount() override {
        return (Steinberg::int32)events.size();
    }

    virtual Steinberg::tresult PLUGIN_API getEvent(Steinberg::int32 index, Steinberg::Vst::Event& event) override {
        if (index < 0 || index >= (Steinberg::int32)events.size()) {
            return Steinberg::kInvalidArgument;
        }
        event = events[index];
        return Steinberg::kResultOk;
    }

    virtual Steinberg::tresult PLUGIN_API addEvent(Steinberg::Vst::Event& event) override {
        events.push_back(event);
        return Steinberg::kResultOk;
    }

private:
    std::atomic<Steinberg::uint32> refCount;
    std::vector<Steinberg::Vst::Event> events;
};

} // namespace ifrit
