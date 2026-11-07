// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once

// Engine-owned gameplay randomness. PCG-XSH-RR 32 (v1) is specified here
// rather than delegated to a standard-library distribution whose output may
// change across standard-library implementations.

#include <cstdint>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tour {

class RandomStream {
  public:
    struct State {
        uint64_t state = 0;
        uint64_t increment = 1;
    };

    explicit RandomStream(uint64_t seed = 0, uint64_t sequence = 1) noexcept {
        Reseed(seed, sequence);
    }

    void Reseed(uint64_t seed, uint64_t sequence = 1) noexcept {
        state_ = 0;
        increment_ = (sequence << 1u) | 1u;
        (void)NextU32();
        state_ += seed;
        (void)NextU32();
    }

    [[nodiscard]] uint32_t NextU32() noexcept {
        const uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + increment_;
        const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rotation = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rotation) | (xorshifted << ((-rotation) & 31u));
    }

    // Uniform value in [0, exclusiveUpper), without modulo bias.
    // A zero bound is an error (including in release builds).
    [[nodiscard]] uint32_t NextBounded(uint32_t exclusiveUpper) {
        if (exclusiveUpper == 0)
            throw std::invalid_argument("RandomStream::NextBounded requires a nonzero bound");
        const uint32_t threshold = static_cast<uint32_t>(-exclusiveUpper) % exclusiveUpper;
        for (;;) {
            const uint32_t value = NextU32();
            if (value >= threshold)
                return value % exclusiveUpper;
        }
    }

    // Exactly 24 random bits; guaranteed to be in [0, 1), never 1.
    [[nodiscard]] float NextFloat01() noexcept {
        return static_cast<float>(NextU32() >> 8u) * (1.0f / 16777216.0f);
    }

    [[nodiscard]] State Save() const noexcept {
        return {state_, increment_};
    }

    void Restore(State value) {
        if ((value.increment & 1u) == 0)
            throw std::invalid_argument("RandomStream::Restore requires an odd increment");
        state_ = value.state;
        increment_ = value.increment;
    }

  private:
    uint64_t state_ = 0;
    uint64_t increment_ = 1;
};

// One service per game session/host, not a process-global RNG. Each named
// stream has independent mutable state derived solely from the master seed
// and its name (independent of the order other streams were first accessed).
// Stream() and Reseed() must be called on the owning thread. Worker jobs can
// take independent *values* from MakeStream() and mutate those privately.
class RandomService {
  public:
    explicit RandomService(uint64_t seed = 0) noexcept : seed_(seed) {}

    [[nodiscard]] uint64_t Seed() const noexcept {
        return seed_;
    }

    // Clears all named streams. References returned by Stream() are invalid
    // after this call; reseed at launch or a known simulation reset point.
    void Reseed(uint64_t seed) {
        seed_ = seed;
        streams_.clear();
    }

    [[nodiscard]] RandomStream MakeStream(std::string_view name) const noexcept {
        const uint64_t nameHash = HashName(name);
        return RandomStream(Mix64(seed_ ^ nameHash),
                            Mix64(seed_ ^ nameHash ^ 0x9e3779b97f4a7c15ULL));
    }

    RandomStream& Stream(std::string_view name) {
        auto [it, inserted] = streams_.try_emplace(std::string(name), MakeStream(name));
        (void)inserted;
        return it->second;
    }

    RandomStream& Gameplay() {
        return Stream("gameplay");
    }

    // Deterministic checkpoint over all persistent named streams. Sorted by
    // name so that serialized state is independent of unordered_map order.
    // By-value MakeStream() streams belong to their caller and are not tracked.
    struct Snapshot {
        uint64_t seed = 0;
        std::map<std::string, RandomStream::State> streams;
    };
    [[nodiscard]] Snapshot Capture() const {
        Snapshot snapshot;
        snapshot.seed = seed_;
        for (const auto& [name, stream] : streams_)
            snapshot.streams.emplace(name, stream.Save());
        return snapshot;
    }

    // Strong exception safety: validate every stream before replacing the
    // active collection. Retained Stream() references are invalidated.
    void Restore(const Snapshot& snapshot) {
        std::unordered_map<std::string, RandomStream> restored;
        for (const auto& [name, state] : snapshot.streams) {
            RandomStream stream;
            stream.Restore(state); // throws on an invalid even increment
            restored.emplace(name, stream);
        }
        streams_.swap(restored);
        seed_ = snapshot.seed;
    }

    // Entropy is used only to choose the initial seed for unpinned launches.
    // The resulting numeric seed must be logged/stored for later replay.
    [[nodiscard]] static uint64_t GenerateSeed() {
        std::random_device device;
        return (static_cast<uint64_t>(device()) << 32u) | static_cast<uint64_t>(device());
    }

  private:
    [[nodiscard]] static uint64_t HashName(std::string_view name) noexcept {
        uint64_t hash = 14695981039346656037ULL; // FNV-1a 64-bit
        for (char ch : name) {
            hash ^= static_cast<unsigned char>(ch);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    [[nodiscard]] static uint64_t Mix64(uint64_t x) noexcept {
        x ^= x >> 30u;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27u;
        x *= 0x94d049bb133111ebULL;
        return x ^ (x >> 31u);
    }

    uint64_t seed_ = 0;
    std::unordered_map<std::string, RandomStream> streams_;
};

} // namespace tour
