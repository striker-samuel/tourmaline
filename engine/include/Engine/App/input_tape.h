// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Engine-owned, platform-neutral input tape. Versioned little-endian binary
// format; no SDL, renderer or game-code dependency. Ticks are zero based and
// represent the inputs delivered BEFORE the corresponding simulation update.
#include <Engine/App/input.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tour::input {

struct RecordedInputFrame {
    uint64_t tick = 0;
    uint64_t deltaTimeNs = 0;
    std::vector<Event> events;
};

class InputTape {
  public:
    static constexpr uint32_t Version = 1;
    static constexpr uint32_t MaxFrames = 1'000'000;
    static constexpr uint32_t MaxEventsPerFrame = 4096;
    static constexpr uint32_t MaxTextBytes = 4096;
    static constexpr size_t MaxFileBytes = 128u * 1024u * 1024u;

    [[nodiscard]] const std::vector<RecordedInputFrame>& Frames() const {
        return frames_;
    }
    void Clear() {
        frames_.clear();
    }

    static bool IsRecordable(EventType type) {
        // Quit, window-close and file-drop remain live OS operations. In
        // particular, never replay a recorded path into a different host.
        return type >= EventType::KeyDown && type <= EventType::GamepadButtonUp;
    }

    bool BeginFrame(uint64_t tick, std::string& error) {
        if (frames_.size() >= MaxFrames || tick != frames_.size()) {
            error = "input tape: nonsequential tick or frame limit exceeded";
            return false;
        }
        frames_.push_back({tick, 0, {}});
        return true;
    }

    bool Record(const Event& event, std::string& error) {
        if (!IsRecordable(event.type))
            return true;
        if (frames_.empty() || frames_.back().events.size() >= MaxEventsPerFrame ||
            !ValidEvent(event)) {
            error = "input tape: invalid event or event limit exceeded";
            return false;
        }
        frames_.back().events.push_back(event);
        return true;
    }

    bool SetDeltaTime(uint64_t nanoseconds, std::string& error) {
        if (frames_.empty() || nanoseconds == 0 || nanoseconds > 10'000'000'000ull) {
            error = "input tape: invalid simulation timestep";
            return false;
        }
        frames_.back().deltaTimeNs = nanoseconds;
        return true;
    }

    [[nodiscard]] const RecordedInputFrame* Frame(uint64_t tick) const {
        return tick < frames_.size() ? &frames_[static_cast<size_t>(tick)] : nullptr;
    }

    bool Encode(std::vector<uint8_t>& output, std::string& error) const {
        output.clear();
        if (frames_.empty() || frames_.size() > MaxFrames) {
            error = "input tape: expected 1..1000000 frames";
            return false;
        }
        for (char c : std::string_view("TOURINP1", 8))
            output.push_back(static_cast<uint8_t>(c));
        Put32(output, Version);
        Put32(output, static_cast<uint32_t>(frames_.size()));
        for (size_t i = 0; i < frames_.size(); ++i) {
            const auto& frame = frames_[i];
            if (frame.tick != i || frame.deltaTimeNs == 0 ||
                frame.deltaTimeNs > 10'000'000'000ull || frame.events.size() > MaxEventsPerFrame) {
                error = "input tape: invalid frame";
                return false;
            }
            Put64(output, frame.tick);
            Put64(output, frame.deltaTimeNs);
            Put32(output, static_cast<uint32_t>(frame.events.size()));
            for (const auto& e : frame.events) {
                if (!ValidEvent(e)) {
                    error = "input tape: invalid event";
                    return false;
                }
                output.push_back(static_cast<uint8_t>(e.type));
                Put32(output, static_cast<uint32_t>(e.key));
                Put32(output, static_cast<uint32_t>(e.mouseButton));
                Put32(output, static_cast<uint32_t>(e.gamepadButton));
                Put32(output, static_cast<uint32_t>(e.gamepadAxis));
                Put32(output, e.gamepadId);
                Put32(output, e.mods);
                PutFloat(output, e.axis);
                PutFloat(output, e.x);
                PutFloat(output, e.y);
                PutFloat(output, e.xrel);
                PutFloat(output, e.yrel);
                output.push_back(e.repeat ? 1u : 0u);
                Put32(output, static_cast<uint32_t>(e.text.size()));
                output.insert(output.end(), e.text.begin(), e.text.end());
                if (output.size() > MaxFileBytes) {
                    error = "input tape: exceeds 128 MiB";
                    return false;
                }
            }
            if (output.size() > MaxFileBytes) {
                error = "input tape: exceeds 128 MiB";
                return false;
            }
        }
        return true;
    }

