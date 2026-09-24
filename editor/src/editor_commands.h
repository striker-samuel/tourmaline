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
	SetVisibility,  // Clarisse-style self hidden flag (hierarchical effect)
	Parent, // world parent id
 	CreateLight, // append a light node (world); params below. c.node =
	             // CREATED world id once forward-executed (UINT32_MAX
	             // before); c.node2 = parent world id
 	CreateTerrain, // append a procedural terrain mesh node (world).
 	             // Mesh params ride vec4/number (tour-shaped, immutable
 	             // after creation): vec4 = (size[m], res, height[m],
 	             // freq[1/m]), number = noise seed. c.node = CREATED
 	             // world id once forward-executed; c.node2 = parent;
 	             // material = world material idx; afterVec = local
 	             // position (= terrain origin corner); afterText = name.
 	             // Post-creation edits address it via tour
 	             // kCreateTerrainRefBase (see CreatedTerrainIndex).
 	SetLightAttr, // light property edit; c.node = light node (world),
	              // lightAttr selects the channel (see below)
};

struct EditCmd {
	EditVerb verb = EditVerb::Rename;
	uint32_t node = UINT32_MAX;  // world node
	uint32_t node2 = UINT32_MAX; // Parent: new parent (world);
	                             // CreateLight: parent (world)
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
	// CreateLight payload (live-world domain: type 1 = point, 2 = spot;
	// cone angles in DEGREES, editor convention; position in afterVec;
	// name in afterText; parent in node2):
	uint32_t lightType = 0;
	glm::vec3 lightColor{ 1.0f, 1.0f, 1.0f };
	float lightIntensity = 0.0f;
	float lightRange = 0.0f; // 0 = infinite
	float lightInnerDeg = 0.0f;
	float lightOuterDeg = 0.0f;
	bool lightStatic = false; // bake-only (kSceneLightFlag_Static)
	bool lightShadow = false; // spot shadow request (spots only)
	// SetLightAttr channel ("color","intensity","range","inner","outer",
	// "static","shadow","name"). Value rides vec4 (color) / number
	// (scalars, degrees for cones) / afterFlag (static/shadow) /
	// afterText (name); before-state rides beforeVec4 / beforeNum /
	// beforeFlag / beforeText.
	std::string lightAttr;
};

class EditorApp; // command execution lives on the app (world access).

} // namespace editor
