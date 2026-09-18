#pragma once
// Editor CRUD command stack (Maya-style): every scene edit is a command
// with an exact inverse, giving unlimited undo/redo. The stack IS the
// document: saving writes commands[0..cursor] to the .tour ASCII file
// (cluster-space ids), loading replays a .tour into the stack.
//
// World-space ids drive the live viewport; cluster-space ids (stable across
// the deterministic import) go into the file. Translation happens at the
// save/load boundary only.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace editor {

enum class EditVerb : uint8_t {
	Rename,
	Translate,
	Rotate, // degrees XYZ (inspector convention)
	Scale,
	AssignMaterial, // world material index
	SetMatAttr,     // shared material value
	SetNoLightmap,
	SetDitherCloseup, // subtree dither on camera closeup
	Parent, // world parent id
};

struct EditCmd {
	EditVerb verb = EditVerb::Rename;
	uint32_t node = UINT32_MAX;  // world node
	uint32_t node2 = UINT32_MAX; // Parent: new parent (world)
	uint32_t material = UINT32_MAX; // AssignMaterial: new mat (world idx)
	// SetMatAttr target + value:
	uint32_t matWorld = UINT32_MAX;
	std::string attr;
	glm::vec4 vec4{ 0.0f };
	double number = 0.0;
	// After-state (transforms as triple; rename text):
	std::string afterText;
	glm::vec3 afterVec{ 0.0f };
	// Before-state (exact inverse):
	std::string beforeText;
	glm::mat4 beforeMat{ 1.0f }; // transforms: full local matrix
	glm::vec4 beforeVec4{ 0.0f };
	double beforeNum = 0.0;
	bool beforeFlag = false, afterFlag = false;
	uint32_t beforeMaterial = UINT32_MAX; // AssignMaterial undo (world idx)
	uint32_t beforeParent = UINT32_MAX;   // Parent undo (world id)
};

class EditorApp; // command execution lives on the app (world access).

} // namespace editor
