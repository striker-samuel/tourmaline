#include <Engine/Memory/arena_flat_array.h>
#include <Engine/Scene/scene.h>
#include <cassert>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

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
    // Copy/move must preserve scene values and transfer storage safely.
    tour::mem::ArenaFlatArray<uint32_t> copied(rows);
    assert(copied.size() == rows.size() && copied.data() != rows.data());
    assert(copied[0] == rows[0]);
    tour::mem::ArenaFlatArray<uint32_t> moved(std::move(copied));
    assert(copied.empty() && copied.data() == nullptr);
    assert(moved.size() == rows.size());
    std::vector<uint32_t> standard = moved;
    assert(standard.size() == moved.size() && standard.back() == 11);
    copied = standard;
    assert(copied.size() == standard.size());
    moved = std::move(copied);
    assert(copied.empty() && moved.size() == standard.size());
    // Authoritative scene arrays use the same storage and keep stable slots.
    Scene graph;
    const int root = addNode(graph, -1, 0);
    const int child = addNode(graph, root, 1);
    assert(graph.hierarchy.size() == 2);
    assert(graph.hierarchy[child].parent == root);
    assert(graph.localTransform.size() == graph.globalTransform.size());
    const auto saved = graph.localTransform;
    assert(saved.size() == graph.localTransform.size());
    assert(saved.data() != graph.localTransform.data());

    Scene movedGraph(std::move(graph));
    assert(movedGraph.hierarchy[child].parent == root);
    assert(movedGraph.globalTransform.size() == 2);
    UnlinkNodeFromParent(movedGraph, child);
    RetireNodeSlot(movedGraph, child);
    assert(!IsNodeAlive(movedGraph, child));
    assert(addNode(movedGraph, root, 1) == child);
    rows.clear();
    assert(rows.empty() && rows.capacity() >= 5);
}
