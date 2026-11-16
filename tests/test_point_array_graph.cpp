// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Placement/point_array_graph.h>
#include <cassert>
#include <cmath>
#include <limits>
#include <string>

using namespace tour::placement::point_graph;

int main() {
    const PointArray original = {
        Point{glm::vec3(1, 2, 3), glm::vec4(1), glm::vec3(0), 0, 10},
        Point{glm::vec3(3, 4, 5), glm::vec4(1), glm::vec3(0), 0, 11},
        Point{glm::vec3(5, 6, 7), glm::vec4(1), glm::vec3(0), 0, 12}
    };
    Graph g;
    g.nodes = {
        Node{1, Op::Input, 0},
        Node{2, Op::Translate, 1, glm::vec4(2, 0, -1, 0)},
        Node{3, Op::SetColor, 2, glm::vec4(0.2f, 0.4f, 0.6f, 1)},
        Node{4, Op::SetVelocity, 3, glm::vec4(0, 3, 0, 0)},
        Node{5, Op::SetPrototype, 4, glm::vec4(0), 7},
        Node{6, Op::KeepEveryNth, 5, glm::vec4(0), 2},
        Node{7, Op::Scale, 1, glm::vec4(9, 9, 9, 0)} // dead branch
    };
    g.output = 6;
    Program program;
    std::string error;
    assert(Compile(g, program, error));
    assert(program.steps.size() == 6);
    PointArray result;
    assert(Evaluate(program, original, result, error));
    assert(result.size() == 2);
    assert(result[0].id == 10 && result[1].id == 12);
    assert(result[0].position == glm::vec3(3, 2, 2));
    assert(result[0].color == glm::vec4(0.2f, 0.4f, 0.6f, 1));
    assert(result[0].velocity == glm::vec3(0, 3, 0));
    assert(result[0].prototype == 7);
    assert(original[0].position == glm::vec3(1, 2, 3));
    assert(original[0].color == glm::vec4(1));

    // Original source can feed another pure output without mutation.
    g.output = 7;
    assert(Compile(g, program, error));
    assert(program.steps.size() == 2);
    assert(Evaluate(program, original, result, error));
    assert(result.size() == 3 && result[0].position == glm::vec3(9, 18, 27));

    const Program prior = program;
    g.nodes.push_back(Node{8, Op::Translate, 8, glm::vec4(1)});
    g.output = 8;
    assert(!Compile(g, program, error) && !error.empty());
    assert(program.steps.size() == prior.steps.size());

    g.output = 9; // unresolved output
    assert(!Compile(g, program, error));
    g.output = 8;
    g.nodes.back().input = 50; // dangling dependency
    assert(!Compile(g, program, error));

    g.nodes.back().input = 1;
    g.nodes.back().value.x = std::numeric_limits<float>::infinity();
    assert(!Compile(g, program, error));

    g.nodes.back().value = glm::vec4(0);
    g.nodes.back().op = Op::KeepEveryNth;
    g.nodes.back().index = 0;
    assert(!Compile(g, program, error));

    // Compilation is deterministic regardless of node insertion order.
    g.nodes.pop_back();
    g.output = 6;
    std::reverse(g.nodes.begin(), g.nodes.end());
    assert(Compile(g, program, error));
    assert(program.steps.size() == 6 && program.steps.front().id == 1);
}
