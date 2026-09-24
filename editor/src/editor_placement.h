#pragma once
// PlacementGraph editor: Clarisse-style graph authoring for the engine's
// procedural placement system (CPU oracle + GPU runtime).
//
// This header holds the document model, the graph->IR compiler and the
// editor state. It is UI-independent (no ImGui, no EditorApp): the canvas,
// panels and viewport overlay are EditorApp methods in
// editor_placement_ui.cpp; persistence/preview/mesh extraction live in
// editor_placement.cpp.
//
// Graphs persist as place.* editorSettings in the .tour (never cooked):
//   place.graph.<gid>.name                     = <name>
//   place.graph.<gid>.node.<nid>               = <kind> <nf> <f...> <ni> <i...> | <s0> | <s1> ...
//   place.graph.<gid>.pos.<nid>                = <x> <y>
//   place.graph.<gid>.link.<lid>               = <from> <fromPin> <to> <toPin>
//   place.ui.active                            = <gid>
//   place.ui.open                              = 0|1
// Node id allocation reuses freed ids (keeps files compact).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/Placement/placement_domain.h>
#include <Engine/Placement/placement_eval.h>
#include <Engine/Placement/placement_ids.h>
#include <Engine/Placement/placement_ir.h>
#include <Engine/Placement/placement_point.h>
#include <Engine/Placement/placement_prototype.h>
#include <Engine/Placement/placement_tile.h>

namespace editor::placement {

// Small tolerant string/number helpers shared by the model and the
// EditorApp integration TUs.
namespace detail {
inline std::string FStr(float v) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.6g", (double)v);
	return buf;
}
inline float StrF(const std::string& s, float fb) {
	try { return s.empty() ? fb : std::stof(s); }
	catch (...) { return fb; }
}
inline int StrI(const std::string& s, int fb) {
	try { return s.empty() ? fb : std::stoi(s); }
	catch (...) { return fb; }
}
inline uint64_t StrU64(const std::string& s, uint64_t fb) {
	try { return s.empty() ? fb : std::stoull(s); }
	catch (...) { return fb; }
}
} // namespace detail

// ---- node kinds (stable ids: serialized) ------------------------------
enum class NodeKind : int {
	Grid = 0,       // domain: jittered lattice. f: ox,oy,oz,ex,ez,cx,cz
	Mesh = 1,       // domain: scene mesh surface. i: worldNodeId
	Density = 2,    // const field. f: value
	Noise = 3,      // value-noise field (world XZ). f: freq,ox,oz
	Slope = 4,      // slope mask (surface normal). f: flatDeg,steepDeg
	Altitude = 5,   // altitude band (world Y). f: lo0,lo1,hi0,hi1
	Add = 6,        // a+b
	Mul = 7,        // a*b
	Min = 8,        // min(a,b)
	Max = 9,        // max(a,b)
	Clamp = 10,     // clamp01(x)
	Remap = 11,     // remap x [in0,in1]->[out0,out1]. f: 4
	Smoothstep = 12,// smoothstep(e0,e1,x). f: e0,e1
	Power = 13,     // pow(x,exp). f: exp
	Invert = 14,    // 1-x
	Accept = 15,    // stochastic accept. in density, out density (scaled).
	                // Layer prefers Accept input (scale applies); a bare
	                // field auto-wraps at scale 1.
	Prototypes = 16,// prototype table. f: slopeRock,threshold,w0,r0,... s: uri0,...
	Transform = 17, // scale range. f: minS,maxS
	Spacing = 18,   // min-distance. f: minSpacing (0 = off)
	Layer = 19,     // output sink. in domain,density,protos,xform,spacing.
	                // f: candidates. s: seedU64.
	Count = 20,
};

const char* NodeKindName(NodeKind k);
const char* NodeKindCategory(NodeKind k); // Domain | Field | Flow | Output
// Execution capability badge (spec: compiler reports what blocks GPU).
// MVP: every kind runs on both backends; the table exists so future
// BAKE_ONLY kinds plug in with one line.
uint32_t NodeKindCapabilities(NodeKind k);

// ---- pins --------------------------------------------------------------
enum class PinType { Domain, Field, Protos, Xform, Spacing };
const char* PinTypeName(PinType t);
struct PinDef { const char* name; PinType type; };
std::vector<PinDef> NodeInputs(NodeKind k);
std::vector<PinDef> NodeOutputs(NodeKind k);

// ---- document -----------------------------------------------------------
struct PNode {
	uint32_t id = 0;
	NodeKind kind = NodeKind::Density;
	float pos[2] = { 0, 0 }; // canvas position
	std::vector<float> f;    // params (kind-specific layout, see above)
	std::vector<int> i;      // int params (Mesh node id)
	std::vector<std::string> s; // string params (uris, seed)
};
struct PLink {
	uint32_t id = 0;
	uint32_t from = 0;       // node id
	std::string fromPin;     // pin name ("out" for single-output nodes)
	uint32_t to = 0;
	std::string toPin;       // input pin name
};

