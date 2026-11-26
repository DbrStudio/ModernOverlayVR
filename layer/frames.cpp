#include "frames.hpp"
#include "panel.hpp"
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <memory>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace edmo {
namespace {
struct Header {
    char magic[4];
    uint32_t version, width, height;
    FrameId id;
};
static_assert(sizeof(Header) == 32 && sizeof(FrameId) == 16);
// Protocol v1 is little-endian, tightly packed, straight RGBA8.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);


bool readDisplay(const char* file, DisplaySettings& output) {
    try {
        std::ifstream stream(file);
        if (!stream) return false;
        auto j = nlohmann::json::parse(stream);
        DisplaySettings d;
        const auto resolution = j.at("resolution").get<std::string>();
        if (resolution=="640x480") { d.w=640; d.h=480; }
        else if (resolution=="960x720") { d.w=960; d.h=720; }
        else if (resolution=="1280x960") { d.w=1280; d.h=960; }
        else if (resolution=="1920x1440") { d.w=1920; d.h=1440; }
        else return false;
        d.enabled=j.at("enabled").get<bool>();
        if ((j.at("anchor")!="room" && j.at("anchor")!="head") ||
            (j.at("shape")!="curved" && j.at("shape")!="flat")) return false;
        d.room=j.at("anchor")=="room"; d.curved=j.at("shape")=="curved";
        d.x=j.at("x"); d.y=j.at("y"); d.arc=j.at("arc");
        d.size=j.at("width"); d.distance=j.at("distance");
        if (!(std::abs(d.x)<=5 && std::abs(d.y)<=5 && d.arc>=5 && d.arc<=180 &&
              d.size>=.1f && d.size<=5 && d.distance>=.1f && d.distance<=10)) return false;
        output=d;
        return true;
    } catch (const nlohmann::json::exception&) { return false; }
}

class Mapping {
  public:
    Mapping(uint32_t w, uint32_t h) : width(w), height(h), channelSize(sizeof(Header) + size_t(w)*h*4) {}
    ~Mapping() { closeFile(); }
    void closeFile() {
        if (data_)
            munmap(data_, channelSize);
        if (fd_ >= 0)
            close(fd_);
        data_ = nullptr;
        fd_ = -1;
    }

    // False means no living publisher. A busy writer leaves the last snapshot intact.
    bool read(const char *path, FrameId &last, LivePixels &pixels) {
        if (fd_ < 0) {
            fd_ = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
            struct stat st{};
            if (fd_ < 0 || fstat(fd_, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
                st.st_size != static_cast<off_t>(channelSize))
                return false;
            auto *view = mmap(nullptr, channelSize, PROT_READ, MAP_SHARED, fd_, 0);
            if (view == MAP_FAILED)
                return false;
            data_ = static_cast<unsigned char *>(view);
        }
        if (flock(fd_, LOCK_SH | LOCK_NB))
            return errno == EWOULDBLOCK || errno == EINTR;
        struct flock lease{};
        lease.l_type = F_RDLCK;
        lease.l_whence = SEEK_SET;
        lease.l_len = 1;
        bool online = fcntl(fd_, F_GETLK, &lease) == 0 && lease.l_type == F_WRLCK;
        if (online) {
            Header header;
            std::memcpy(&header, data_, sizeof(header));
            online = !std::memcmp(header.magic, "EDM3", 4) && header.version == 1 && header.width == width &&
                     header.height == height && header.id.generation && header.id.sequence;
            if (online && !(last == header.id)) {
                std::memcpy(pixels.data(), data_ + sizeof(header), pixels.size() * sizeof(uint32_t));
                last = header.id;
            }
        }
        flock(fd_, LOCK_UN);
        return online;
    }

  private:
    uint32_t width, height;
    size_t channelSize;
    int fd_{-1};
    unsigned char *data_{};
};
} // namespace

bool FrameReader::start(DisplaySettings initial) noexcept {
    const char *path = std::getenv("EDMO_XR_CHANNEL");
    if (!path || !*path)
        return false;
    try {
        settings_ = initial;
        worker_ = std::thread([this, path = std::string(path)] { try { run(path.c_str()); } catch (const std::exception&) { log("event=renderer_worker_error"); } });
        return true;
    } catch (const std::exception &) {
        log("event=renderer_error reason=worker_start");
        return false;
    }
}

void FrameReader::stop() noexcept {
    if (!worker_.joinable())
        return;
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    worker_.join();
}

void FrameReader::latest(FrameId &current, LivePixels &pixels, DisplaySettings& display) noexcept {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock) return;
    display = settings_;
    if (current == id_)
        return;
    current = id_;
    if (current.sequence)
        try { pixels = pixels_; } catch (const std::bad_alloc&) { current = {}; }
}

void FrameReader::run(const char *path) {
    using namespace std::chrono_literals;
    DisplaySettings display = settings_;
    std::string channel(path);
    const char* file = std::getenv("EDMO_DISPLAY_FILE");
    auto mapping = std::make_unique<Mapping>(display.w, display.h);
    FrameId last{};
    LivePixels pixels(size_t(display.w)*display.h);
    auto nextCheck = std::chrono::steady_clock::time_point::min();
    for (;;) {
        auto now = std::chrono::steady_clock::now();
        if (file && now >= nextCheck) {
            nextCheck = now + 200ms;
            auto next = display;
            if (readDisplay(file, next)) {
                if (next.w != display.w || next.h != display.h) {
                    mapping = std::make_unique<Mapping>(next.w, next.h);
                    pixels.resize(size_t(next.w)*next.h);
                    last = {};
                }
                std::string base(file);
                channel = base.substr(0, base.rfind('/')+1) + "frame-" + std::to_string(next.w) +
                          "x" + std::to_string(next.h) + ".shm";
                display = next;
            }
        }
        bool online = display.enabled && mapping->read(channel.c_str(), last, pixels);
        if (!online) {
            mapping->closeFile();
            last = {};
        }
        std::unique_lock lock(mutex_);
        settings_ = display;
        if (!(id_ == last)) {
            if (id_.generation != last.generation)
                log("event=renderer_%s", last.generation ? "connected" : "offline");
            id_ = last;
            if (online)
                pixels_ = pixels;
        }
        if (wake_.wait_for(lock, online ? 33ms : 250ms, [this] { return stopping_; }))
            return;
    }
}
} // namespace edmo
