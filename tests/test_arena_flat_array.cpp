#include <Engine/Memory/arena_flat_array.h>
#include <cassert>
#include <cstdint>
#include <span>

int main() {
    tour::mem::ArenaFlatArray<uint32_t> rows;
    uint32_t initial[] = {1, 2};
    rows.Assign(std::span<const uint32_t>(initial));
    assert(rows.size() == 2 && rows[1] == 2);
    const uint32_t* ptr = rows.data();
    rows.Assign(std::span<const uint32_t>(initial, 1));
    assert(rows.data() == ptr && rows.size() == 1);
    uint32_t larger[] = {3, 4, 5};
    rows.Assign(std::span<const uint32_t>(larger));
    assert(rows.size() == 3 && rows[2] == 5);
    rows.resize(5, 42);
    assert(rows.size() == 5 && rows[3] == 42 && rows[4] == 42);
    rows.assign(3, 7);
    assert(rows.size() == 3 && rows[0] == 7 && rows[2] == 7);
    rows.push_back(11);
    assert(rows.size() == 4 && rows[3] == 11);
    assert(rows.begin() != rows.end());
    rows.clear();
    assert(rows.empty() && rows.capacity() >= 5);
}
