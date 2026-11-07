// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Portable PNG sequence metadata, independent of Vulkan and game code.
// One ancillary "tsNp" PNG chunk carries a versioned JSON keyframe/delta.
// This preserves the PNG's normal image bytes and uses PNG CRC validation.
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace tour::snapshot {

using Json = nlohmann::json;

inline std::string Fnv64(std::string_view data) {
    uint64_t value = 14695981039346656037ull;
    for (unsigned char byte : data) {
        value ^= byte;
        value *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}
inline std::string Fnv64(std::span<const uint8_t> data) {
    return Fnv64(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

inline uint32_t PngCrc(std::span<const uint8_t> bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t value : bytes) {
        crc ^= value;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1u) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

inline void PutBe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value >> 24u));
    out.push_back(uint8_t(value >> 16u));
    out.push_back(uint8_t(value >> 8u));
    out.push_back(uint8_t(value));
}

inline bool EmbedPngMetadata(const std::filesystem::path& path,
                             std::string_view payload,
                             std::string& error) {
    static constexpr std::array<uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    static constexpr std::array<uint8_t, 12> iend{
        0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
    if (payload.size() > 64u * 1024u * 1024u - 6u) {
        error = "snapshot payload exceeds 64 MiB";
        return false;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < 33 || size > 512u * 1024u * 1024u) {
        error = "snapshot PNG size invalid";
        return false;
    }
    std::ifstream src(path, std::ios::binary);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!src.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()))) {
        error = "cannot read PNG for metadata";
        return false;
    }
    src.close();
    if (!std::equal(signature.begin(), signature.end(), bytes.begin()) ||
        !std::equal(iend.begin(), iend.end(), bytes.end() - iend.size())) {
        error = "PNG signature or IEND invalid";
        return false;
    }
    // Embed immediately before IEND. Private, ancillary, reserved-bit
    // valid and safe-to-copy: standard viewers ignore the tsNp chunk.
    const auto insertAt = bytes.end() - iend.size();
    std::vector<uint8_t> chunk;
    chunk.reserve(payload.size() + 18);
    PutBe32(chunk, static_cast<uint32_t>(payload.size() + 6u));
    chunk.insert(chunk.end(), {'t', 's', 'N', 'p', 'T', 'S', 'N', 'P', '1', 0});
    chunk.insert(chunk.end(), payload.begin(), payload.end());
    const uint32_t crc = PngCrc(std::span<const uint8_t>(chunk.data() + 4, chunk.size() - 4));
    PutBe32(chunk, crc);
    bytes.insert(insertAt, chunk.begin(), chunk.end());
    const std::filesystem::path staging = path.string() + ".meta.tmp";
    if (std::filesystem::exists(staging)) {
        error = "metadata temporary path already exists";
        return false;
    }
    {
        std::ofstream out(staging, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()),
                               static_cast<std::streamsize>(bytes.size()))) {
            error = "cannot write PNG metadata";
            std::filesystem::remove(staging, ec);
            return false;
        }
        out.flush();
        if (!out) {
            error = "PNG metadata flush failed";
            out.close();
            std::filesystem::remove(staging, ec);
            return false;
        }
    }
    // The original is only an unpublished .pending.png; remove before
    // rename because Windows refuses rename-over-existing destinations.
    std::filesystem::remove(path, ec);
    if (ec) {
        error = "cannot retire PNG pending file";
        std::filesystem::remove(staging, ec);
        return false;
    }
    std::filesystem::rename(staging, path, ec);
    if (ec) {
        error = "cannot install PNG metadata";
        std::filesystem::remove(staging, ec);
        return false;
    }
    return true;
}

