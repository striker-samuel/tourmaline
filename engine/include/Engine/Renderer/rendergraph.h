#pragma once

enum RenderNodeType {
	Compute,
	Draw
}

struct RenderGraph {
	// Pass description?
	struct RenderNode {
		RenderNodeType type;
	};

	std::vector<RenderNode> nodes;

	void AddPass();
};
