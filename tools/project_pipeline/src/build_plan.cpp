// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// BuildPlan helpers: the include-glob matcher ('**' crosses directory
// boundaries, '*'/'?' stay within one segment) and the plan lookups.
// The resolver (recipe_resolver.cpp) drives the matcher against the
// project tree; tests drive it directly.

#include <project_pipeline/build_plan.h>

namespace smvk::project {

namespace {

std::vector<std::string> SplitSegments(const std::string& p) {
	std::vector<std::string> segs;
	size_t start = 0;
	while (start < p.size()) {
		const size_t end = p.find('/', start);
		if (end == std::string::npos) {
			segs.push_back(p.substr(start));
			break;
		}
		segs.push_back(p.substr(start, end - start));
		start = end + 1;
	}
	return segs;
}

// fnmatch-lite within ONE segment (no '/' cases — segmentation already
// happened): classic two-pointer with '*' backtracking.
bool SegmentMatch(const std::string& pat, const std::string& seg) {
	size_t pi = 0, si = 0;
	size_t star = std::string::npos, mark = 0;
	while (si < seg.size()) {
		if (pi < pat.size()
				&& (pat[pi] == '?' || pat[pi] == seg[si])) {
			++pi;
			++si;
		}
		else if (pi < pat.size() && pat[pi] == '*') {
			star = pi++;
			mark = si;
		}
		else if (star != std::string::npos) {
			pi = star + 1;
			si = ++mark;
		}
		else {
			return false;
		}
	}
	while (pi < pat.size() && pat[pi] == '*')
		++pi;
	return pi == pat.size();
}

// '**' = zero or more segments; everything else is per-segment.
bool MatchAt(const std::vector<std::string>& pat, size_t pi,
	const std::vector<std::string>& seg, size_t si) {
	if (pi == pat.size())
		return si == seg.size();
	if (pat[pi] == "**") {
		for (size_t k = si; k <= seg.size(); ++k)
			if (MatchAt(pat, pi + 1, seg, k))
				return true;
		return false;
	}
	if (si == seg.size())
		return false;
	if (!SegmentMatch(pat[pi], seg[si]))
		return false;
	return MatchAt(pat, pi + 1, seg, si + 1);
}

} // namespace

bool RecipeGlobMatch(const std::string& pattern, const std::string& path) {
	const std::vector<std::string> pat = SplitSegments(pattern);
	const std::vector<std::string> seg = SplitSegments(path);
	if (pat.empty() || seg.empty())
		return false;
	return MatchAt(pat, 0, seg, 0);
}

const PlanPak* BuildPlan::FindPak(const std::string& id) const {
	for (const PlanPak& p : paks)
		if (p.id == id)
			return &p;
	return nullptr;
}

} // namespace smvk::project