// Stateful keyframe/delta builder. The caller captures CPU state at the
// SAME simulation tick as the renderer submits the image and only calls
// Complete after that image's GPU readback has finished.
class SequenceWriter {
  public:
    static constexpr uint32_t Version = 1;
    bool Configure(std::filesystem::path directory,
                   uint32_t keyInterval,
                   uint64_t firstTick,
                   uint32_t count,
                   std::string id,
                   std::string& error) {
        if (directory.empty() || !keyInterval || !count || count > 1'000'000 || id.empty() ||
            !std::filesystem::is_directory(directory)) {
            error = "invalid sequence directory/interval/count/id";
            return false;
        }
        // Avoid silently mixing previous captures with a new sequence.
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().filename().string().starts_with("Frame_") &&
                (entry.path().extension() == ".png" || entry.path().extension() == ".rgba" ||
                 entry.path().extension() == ".json")) {
                error = "sequence output contains existing Frame_*.png";
                return false;
            }
        }
        directory_ = std::move(directory);
        interval_ = keyInterval;
        firstTick_ = firstTick;
        count_ = count;
        sequenceId_ = std::move(id);
        return true;
    }

    [[nodiscard]] bool Enabled() const {
        return count_ != 0;
    }
    [[nodiscard]] bool WantsTick(uint64_t tick) const {
        return Enabled() && tick >= firstTick_ && tick - firstTick_ < count_;
    }
    [[nodiscard]] uint32_t Written() const {
        return written_;
    }
    [[nodiscard]] uint32_t Count() const {
        return count_;
    }
    [[nodiscard]] const std::filesystem::path& Directory() const {
        return directory_;
    }

    bool Stage(uint64_t tick,
               uint64_t dtNs,
               const Json& state,
               Json events,
               Json provenance,
               std::string& error) {
        if (!WantsTick(tick) || pending_ || tick != firstTick_ + written_ || !dtNs ||
            dtNs > 10'000'000'000ull || !events.is_array() || events.size() > 4096 ||
            !state.is_object()) {
            error = "invalid or non-contiguous snapshot frame";
            return false;
        }
        const bool key = written_ == 0 || (written_ % interval_) == 0;
        pendingRecord_ = {
            {"format", "tourmaline.snapshot"},
            {"version", Version},
            {"sequence_id", sequenceId_},
            {"frame", written_ + 1},
            {"tick", tick},
            {"dt_ns", dtNs},
            {"kind", key ? "key" : "delta"},
            {"previous_payload_fnv64", written_ == 0 ? Json(nullptr) : Json(previousPayloadHash_)},
            {"state_fnv64", Fnv64(state.dump())},
            {"events", std::move(events)}};
        if (key) {
            pendingRecord_["state"] = state;
            pendingRecord_["provenance"] = std::move(provenance);
        } else {
            pendingRecord_["previous_frame"] = written_;
            pendingRecord_["patch"] = Json::diff(previousState_, state);
        }
        previousState_ = state;
        pending_ = true;
        return true;
    }

    bool Complete(std::string_view pngPath, std::span<const uint8_t> rgba, std::string& error) {
        if (!pending_) {
            error = "sequence readback has no matching state";
            return false;
        }
        pendingRecord_["pixels_fnv64"] = Fnv64(rgba);
        const std::string serialized = pendingRecord_.dump();
        if (serialized.size() > 64u * 1024u * 1024u - 6u) {
            error = "snapshot metadata too large";
            return false;
        }
        if (!EmbedPngMetadata(std::filesystem::path(std::string(pngPath)), serialized, error))
            return false;
        std::string payload = "TSNP1";
        payload.push_back('\0');
        payload += serialized;
        previousPayloadHash_ = Fnv64(payload);
        pending_ = false;
        ++written_;
        return true;
    }

    // Raw capture is intentionally lossless and byte-for-byte RGBA8. The
    // sidecar contains the same keyframe/delta record and pixel hash as
    // the PNG ancillary chunk. Sidecars are NOT PNGs: existing PNG-only
    // tools must use the raw-aware verifier instead of silently accepting
    // an incomplete sequence.
    bool CompleteRaw(std::span<const uint8_t> rgba, std::string& error) {
        if (!pending_) {
            error = "raw sequence readback has no matching state";
            return false;
        }
        pendingRecord_["pixels_fnv64"] = Fnv64(rgba);
        pendingRecord_["pixel_format"] = "rgba8";
        const std::string serialized = pendingRecord_.dump();
        if (serialized.size() > 64u * 1024u * 1024u - 6u) {
            error = "raw snapshot metadata too large";
            return false;
        }
        const auto rgbaPath = FramePath(".rgba");
        const auto statePath = FramePath(".json");
        if (std::filesystem::exists(rgbaPath) || std::filesystem::exists(statePath)) {
            error = "raw snapshot output already exists";
            return false;
        }
        const auto rgbaTmp = std::filesystem::path(rgbaPath.string() + ".pending");
        const auto jsonTmp = std::filesystem::path(statePath.string() + ".pending");
        if (std::filesystem::exists(rgbaTmp) || std::filesystem::exists(jsonTmp)) {
            error = "raw snapshot staging path already exists";
            return false;
        }
        std::error_code ec;
        auto cleanup = [&] {
            std::filesystem::remove(rgbaTmp, ec);
            std::filesystem::remove(jsonTmp, ec);
        };
        {
            std::ofstream pixels(rgbaTmp, std::ios::binary | std::ios::trunc);
            if (!pixels.write(reinterpret_cast<const char*>(rgba.data()),
                              static_cast<std::streamsize>(rgba.size()))) {
                error = "cannot write raw RGBA data";
                cleanup();
                return false;
            }
        }
        {
            std::ofstream meta(jsonTmp, std::ios::binary | std::ios::trunc);
            if (!meta.write(serialized.data(), static_cast<std::streamsize>(serialized.size()))) {
                error = "cannot write raw snapshot sidecar";
                cleanup();
                return false;
            }
        }
        std::filesystem::rename(rgbaTmp, rgbaPath, ec);
        if (ec) {
            error = "cannot publish raw RGBA frame: " + ec.message();
            cleanup();
            return false;
        }
        std::filesystem::rename(jsonTmp, statePath, ec);
        if (ec) {
            error = "cannot publish raw state sidecar: " + ec.message();
            std::filesystem::remove(rgbaPath, ec);
            cleanup();
            return false;
        }
        std::string payload = "TSNP1";
        payload.push_back('\0');
        payload += serialized;
        previousPayloadHash_ = Fnv64(payload);
        pending_ = false;
        ++written_;
        return true;
    }

    [[nodiscard]] std::filesystem::path FramePath(std::string_view extension) const {
        std::ostringstream name;
        name << "Frame_" << std::setw(4) << std::setfill('0') << (written_ + 1) << extension;
        return directory_ / name.str();
    }

    [[nodiscard]] std::filesystem::path FramePath() const {
        return FramePath(".png");
    }

  private:
    std::filesystem::path directory_;
    uint32_t interval_ = 0;
    uint64_t firstTick_ = 0;
    uint32_t count_ = 0;
    uint32_t written_ = 0;
    std::string sequenceId_;
    std::string previousPayloadHash_;
    Json previousState_;
    Json pendingRecord_;
    bool pending_ = false;
};

} // namespace tour::snapshot
