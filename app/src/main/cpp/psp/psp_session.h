// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <android/native_window.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace emuorbit::psp {
constexpr unsigned kWidth=480, kHeight=272, kAudioFrames=8192;
constexpr size_t kMaxStateBytes=192u*1024*1024;
struct Config {
    std::string core, media, systemDirectory, saveDirectory;
    void (*attachThread)();
    void (*detachThread)();
};
struct Frame { unsigned audioFrames; bool presented; };
struct Stats {
    uint64_t hardwareFrames, presentedFrames, audioFrames, nonzeroAudioSamples,
             droppedAudioFrames, priorCoreGlErrors, contextGeneration;
};
class Session final {
public:
    explicit Session(const Config& config);
    ~Session() noexcept;
    Session(const Session&)=delete;
    Session& operator=(const Session&)=delete;
    // Consumes exactly the acquired reference, even when attachment fails.
    void window(ANativeWindow* acquiredWindow);
    void input(uint16_t buttons, int16_t x, int16_t y);
    Frame step(int16_t* pcm, size_t capacityFrames);
    bool pixels(uint8_t* rgba, size_t capacity);
    const uint8_t* capture(size_t& size);
    void releaseCapture();
    void restore(const uint8_t* bytes, size_t size);
    void reset();
    void pause();
    Stats stats() const;
    double fps() const;
    unsigned sampleRate() const;
    static bool quiescent();
    void close();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