    bool Decode(std::span<const uint8_t> input, std::string& error) {
        if (input.size() < 16 || input.size() > MaxFileBytes ||
            std::memcmp(input.data(), "TOURINP1", 8) != 0) {
            error = "input tape: invalid header or size";
            return false;
        }
        Reader reader{input, 8};
        uint32_t version = 0, count = 0;
        if (!reader.U32(version) || version != Version || !reader.U32(count) || count == 0 ||
            count > MaxFrames) {
            error = "input tape: unsupported version or frame count";
            return false;
        }
        std::vector<RecordedInputFrame> decoded;
        decoded.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            RecordedInputFrame frame;
            uint32_t events = 0;
            if (!reader.U64(frame.tick) || frame.tick != i || !reader.U64(frame.deltaTimeNs) ||
                frame.deltaTimeNs == 0 || frame.deltaTimeNs > 10'000'000'000ull ||
                !reader.U32(events) || events > MaxEventsPerFrame) {
                error = "input tape: invalid/truncated frame";
                return false;
            }
            frame.events.reserve(events);
            for (uint32_t n = 0; n < events; ++n) {
                Event e;
                uint8_t type = 0, repeated = 0;
                uint32_t key = 0, mouse = 0, button = 0, axis = 0, mods = 0, length = 0;
                if (!reader.U8(type) || !reader.U32(key) || !reader.U32(mouse) ||
                    !reader.U32(button) || !reader.U32(axis) || !reader.U32(e.gamepadId) ||
                    !reader.U32(mods) || !reader.Float(e.axis) || !reader.Float(e.x) ||
                    !reader.Float(e.y) || !reader.Float(e.xrel) || !reader.Float(e.yrel) ||
                    !reader.U8(repeated) || !reader.U32(length) || length > MaxTextBytes ||
                    length > input.size() - reader.offset || repeated > 1u ||
                    key >= static_cast<uint32_t>(KeyCount) || mods > UINT16_MAX) {
                    error = "input tape: invalid/truncated event";
                    return false;
                }
                e.type = static_cast<EventType>(type);
                e.key = static_cast<Key>(key);
                e.mouseButton = static_cast<MouseButton>(mouse);
                e.gamepadButton = static_cast<GamepadButton>(button);
                e.gamepadAxis = static_cast<GamepadAxis>(axis);
                e.mods = static_cast<uint16_t>(mods);
                e.repeat = repeated != 0;
                e.text.assign(reinterpret_cast<const char*>(input.data() + reader.offset), length);
                reader.offset += length;
                if (!ValidEvent(e)) {
                    error = "input tape: invalid event type or values";
                    return false;
                }
                frame.events.push_back(std::move(e));
            }
            decoded.push_back(std::move(frame));
        }
        if (reader.offset != input.size()) {
            error = "input tape: trailing data";
            return false;
        }
        frames_ = std::move(decoded); // A failed parse never changes the tape.
        return true;
    }

    bool Save(std::string_view path, std::string& error) const {
        if (path.empty()) {
            error = "input tape: empty output path";
            return false;
        }
        std::vector<uint8_t> bytes;
        if (!Encode(bytes, error))
            return false;
        const std::filesystem::path target{std::string(path)};
        const std::filesystem::path temp{std::string(path) + ".tmp"};
        std::error_code ec;
        if (std::filesystem::exists(target, ec) || std::filesystem::exists(temp, ec)) {
            error = "input tape: destination or temporary file already exists";
            return false;
        }
        {
            std::ofstream output(temp, std::ios::binary | std::ios::trunc);
            if (!output) {
                error = "input tape: cannot create output";
                return false;
            }
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output) {
                error = "input tape: write failed";
                output.close();
                std::filesystem::remove(temp, ec);
                return false;
            }
        }
        std::filesystem::rename(temp, target, ec);
        if (ec) {
            std::filesystem::remove(temp, ec);
            error = "input tape: rename failed";
            return false;
        }
        return true;
    }

    bool Load(std::string_view path, std::string& error) {
        const std::filesystem::path source{std::string(path)};
        std::error_code ec;
        const auto size = std::filesystem::file_size(source, ec);
        if (ec || size < 16 || size > MaxFileBytes) {
            error = "input tape: file missing or invalid size";
            return false;
        }
        std::ifstream file(source, std::ios::binary);
        if (!file) {
            error = "input tape: cannot open file";
            return false;
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()))) {
            error = "input tape: cannot read file";
            return false;
        }
        return Decode(bytes, error);
    }

  private:
    struct Reader {
        std::span<const uint8_t> data;
        size_t offset = 0;
        bool U8(uint8_t& out) {
            if (offset >= data.size())
                return false;
            out = data[offset++];
            return true;
        }
        bool U32(uint32_t& out) {
            if (data.size() - offset < 4)
                return false;
            out = 0;
            for (unsigned i = 0; i < 4; ++i)
                out |= uint32_t(data[offset++]) << (i * 8);
            return true;
        }
        bool U64(uint64_t& out) {
            if (data.size() - offset < 8)
                return false;
            out = 0;
            for (unsigned i = 0; i < 8; ++i)
                out |= uint64_t(data[offset++]) << (i * 8);
            return true;
        }
        bool Float(float& out) {
            uint32_t bits = 0;
            if (!U32(bits))
                return false;
            std::memcpy(&out, &bits, sizeof(out));
            return true;
        }
    };

    static void Put32(std::vector<uint8_t>& out, uint32_t n) {
        for (unsigned i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(n >> (i * 8)));
    }
    static void Put64(std::vector<uint8_t>& out, uint64_t n) {
        for (unsigned i = 0; i < 8; ++i)
            out.push_back(static_cast<uint8_t>(n >> (i * 8)));
    }
    static void PutFloat(std::vector<uint8_t>& out, float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        Put32(out, bits);
    }
    static bool ValidEvent(const Event& e) {
        if (!IsRecordable(e.type) || static_cast<uint32_t>(e.key) >= KeyCount ||
            static_cast<int>(e.mouseButton) < 1 ||
            static_cast<int>(e.mouseButton) >= MouseButtonCount ||
            static_cast<int>(e.gamepadButton) < 0 ||
            static_cast<int>(e.gamepadButton) >= GamepadButtonCount ||
            static_cast<int>(e.gamepadAxis) < 0 ||
            static_cast<int>(e.gamepadAxis) >= GamepadAxisCount || e.text.size() > MaxTextBytes ||
            !std::isfinite(e.axis) || !std::isfinite(e.x) || !std::isfinite(e.y) ||
            !std::isfinite(e.xrel) || !std::isfinite(e.yrel))
            return false;
        if ((e.type == EventType::KeyDown || e.type == EventType::KeyUp) && e.key == Key::Unknown)
            return false;
        return true;
    }

    std::vector<RecordedInputFrame> frames_;
};

} // namespace tour::input
