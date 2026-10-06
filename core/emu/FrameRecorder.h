#pragma once

#include "io/Platform.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Records gameplay for side-by-side videos: every emulated frame of one eye
// twice - as the original hardware showed it (red) and as the app colors it -
// into two numbered PNG sequences, plus the game's audio as a WAV. Folder
// (under the ROMs folder): recordings/<rom> NNN/original/original_00000.png,
// .../colored/colored_00000.png, .../game audio.wav. Both sequences have the
// same frames, so they stay in sync in a video editor (import each folder as
// an image sequence at 50 fps).
//
// PNGs are upscaled (nearest neighbor, kScale) so editors don't blur the
// pixels, and encoded on worker threads so the game keeps running; Stop()
// waits for whatever is still queued.
class FrameRecorder
{
public:
    // 384 x 5 = 1920: two videos side by side fill a 4K frame's width (half
    // of it at 1080p).
    static constexpr int kScale = 5;
    // The Virtual Boy's frame rate; the WAV claims a sample rate scaled by
    // 50 / this, so it lasts exactly as long as the frames played at 50 fps.
    static constexpr double kGameFps = 50.27;

    ~FrameRecorder();

    // folder: relative to the ROMs folder (e.g. "recordings/<rom> 001").
    bool Start(Platform *platform, const std::string &folder, uint32_t width, uint32_t height, uint32_t audioRate);
    // Both images width x height, RGB.
    void AddFrame(const uint8_t *originalRgb, const uint8_t *coloredRgb);
    void AddAudio(const int16_t *stereo, size_t frames);
    // Waits for the queued frames, writes the WAV. Returns the frame count.
    size_t Stop();

    bool Active() const { return m_active; }
    size_t Frames() const { return m_frames; }
    const std::string &Folder() const { return m_folder; }

private:
    struct Item
    {
        size_t index;
        std::vector<uint8_t> original, colored;
    };
    void Work();
    void WritePng(const std::string &path, const std::vector<uint8_t> &rgb);

    Platform *m_platform = nullptr;
    std::string m_folder;
    uint32_t m_width = 0, m_height = 0, m_audioRate = 0;
    bool m_active = false;
    size_t m_frames = 0;
    std::vector<int16_t> m_audio;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Item> m_queue;
    bool m_stopping = false;
    std::vector<std::thread> m_workers;
};