struct GraphDoc {
	std::string name = "Scatter";
	std::map<uint32_t, PNode> nodes; // ordered: deterministic files
	std::map<uint32_t, PLink> links;
	uint32_t nextId = 1;

	PNode* find(uint32_t id);
	const PNode* find(uint32_t id) const;
	// Single link feeding (node,toPin), or null.
	const PLink* inputLink(uint32_t node, const std::string& pin) const;
	// Fresh id (reuses freed ids).
	uint32_t allocId();
	// Structural edits (no undo here: EditorDoc wraps them).
	uint32_t addNode(NodeKind kind, float x, float y);
	bool removeNode(uint32_t id); // also drops attached links
	uint32_t addLink(uint32_t from, const std::string& fromPin,
		uint32_t to, const std::string& toPin); // 0 = rejected (type clash)
	bool removeLink(uint32_t id);
};

// Default scaffold: Grid -> Density -> Accept -> Layer plus Prototypes,
// Transform, Spacing pre-wired. A new graph compiles and previews
// immediately.
void BuildDefaultGraph(GraphDoc& doc);

// ---- serialization (one value per tour editorSetting; see header doc) --
std::string SerializeNode(const PNode& n);
bool ParseNode(const std::string& val, PNode& out); // out.id set by caller
std::string SerializeLink(const PLink& l);
bool ParseLink(const std::string& val, PLink& out);
std::string SerializeGraph(const GraphDoc& doc); // multi-line (undo snapshots)
bool ParseGraph(const std::string& text, GraphDoc& out);

// ---- compiler ------------------------------------------------------------
struct CompileResult {
	bool ok = false;
	std::string error;   // first error (UI shows it)
	std::string warning; // non-fatal (defaults applied, preview caps)
	smvk::placement::PlacementProgram program;
	std::vector<int> instrToNode; // program.instrs[i] came from editor node id
	smvk::placement::TileGenConfig config;
	smvk::placement::PrototypeTable prototypes;
	smvk::placement::GridDomain grid; // valid when domainKind == Grid
	uint32_t meshNode = UINT32_MAX;   // valid when domainKind == MeshSurface
	smvk::placement::DomainKind domainKind = smvk::placement::DomainKind::Grid;
	uint64_t seed = 0;
	bool gpuOk = true;      // MVP: always true; framework for BAKE_ONLY
	std::string gpuBlocker; // node label that blocks GPU runtime (future)
};
CompileResult CompileGraph(const GraphDoc& doc, uint64_t graphId);

// ---- live preview (rebuilt on dirty) --------------------------------------
struct PreviewPoint {
	glm::vec3 p{ 0.0f };
	uint32_t proto = 0;
	uint32_t candidate = 0; // candidateIndex (recomputes the StableID)
	smvk::placement::StableID id;
};
struct PreviewCache {
	bool dirty = true;
	bool ok = false;
	std::string error;
	std::string stats;
	CompileResult compiled;
	smvk::placement::PlacementSystem system;
	uint64_t layerKey = 0;
	smvk::placement::GridDomain grid;   // bound domain (for trace re-eval)
	smvk::placement::PlacementMeshData mesh;
	smvk::placement::DomainKind domainKind = smvk::placement::DomainKind::Grid;
	std::vector<PreviewPoint> accepted; // sorted (system order)
	std::vector<PreviewPoint> rejected; // capped sample
	uint32_t candidateTotal = 0;
	uint32_t meshTris = 0; // mesh domain extraction info
	double genMs = 0.0;
	int picked = -1; // index into accepted (-1 = none)
};

// ---- editor session state --------------------------------------------------
struct EditorDoc {
	std::map<uint64_t, GraphDoc> graphs; // ordered: deterministic files
	uint64_t active = 0;
	uint64_t nextGraph = 1;
	bool showGraph = false;
	bool showOverlay = true;
	bool showRejected = true;
	uint32_t previewCap = 4096; // candidate cap for UI responsiveness
	// canvas interaction (per active graph; reset on switch)
	int selected = -1;       // selected node id (-1 = none)
	int selectedLink = -1;   // selected link id
	int linkFrom = -1;       // link-drag source node (or -1)
	std::string linkFromPin; // link-drag source pin
	float pan[2] = { 0, 0 };
	// undo: full-doc snapshots (cap 50)
	std::vector<std::string> undoStack;
	int undoPos = -1;
	void pushUndo();
	bool undo();
	bool redo();
	GraphDoc* current();
	const GraphDoc* current() const;
	uint64_t addGraph(const std::string& name);
	PreviewCache preview;
};

} // namespace editor::placement
