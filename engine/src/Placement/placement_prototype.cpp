// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Placement/placement_prototype.h>

namespace smvk::placement {

void PrototypeTable::add(Prototype p) {
	if (p.weight < 0.0f) p.weight = 0.0f;
	protos_.push_back(p);
	total_ += p.weight;
	prefix_.push_back(total_);
}

std::optional<uint32_t> PrototypeTable::select(float r) const {
	if (protos_.empty() || total_ <= 0.0f) return std::nullopt;
	// Clamp r into [0,1); r==1 would overrun the last bucket.
	if (r < 0.0f) r = 0.0f;
	if (r >= 1.0f) r = 0.99999994f;
	const float target = r * total_;
	// Linear scan is fine for small tables (typical < 32); switch to
	// binary search if tables ever grow large.
	for (size_t i = 0; i < prefix_.size(); ++i) {
		if (target < prefix_[i]) return static_cast<uint32_t>(i);
	}
	return static_cast<uint32_t>(protos_.size() - 1);
}

uint32_t SelectBySlope(const PrototypeTable& table, float slopeDeg, float thresholdDeg,
	uint32_t vegProto, uint32_t rockProto) {
	if (table.empty()) return 0;
	const uint32_t pick = (slopeDeg > thresholdDeg) ? rockProto : vegProto;
	return pick < table.size() ? pick : 0;
}

} // namespace smvk::placement
