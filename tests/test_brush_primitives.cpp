// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include <Engine/Placement/brush_primitives.h>
#include <cmath>
#include <iostream>
using namespace tour::placement::brush;
int main() {
	int failed = 0;
	const auto check = [&](bool condition, const char* label) {
		std::cout << (condition ? "PASS " : "FAIL ") << label << '\n';
		if (!condition) ++failed;
	};
	Settings s{};
	s.radius = 2.0f; s.density = 3.0f; s.seed = 123;
	const glm::vec3 center(1.0f, 2.0f, 3.0f);
	const auto ball = Scatter(s, center, 1, 3);
	check(!ball.empty(), "brush.ball.not-empty");
	const auto repeat = Scatter(s, center, 1, 3);
	bool same = ball.size() == repeat.size();
	for (size_t i = 0; same && i < ball.size(); ++i)
		same = glm::all(glm::equal(ball[i].position, repeat[i].position))
			&& glm::all(glm::equal(ball[i].normal, repeat[i].normal))
			&& ball[i].weight == repeat[i].weight;
	check(same, "brush.ball.deterministic");
	for (const auto& x : ball) {
		check(glm::length(x.position - center) <= s.radius + 0.0001f,
			"brush.ball.inside");
		check(x.weight >= 0.0f && x.weight <= s.strength,
			"brush.ball.weight");
	}
	s.shape = Shape::SphereShell;
	const auto shell = Scatter(s, center, 1, 3);
	check(!shell.empty(), "brush.shell.not-empty");
	for (const auto& x : shell)
		check(std::abs(glm::length(x.position - center) - s.radius) < 0.0001f,
			"brush.shell.surface");
	s.shape = Shape::Disk;
	for (const auto& x : Scatter(s, center, 2, 7)) {
		check(x.position.y == center.y, "brush.disk.plane");
		check(glm::length(x.position - center) <= s.radius + 0.0001f,
			"brush.disk.radius");
	}
	s.shape = Shape::Box;
	for (const auto& x : Scatter(s, center, 2, 7)) {
		const glm::vec3 d = glm::abs(x.position - center);
		check(d.x <= s.radius && d.y <= s.radius && d.z <= s.radius,
			"brush.box.bounds");
	}
	check(Weight(0.0f, Falloff::Smooth) == 1.0f,
		"brush.falloff.center");
	check(Weight(1.0f, Falloff::Smooth) == 0.0f,
		"brush.falloff.edge");
	int count = 0;
	Stroke(center, center + glm::vec3(1,0,0), s,
		[&](const glm::vec3&, uint32_t) { ++count; });
	check(count >= 2, "brush.stroke.spacing");
	s.density = 1e10f;
	check(Scatter(s, center, 0, 0, 32).size() == 32,
		"brush.sample.cap");
	return failed ? 1 : 0;
}
