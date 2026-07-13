#pragma once

#include <vector>
#include <cstring>
#include <atomic>
#include "pluginterfaces/base/ibstream.h"

namespace ifrit {

class Vst3StateStream : public Steinberg::IBStream {
public:
    Vst3StateStream() : refCount(1), position(0) {}
    Vst3StateStream(const std::vector<uint8_t>& initialData) : refCount(1), data(initialData), position(0) {}

    const std::vector<uint8_t>& getData() const { return data; }
    void clear() {
        data.clear();
        position = 0;
    }

    // FUnknown
    virtual Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::IBStream::iid.toTUID()) ||
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

    // IBStream
    virtual Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesRead) override {
        if (!buffer) return Steinberg::kInvalidArgument;
        
        int64_t available = (int64_t)data.size() - position;
        int64_t toRead = numBytes;
        if (toRead > available) {
            toRead = available;
        }
        
        if (toRead > 0) {
            std::memcpy(buffer, &data[position], toRead);
            position += toRead;
        }
        
        if (numBytesRead) {
            *numBytesRead = (Steinberg::int32)toRead;
        }
        
        return Steinberg::kResultOk;
    }

    virtual Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesWritten) override {
        if (!buffer) return Steinberg::kInvalidArgument;
        
        if (numBytes > 0) {
            int64_t needed = position + numBytes;
            if (needed > (int64_t)data.size()) {
                data.resize(needed);
            }
            std::memcpy(&data[position], buffer, numBytes);
            position += numBytes;
        }
        
        if (numBytesWritten) {
            *numBytesWritten = numBytes;
        }
        
        return Steinberg::kResultOk;
    }

    virtual Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode, Steinberg::int64* result) override {
        int64_t newPos = position;
        switch (mode) {
            case kIBSeekSet:
                newPos = pos;
                break;
            case kIBSeekCur:
                newPos = position + pos;
                break;
            case kIBSeekEnd:
                newPos = (int64_t)data.size() + pos;
                break;
            default:
                return Steinberg::kInvalidArgument;
        }
        
        if (newPos < 0) {
            return Steinberg::kInvalidArgument;
        }
        
        position = newPos;
        if (result) {
            *result = position;
        }
        return Steinberg::kResultOk;
    }

    virtual Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override {
        if (!pos) return Steinberg::kInvalidArgument;
        *pos = position;
        return Steinberg::kResultOk;
    }

private:
    virtual ~Vst3StateStream() = default;

    std::atomic<Steinberg::uint32> refCount;
    std::vector<uint8_t> data;
    int64_t position;
};

} // namespace ifrit
