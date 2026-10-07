#include "emu/FrameRecorder.h"

#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h> // (implementation in Emulator.cpp)

#include <algorithm>
#include <cstdio>
#include <cstring>

FrameRecorder::~FrameRecorder()
{
    if (m_active)
        Stop();
}

bool FrameRecorder::Start(Platform *platform, const std::string &folder, uint32_t width, uint32_t height, uint32_t audioRate)
{
    if (m_active || !platform || !width || !height)
        return false;
    m_platform = platform;
    m_folder = folder;
    m_width = width;
    m_height = height;
    m_audioRate = audioRate;
    m_frames = 0;
    m_audio.clear();
    m_stopping = false;
    // (encoding is the slow part: as many encoders as the machine has cores to spare)
    const unsigned workers = std::max(2u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u);
    for (unsigned i = 0; i < workers; ++i)
        m_workers.emplace_back(&FrameRecorder::Work, this);
    m_active = true;
    return true;
}

void FrameRecorder::AddFrame(const uint8_t *originalRgb, const uint8_t *coloredRgb)
{
    if (!m_active)
        return;
    const size_t bytes = static_cast<size_t>(m_width) * m_height * 3;
    Item item{m_frames++, std::vector<uint8_t>(originalRgb, originalRgb + bytes), std::vector<uint8_t>(coloredRgb, coloredRgb + bytes)};
    {
        // A machine whose encoders can't keep up slows the game down while
        // recording (no frame is ever dropped) instead of piling frames up
        // in memory - and stopping then only waits for a few seconds' worth.
        std::unique_lock<std::mutex> lock(m_mutex);
        m_drained.wait(lock, [this] { return m_queue.size() < kMaxQueued; });
        m_queue.push_back(std::move(item));
    }
    m_wake.notify_one();
}

void FrameRecorder::AddAudio(const int16_t *stereo, size_t frames)
{
    if (m_active)
        m_audio.insert(m_audio.end(), stereo, stereo + frames * 2);
}

size_t FrameRecorder::Stop()
{
    if (!m_active)
        return 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    for (std::thread &t : m_workers)
        t.join();
    m_workers.clear();
    m_active = false;

    // The game's audio, 16-bit stereo - its rate scaled so it lasts as long
    // as the frames do at 50 fps (0.5% slower than the hardware).
    const uint32_t rate = static_cast<uint32_t>(m_audioRate * 50.0 / kGameFps + 0.5);
    const uint32_t dataBytes = static_cast<uint32_t>(m_audio.size() * 2);
    std::vector<uint8_t> wav(44 + dataBytes);
    auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) wav[at + i] = static_cast<uint8_t>(v >> (8 * i)); };
    auto put16 = [&](size_t at, uint16_t v) { wav[at] = static_cast<uint8_t>(v); wav[at + 1] = static_cast<uint8_t>(v >> 8); };
    std::memcpy(&wav[0], "RIFF", 4);
    put32(4, 36 + dataBytes);
    std::memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1); // PCM
    put16(22, 2);
    put32(24, rate);
    put32(28, rate * 4);
    put16(32, 4);
    put16(34, 16);
    std::memcpy(&wav[36], "data", 4);
    put32(40, dataBytes);
    for (size_t i = 0; i < m_audio.size(); ++i)
        put16(44 + i * 2, static_cast<uint16_t>(m_audio[i]));
    m_platform->WriteRomsFile(m_folder + "/game audio.wav", false, wav.data(), wav.size());
    m_audio.clear();
    m_audio.shrink_to_fit();
    return m_frames;
}

void FrameRecorder::Work()
{
    for (;;)
    {
        Item item;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty())
                return; // stopping, and nothing left
            item = std::move(m_queue.front());
            m_queue.pop_front();
        }
        m_drained.notify_one();
        char name[64];
        std::snprintf(name, sizeof(name), "/original/original_%05zu.png", item.index);
        WritePng(m_folder + name, item.original);
        std::snprintf(name, sizeof(name), "/colored/colored_%05zu.png", item.index);
        WritePng(m_folder + name, item.colored);
    }
}

void FrameRecorder::WritePng(const std::string &path, const std::vector<uint8_t> &rgb)
{
    const uint32_t w = m_width * kScale, h = m_height * kScale;
    std::vector<uint8_t> up(static_cast<size_t>(w) * h * 3);
    for (uint32_t y = 0; y < m_height; ++y)
    {
        uint8_t *row = &up[static_cast<size_t>(y) * kScale * w * 3];
        for (uint32_t x = 0; x < m_width; ++x)
            for (int k = 0; k < kScale; ++k)
                std::memcpy(&row[(static_cast<size_t>(x) * kScale + k) * 3], &rgb[(static_cast<size_t>(y) * m_width + x) * 3], 3);
        for (int k = 1; k < kScale; ++k)
            std::memcpy(row + static_cast<size_t>(k) * w * 3, row, static_cast<size_t>(w) * 3);
    }
    std::vector<uint8_t> png;
    stbi_write_png_to_func([](void *ctx, void *data, int size)
                           {
                               auto *out = static_cast<std::vector<uint8_t> *>(ctx);
                               out->insert(out->end(), static_cast<uint8_t *>(data), static_cast<uint8_t *>(data) + size);
                           },
                           &png, static_cast<int>(w), static_cast<int>(h), 3, up.data(), static_cast<int>(w) * 3);
    m_platform->WriteRomsFile(path, false, png.data(), png.size());
}
