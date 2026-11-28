// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Rendergraph/shadow_work_batches.h>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>

using namespace tour::shadow_work;
namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}
}

int main() {
    Check(!Enabled(nullptr) && !Enabled("") && !Enabled("0") &&
          !Enabled("10") && !Enabled("true") && Enabled("1"), "strict opt-in");
    for (bool enabled : {false, true})
        for (bool evsm : {false, true})
            for (bool ready : {false, true})
                Check(BatchCount(enabled, evsm, ready) ==
                      (enabled && !evsm && ready ? 3u : 1u), "fallback gates");
    for (uint32_t stride : std::array<uint32_t, 10>{0,1,2,3,4,17,65535,65536,131071,
                                                   std::numeric_limits<uint32_t>::max()}) {
        for (uint32_t batches : {1u, 3u}) {
            uint32_t next = 0;
            uint64_t total = 0;
            for (uint32_t batch = 0; batch < batches; ++batch) {
                const auto range = Slice(stride, batch, batches);
                Check(range.begin == next && range.end >= range.begin &&
                      range.end <= stride, "adjacent and bounded slices");
                total += uint64_t(range.end) - range.begin;
                // Same chunking contract as indirect recording, including max draw count.
                uint64_t chunksTotal = 0;
                for (uint32_t i = range.begin; i < range.end;) {
                    const uint32_t n = range.end - i > 65535u ? 65535u : range.end - i;
                    chunksTotal += n;
                    i += n;
                }
                Check(chunksTotal == uint64_t(range.end) - range.begin, "chunk coverage");
                next = range.end;
            }
            Check(next == stride && total == stride, "complete command coverage");
        }
    }
    for (uint32_t batches : {1u, 3u}) {
        std::array<uint32_t,4> clears{}, direct{}, visits{};
        for (uint32_t p = 0; p < 4u * batches; ++p) {
            const auto s = Step(p, 4u, batches);
            Check(s.cascade == p % 4u && s.batch == p / 4u, "round-robin pass order");
            Check(visits[s.cascade]++ == s.batch, "per-cascade batch order");
            if (s.first) ++clears[s.cascade];
            if (s.last) ++direct[s.cascade];
            Check(s.first == (s.batch == 0u) && s.last == (s.batch + 1u == batches),
                  "clear first and direct last");
        }
        for (uint32_t c = 0; c < 4; ++c)
            Check(clears[c] == 1 && direct[c] == 1 && visits[c] == batches,
                  "one clear, one direct fallback per cascade");
    }
    return failures ? 1 : 0;
}
