#pragma once
#include "texture.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>

namespace edmo {
struct DisplaySettings {
    uint32_t w{}, h{};
    bool enabled{true}, room{}, curved{};
    float size{1.5f}, distance{1.3f}, arc{70}, x{}, y{};
    bool operator==(const DisplaySettings& b) const {
        return w==b.w && h==b.h && enabled==b.enabled && room==b.room && curved==b.curved &&
               size==b.size && distance==b.distance && arc==b.arc && x==b.x && y==b.y;
    }
};
struct FrameId {
    uint64_t generation{}, sequence{};
    bool operator==(const FrameId &other) const noexcept {
        return generation == other.generation && sequence == other.sequence;
    }
};

// One reader per live XR session. Only the worker touches the IPC channel.
class FrameReader {
  public:
    ~FrameReader() { stop(); }
    bool start(DisplaySettings initial) noexcept;
    void stop() noexcept;
    void latest(FrameId &current, LivePixels &pixels, DisplaySettings& display) noexcept;

  private:
    void run(const char *path);
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool stopping_{};
    DisplaySettings settings_{};
    FrameId id_{};
    LivePixels pixels_{};
};
} // namespace edmo
