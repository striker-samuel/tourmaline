// .tour ASCII scene overlay: writer/parser + cook-cluster replay.
// See Engine/Asset/tour.h for the format contract.

#include <Engine/Asset/tour.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <Engine/App/cluster.h>
#include <Engine/Placement/placement_terrain.h>
#include <Engine/Scene/scene.h>

namespace smvk::tour {
namespace {

std::string Quote(const std::string& s) {
	std::string o = "\"";
	for (char c : s) {
		if (c == '"' || c == '\\')
			o.push_back('\\');
		o.push_back(c);
	}
	o.push_back('"');
	return o;
}

// Tokenize one line: whitespace split, double-quote grouping with
// backslash escapes, '//' comment starts outside quotes. An explicitly
// quoted empty string ("") yields one empty token (so cook lines with
// empty quoted values, e.g. -lmExcludeKw "" / -cubemapName "", round-trip
// instead of swallowing the next flag).
std::vector<std::string> Tokenize(const std::string& line) {
	std::vector<std::string> toks;
	std::string cur;
	bool inQ = false;
	bool esc = false;
	bool tokActive = false; // any chars or quote pairing for the
	                        // current token (preserves "")
	for (size_t i = 0; i < line.size(); ++i) {
		const char c = line[i];
		if (esc) {
			cur.push_back(c);
			tokActive = true;
			esc = false;
			continue;
		}
		if (c == '\\' && inQ) {
			esc = true;
			continue;
		}
		if (c == '"') {
			inQ = !inQ;
			tokActive = true;
			continue;
		}
		if (!inQ && c == '/' && i + 1 < line.size() && line[i + 1] == '/')
			break;
		if (!inQ && std::isspace((unsigned char)c)) {
			if (tokActive) {
				toks.push_back(cur);
				cur.clear();
				tokActive = false;
			}
			continue;
		}
		cur.push_back(c);
		tokActive = true;
	}
	if (tokActive)
		toks.push_back(cur);
	return toks;
}

bool ParseU32(const std::string& s, uint32_t& out) {
	unsigned long long v = 0;
	auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
	if (ec != std::errc() || p != s.data() + s.size())
		return false;
	out = uint32_t(v);
	return true;
}

bool ParseDouble(const std::string& s, double& out) {
	try {
		size_t n = 0;
		out = std::stod(s, &n);
		return n == s.size();
	}
	catch (...) {
		return false;
	}
}

const char* FlagVal(bool b) { return b ? "1" : "0"; }

// One command line (Maya setAttr flavor: non-scalar values carry an
// explicit -type — double3/float4/string/bool/enum; plain scalars stay
// bare). `indent` prefixes the line: "" top-level, "\t" for -ref
// properties tabulated under their create.
static void WriteCommandLine(std::ostream& f, const Command& c,
	const char* indent) {
	f << indent;
	switch (c.verb) {
	case Verb::Rename:
		f << "rename -n " << c.node << " " << Quote(c.text);
		break;
	case Verb::SetTranslate:
		f << "setAttr -n " << c.node << " -at translate -type \"double3\" -t "
			<< c.vec.x << " " << c.vec.y << " " << c.vec.z;
		break;
	case Verb::SetRotate:
		f << "setAttr -n " << c.node << " -at rotate -type \"double3\" -t "
			<< c.vec.x << " " << c.vec.y << " " << c.vec.z;
		break;
	case Verb::SetScale:
		f << "setAttr -n " << c.node << " -at scale -type \"double3\" -t "
			<< c.vec.x << " " << c.vec.y << " " << c.vec.z;
		break;
	case Verb::AssignMaterial:
		f << "assignMaterial -n " << c.node << " -m " << c.material;
		break;
	case Verb::SetMatAttr:
		f << "setMatAttr -m " << c.material << " -at " << c.text;
		if (c.text == "baseColor" || c.text == "emissive" ||
			c.text == "layers")
			f << " -type \"float4\" -t " << c.vec4.x << " " << c.vec4.y << " "
				<< c.vec4.z << " " << c.vec4.w;
		else if (c.text == "alphaMode")
			f << " -t " << (c.number < 0.5 ? "opaque"
				: c.number < 1.5 ? "mask" : "blend");
		else
			f << " -v " << c.number;
		break;
	case Verb::SetNoLightmap:
		f << "setNoLightmap -n " << c.node << " -type \"bool\" -v "
			<< FlagVal(c.flag);
		break;
	case Verb::SetDitherCloseup:
		f << "setDitherCloseup -n " << c.node << " -type \"bool\" -v "
			<< FlagVal(c.flag);
		break;
	case Verb::SetVisibility:
		f << "setVisibility -n " << c.node << " -type \"bool\" -v "
			<< FlagVal(c.flag);
		break;
	case Verb::SetStatic:
		f << "setStatic -n " << c.node << " -type \"bool\" -v "
			<< FlagVal(c.flag);
		break;
	case Verb::Delete:
		f << "delete";
		if (!c.uuid.empty())
			f << " -uuid " << c.uuid;
		f << " -n " << c.node;
		if (!c.name.empty())
			f << " -name " << Quote(c.name);
		break;
		case Verb::SetUuid:
			f << "uuid -n " << c.node << " -v " << c.uuid;
			break;
		case Verb::CreateNode: {
			// Unified form (new files write ONLY this): createNode
			// -at <type> with the per-type payload below. -p is the
			// parent: bare id (legacy) or quoted name/path/uuid.
			f << "createNode -at " << c.nodeType << " -p ";
			if (!c.parentRef.empty())
				f << Quote(c.parentRef);
			else
				f << c.node;
			f << " -name " << Quote(c.text);
			if (c.nodeType == "transform") {
				f << " -t " << c.vec.x << " " << c.vec.y << " "
					<< c.vec.z << " " << c.vec4.x << " " << c.vec4.y
					<< " " << c.vec4.z << " -s " << c.svec.x << " "
					<< c.svec.y << " " << c.svec.z;
			}
			else if (c.nodeType == "pointLight"
				|| c.nodeType == "spotLight") {
				f << " -t " << c.vec.x << " " << c.vec.y << " "
					<< c.vec.z << " " << c.vec4.y << " " << c.vec4.z
					<< " " << c.vec4.w << " " << c.number << " "
					<< (c.flag ? 1 : 0) << " " << c.node2;
			}
			else if (c.nodeType == "terrain") {
				f << " -t " << c.vec.x << " " << c.vec.y << " "
					<< c.vec.z << " " << c.vec4.x << " " << c.vec4.y
					<< " " << c.vec4.z << " " << c.vec4.w << " "
					<< c.number << " " << (c.node2 & 0xFFFFu) << " "
					<< ((c.node2 >> 16) & 0xFFFFu) << " -m "
					<< c.material;
			}
			else if (c.nodeType == "cvolume") {
				// -t packs local position (box center: x y z) +
				// priority spacing insideFade outsideFade
				// (7 values); -s packs local size (full extents).
				f << " -t " << c.vec.x << " " << c.vec.y << " "
					<< c.vec.z << " " << c.vec4.x << " " << c.vec4.y
					<< " " << c.vec4.z << " " << c.vec4.w << " -s "
					<< c.svec.x << " " << c.svec.y << " " << c.svec.z;
			}
			else if (c.nodeType == "cubemap") {
				// -t packs local position (pinned probe: x y z);
				// -v packs the influence radius (m).
				f << " -t " << c.vec.x << " " << c.vec.y << " "
					<< c.vec.z << " -v " << c.number;
			}
			else if (c.nodeType == "camera") {
				// -t packs local position + rotation-deg (x y z rx
				// ry rz) + lens (projFloat fovY near far orthoHalfH).
				f << " -t " << c.vec.x << " " << c.vec.y << " "
					<< c.vec.z << " " << c.svec.x << " " << c.svec.y
					<< " " << c.svec.z << " " << c.vec4.x << " "
					<< c.vec4.y << " " << c.vec4.z << " " << c.vec4.w
					<< " " << c.number;
			}
			// System types (material/file/image) carry no payload:
			// the line above is the whole record.
			break;
		}
		case Verb::CreateTransform: {
			// -t packs local position + rotation: x y z rx ry rz
			// (rotation in degrees — editor convention); -s packs
			// local scale sx sy sz. -p is the parent: bare id
			// (legacy) or quoted name/path/uuid.
			f << "createTransform -p ";
			if (!c.parentRef.empty())
				f << Quote(c.parentRef);
			else
				f << c.node;
			f << " -name "
				<< Quote(c.text) << " -t " << c.vec.x << " " << c.vec.y
				<< " " << c.vec.z << " " << c.vec4.x << " " << c.vec4.y
				<< " " << c.vec4.z << " -s " << c.svec.x << " "
				<< c.svec.y << " " << c.svec.z;
			break;
		}
		case Verb::Parent:
			f << "parent -n " << c.node;
			if (!c.parentRef.empty())
				f << " -p " << Quote(c.parentRef);
			else
				f << " -p " << c.node2;
			break;
		case Verb::CreateLight: {
			// -t packs position + every scalar: x y z intensity range
			// innerDeg outerDeg static01 shadow01 (9 values). -p is the
			// parent: bare id (legacy) or quoted name/path/uuid.
			f << "createLight -p ";
			if (!c.parentRef.empty())
				f << Quote(c.parentRef);
			else
				f << c.node;
			f << " -name " << Quote(c.text)
			<< " -at " << (c.vec4.x >= 1.5f ? "spot" : "point")
			<< " -t " << c.vec.x << " " << c.vec.y << " " << c.vec.z
			<< " " << c.vec4.y << " " << c.vec4.z << " " << c.vec4.w
			<< " " << c.number << " " << (c.flag ? 1 : 0) << " " << c.node2;
		break;
	}
	case Verb::CreateTerrain: {
		// -t packs position + mesh params: x y z size res height
		// freq seed tilesX tilesZ (10 values); -m is the cluster
		// material pool index; the tile split rides node2 packed as
		// tilesX | (tilesZ << 16). -p is the parent: bare id
		// (legacy) or quoted name/path/uuid.
		f << "createTerrain -p ";
		if (!c.parentRef.empty())
			f << Quote(c.parentRef);
		else
			f << c.node;
		f << " -name " << Quote(c.text)
			<< " -at terrain"
			<< " -t " << c.vec.x << " " << c.vec.y << " " << c.vec.z
			<< " " << c.vec4.x << " " << c.vec4.y << " " << c.vec4.z
			<< " " << c.vec4.w << " " << c.number
			<< " " << (c.node2 & 0xFFFFu)
			<< " " << ((c.node2 >> 16) & 0xFFFFu)
			<< " -m " << c.material;
		break;
	}
	case Verb::CreateCVolume: {
		// -t packs local position (box center: x y z) + priority
		// spacing insideFade outsideFade (7 values); -s packs local
		// size (full extents: sx sy sz). -p is the parent: bare id
		// (legacy) or quoted name/path/uuid.
		f << "createCvolume -p ";
		if (!c.parentRef.empty())
			f << Quote(c.parentRef);
		else
			f << c.node;
		f << " -name " << Quote(c.text)
			<< " -t " << c.vec.x << " " << c.vec.y << " " << c.vec.z
			<< " " << c.vec4.x << " " << c.vec4.y << " "
			<< c.vec4.z << " " << c.vec4.w
			<< " -s " << c.svec.x << " " << c.svec.y << " " << c.svec.z;
		break;
	}
	case Verb::CreateCubemap: {
		// -t packs local position (pinned probe: x y z); -v packs
		// the influence radius (m). -p is the parent: bare id
		// (legacy) or quoted name/path/uuid.
		f << "createCubemap -p ";
		if (!c.parentRef.empty())
			f << Quote(c.parentRef);
		else
			f << c.node;
		f << " -name " << Quote(c.text)
			<< " -t " << c.vec.x << " " << c.vec.y << " " << c.vec.z
			<< " -v " << c.number;
		break;
	}
	case Verb::CreateCamera: {
		// -t packs local position + rotation-deg (x y z rx ry rz) +
		// lens (projFloat fovY near far orthoHalfH, 11 values); -at
		// echoes "camera" (unified createNode form). -p is the parent:
		// bare id (legacy) or quoted name/path/uuid.
		f << "createCamera -p ";
		if (!c.parentRef.empty())
			f << Quote(c.parentRef);
		else
			f << c.node;
		f << " -name " << Quote(c.text)
			<< " -at camera"
			<< " -t " << c.vec.x << " " << c.vec.y << " " << c.vec.z
			<< " " << c.svec.x << " " << c.svec.y << " " << c.svec.z
			<< " " << c.vec4.x << " " << c.vec4.y << " " << c.vec4.z
			<< " " << c.vec4.w << " " << c.number;
		break;
	}
	case Verb::SetCamAttr: {
		// Same addressing shape as setLightAttr (-ref k of the k-th
		// createCamera, else -n cluster id). Scalars ride -v; name
		// rides -v as a string; projection rides -v as 0/1.
		f << "setCamAttr ";
		if ((c.node & kCreateCameraRefBase) != 0)
			f << "-ref " << (c.node & ~kCreateCameraRefBase);
		else
			f << "-n " << c.node;
		f << " -at " << c.text;
		if (c.text == "name")
			f << " -type \"string\" -v " << Quote(c.name);
		else if (c.text == "position" || c.text == "rotation")
			f << " -type \"double3\" -t " << c.vec4.x << " " << c.vec4.y
				<< " " << c.vec4.z;
		else if (c.text == "projection")
			f << " -v " << (c.number >= 0.5 ? 1 : 0);
		else
			f << " -v " << c.number;
		break;
	}
	case Verb::TerrainPaint: {
		// -t packs: worldX worldZ radius flattenTarget (4 values);
		// -v carries strength; -ref names the k-th createTerrain.
		f << "terrainPaint -ref " << (c.node & ~kCreateTerrainRefBase)
			<< " -at " << c.text
			<< " -t " << c.vec4.x << " " << c.vec4.y
			<< " " << c.vec4.z << " " << c.vec4.w
			<< " -v " << c.number;
		break;
	}
	case Verb::SetLightAttr: {
		f << "setLightAttr ";
		if ((c.node & kCreateLightRefBase) != 0)
			f << "-ref " << (c.node & ~kCreateLightRefBase);
		else
			f << "-n " << c.node;
		f << " -at " << c.text;
		if (c.text == "color" || c.text == "position"
			|| c.text == "rotation")
			f << " -type \"double3\" -t " << c.vec4.x << " " << c.vec4.y
				<< " " << c.vec4.z;
		else if (c.text == "name")
			f << " -type \"string\" -v " << Quote(c.name);
		else if (c.text == "static" || c.text == "shadow")
			f << " -type \"bool\" -v " << (c.flag ? "on" : "off");
		else
			f << " -v " << c.number;
		break;
	}
	case Verb::SetCvolAttr: {
		// Same addressing shape as setLightAttr (-ref k of the k-th
		// createCvolume, else -n cluster id). Triples (position,
		// rotation-deg, size) ride -t; name rides -v as a string;
		// scalars (priority, spacing, inside, outside) ride -v.
		f << "setCvolAttr ";
		if ((c.node & kCreateCvolRefBase) != 0)
			f << "-ref " << (c.node & ~kCreateCvolRefBase);
		else
			f << "-n " << c.node;
		f << " -at " << c.text;
		if (c.text == "position" || c.text == "rotation"
			|| c.text == "size")
			f << " -type \"double3\" -t " << c.vec4.x << " " << c.vec4.y
				<< " " << c.vec4.z;
		else if (c.text == "name")
			f << " -type \"string\" -v " << Quote(c.name);
		else
			f << " -v " << c.number;
		break;
	}
	case Verb::SetCubemapAttr: {
		// Same addressing shape as setCvolAttr (-ref k of the k-th
		// createCubemap, else -n cluster id). Position rides -t;
		// name rides -v as a string; radius rides -v.
		f << "setCubemapAttr ";
		if ((c.node & kCreateCubemapRefBase) != 0)
			f << "-ref " << (c.node & ~kCreateCubemapRefBase);
		else
			f << "-n " << c.node;
		f << " -at " << c.text;
		if (c.text == "position")
			f << " -type \"double3\" -t " << c.vec4.x << " " << c.vec4.y
				<< " " << c.vec4.z;
		else if (c.text == "name")
			f << " -type \"string\" -v " << Quote(c.name);
		else
			f << " -v " << c.number;
		break;
	}
	}
	if (!c.name.empty())
		f << " // " << c.name;
	f << ";\n";
}

} // namespace

bool OverrideAttrFromString(const std::string& s, OverrideAttr& out) {
	if (s == "visibility" || s == "hidden" || s == "hide") {
		out = OverrideAttr::Visibility;
		return true;
	}
	if (s == "material") {
		out = OverrideAttr::Material;
		return true;
	}
	if (s == "noLightmap" || s == "no_light_map") {
		out = OverrideAttr::NoLightmap;
		return true;
	}
	if (s == "ditherCloseup" || s == "dither" || s == "dither_closeup") {
		out = OverrideAttr::DitherCloseup;
		return true;
	}
	return false;
}

const char* OverrideAttrName(OverrideAttr a) {
	switch (a) {
	case OverrideAttr::Visibility: return "visibility";
	case OverrideAttr::Material: return "material";
	case OverrideAttr::NoLightmap: return "noLightmap";
	case OverrideAttr::DitherCloseup: return "ditherCloseup";
	}
	return "?";
}

bool OverridePatternMatches(const std::string& pattern,
	const std::string& nodeName) {
	if (pattern.empty())
		return false;
	try {
		std::regex rx(pattern, std::regex::ECMAScript | std::regex::optimize);
		return std::regex_search(nodeName, rx);
	}
	catch (...) {
		return false;
	}
}

std::string DescribeOverride(const OverrideRule& r) {
	char buf[512];
	if (r.attr == OverrideAttr::Material)
		std::snprintf(buf, sizeof(buf), "override /%s/ .%s -> mat.%u%s",
			r.pattern.c_str(), OverrideAttrName(r.attr), r.material,
			r.enabled ? "" : " [off]");
	else
		std::snprintf(buf, sizeof(buf), "override /%s/ .%s = %d%s",
			r.pattern.c_str(), OverrideAttrName(r.attr), r.flag ? 1 : 0,
			r.enabled ? "" : " [off]");
	return buf;
}

std::string Fingerprint(const std::string& path) {
	std::error_code ec;
	const auto sz = std::filesystem::file_size(path, ec);
	if (ec)
		return {};
	const auto mt = std::filesystem::last_write_time(path, ec);
	if (ec)
		return {};
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%llx:%llx",
		(unsigned long long)sz,
		(unsigned long long)mt.time_since_epoch().count());
	return buf;
}

bool SaveTour(const std::string& path, const Doc& doc, std::string& err) {
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f) {
		err = "cannot write: " + path;
		return false;
	}
	// Maya-style header: scene name + last-modified stamp (the old
	// source-asset/cluster-space comment lines are gone).
	{
		std::string name = path;
		const size_t slash = name.find_last_of("/\\");
		if (slash != std::string::npos)
			name = name.substr(slash + 1);
		std::time_t now = std::time(nullptr);
		char stamp[64] = {};
		if (std::tm* lt = std::localtime(&now))
			std::strftime(stamp, sizeof(stamp), "%a, %b %d, %Y %I:%M:%S %p",
				lt);
		f << "//Name: " << name << "\n";
		f << "//Last modified: " << stamp << "\n";
	}
	// `source` is DEPRECATED (geometry may come entirely from
	// references): written only when the session actually has one.
	if (!doc.source.empty() || !doc.fingerprint.empty())
		f << "source -p " << Quote(doc.source)
			<< " -fingerprint " << Quote(doc.fingerprint) << ";\n";
	// References FIRST (Maya file-block order), then the fileInfo
	// constants block, then the command list.
	for (const ReferenceCmd& r : doc.references) {
		f << "reference";
		if (r.root)
			f << " -root"; // the scene's own source: no placeholder/ns
		f << " -n " << r.parent
			<< " -source " << Quote(r.source);
		if (!r.guid.empty())
			f << " -guid " << r.guid;
		if (!r.name.empty())
			f << " -name " << Quote(r.name);
		if (r.hasTranslate)
			f << " -t " << r.translate.x << " " << r.translate.y
				<< " " << r.translate.z;
		if (!r.enabled)
			f << " -enabled off";
		f << ";\n";
	}
	// fileInfo constants (Maya fileInfo block): fixed format metadata,
	// then anything the doc carries.
	{
		bool hasApp = false, hasProduct = false, hasVersion = false;
		for (const auto& [k, v] : doc.fileInfo) {
			if (k == "application")
				hasApp = true;
			if (k == "product")
				hasProduct = true;
			if (k == "version")
				hasVersion = true;
		}
		if (!hasApp)
			f << "fileInfo -k " << Quote("application")
				<< " -v " << Quote("vulkantoy-tour") << ";\n";
		if (!hasProduct)
			f << "fileInfo -k " << Quote("product")
				<< " -v " << Quote("VulkanToy") << ";\n";
		if (!hasVersion)
			f << "fileInfo -k " << Quote("version")
				<< " -v " << Quote(std::to_string(kTourVersion)) << ";\n";
		for (const auto& [k, v] : doc.fileInfo)
			f << "fileInfo -k " << Quote(k) << " -v " << Quote(v) << ";\n";
	}
	// File nodes (Maya file/image declarations): cooked records — the
	// cooker consumes them for KTX conversion. One line per node.
	for (const FileNode& fn : doc.fileNodes) {
		if (fn.path.empty())
			continue; // half-written record: skip (mirrors image load)
		const char* cs = (fn.colorspace != 0) ? "linear" : "srgb";
		const char* wrap = "repeat";
		if (fn.wrap == 1)
			wrap = "clamp";
		else if (fn.wrap == 2)
			wrap = "mirrored";
		const char* filter = (fn.filter != 0) ? "nearest" : "linear";
		f << "fileNode -name " << Quote(fn.name)
			<< " -path " << Quote(fn.path)
			<< " -colorspace " << cs
			<< " -wrap " << wrap
			<< " -filter " << filter << ";\n";
	}
	// Material-asset imports (ordered; see MaterialAssetCmd): replayed
	// BEFORE the command list so later assignMaterial pool indices match
	// the authoring session. The default seed carries no path.
	for (const MaterialAssetCmd& ma : doc.materialAssets) {
		if (ma.editorDefault) {
			f << "materialAsset -default";
			if (!ma.name.empty())
				f << " -name " << Quote(ma.name);
			f << ";\n";
			continue;
		}
		if (ma.path.empty())
			continue; // half-written record: skip (mirrors fileNode)
		f << "materialAsset -path " << Quote(ma.path);
		if (!ma.name.empty())
			f << " -name " << Quote(ma.name);
		f << ";\n";
	}
	// Tabulated create properties (Maya createNode + setAttr flavor):
	// -ref-addressed setLightAttr (-ref k of the k-th light-family
	// create), TerrainPaint (-ref k of the k-th terrain-family create)
	// and setCvolAttr (-ref k of the k-th cvolume-family create) ride
	// directly under their create, tab-indented.
	// Families span both spellings (legacy createLight/createTerrain/
	// createCvolume AND unified createNode pointLight/spotLight/
	// terrain/cvolume) in one file-order index each, so -ref indices
	// resolve identically.
	auto createFamily = [](const Command& c) -> int {
		if (c.verb == Verb::CreateLight)
			return 0;
		if (c.verb == Verb::CreateTerrain)
			return 1;
		if (c.verb == Verb::CreateCVolume)
			return 2;
		if (c.verb == Verb::CreateCubemap)
			return 3;
		if (c.verb == Verb::CreateCamera)
			return 4;
		if (c.verb == Verb::CreateNode) {
			if (c.nodeType == "pointLight" || c.nodeType == "spotLight")
				return 0;
			if (c.nodeType == "terrain")
				return 1;
			if (c.nodeType == "cvolume")
				return 2;
			if (c.nodeType == "cubemap")
				return 3;
			if (c.nodeType == "camera")
				return 4;
		}
		return -1;
	};
	std::vector<char> cmdConsumed(doc.commands.size(), 0);
	for (size_t i = 0; i < doc.commands.size(); ++i) {
		if (cmdConsumed[i])
			continue;
		const Command& c = doc.commands[i];
		WriteCommandLine(f, c, "");
		const int family = createFamily(c);
		if (family < 0)
			continue;
		// k = this create's file-order index within its family.
		size_t k = 0;
		for (size_t m = 0; m < i; ++m)
			if (createFamily(doc.commands[m]) == family)
				++k;
		for (size_t j = 0; j < doc.commands.size(); ++j) {
			if (cmdConsumed[j])
				continue;
			const Command& d = doc.commands[j];
			const bool child =
				(family == 0 && d.verb == Verb::SetLightAttr
					&& (d.node & kCreateLightRefBase) != 0
					&& (d.node & ~kCreateLightRefBase) == k)
				|| (family == 1 && d.verb == Verb::TerrainPaint
					&& (d.node & kCreateTerrainRefBase) != 0
					&& (d.node & ~kCreateTerrainRefBase) == k)
				|| (family == 2 && d.verb == Verb::SetCvolAttr
					&& (d.node & kCreateCvolRefBase) != 0
					&& (d.node & ~kCreateCvolRefBase) == k)
				|| (family == 3 && d.verb == Verb::SetCubemapAttr
					&& (d.node & kCreateCubemapRefBase) != 0
					&& (d.node & ~kCreateCubemapRefBase) == k)
				|| (family == 4 && d.verb == Verb::SetCamAttr
					&& (d.node & kCreateCameraRefBase) != 0
					&& (d.node & ~kCreateCameraRefBase) == k);
			if (child) {
				WriteCommandLine(f, d, "\t");
				cmdConsumed[j] = 1;
			}
		}
	}
	// Wide Clarisse-style overrides (ordered; later wins at replay).
	for (const OverrideRule& r : doc.overrides) {
		f << "override -pattern " << Quote(r.pattern)
			<< " -at " << OverrideAttrName(r.attr);
		if (r.attr == OverrideAttr::Material)
			f << " -m " << r.material;
		else
			f << " -v " << FlagVal(r.flag);
		f << " -enabled " << (r.enabled ? "on" : "off") << ";\n";
	}
	// Editor-only settings (editorSet): persisted for the editor, never
	// applied at cook (ApplyTourToCluster ignores doc.editorSettings).
	for (const auto& [k, v] : doc.editorSettings)
		f << "editorSet -k " << Quote(k) << " -v " << Quote(v) << ";\n";
	// (Scene references write up front, right after the source line —
	// Maya file-block order.)
	if (doc.hasCook) {
		const TourCook& k = doc.cook;
		f << "cook -outPak " << Quote(k.outPak)
			<< " -meshopt " << (k.meshopt ? "on" : "off")
			<< " -lods " << (k.lods ? "on" : "off")
			<< " -lodRatios " << Quote(k.lodRatios)
			<< " -lodError " << k.lodError
			<< " -lightmap " << (k.lightmap ? "on" : "off")
			<< " -lmMetersPerTexel " << k.lmMetersPerTexel
			<< " -lmLobes " << k.lmLobes
			<< " -lmAtlasWidth " << k.lmAtlasWidth
			<< " -lmRays " << k.lmRays
			<< " -lmProxyRes " << k.lmProxyRes
			<< " -lmExcludeSmall " << k.lmExcludeSmall
			<< " -lmExcludeKw " << Quote(k.lmExcludeKw)
			<< " -sunAzimuth " << k.sunAzimuth
			<< " -sunElevation " << k.sunElevation
			<< " -sunIntensity " << k.sunIntensity
			<< " -skyIntensity " << k.skyIntensity
			<< " -sunColor " << Quote(std::to_string(k.sunColor.x) + " "
				+ std::to_string(k.sunColor.y) + " "
				+ std::to_string(k.sunColor.z))
			<< " -cubemap " << (k.cubemap ? "on" : "off")
			<< " -cubemapName " << Quote(k.cubemapName)
			<< " -cubemapSize " << k.cubemapSize
			<< " -cubemapRays " << k.cubemapRays
			<< " -meshlets " << (k.meshlets ? "on" : "off")
			<< " -meshletVerts " << k.meshletMaxVerts
			<< " -meshletTris " << k.meshletMaxTris
			<< " -meshletConeWeight " << k.meshletConeWeight
			<< " -giCvol " << (k.giCvol ? "on" : "off")
			<< " -giWorld " << (k.giWorld ? "on" : "off")
			<< " -autoKtx " << (k.autoKtx ? "on" : "off")
			<< ";\n";
	}
	if (!f.good()) {
		err = "write failed: " + path;
		return false;
	}
	return true;
}

// -n <id> / -m <id> / -p <id> / -at <name> / -t <nums...> / -v <num|word>
// -pattern <regex> / -enabled <on|off> (override rules only)
// -source <path> / -name <word> (reference lines only)
// -ref <k> (setLightAttr on the k-th createLight, terrainPaint on the
// k-th createTerrain, setCvolAttr on the k-th createCvolume in the doc)
// -uuid <hex> (delete addressing / uuid lines)
// -type <datatype> (setAttr value flavor, informational)
// -s <nums...> (createTransform scale triple)
struct Flags {
	bool hasN = false, hasM = false, hasP = false, hasAt = false;
	uint32_t n = 0, m = 0, p = 0;
	std::string pStr;   // -p symbolic form (name/path/uuid)
	bool hasPStr = false;
	std::string at;
	std::vector<std::string> t; // -t values
	std::vector<std::string> svec; // -s values (createTransform scale)
	bool hasS = false;
	std::string v;              // -v value (raw)
	bool hasV = false;
	bool hasRef = false;
	uint32_t ref = 0; // createLight creation-order index
	std::string pattern;
	bool hasPattern = false;
	bool enabled = true;
	bool hasEnabled = false;
	std::string source;
	bool hasSource = false;
	std::string name;
	bool hasName = false;
	bool root = false;     // -root (reference lines only: the scene's
	                       // own source, inlined without placeholder/ns)
	bool hasRoot = false;
	std::string uuid;      // -uuid <hex> (delete addressing / uuid lines)
	bool hasUuid = false;
	std::string guid;      // -guid <hex> (reference asset identity:
	                       // the source file's .meta GUID, stable across
	                       // moves/renames; -source stays as fallback)
	bool hasGuid = false;
	std::string type;      // -type <datatype> (setAttr value flavor)
	bool hasType = false;
};

static bool ParseFlagWord(const std::string& w, bool& out) {
	if (w == "1" || w == "on" || w == "true" || w == "yes") {
		out = true;
		return true;
	}
	if (w == "0" || w == "off" || w == "false" || w == "no") {
		out = false;
		return true;
	}
	return false;
}

static bool ParseFlags(const std::vector<std::string>& toks, size_t from,
	Flags& f, std::string& err, int lineNo) {
	for (size_t i = from; i < toks.size();) {
		const std::string& k = toks[i];
		if ((k == "-n" || k == "-m") && i + 1 < toks.size()) {
			std::string idTok = toks[i + 1];
			if (!idTok.empty() && idTok.back() == ';')
				idTok.pop_back();
			uint32_t v = 0;
			if (!ParseU32(idTok, v)) {
				err = "line " + std::to_string(lineNo) + ": bad id '" + toks[i + 1] + "'";
				return false;
			}
			if (k == "-n") {
				f.hasN = true;
				f.n = v;
			}
			else {
				f.hasM = true;
				f.m = v;
			}
			i += 2;
		}
		else if (k == "-p" && i + 1 < toks.size()) {
			// Parent: bare number (legacy id) or quoted name/path/uuid
			// (symbolic form — string/dag addressing + uuid coverage
			// for unnamed nodes).
			std::string idTok = toks[i + 1];
			if (!idTok.empty() && idTok.back() == ';')
				idTok.pop_back();
			uint32_t v = 0;
			if (ParseU32(idTok, v)) {
				f.hasP = true;
				f.p = v;
			}
			else {
				f.hasPStr = true;
				f.pStr = idTok;
			}
			i += 2;
		}
		else if (k == "-at" && i + 1 < toks.size()) {
			f.hasAt = true;
			f.at = toks[i + 1];
			i += 2;
		}
		else if (k == "-root") {
			f.hasRoot = true;
			f.root = true;
			++i;
		}
		else if (k == "-t") {
			++i;
			auto isFlag = [](const std::string& w) {
				return w == "-n" || w == "-m" || w == "-p" ||
					w == "-at" || w == "-t" || w == "-v" || w == "-s" ||
					w == "-pattern" || w == "-enabled" ||
					w == "-fingerprint" || w == "-ref" ||
					w == "-uuid" || w == "-source" || w == "-name" ||
					w == "-type" || w == "-root";
			};
			while (i < toks.size() && !isFlag(toks[i])) {
				std::string v = toks[i];
				bool term = false;
				if (!v.empty() && v.back() == ';') {
					v.pop_back();
					term = true;
				}
				f.t.push_back(v);
				++i;
				if (term)
					break;
			}
		}
		else if (k == "-s") {
			++i;
			auto isFlag = [](const std::string& w) {
				return w == "-n" || w == "-m" || w == "-p" ||
					w == "-at" || w == "-t" || w == "-v" || w == "-s" ||
					w == "-pattern" || w == "-enabled" ||
					w == "-fingerprint" || w == "-ref" ||
					w == "-uuid" || w == "-source" || w == "-name" ||
					w == "-type" || w == "-root";
			};
			while (i < toks.size() && !isFlag(toks[i])) {
				std::string v = toks[i];
				bool term = false;
				if (!v.empty() && v.back() == ';') {
					v.pop_back();
					term = true;
				}
				f.svec.push_back(v);
				++i;
				if (term)
					break;
			}
			f.hasS = !f.svec.empty();
		}
		else if (k == "-v" && i + 1 < toks.size()) {
			f.hasV = true;
			f.v = toks[i + 1];
			if (!f.v.empty() && f.v.back() == ';')
				f.v.pop_back();
			i += 2;
		}
		else if (k == "-ref" && i + 1 < toks.size()) {
			std::string idTok = toks[i + 1];
			if (!idTok.empty() && idTok.back() == ';')
				idTok.pop_back();
			uint32_t v = 0;
			if (!ParseU32(idTok, v)) {
				err = "line " + std::to_string(lineNo) + ": bad ref '" + toks[i + 1] + "'";
				return false;
			}
			f.hasRef = true;
			f.ref = v;
			i += 2;
		}
		else if (k == "-pattern" && i + 1 < toks.size()) {
			f.hasPattern = true;
			f.pattern = toks[i + 1];
			if (!f.pattern.empty() && f.pattern.back() == ';')
				f.pattern.pop_back();
			i += 2;
		}
		else if (k == "-enabled" && i + 1 < toks.size()) {
			std::string w = toks[i + 1];
			if (!w.empty() && w.back() == ';')
				w.pop_back();
			bool b = true;
			if (ParseFlagWord(w, b)) {
				f.hasEnabled = true;
				f.enabled = b;
			}
			i += 2;
		}
		else if (k == "-source" && i + 1 < toks.size()) {
			f.hasSource = true;
			f.source = toks[i + 1];
			if (!f.source.empty() && f.source.back() == ';')
				f.source.pop_back();
			i += 2;
		}
		else if (k == "-name" && i + 1 < toks.size()) {
			f.hasName = true;
			f.name = toks[i + 1];
			if (!f.name.empty() && f.name.back() == ';')
				f.name.pop_back();
			i += 2;
		}
		else if (k == "-uuid" && i + 1 < toks.size()) {
			std::string w = toks[i + 1];
			if (!w.empty() && w.back() == ';')
				w.pop_back();
			f.hasUuid = true;
			f.uuid = w;
			i += 2;
		}
		else if (k == "-guid" && i + 1 < toks.size()) {
			std::string w = toks[i + 1];
			if (!w.empty() && w.back() == ';')
				w.pop_back();
			f.hasGuid = true;
			f.guid = w;
			i += 2;
		}
		else if (k == "-type" && i + 1 < toks.size()) {
			// Value datatype (Maya -type flavor: double3/float4/
			// string/bool/enum) — informational for now: the values
			// parse exactly as before.
			std::string w = toks[i + 1];
			if (!w.empty() && w.back() == ';')
				w.pop_back();
			f.hasType = true;
			f.type = w;
			i += 2;
		}
		else if (k == "-fingerprint" && i + 1 < toks.size()) {
			i += 2; // handled by the source parser below
		}
		else {
			// positional (rename target name, source path, cook values...).
			// A bare ";" (space before the terminator) carries nothing.
			std::string pos = k;
			if (!pos.empty() && pos.back() == ';')
				pos.pop_back();
			if (!pos.empty())
				f.t.push_back(pos);
			++i;
		}
	}
	return true;
}

// createNode -at type table (Maya node types): DAG kinds take scene
// presence (hierarchy node: transform + parentable), system kinds are
// editor-managed data with no DAG node.
static const CreateNodeType kCreateNodeTypes[] = {
	{ "transform", CreateNodeKind::Dag },
	{ "pointLight", CreateNodeKind::Dag },
	{ "spotLight", CreateNodeKind::Dag },
	{ "terrain", CreateNodeKind::Dag },
	{ "cvolume", CreateNodeKind::Dag },
	{ "cubemap", CreateNodeKind::Dag },
	{ "camera", CreateNodeKind::Dag },
	{ "material", CreateNodeKind::System },
	{ "file", CreateNodeKind::System },
	{ "image", CreateNodeKind::System },
};

const CreateNodeType* CreateNodeTypes(size_t& countOut) {
	countOut = sizeof(kCreateNodeTypes) / sizeof(kCreateNodeTypes[0]);
	return kCreateNodeTypes;
}

const CreateNodeType* FindCreateNodeType(const std::string& at) {
	for (const CreateNodeType& t : kCreateNodeTypes)
		if (at == t.name)
			return &t;
	// Legacy light aliases from the createLight -at vocabulary.
	if (at == "point")
		return &kCreateNodeTypes[1];
	if (at == "spot")
		return &kCreateNodeTypes[2];
	return nullptr;
}

bool LoadTour(const std::string& path, Doc& doc, std::string& err,
	std::vector<std::string>* warnings) {	std::ifstream f(path, std::ios::binary);
	if (!f) {
		err = "cannot open: " + path;
		return false;
	}
	std::stringstream ss;
	ss << f.rdbuf();
	return LoadTourFromString(ss.str(), doc, err, warnings);
}

bool LoadTourFromString(const std::string& text, Doc& doc, std::string& err,
	std::vector<std::string>* warnings) {
	doc = Doc{};
	auto warn = [&](const std::string& w) {
		if (warnings)
			warnings->push_back(w);
	};
	std::istringstream f(text);
	int lineNo = 0;
	std::string line;
	while (std::getline(f, line)) {
		++lineNo;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		auto toks = Tokenize(line);
		if (toks.empty())
			continue;
		// strip trailing ';' token
		if (!toks.empty() && toks.back() == ";")
			toks.pop_back();
		if (toks.empty())
			continue;
		const std::string& verb = toks[0];
		if (verb == "source") {
			// source -p "<path>" [-fingerprint "<hex>"]; (-p is a PATH
			// here, not a parent id, so parse manually, not via ParseFlags)
			for (size_t i = 1; i < toks.size(); ++i) {
				std::string tok = toks[i];
				if (!tok.empty() && tok.back() == ';')
					tok.pop_back();
				if (tok == "-p" && i + 1 < toks.size()) {
					doc.source = toks[i + 1];
					if (!doc.source.empty() && doc.source.back() == ';')
						doc.source.pop_back();
					++i;
				}
				else if (tok == "-fingerprint" && i + 1 < toks.size()) {
					doc.fingerprint = toks[i + 1];
					if (!doc.fingerprint.empty() && doc.fingerprint.back() == ';')
						doc.fingerprint.pop_back();
					++i;
				}
			}
			continue;
		}
		if (verb == "editorSet") {
			// editorSet -k "<key>" -v "<value>"; editor-only (never cooked).
			std::string key, val;
			for (size_t i = 1; i < toks.size(); ++i) {
				if (toks[i] == "-k" && i + 1 < toks.size()) {
					key = toks[i + 1];
					++i;
				}
				else if (toks[i] == "-v" && i + 1 < toks.size()) {
					val = toks[i + 1];
					++i;
				}
			}
			if (!key.empty())
				doc.editorSettings.emplace_back(key, val);
			continue;
		}
		if (verb == "fileInfo") {
			// fileInfo -k "<key>" -v "<value>"; format constants block.
			std::string key, val;
			for (size_t i = 1; i < toks.size(); ++i) {
				if (toks[i] == "-k" && i + 1 < toks.size()) {
					key = toks[i + 1];
					++i;
				}
				else if (toks[i] == "-v" && i + 1 < toks.size()) {
					val = toks[i + 1];
					++i;
				}
			}
			if (!key.empty())
				doc.fileInfo.emplace_back(key, val);
			continue;
		}
		if (verb == "fileNode") {
			// fileNode -name "<name>" -path "<path>"
			//   -colorspace srgb|linear -wrap repeat|clamp|mirrored
			//   -filter linear|nearest; cooked file/image declaration.
			// Words and 0/1 numerics both accepted (forward tolerant).
			FileNode fn;
			auto lower = [](std::string s) {
				for (char& c : s)
					c = char(std::tolower(uint8_t(c)));
				return s;
			};
			for (size_t i = 1; i < toks.size(); ++i) {
				auto get = [&](const char* key, std::string& out) {
					if (toks[i] == key && i + 1 < toks.size()) {
						out = toks[i + 1];
						if (!out.empty() && out.back() == ';')
							out.pop_back();
						++i;
						return true;
					}
					return false;
				};
				std::string v;
				if (get("-name", v))
					fn.name = v;
				else if (get("-path", v))
					fn.path = v;
				else if (get("-colorspace", v)) {
					v = lower(v);
					fn.colorspace = (v == "linear" || v == "lin" || v == "1") ? 1 : 0;
				}
				else if (get("-wrap", v)) {
					v = lower(v);
					fn.wrap = (v == "clamp" || v == "clamp-edge" || v == "clamp_edge" || v == "1") ? 1
						: (v == "mirrored" || v == "mirror" || v == "mirrored-repeat" || v == "2") ? 2 : 0;
				}
				else if (get("-filter", v)) {
					v = lower(v);
					fn.filter = (v == "nearest" || v == "1") ? 1 : 0;
				}
			}
		if (!fn.path.empty())
			doc.fileNodes.push_back(std::move(fn));
		else
			warn("tour:" + std::to_string(lineNo) +
				": fileNode without -path (skipped)");
		continue;
	}
	if (verb == "materialAsset") {
		// materialAsset -path "<p>" -name "<n>" [-default];
		// Ordered editor-side pool imports (see MaterialAssetCmd): replayed
		// BEFORE the command list so assignMaterial pool indices match.
		MaterialAssetCmd ma;
		for (size_t i = 1; i < toks.size(); ++i) {
			auto get = [&](const char* key, std::string& out) {
				if (toks[i] == key && i + 1 < toks.size()) {
					out = toks[i + 1];
					if (!out.empty() && out.back() == ';')
						out.pop_back();
					++i;
					return true;
				}
				return false;
			};
			std::string v;
			if (get("-path", v))
				ma.path = v;
			else if (get("-name", v))
				ma.name = v;
			else {
				// Valueless flag: the trailing ';' may be attached when the
				// line ends without a space ("... -default;").
				std::string t = toks[i];
				if (!t.empty() && t.back() == ';')
					t.pop_back();
				if (t == "-default")
					ma.editorDefault = true;
			}
		}
		if (ma.editorDefault || !ma.path.empty())
			doc.materialAssets.push_back(std::move(ma));
		else
			warn("tour:" + std::to_string(lineNo) +
				": materialAsset without -path/-default (skipped)");
		continue;
	}
		if (verb == "cook") {
			doc.hasCook = true;			TourCook& k = doc.cook;
			for (size_t i = 1; i < toks.size(); ++i) {
				auto get = [&](const char* key, std::string& out) {
					if (toks[i] == key && i + 1 < toks.size()) {
						out = toks[i + 1];
						if (!out.empty() && out.back() == ';')
							out.pop_back();
						++i;
						return true;
					}
					return false;
				};
			std::string v;
			if (get("-outPak", v))
				k.outPak = v;
			else if (get("-meshopt", v))
				k.meshopt = (v == "on" || v == "1");
			else if (get("-lods", v))
				k.lods = (v == "on" || v == "1");
			else if (get("-lodRatios", v))
				k.lodRatios = v;
			else if (get("-lodError", v))
				k.lodError = std::max(std::stof(v), 0.0f);
		else if (get("-lightmap", v))
			k.lightmap = (v == "on" || v == "1");
			else if (get("-lmMetersPerTexel", v))
				k.lmMetersPerTexel = std::max(std::stof(v), 1e-4f);
			else if (get("-lmDensity", v)) {
				// Legacy: texels/meter -> meters/texel.
				const float tpm = std::max(std::stof(v), 2.0f);
				k.lmMetersPerTexel = 1.0f / tpm;
			}
			else if (get("-lmLobes", v))
				k.lmLobes = uint32_t(std::stoul(v));
			else if (get("-lmAtlasWidth", v))
				k.lmAtlasWidth = uint32_t(std::stoul(v));
			else if (get("-lmBudget", v)) {
				// Legacy (removed with --lm-budget): ignored — the atlas
				// width is fixed and the height grows with content now.
			}
			else if (get("-lmRays", v))
					k.lmRays = uint32_t(std::stoul(v));
				else if (get("-lmProxyRes", v))
					k.lmProxyRes = std::clamp(std::stoi(v), 0, 256);
				else if (get("-lmExcludeSmall", v))
					k.lmExcludeSmall = std::stof(v);
				else if (get("-lmExcludeKw", v))
					k.lmExcludeKw = v;
				else if (get("-sunAzimuth", v))
					k.sunAzimuth = std::stof(v);
				else if (get("-sunElevation", v))
					k.sunElevation = std::stof(v);
				else if (get("-sunIntensity", v))
					k.sunIntensity = std::stof(v);
				else if (get("-skyIntensity", v))
					k.skyIntensity = std::stof(v);
				else if (get("-sunColor", v)) {
					// "r g b" (also accepts "r,g,b", the CLI spelling).
					for (char& c : v)
						if (c == ',')
							c = ' ';
					std::istringstream in(v);
					glm::vec3 c = k.sunColor;
					if (in >> c.x >> c.y >> c.z)
						k.sunColor = c;
				}
				else if (get("-cubemap", v))
					k.cubemap = (v == "on" || v == "1");
				else if (get("-cubemapName", v))
					k.cubemapName = v;
				else if (get("-cubemapSize", v))
					k.cubemapSize = std::max<uint32_t>(
						uint32_t(std::stoul(v)), 4);
			else if (get("-cubemapRays", v))
				k.cubemapRays = std::max<uint32_t>(
					uint32_t(std::stoul(v)), 1);
			else if (get("-meshlets", v))
				k.meshlets = (v == "on" || v == "1");
			else if (get("-meshletVerts", v))
				k.meshletMaxVerts = std::clamp<uint32_t>(
					uint32_t(std::stoul(v)), 3, 255);
			else if (get("-meshletTris", v))
				k.meshletMaxTris = std::clamp<uint32_t>(
					uint32_t(std::stoul(v)), 1, 512);
			else if (get("-meshletConeWeight", v))
				k.meshletConeWeight = std::clamp(std::stof(v), 0.0f, 1.0f);
			else if (get("-giCvol", v))
				k.giCvol = (v == "on" || v == "1");
			else if (get("-giWorld", v))
				k.giWorld = (v == "on" || v == "1");
			else if (get("-autoKtx", v))
				k.autoKtx = (v == "on" || v == "1");
			}
			continue;
		}
		Command c{};
		Flags fl{};
		if (!ParseFlags(toks, 1, fl, err, lineNo))
			return false;
		// createNode lowers DAG types to their legacy per-type verb
		// (single payload path per shape downstream); system types
		// (material/file/image) stay CreateNode for the editor.
		std::string effVerb = verb;
		if (effVerb == "createNode") {
			if (!fl.hasAt) {
				err = "line " + std::to_string(lineNo)
					+ ": createNode wants -at <type>";
				return false;
			}
			const CreateNodeType* t = FindCreateNodeType(fl.at);
			if (!t) {
				err = "line " + std::to_string(lineNo)
					+ ": unknown createNode type '" + fl.at + "'";
				return false;
			}
			if (t->kind == CreateNodeKind::System) {
				if (!fl.hasName) {
					err = "line " + std::to_string(lineNo)
						+ ": createNode system type wants -name";
					return false;
				}
				c.verb = Verb::CreateNode;
				c.nodeType = fl.at;
				c.node = fl.hasP ? fl.p : 0;
				if (fl.hasPStr)
					c.parentRef = fl.pStr;
				c.text = fl.name;
				doc.commands.push_back(std::move(c));
				continue;
			}
		const std::string tn = t->name;
		if (tn == "transform")
			effVerb = "createTransform";
		else if (tn == "terrain")
			effVerb = "createTerrain";
		else if (tn == "cvolume")
			effVerb = "createCvolume";
		else if (tn == "cubemap")
			effVerb = "createCubemap";
		else if (tn == "camera")
			effVerb = "createCamera";
		else
			effVerb = "createLight"; // pointLight | spotLight
			// Legacy light -at vocabulary for the shared branch below.
			if (fl.at == "pointLight")
				fl.at = "point";
			if (fl.at == "spotLight")
				fl.at = "spot";
		}
		if (effVerb == "rename" && fl.hasN && !fl.t.empty()) {
			c.verb = Verb::Rename;
			c.node = fl.n;
			c.text = fl.t[0];
		}
		else if (effVerb == "setAttr" && fl.hasN && fl.hasAt && fl.t.size() >= 3) {
			double x = 0, y = 0, z = 0;
			if (!ParseDouble(fl.t[0], x) || !ParseDouble(fl.t[1], y) ||
				!ParseDouble(fl.t[2], z)) {
				err = "line " + std::to_string(lineNo) + ": bad triple";
				return false;
			}
			c.node = fl.n;
			c.vec = glm::vec3(float(x), float(y), float(z));
			if (fl.at == "translate")
				c.verb = Verb::SetTranslate;
			else if (fl.at == "rotate")
				c.verb = Verb::SetRotate;
			else if (fl.at == "scale")
				c.verb = Verb::SetScale;
			else {
				warn("tour:" + std::to_string(lineNo) +
					": unknown setAttr '" + fl.at + "' (skipped)");
				continue;
			}
		}
		else if (effVerb == "assignMaterial" && fl.hasN && fl.hasM) {
			c.verb = Verb::AssignMaterial;
			c.node = fl.n;
			c.material = fl.m;
		}
		else if (effVerb == "setMatAttr" && fl.hasM && fl.hasAt) {
			c.verb = Verb::SetMatAttr;
			c.material = fl.m;
			c.text = fl.at;
			if (fl.at == "baseColor" || fl.at == "emissive" ||
				fl.at == "layers") {
				if (fl.t.size() < 4) {
					err = "line " + std::to_string(lineNo) + ": want 4 values";
					return false;
				}
				double v[4] = {};
				for (int k = 0; k < 4; ++k)
					if (!ParseDouble(fl.t[k], v[k])) {
						err = "line " + std::to_string(lineNo) + ": bad value";
						return false;
					}
				c.vec4 = glm::vec4(float(v[0]), float(v[1]), float(v[2]), float(v[3]));
			}
			else if (fl.at == "alphaMode") {
				const std::string& w = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
				c.number = (w == "mask") ? 1.0 : (w == "blend") ? 2.0 : 0.0;
			}
			else {
				const std::string& w = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
				if (!ParseDouble(w, c.number)) {
					err = "line " + std::to_string(lineNo) + ": bad value '" + w + "'";
					return false;
				}
			}
		}
		else if (effVerb == "setNoLightmap" && fl.hasN && fl.hasV) {
			c.verb = Verb::SetNoLightmap;
			c.node = fl.n;
			c.flag = (fl.v == "1" || fl.v == "on" || fl.v == "true");
		}
		else if (effVerb == "setDitherCloseup" && fl.hasN && fl.hasV) {
			c.verb = Verb::SetDitherCloseup;
			c.node = fl.n;
			c.flag = (fl.v == "1" || fl.v == "on" || fl.v == "true");
		}
		else if (effVerb == "setStatic" && fl.hasN && fl.hasV) {
			c.verb = Verb::SetStatic;
			c.node = fl.n;
			c.flag = (fl.v == "1" || fl.v == "on" || fl.v == "true");
		}
		else if (effVerb == "setVisibility" && fl.hasN && fl.hasV) {
			bool b = false;
			if (!ParseFlagWord(fl.v, b)) {
				err = "line " + std::to_string(lineNo) + ": bad flag '" + fl.v + "'";
				return false;
			}
			c.verb = Verb::SetVisibility;
			c.node = fl.n;
			c.flag = b;
		}
		// setHidden is the legacy alias for setVisibility (hidden flag).
		else if (effVerb == "setHidden" && fl.hasN && fl.hasV) {
			bool b = false;
			if (!ParseFlagWord(fl.v, b)) {
				err = "line " + std::to_string(lineNo) + ": bad flag '" + fl.v + "'";
				return false;
			}
			c.verb = Verb::SetVisibility;
			c.node = fl.n;
			c.flag = b;
		}
		else if (effVerb == "override" && fl.hasPattern && fl.hasAt) {
			OverrideRule r{};
			if (!OverrideAttrFromString(fl.at, r.attr)) {
				warn("tour:" + std::to_string(lineNo) +
					": unknown override attr '" + fl.at + "' (skipped)");
				continue;
			}
			r.pattern = fl.pattern;
			r.enabled = fl.hasEnabled ? fl.enabled : true;
			if (r.attr == OverrideAttr::Material) {
				if (!fl.hasM) {
					err = "line " + std::to_string(lineNo) + ": override material needs -m";
					return false;
				}
				r.material = fl.m;
			}
			else {
				if (!fl.hasV) {
					err = "line " + std::to_string(lineNo) + ": override needs -v";
					return false;
				}
				bool b = false;
				if (!ParseFlagWord(fl.v, b)) {
					err = "line " + std::to_string(lineNo) + ": bad flag '" + fl.v + "'";
					return false;
				}
				r.flag = b;
			}
			// Validate the regex now (warn-and-skip on bad patterns).
			try {
				std::regex rx(r.pattern, std::regex::ECMAScript);
				(void)rx;
			}
			catch (const std::exception& e) {
				warn("tour:" + std::to_string(lineNo) +
					": bad override regex '" + r.pattern + "' (" + e.what() + ", skipped)");
				continue;
			}
			doc.overrides.push_back(std::move(r));
			continue;
		}
		else if (effVerb == "reference" && fl.hasSource) {
			// Clarisse-style scene reference: placeholder under -n with a
			// linked source; the gltf_import loader inlines the subtree
			// (recursively, via each source's own tour) at scene build.
			// -root marks the scene's OWN source (reference #0): inlined
			// under the scene root, no placeholder, no namespace.
			ReferenceCmd r{};
			r.source = fl.source;
			// Stored verbatim (validated at resolve: malformed GUIDs
			// fall back to the -source path silently).
			r.guid = fl.hasGuid ? fl.guid : "";
			r.parent = fl.hasN ? fl.n : 0;
			r.root = fl.hasRoot;
			if (fl.hasName)
				r.name = fl.name;
			if (fl.t.size() >= 3) {
				double x = 0, y = 0, z = 0;
				if (!ParseDouble(fl.t[0], x) || !ParseDouble(fl.t[1], y) ||
					!ParseDouble(fl.t[2], z)) {
					err = "tour:" + std::to_string(lineNo) + ": bad translate";
					return false;
				}
				r.translate = glm::vec3(float(x), float(y), float(z));
				r.hasTranslate = true;
			}
			r.enabled = fl.hasEnabled ? fl.enabled : true;
			doc.references.push_back(std::move(r));
			continue;
		}
		else if (effVerb == "parent" && fl.hasN && (fl.hasP || fl.hasPStr)) {
			c.verb = Verb::Parent;
			c.node = fl.n;
			c.node2 = fl.hasP ? fl.p : 0;
			if (fl.hasPStr)
				c.parentRef = fl.pStr;
		}
		else if (effVerb == "delete" && (fl.hasN || fl.hasName || fl.hasUuid)) {
			// Maya delete: drop the node's live-scene links. Ids stay
			// stable; resources are not reclaimed. Addressing is
			// uuid-primary (-uuid), name-secondary (-name), id fallback
			// (-n, UINT32_MAX when the line carries no id).
			c.verb = Verb::Delete;
			c.node = fl.hasN ? fl.n : UINT32_MAX;
			if (fl.hasName)
				c.name = fl.name;
			if (fl.hasUuid)
				c.uuid = fl.uuid;
		}
		else if (effVerb == "uuid" && fl.hasN && fl.hasV) {
			c.verb = Verb::SetUuid;
			c.node = fl.n;
			c.uuid = fl.v;
		}
		else if (effVerb == "createTransform" && (fl.hasP || fl.hasPStr)
			&& fl.hasName && fl.t.size() >= 6 && fl.hasS
			&& fl.svec.size() >= 3) {
			// -t packs: x y z rx ry rz (rotation in degrees); -s
			// packs: sx sy sz.
			double v[6] = {};
			for (int k = 0; k < 6; ++k)
				if (!ParseDouble(fl.t[size_t(k)], v[k])) {
					err = "line " + std::to_string(lineNo)
						+ ": bad createTransform value";
					return false;
				}
			double s[3] = { 1.0, 1.0, 1.0 };
			for (int k = 0; k < 3; ++k)
				if (!ParseDouble(fl.svec[size_t(k)], s[k])) {
					err = "line " + std::to_string(lineNo)
						+ ": bad createTransform scale";
					return false;
				}
			c.verb = Verb::CreateTransform;
			c.node = fl.hasP ? fl.p : 0;
			if (fl.hasPStr)
				c.parentRef = fl.pStr;
			c.text = fl.name;
			c.vec = glm::vec3(float(v[0]), float(v[1]), float(v[2]));
			c.vec4 = glm::vec4(float(v[3]), float(v[4]), float(v[5]), 0.0f);
			c.svec = glm::vec3(float(s[0]), float(s[1]), float(s[2]));
		}
		else if (effVerb == "createLight" && (fl.hasP || fl.hasPStr)
			&& fl.hasName && fl.hasAt
			&& fl.t.size() >= 9) {
			// -t packs: x y z intensity range innerDeg outerDeg
			// static01 shadow01.
			if (fl.at != "point" && fl.at != "spot") {
				err = "line " + std::to_string(lineNo) + ": createLight wants -at point|spot";
				return false;
			}
			double v[9] = {};
			for (int k = 0; k < 9; ++k)
				if (!ParseDouble(fl.t[size_t(k)], v[k])) {
					err = "line " + std::to_string(lineNo) + ": bad createLight value";
					return false;
				}
			c.verb = Verb::CreateLight;
			c.node = fl.hasP ? fl.p : 0;
			if (fl.hasPStr)
				c.parentRef = fl.pStr;
			c.text = fl.name;
			c.vec = glm::vec3(float(v[0]), float(v[1]), float(v[2]));
			c.vec4 = glm::vec4(fl.at == "spot" ? 2.0f : 1.0f,
				float(v[3]), float(v[4]), float(v[5]));
			c.number = v[6];
			c.flag = v[7] != 0.0;
			c.node2 = v[8] != 0.0 ? 1u : 0u;
		}
		else if (effVerb == "createCamera" && (fl.hasP || fl.hasPStr)
			&& fl.hasName && (fl.t.size() == 3 || fl.t.size() == 6
				|| fl.t.size() == 11)) {
			// -t packs: x y z [rx ry rz] [projFloat fovY near far
			// orthoHalfH] (rotation in degrees, editor TRS convention;
			// projFloat 0 = perspective, 1 = orthographic). -at is
			// accepted and must read "camera" when present (unified
			// createNode form).
			if (fl.hasAt && fl.at != "camera") {
				err = "line " + std::to_string(lineNo) + ": createCamera wants -at camera";
				return false;
			}
			double v[11] = {};
			for (size_t k = 0; k < fl.t.size(); ++k)
				if (!ParseDouble(fl.t[k], v[k])) {
					err = "line " + std::to_string(lineNo) + ": bad createCamera value";
					return false;
				}
			if (fl.t.size() == 11
				&& ((v[6] != 0.0 && v[6] != 1.0) || v[7] <= 0.0
					|| v[7] >= 180.0 || v[8] <= 0.0 || v[9] <= v[8]
					|| v[10] <= 0.0)) {
				err = "line " + std::to_string(lineNo) + ": bad createCamera lens";
				return false;
			}
			c.verb = Verb::CreateCamera;
			c.node = fl.hasP ? fl.p : 0;
			if (fl.hasPStr)
				c.parentRef = fl.pStr;
			c.text = fl.name;
			c.vec = glm::vec3(float(v[0]), float(v[1]), float(v[2]));
			if (fl.t.size() >= 6)
				c.svec = glm::vec3(float(v[3]), float(v[4]), float(v[5]));
			else
				c.svec = glm::vec3(0.0f);
			if (fl.t.size() == 11)
				c.vec4 = glm::vec4(float(v[6]), float(v[7]), float(v[8]),
					float(v[9]));
			else
				c.vec4 = glm::vec4(0.0f, 50.0f, 0.1f, 1000.0f);
			c.number = (fl.t.size() == 11) ? v[10] : 10.0;
		}
		else if (effVerb == "createTerrain" && (fl.hasP || fl.hasPStr)
			&& fl.hasName && fl.hasAt
			&& fl.hasM && fl.t.size() >= 10) {
			// -t packs: x y z size res height freq seed tilesX tilesZ.
			if (fl.at != "terrain") {
				err = "line " + std::to_string(lineNo) + ": createTerrain wants -at terrain";
				return false;
			}
			double v[10] = {};
			for (int k = 0; k < 10; ++k)
				if (!ParseDouble(fl.t[size_t(k)], v[k])) {
					err = "line " + std::to_string(lineNo) + ": bad createTerrain value";
					return false;
				}
			if (v[3] <= 0.0 || v[4] < 2.0 || v[4] > 512.0 || v[5] < 0.0
				|| v[6] <= 0.0 || v[7] < 0.0 || v[8] < 1.0 || v[8] > 16.0
				|| v[9] < 1.0 || v[9] > 16.0) {
				err = "line " + std::to_string(lineNo) + ": createTerrain params out of range";
				return false;
			}
		c.verb = Verb::CreateTerrain;
		c.node = fl.hasP ? fl.p : 0;
		if (fl.hasPStr)
			c.parentRef = fl.pStr;
		c.text = fl.name;
		c.vec = glm::vec3(float(v[0]), float(v[1]), float(v[2]));
		c.vec4 = glm::vec4(float(v[3]), float(v[4]), float(v[5]), float(v[6]));
		c.number = v[7];
		c.material = fl.m;
		c.node2 = (uint32_t(v[8]) & 0xFFFFu)
			| ((uint32_t(v[9]) & 0xFFFFu) << 16);
	}
	else if (effVerb == "createCvolume" && (fl.hasP || fl.hasPStr)
		&& fl.hasName && fl.t.size() >= 7 && fl.hasS
		&& fl.svec.size() >= 3) {
		// -t packs: x y z priority spacing insideFade outsideFade;
		// -s packs: sx sy sz (full extents). -at is accepted and
		// must read "cvolume" when present (unified-form echo).
		if (fl.hasAt && fl.at != "cvolume") {
			err = "line " + std::to_string(lineNo) + ": createCvolume wants -at cvolume";
			return false;
		}
		double v[10] = {};
		for (int k = 0; k < 7; ++k)
			if (!ParseDouble(fl.t[size_t(k)], v[k])) {
				err = "line " + std::to_string(lineNo)
					+ ": bad createCvolume value";
				return false;
			}
		double s[3] = { 1.0, 1.0, 1.0 };
		for (int k = 0; k < 3; ++k)
			if (!ParseDouble(fl.svec[size_t(k)], s[k])) {
				err = "line " + std::to_string(lineNo)
					+ ": bad createCvolume size";
				return false;
			}
		if (s[0] <= 0.0 || s[1] <= 0.0 || s[2] <= 0.0 || v[4] <= 0.0
			|| v[5] < 0.0 || v[6] < 0.0 || v[3] < 0.0) {
			err = "line " + std::to_string(lineNo) + ": createCvolume params out of range";
			return false;
		}
		c.verb = Verb::CreateCVolume;
		c.node = fl.hasP ? fl.p : 0;
		if (fl.hasPStr)
			c.parentRef = fl.pStr;
		c.text = fl.name;
		c.vec = glm::vec3(float(v[0]), float(v[1]), float(v[2]));
		c.svec = glm::vec3(float(s[0]), float(s[1]), float(s[2]));
		c.vec4 = glm::vec4(float(v[3]), float(v[4]), float(v[5]),
			float(v[6]));
	}
	else if (effVerb == "createCubemap" && (fl.hasP || fl.hasPStr)
		&& fl.hasName && fl.t.size() >= 3 && fl.hasV) {
		// -t packs: x y z (pinned probe position); -v packs the
		// influence radius (m). -at is accepted and must read
		// "cubemap" when present (unified-form echo).
		if (fl.hasAt && fl.at != "cubemap") {
			err = "line " + std::to_string(lineNo) + ": createCubemap wants -at cubemap";
			return false;
		}
		double v[3] = {};
		for (int k = 0; k < 3; ++k)
			if (!ParseDouble(fl.t[size_t(k)], v[k])) {
				err = "line " + std::to_string(lineNo)
					+ ": bad createCubemap position";
				return false;
			}
		double radius = 0.0;
		if (!ParseDouble(fl.v, radius)) {
			err = "line " + std::to_string(lineNo)
				+ ": bad createCubemap radius";
			return false;
		}
		if (radius <= 0.0) {
			err = "line " + std::to_string(lineNo)
				+ ": createCubemap radius must be > 0";
			return false;
		}
		c.verb = Verb::CreateCubemap;
		c.node = fl.hasP ? fl.p : 0;
		if (fl.hasPStr)
			c.parentRef = fl.pStr;
		c.text = fl.name;
		c.vec = glm::vec3(float(v[0]), float(v[1]), float(v[2]));
		c.number = radius;
	}
		else if (effVerb == "terrainPaint" && fl.hasRef && fl.hasAt
			&& fl.t.size() >= 4 && fl.hasV) {
			// -t packs: worldX worldZ radius flattenTarget; -v = strength.
			static const char* kOps[] = { "raise", "lower", "flatten", "smooth" };
			bool known = false;
			for (const char* a : kOps)
				if (fl.at == a) {
					known = true;
					break;
				}
			if (!known) {
				err = "line " + std::to_string(lineNo) + ": terrainPaint wants -at raise|lower|flatten|smooth";
				return false;
			}
			double v[4] = {};
			for (int k = 0; k < 4; ++k)
				if (!ParseDouble(fl.t[size_t(k)], v[k])) {
					err = "line " + std::to_string(lineNo) + ": bad terrainPaint value";
					return false;
				}
			double strength = 0.0;
			if (!ParseDouble(fl.v, strength)) {
				err = "line " + std::to_string(lineNo) + ": bad terrainPaint strength";
				return false;
			}
			c.verb = Verb::TerrainPaint;
			c.node = kCreateTerrainRefBase | fl.ref;
			c.text = fl.at;
			c.vec4 = glm::vec4(float(v[0]), float(v[1]), float(v[2]), float(v[3]));
			c.number = strength;
		}
		else if (effVerb == "setLightAttr" && (fl.hasN || fl.hasRef) && fl.hasAt) {
			static const char* kAttrs[] = { "color", "intensity", "range",
				"inner", "outer", "static", "shadow", "name", "position",
				"rotation" };
			bool known = false;
			for (const char* a : kAttrs)
				if (fl.at == a) {
					known = true;
					break;
				}
			if (!known) {
				err = "line " + std::to_string(lineNo) + ": bad light attr '" + fl.at + "'";
				return false;
			}
		c.verb = Verb::SetLightAttr;
		c.node = fl.hasRef ? (kCreateLightRefBase | fl.ref) : fl.n;
		c.text = fl.at;
		if (fl.at == "color" || fl.at == "position" || fl.at == "rotation") {
			if (fl.t.size() < 3) {
				err = "line " + std::to_string(lineNo) + ": light triple wants 3 values";
				return false;
			}
			double r = 0, g = 0, b = 0;
			if (!ParseDouble(fl.t[0], r) || !ParseDouble(fl.t[1], g) ||
				!ParseDouble(fl.t[2], b)) {
				err = "line " + std::to_string(lineNo) + ": bad light triple";
				return false;
			}
			c.vec4 = glm::vec4(float(r), float(g), float(b), 1.0f);
		}
		else if (fl.at == "name") {
			c.name = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
		}
		else if (fl.at == "static" || fl.at == "shadow") {
			bool on = false;
			const std::string& w = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
			if (!ParseFlagWord(w, on)) {
				err = "line " + std::to_string(lineNo) + ": bad flag '" + w + "'";
				return false;
			}
			c.flag = on;
		}
		else {
			const std::string& w = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
			if (!ParseDouble(w, c.number)) {
				err = "line " + std::to_string(lineNo) + ": bad value '" + w + "'";
				return false;
			}
		}
	}
	else if (effVerb == "setCvolAttr" && (fl.hasN || fl.hasRef) && fl.hasAt) {
		static const char* kAttrs[] = { "priority", "spacing",
			"inside", "outside", "position", "rotation", "size",
			"name" };
		bool known = false;
		for (const char* a : kAttrs)
			if (fl.at == a) {
				known = true;
				break;
			}
		if (!known) {
			err = "line " + std::to_string(lineNo) + ": bad cvolume attr '" + fl.at + "'";
			return false;
		}
		c.verb = Verb::SetCvolAttr;
		c.node = fl.hasRef ? (kCreateCvolRefBase | fl.ref) : fl.n;
		c.text = fl.at;
		if (fl.at == "position" || fl.at == "rotation"
			|| fl.at == "size") {
			if (fl.t.size() < 3) {
				err = "line " + std::to_string(lineNo) + ": cvolume triple wants 3 values";
				return false;
			}
			double x = 0, y = 0, z = 0;
			if (!ParseDouble(fl.t[0], x) || !ParseDouble(fl.t[1], y) ||
				!ParseDouble(fl.t[2], z)) {
				err = "line " + std::to_string(lineNo) + ": bad cvolume triple";
				return false;
			}
			c.vec4 = glm::vec4(float(x), float(y), float(z), 1.0f);
		}
		else if (fl.at == "name") {
			c.name = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
		}
		else {
			const std::string& w = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
			if (!ParseDouble(w, c.number)) {
				err = "line " + std::to_string(lineNo) + ": bad value '" + w + "'";
				return false;
			}
		}
	}
	else if (effVerb == "setCubemapAttr" && (fl.hasN || fl.hasRef) && fl.hasAt) {
		static const char* kAttrs[] = { "position", "radius", "name" };
		bool known = false;
		for (const char* a : kAttrs)
			if (fl.at == a) {
				known = true;
				break;
			}
		if (!known) {
			err = "line " + std::to_string(lineNo) + ": bad cubemap attr '" + fl.at + "'";
			return false;
		}
		c.verb = Verb::SetCubemapAttr;
		c.node = fl.hasRef ? (kCreateCubemapRefBase | fl.ref) : fl.n;
		c.text = fl.at;
		if (fl.at == "position") {
			if (fl.t.size() < 3) {
				err = "line " + std::to_string(lineNo) + ": cubemap triple wants 3 values";
				return false;
			}
			double x = 0, y = 0, z = 0;
			if (!ParseDouble(fl.t[0], x) || !ParseDouble(fl.t[1], y) ||
				!ParseDouble(fl.t[2], z)) {
				err = "line " + std::to_string(lineNo) + ": bad cubemap triple";
				return false;
			}
			c.vec4 = glm::vec4(float(x), float(y), float(z), 1.0f);
		}
		else if (fl.at == "name") {
			c.name = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
		}
		else {
			const std::string& w = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
			if (!ParseDouble(w, c.number)) {
				err = "line " + std::to_string(lineNo) + ": bad value '" + w + "'";
				return false;
			}
		}
	}
	else if (effVerb == "setCamAttr" && (fl.hasN || fl.hasRef) && fl.hasAt) {
		static const char* kAttrs[] = { "projection", "fovY",
			"orthoHalfH", "near", "far", "name", "position", "rotation" };
		bool known = false;
		for (const char* a : kAttrs)
			if (fl.at == a) {
				known = true;
				break;
			}
		if (!known) {
			err = "line " + std::to_string(lineNo) + ": bad camera attr '" + fl.at + "'";
			return false;
		}
		c.verb = Verb::SetCamAttr;
		c.node = fl.hasRef ? (kCreateCameraRefBase | fl.ref) : fl.n;
		c.text = fl.at;
		if (fl.at == "name") {
			c.name = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
		}
		else if (fl.at == "position" || fl.at == "rotation") {
			if (fl.t.size() < 3) {
				err = "line " + std::to_string(lineNo) + ": camera triple wants 3 values";
				return false;
			}
			double x = 0, y = 0, z = 0;
			if (!ParseDouble(fl.t[0], x) || !ParseDouble(fl.t[1], y) ||
				!ParseDouble(fl.t[2], z)) {
				err = "line " + std::to_string(lineNo) + ": bad camera triple";
				return false;
			}
			c.vec4 = glm::vec4(float(x), float(y), float(z), 1.0f);
		}
		else if (fl.at == "projection") {
			const std::string& w = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
			std::string low;
			for (char ch : w)
				low.push_back(char(std::tolower(uint8_t(ch))));
			if (low == "orthographic" || low == "ortho" || low == "1")
				c.number = 1.0;
			else if (low == "perspective" || low == "persp" || low == "0")
				c.number = 0.0;
			else {
				err = "line " + std::to_string(lineNo) + ": bad camera projection '" + w + "'";
				return false;
			}
		}
		else {
			const std::string& w = fl.hasV ? fl.v : (fl.t.empty() ? "" : fl.t[0]);
			if (!ParseDouble(w, c.number)) {
				err = "line " + std::to_string(lineNo) + ": bad value '" + w + "'";
				return false;
			}
		}
	}
	else {
			warn("tour:" + std::to_string(lineNo) + ": unknown verb '" +
				verb + "' (skipped)");
			continue;
		}
		doc.commands.push_back(std::move(c));
	}
	return true;
}

void TourReparent(Scene& scene, uint32_t node, uint32_t newParent) {
	if (node >= scene.hierarchy.size() || newParent >= scene.hierarchy.size() ||
		node == newParent || node == 0)
		return;
	// Cycle guard.
	for (int n = int(newParent); n >= 0 && n < int(scene.hierarchy.size());) {
		if (uint32_t(n) == node)
			return;
		n = scene.hierarchy[size_t(n)].parent;
	}
	const int oldParent = scene.hierarchy[node].parent;
	if (oldParent == int(newParent))
		return;
	// Depth guard (changedAtThisFrame has MAX_NODE_LEVEL levels).
	const int newLevel = scene.hierarchy[newParent].level + 1;
	{
		int maxDepth = 0;
		std::vector<std::pair<uint32_t, int>> stack{ { node, 0 } };
		while (!stack.empty()) {
			auto [n, d] = stack.back();
			stack.pop_back();
			maxDepth = std::max(maxDepth, d);
			for (int c = scene.hierarchy[n].firstChild; c != -1;
				c = scene.hierarchy[c].nextSibling)
				stack.emplace_back(uint32_t(c), d + 1);
		}
		if (newLevel + maxDepth >= MAX_NODE_LEVEL)
			return;
	}
	// Unlink.
	if (oldParent >= 0) {
		Hierarchy& op = scene.hierarchy[size_t(oldParent)];
		int prev = -1;
		for (int c = op.firstChild; c != -1; c = scene.hierarchy[c].nextSibling) {
			if (c == int(node)) {
				if (prev < 0)
					op.firstChild = scene.hierarchy[c].nextSibling;
				else
					scene.hierarchy[size_t(prev)].nextSibling =
					scene.hierarchy[c].nextSibling;
				break;
			}
			prev = c;
		}
		int last = -1;
		for (int c = op.firstChild; c != -1; c = scene.hierarchy[c].nextSibling)
			last = c;
		op.lastSibling = last;
	}
	// Link (append).
	Hierarchy& np = scene.hierarchy[newParent];
	scene.hierarchy[node].parent = int(newParent);
	scene.hierarchy[node].nextSibling = -1;
	if (np.firstChild < 0) {
		np.firstChild = int(node);
		np.lastSibling = int(node);
	}
	else {
		int last = (np.lastSibling >= 0) ? np.lastSibling : np.firstChild;
		for (; scene.hierarchy[size_t(last)].nextSibling >= 0;
			last = scene.hierarchy[size_t(last)].nextSibling) {
		}
		scene.hierarchy[size_t(last)].nextSibling = int(node);
		np.lastSibling = int(node);
	}
	scene.hierarchy[node].lastSibling = int(node);
	// Levels.
	{
		std::vector<uint32_t> stack{ node };
		scene.hierarchy[node].level = newLevel;
		while (!stack.empty()) {
			const uint32_t n = stack.back();
			stack.pop_back();
			for (int c = scene.hierarchy[n].firstChild; c != -1;
				c = scene.hierarchy[c].nextSibling) {
				scene.hierarchy[size_t(c)].level = scene.hierarchy[n].level + 1;
				stack.push_back(uint32_t(c));
			}
		}
	}
}

static Material* CookMaterial(Cluster& cluster, uint32_t poolIdx) {
	const ClusterMaterialHandle h = cluster.materials.getHandle(poolIdx);
	if (!h.valid())
		return nullptr;
	return cluster.materials.get(h);
}

static void ApplyMatAttr(Material& mat, const Command& c) {
	const std::string& at = c.text;
	if (at == "baseColor")
		mat.values.diffuse = c.vec4;
	else if (at == "emissive") {
		mat.values.emissiveFactor = glm::vec4(c.vec4.x, c.vec4.y, c.vec4.z, 0.0f);
		if (c.vec4.w >= 0.0f)
			mat.values.params2.z = c.vec4.w; // emissiveStrength rides w
	}
	else if (at == "metallic")
		mat.values.params.y = float(c.number);
	else if (at == "roughness")
		mat.values.params.z = float(c.number);
	else if (at == "alphaCutoff")
		mat.values.params.x = float(c.number);
	else if (at == "emissiveStrength")
		mat.values.params2.z = float(c.number);
	else if (at == "normalScale")
		mat.values.params2.y = float(c.number);
	else if (at == "occlusionStrength")
		mat.values.params2.x = float(c.number);
	else if (at == "doubleSided")
		mat.doubleSided = (c.number != 0.0);
	else if (at == "castsShadows")
		mat.castsShadows = (c.number != 0.0);
	else if (at == "layers") {
		// Cluster-local pool ids (the cook domain, pre-merge): x/y/z =
		// layers 1..3 (-1 none), w = active layer count (0 = legacy).
		mat.values.layerMaterials = glm::ivec4(
			int(c.vec4.x), int(c.vec4.y), int(c.vec4.z), int(c.vec4.w));
	}
	// Texture slots (RHI pool indices at runtime, cook-table ids when
	// cooking — the same integer domain MaterialValues stores). -1 clears.
	else if (at == "baseColorTex")
		mat.values.textures.x = int(c.number);
	else if (at == "metallicRoughTex")
		mat.values.textures.y = int(c.number);
	else if (at == "normalTex")
		mat.values.textures.z = int(c.number);
	else if (at == "occlusionTex")
		mat.values.textures.w = int(c.number);
	else if (at == "emissiveTex")
		mat.values.textures2.x = int(c.number);
	else if (at == "alphaMode") {
		mat.alphaMode = (c.number < 0.5) ? AlphaMode::Opaque
			: (c.number < 1.5) ? AlphaMode::Mask : AlphaMode::Blend;
		mat.shadingModel = (mat.alphaMode == AlphaMode::Mask)
			? ShadingModel::Masked : ShadingModel::DefaultLit;
		mat.values.params.w =
			(mat.alphaMode == AlphaMode::Mask) ? 2.0f : 0.0f;
		mat.values.params2.w = float(int(mat.alphaMode));
	}
	mat.rebuildPipeline();
}

namespace {
// Authored-environment blocks (tour -> cluster scene): parses the env
// blocks the editor persists (env.default.* + env.n.* node groups +
// env.current — same keys/grouping as the editor loader, see
// editor_env.cpp SaveEnvToDoc/LoadEnvFromDoc) and resolves the ACTIVE
// block (the doc's env.current node, else the default block). Node ids
// are cluster-space (validated against the replayed scene by the caller;
// dangling blocks warn + drop, never fatal).
struct TourEnvBlocks {
	SceneEnv def{};
	std::vector<std::pair<uint32_t, SceneEnv>> nodeEnvs; // cluster-space, file order
	uint32_t currentCluster = UINT32_MAX;
	bool haveCurrent = false;
	bool hasBlocks = false;
};
float TourEnvFloat(const std::string& s, float fallback) {
	try {
		return s.empty() ? fallback : std::stof(s);
	}
	catch (...) {
		return fallback;
	}
}
glm::vec3 TourEnvVec3(const std::string& s, const glm::vec3& fallback) {
	std::istringstream in(s);
	glm::vec3 v = fallback;
	in >> v.x >> v.y >> v.z;
	return v;
}
void TourEnvApplyField(SceneEnv& e, const std::string& f, const std::string& v) {
	if (f == "skyColor") e.skyColor = TourEnvVec3(v, e.skyColor);
	else if (f == "sunColor") e.sunColor = TourEnvVec3(v, e.sunColor);
	else if (f == "fogColor") e.fogColor = TourEnvVec3(v, e.fogColor);
	else if (f == "skyIntensity") e.skyIntensity = TourEnvFloat(v, e.skyIntensity);
	else if (f == "sunAzimuth") e.sunAzimuth = TourEnvFloat(v, e.sunAzimuth);
	else if (f == "sunElevation") e.sunElevation = TourEnvFloat(v, e.sunElevation);
	else if (f == "sunIntensity") e.sunIntensity = TourEnvFloat(v, e.sunIntensity);
	else if (f == "exposure") e.exposure = TourEnvFloat(v, e.exposure);
	else if (f == "fogDensity") e.fogDensity = TourEnvFloat(v, e.fogDensity);
	else if (f == "volumetrics") e.volumetrics = TourEnvFloat(v, e.volumetrics);
	else if (f == "skyboxUri") e.skyboxUri = v;
}
TourEnvBlocks ParseTourEnvBlocks(const Doc& doc) {
	TourEnvBlocks eb{};
	bool inNode = false;
	uint32_t nodeCluster = UINT32_MAX;
	SceneEnv cur{};
	for (const auto& [key, val] : doc.editorSettings) {
		const size_t dot = key.find_last_of('.');
		const std::string field =
			(dot == std::string::npos) ? key : key.substr(dot + 1);
		if (key.rfind("env.default.", 0) == 0) {
			if (inNode) {
				eb.nodeEnvs.emplace_back(nodeCluster, cur);
				inNode = false;
			}
			eb.hasBlocks = true;
			TourEnvApplyField(eb.def, field, val);
		}
		else if (key.rfind("env.n.", 0) == 0) {
			if (field == "node") {
				if (inNode)
					eb.nodeEnvs.emplace_back(nodeCluster, cur);
				inNode = true;
				nodeCluster = uint32_t(TourEnvFloat(val, -1.0f));
				cur = SceneEnv{};
			}
			else if (inNode) {
				eb.hasBlocks = true;
				TourEnvApplyField(cur, field, val);
			}
		}
		else if (key == "env.current") {
			eb.currentCluster = uint32_t(TourEnvFloat(val, -1.0f));
			eb.haveCurrent = true;
		}
	}
	if (inNode)
		eb.nodeEnvs.emplace_back(nodeCluster, cur);
	return eb;
}
} // namespace

bool ApplyTourToCluster(Cluster& cluster, const Doc& doc,
	std::vector<std::string>* warnings, std::string* err, bool strict) {
	auto warn = [&](const std::string& w) {
		if (warnings)
			warnings->push_back(w);
	};
	// Per-command failure: strict (default) aborts with *err (already
	// set by the failing path); reference replay (strict=false) reports
	// the command and moves on so one bad command cannot drop the rest
	// of a referenced scene's edits.
	auto failCmd = [&](const Command& c) -> bool {
		if (strict)
			return false; // *err stays as the abort reason
		warn((err && !err->empty() ? *err
				: std::string("tour: command failed"))
			+ " (skipped: " + Describe(c) + ")");
		if (err)
			err->clear();
		return true;
	};
	Scene& scene = cluster.scene;
	auto checkNode = [&](uint32_t n, const Command& c) {
		if (n >= scene.hierarchy.size()) {
			if (err)
				*err = "tour: node id out of range: " + std::to_string(n);
			return false;
		}
		if (!c.name.empty() && getNodeName(scene, int(n)) != c.name)
			warn("tour: node " + std::to_string(n) + " name is '" +
				getNodeName(scene, int(n)) + "', tour recorded '" + c.name + "'");
		return true;
	};
	// Creation-order ids for SetLightAttr refs (kCreateLightRefBase | k):
	// created light nodes have no cluster id until replayed.
	std::vector<uint32_t> createdLights;
	// Same scheme for created convex volumes (kCreateCvolRefBase | k),
	// resolved by SetCvolAttr below.
	std::vector<uint32_t> createdCvols;
	// Same scheme for created cubemap captures (kCreateCubemapRefBase |
	// k), resolved by SetCubemapAttr below.
	std::vector<uint32_t> createdCubemaps;
	// Same scheme for created cameras (kCreateCameraRefBase | k),
	// resolved by SetCamAttr below.
	std::vector<uint32_t> createdCameras;
	// Same scheme for created terrain (kCreateTerrainRefBase | k),
	// resolved for the generic verbs below (Rename/SetTranslate/...
	// /Parent) so post-creation edits replay onto the rebuilt node.
	std::vector<uint32_t> createdTerrains;
	// Live terrain group state during replay: heights bake through every
	// terrainPaint stamp; tile meshes upload from the FINAL grid in the
	// post-loop pass below (dirty = tiles touched by stamps; a group with
	// no stamps uploads every tile once).
	struct TourTerrainGroup {
		smvk::placement::TerrainGroupData data;
		uint32_t groupNode = 0;
		uint32_t material = 0;
		std::vector<uint32_t> tileNodes; // [tz * tilesX + tx]
		std::set<uint32_t> dirty;        // packed tx | (tz << 16)
	};
	std::vector<TourTerrainGroup> terrainGroups;
	auto resolveNode = [&](uint32_t n, const Command& c, uint32_t& out) {
		if ((n & kCreateTerrainRefBase) != 0) {
			const uint32_t k = n & ~kCreateTerrainRefBase;
			if (k >= createdTerrains.size()) {
				if (err)
					*err = "tour: terrain ref out of range: " + std::to_string(k);
				return false;
			}
			out = createdTerrains[k];
			return true;
		}
		if (!checkNode(n, c))
			return false;
		out = n;
		return true;
	};
	// Symbolic -p form (name/path/uuid) shared by Parent and the
	// creates: resolves against the CURRENT scene so parents survive
	// numeric id shifts; the bare numeric form is the legacy fast path.
	// (parentId = the verb's parent slot: node2 for Parent, node for
	// the creates.)
	auto resolveParent = [&](const Command& c, const char* what,
		uint32_t parentId, uint32_t& parent) -> bool {
		if (!c.parentRef.empty()) {
			if (ResolveParentTarget(scene, c.parentRef, parent))
				return true;
			if (err)
				*err = std::string("tour: ") + what + " parent '"
					+ c.parentRef + "' unresolved";
			return false;
		}
		uint32_t target = 0;
		if (!resolveNode(parentId, c, target))
			return false;
		parent = target;
		return true;
	};
	for (const Command& c : doc.commands) {
		switch (c.verb) {
		case Verb::Rename: {
			uint32_t target = 0;
			if (!resolveNode(c.node, c, target)) {
				if (!failCmd(c))
					return false;
				break;
			}
			const uint32_t id = uint32_t(scene.nodeNames.size());
			scene.nodeNames.push_back(c.text);
			scene.nameForNode[target] = id;
			break;
		}
		case Verb::SetTranslate:
		case Verb::SetRotate:
		case Verb::SetScale: {
			// Transforms compose through the editor's TRS convention; the
			// tour carries the full triple per node, so rebuild from the
			// CURRENT local matrix with only this channel replaced. To stay
			// order-independent, stash per-node triples here.
			break; // handled in the second pass below
		}
		case Verb::AssignMaterial: {
			uint32_t target = 0;
			if (!resolveNode(c.node, c, target)) {
				if (!failCmd(c))
					return false;
				break;
			}
			if (!CookMaterial(cluster, c.material)) {
				if (err)
					*err = "tour: material out of range: " +
						std::to_string(c.material);
				if (!failCmd(c))
					return false;
				break;
			}
			scene.materialForNode[target] = c.material;
			break;
		}
		case Verb::SetMatAttr: {
			Material* mat = CookMaterial(cluster, c.material);
			if (!mat) {
				if (err)
					*err = "tour: material out of range: " +
						std::to_string(c.material);
				if (!failCmd(c))
					return false;
				break;
			}
			ApplyMatAttr(*mat, c);
			break;
		}
		case Verb::SetNoLightmap: {
			uint32_t target = 0;
			if (!resolveNode(c.node, c, target)) {
				if (!failCmd(c))
					return false;
				break;
			}
			if (scene.noLightmap.size() != scene.hierarchy.size())
				scene.noLightmap.assign(scene.hierarchy.size(), 0);
			scene.noLightmap[target] = c.flag ? 1 : 0;
			break;
		}
	case Verb::SetDitherCloseup: {
		uint32_t target = 0;
		if (!resolveNode(c.node, c, target)) {
			if (!failCmd(c))
				return false;
			break;
		}
		if (scene.ditherCloseup.size() != scene.hierarchy.size())
			scene.ditherCloseup.assign(scene.hierarchy.size(), 0);
		scene.ditherCloseup[target] = c.flag ? 1 : 0;
		break;
	}
	case Verb::SetStatic: {
		// Static-subtree ROOT mark (Scene::staticRoot): the subtree's
		// transforms freeze after the initial build (skip per-frame
		// updates + BVH rebuilds, mega-mesh collapse eligibility).
		// MarkSubtreeStatic/Clear rebuild the staticNode cache.
		uint32_t target = 0;
		if (!resolveNode(c.node, c, target)) {
			if (!failCmd(c))
				return false;
			break;
		}
		if (c.flag)
			MarkSubtreeStatic(scene, int(target));
		else
			ClearSubtreeStatic(scene, int(target));
		break;
	}
		case Verb::SetVisibility: {
			uint32_t target = 0;
			if (!resolveNode(c.node, c, target)) {
				if (!failCmd(c))
					return false;
				break;
			}
			if (scene.hidden.size() != scene.hierarchy.size())
				scene.hidden.assign(scene.hierarchy.size(), 0);
			scene.hidden[target] = c.flag ? 1 : 0;
			break;
		}
		case Verb::Parent: {
			uint32_t target = 0, newParent = 0;
			if (!resolveNode(c.node, c, target)) {
				if (!failCmd(c))
					return false;
				break;
			}
			if (!resolveParent(c, "parent", c.node2, newParent)) {
				if (!failCmd(c))
					return false;
				break;
			}
			TourReparent(scene, target, newParent);
			break;
		}
		case Verb::Delete: {
			// Maya delete at cook replay: drop the node's live links
			// (mesh/material/light/cvolume) so nothing draws, bakes
			// or packs it. Addressed by uuid (primary), name, then
			// id — the file-stable identity chain (numeric ids may
			// shift as reference sets change; names/uuids do not).
			uint32_t target = UINT32_MAX;
			const bool hasId = c.node != UINT32_MAX;
			if (!ResolveDeleteTarget(scene, c.uuid, c.name,
					hasId ? c.node : UINT32_MAX, target)) {
				if (err)
					*err = "tour: delete target unresolved";
				if (!failCmd(c))
					return false;
				break;
			}
			scene.meshForNode.erase(target);
			scene.materialForNode.erase(target);
			scene.lightForNode.erase(target);
			scene.cvolForNode.erase(target);
			scene.cubemapForNode.erase(target);
			break;
		}
		case Verb::SetUuid: {
			// Record the file-stable identity for later deletes
			// (Maya setUuid parity: rejected when another live node
			// already holds it).
			uint32_t target = 0;
			if (!resolveNode(c.node, c, target)) {
				if (!failCmd(c))
					return false;
				break;
			}
			if (!SetNodeUuid(scene, target, c.uuid)) {
				if (err)
					*err = "tour: uuid already held by another live node";
				if (!failCmd(c))
					return false;
				break;
			}
			break;
		}
		case Verb::CreateTransform: {
			// Plain transform node (no mesh/light/pool entry): the
			// editor's reference-displacement placeholders ride this.
			uint32_t parent = 0;
			if (!resolveParent(c, "createTransform", c.node, parent)) {
				if (!failCmd(c))
					return false;
				break;
			}
			const int level = scene.hierarchy[parent].level + 1;
			const int newId = addNode(scene, int(parent), level);
			if (newId < 0) {
				if (err)
					*err = "tour: createTransform failed under parent " +
						std::to_string(parent);
				if (!failCmd(c))
					return false;
				break;
			}
			// TRS rebuild (editor convention: Rz*Ry*Rx, degrees).
			glm::mat4 m(1.0f);
			m = glm::translate(m, c.vec);
			m = glm::rotate(m, glm::radians(c.vec4.z), glm::vec3(0, 0, 1));
			m = glm::rotate(m, glm::radians(c.vec4.y), glm::vec3(0, 1, 0));
			m = glm::rotate(m, glm::radians(c.vec4.x), glm::vec3(1, 0, 0));
			m = glm::scale(m, c.svec);
			scene.localTransform[size_t(newId)] = m;
			const uint32_t nameId = uint32_t(scene.nodeNames.size());
			scene.nodeNames.push_back(c.text);
			scene.nameForNode[uint32_t(newId)] = nameId;
			break;
		}
		case Verb::CreateLight: {
			// Editor-authored punctual light: append a node under the
			// recorded parent + a lights-pool definition. Angles ride in
			// degrees (editor convention); the runtime converts.
			uint32_t parent = 0;
			if (!resolveParent(c, "createLight", c.node, parent)) {
				if (!failCmd(c))
					return false;
				break;
			}
			const int level = scene.hierarchy[parent].level + 1;
			const int newId = addNode(scene, int(parent), level);
			if (newId < 0) {
				if (err)
					*err = "tour: createLight failed under parent " +
						std::to_string(parent);
				if (!failCmd(c))
					return false;
				break;
			}
			scene.localTransform[size_t(newId)] =
				glm::translate(glm::mat4(1.0f), c.vec);
			SceneLight def{};
			def.type = c.vec4.x >= 1.5f
				? SceneLight::Type::Spot : SceneLight::Type::Point;
			def.intensity = std::max(c.vec4.y, 0.0f);
			def.range = std::max(c.vec4.z, 0.0f);
			def.innerConeAngle = glm::radians(std::clamp(c.vec4.w, 0.0f, 89.0f));
			def.outerConeAngle = glm::radians(
				std::clamp(float(c.number), 0.0f, 89.0f));
			if (c.flag)
				def.flags |= SceneLight::kSceneLightFlag_Static;
			if (c.node2 != 0)
				def.flags |= SceneLight::kSceneLightFlag_CastShadow;
			def.name = c.text;
			const uint32_t poolIdx =
				cluster.lights.create(std::move(def)).index();
			const uint32_t nameId = uint32_t(scene.nodeNames.size());
			scene.nodeNames.push_back(c.text);
			scene.nameForNode[uint32_t(newId)] = nameId;
		scene.lightForNode[uint32_t(newId)] = poolIdx;
		createdLights.push_back(uint32_t(newId));
		break;
	}
	case Verb::CreateCVolume: {
		// Authored convex GI volume: append a node under the recorded
		// parent (TRS = translate + scale-as-size, axis-aligned) + a
		// node-local SceneCvol definition. The shape resolves from
		// the node's transform at use; rotation is ignored by
		// design (gi_baker box-spec shape).
		uint32_t parent = 0;
		if (!resolveParent(c, "createCvolume", c.node, parent)) {
			if (!failCmd(c))
				return false;
			break;
		}
		const int level = scene.hierarchy[parent].level + 1;
		const int newId = addNode(scene, int(parent), level);
		if (newId < 0) {
			if (err)
				*err = "tour: createCvolume failed under parent " +
					std::to_string(parent);
			if (!failCmd(c))
				return false;
			break;
		}
		glm::mat4 m(1.0f);
		m = glm::translate(m, c.vec);
		m = glm::scale(m, glm::vec3(std::max(c.svec.x, 1e-3f),
			std::max(c.svec.y, 1e-3f), std::max(c.svec.z, 1e-3f)));
		scene.localTransform[size_t(newId)] = m;
		SceneCvol def{};
		def.priority = uint32_t(std::max(c.vec4.x, 0.0f));
		def.spacing = std::max(c.vec4.y, 1e-3f);
		def.insideFade = std::max(c.vec4.z, 0.0f);
		def.outsideFade = std::max(c.vec4.w, 0.0f);
		scene.cvolForNode[uint32_t(newId)] = def;
		const uint32_t nameId = uint32_t(scene.nodeNames.size());
		scene.nodeNames.push_back(c.text);
		scene.nameForNode[uint32_t(newId)] = nameId;
		createdCvols.push_back(uint32_t(newId));
		break;
	}
	case Verb::CreateCubemap: {
		// Authored cubemap capture: append a node under the recorded
		// parent (translate = pinned probe position) + a node-local
		// SceneCubemap definition (influence radius). The cook's
		// --cubemap stage harvests these as 'pin' rows.
		uint32_t parent = 0;
		if (!resolveParent(c, "createCubemap", c.node, parent)) {
			if (!failCmd(c))
				return false;
			break;
		}
		const int level = scene.hierarchy[parent].level + 1;
		const int newId = addNode(scene, int(parent), level);
		if (newId < 0) {
			if (err)
				*err = "tour: createCubemap failed under parent " +
					std::to_string(parent);
			if (!failCmd(c))
				return false;
			break;
		}
		scene.localTransform[size_t(newId)] =
			glm::translate(glm::mat4(1.0f), c.vec);
		SceneCubemap def{};
		def.radius = std::max(float(c.number), 1e-2f);
		scene.cubemapForNode[uint32_t(newId)] = def;
		const uint32_t nameId = uint32_t(scene.nodeNames.size());
		scene.nodeNames.push_back(c.text);
		scene.nameForNode[uint32_t(newId)] = nameId;
		createdCubemaps.push_back(uint32_t(newId));
		break;
	}
	case Verb::CreateCamera: {
		// Authored camera node: append a node under the recorded parent
		// (TRS = translate + rotation-deg, editor convention) + a
		// cameras-pool lens definition. The pose resolves from the
		// node's transform at use (viewport look-through, framing);
		// forward = node -Z in world.
		uint32_t parent = 0;
		if (!resolveParent(c, "createCamera", c.node, parent)) {
			if (!failCmd(c))
				return false;
			break;
		}
		const int level = scene.hierarchy[parent].level + 1;
		const int newId = addNode(scene, int(parent), level);
		if (newId < 0) {
			if (err)
				*err = "tour: createCamera failed under parent " +
					std::to_string(parent);
			if (!failCmd(c))
				return false;
			break;
		}
		glm::mat4 m(1.0f);
		m = glm::translate(m, c.vec);
		m = glm::rotate(m, glm::radians(c.svec.z), glm::vec3(0, 0, 1));
		m = glm::rotate(m, glm::radians(c.svec.y), glm::vec3(0, 1, 0));
		m = glm::rotate(m, glm::radians(c.svec.x), glm::vec3(1, 0, 0));
		scene.localTransform[size_t(newId)] = m;
		SceneCamera def{};
		def.projection = (c.vec4.x >= 0.5f)
			? SceneCamera::Projection::Orthographic
			: SceneCamera::Projection::Perspective;
		def.fovYDeg = std::clamp(c.vec4.y, 1.0f, 179.0f);
		def.nearClip = std::max(c.vec4.z, 1e-4f);
		def.farClip = std::max(c.vec4.w, def.nearClip * 1.1f + 1e-3f);
		def.orthoHalfH = std::clamp(float(c.number), 1e-2f, 1e5f);
		def.name = c.text;
		const uint32_t poolIdx =
			cluster.cameras.create(std::move(def)).index();
		const uint32_t nameId = uint32_t(scene.nodeNames.size());
		scene.nodeNames.push_back(c.text);
		scene.nameForNode[uint32_t(newId)] = nameId;
		scene.cameraForNode[uint32_t(newId)] = poolIdx;
		createdCameras.push_back(uint32_t(newId));
		break;
	}
		case Verb::CreateTerrain: {
			// Editor-authored procedural terrain: a GROUP node under the
			// recorded parent + tilesX*tilesZ tile children. Tile meshes
			// upload in the post-loop pass so later terrainPaint stamps
			// bake into them (heights are final by then). The group node
			// id joins createdTerrains for -ref addressing.
			uint32_t parent = 0;
			if (!resolveParent(c, "createTerrain", c.node, parent)) {
				if (!failCmd(c))
					return false;
				break;
			}
			uint32_t material = c.material;
			if (!CookMaterial(cluster, material)) {
				// Editor fallback: scenes authored with the editor's
				// default material cook against a fresh import that does
				// not carry it. Mint the same deterministic default here
				// (same URI, same values) so the tour replays.
				warn("tour: createTerrain material out of range: " +
					std::to_string(c.material)
					+ " — creating the editor default material");
				Material def{};
				def.rebuildPipeline();
				const ClusterMaterialHandle dh =
					cluster.materials.create(std::move(def));
				cluster.RegisterMaterialUri("editor://default", dh);
				material = dh.index();
			}
			smvk::placement::TerrainGroupData tg;
			tg.params.origin = glm::vec2(c.vec.x, c.vec.z);
			tg.params.size = std::max(c.vec4.x, 1.0f);
			tg.params.res = std::clamp(
				uint32_t(std::max(c.vec4.y, 2.0f)), 2u, 512u);
			tg.params.height = std::max(c.vec4.z, 0.0f);
			tg.params.freq = std::max(c.vec4.w, 1e-4f);
			tg.params.seed = c.number < 0.0 ? 0ULL : uint64_t(c.number);
			tg.tilesX = std::clamp(uint32_t(std::max(c.node2 & 0xFFFFu, 1u)),
				1u, smvk::placement::kTerrainMaxTiles);
			tg.tilesZ = std::clamp(uint32_t(std::max((c.node2 >> 16) & 0xFFFFu, 1u)),
				1u, smvk::placement::kTerrainMaxTiles);
			tg.heights = smvk::placement::BuildTerrainHeights(tg.params);
			// Group node (no mesh; the transform carries the origin).
			const int level = scene.hierarchy[parent].level + 1;
			const int newId = addNode(scene, int(parent), level);
			if (newId < 0) {
				if (err)
					*err = "tour: createTerrain failed under parent " +
						std::to_string(parent);
				if (!failCmd(c))
					return false;
				break;
			}
			scene.localTransform[size_t(newId)] =
				glm::translate(glm::mat4(1.0f), c.vec);
			const uint32_t nameId = uint32_t(scene.nodeNames.size());
			scene.nodeNames.push_back(c.text);
			scene.nameForNode[uint32_t(newId)] = nameId;
			createdTerrains.push_back(uint32_t(newId));
			// Tile child nodes (positioned by their grid offset).
			TourTerrainGroup& state = terrainGroups.emplace_back();
			state.data = std::move(tg);
			state.groupNode = uint32_t(newId);
			state.material = material;
			state.tileNodes.resize(size_t(state.data.tilesX)
				* state.data.tilesZ);
			const float tileW = state.data.params.size
				/ float(state.data.tilesX);
			const float tileH = state.data.params.size
				/ float(state.data.tilesZ);
			const int tileLevel = level + 1;
			for (uint32_t tz = 0; tz < state.data.tilesZ; ++tz)
				for (uint32_t tx = 0; tx < state.data.tilesX; ++tx) {
					const int tileId = addNode(scene, newId, tileLevel);
					if (tileId < 0) {
						if (err)
							*err = "tour: createTerrain tile node failed";
						if (!failCmd(c))
							return false;
						break;
					}
					scene.localTransform[size_t(tileId)] = glm::translate(
						glm::mat4(1.0f),
						glm::vec3(float(tx) * tileW, 0.0f, float(tz) * tileH));
					const uint32_t tileNameId = uint32_t(scene.nodeNames.size());
					scene.nodeNames.push_back(c.text + "_" + std::to_string(tx)
						+ "_" + std::to_string(tz));
					scene.nameForNode[uint32_t(tileId)] = tileNameId;
					state.tileNodes[size_t(tz) * state.data.tilesX + tx] =
						uint32_t(tileId);
				}
			break;
		}
		case Verb::TerrainPaint: {
			// One brush stamp onto a created terrain group's global grid.
			// Meshes upload in the post-loop pass (tiles marked dirty).
			const uint32_t k = c.node & ~kCreateTerrainRefBase;
			if ((c.node & kCreateTerrainRefBase) == 0
				|| k >= terrainGroups.size()) {
				if (err)
					*err = "tour: terrainPaint ref out of range";
				if (!failCmd(c))
					return false;
				break;
			}
			TourTerrainGroup& state = terrainGroups[k];
			smvk::placement::TerrainBrushOp op;
			if (c.text == "raise")
				op = smvk::placement::TerrainBrushOp::Raise;
			else if (c.text == "lower")
				op = smvk::placement::TerrainBrushOp::Lower;
			else if (c.text == "flatten")
				op = smvk::placement::TerrainBrushOp::Flatten;
			else if (c.text == "smooth")
				op = smvk::placement::TerrainBrushOp::Smooth;
			else {
				if (err)
					*err = "tour: terrainPaint wants -at raise|lower|flatten|smooth";
				return false;
			}
			// -t packs: worldX worldZ radius strength target.
			smvk::placement::TerrainBrushStamp(state.data, op,
				float(c.vec4.x), float(c.vec4.y), float(c.vec4.z),
				float(c.number), float(c.vec4.w));
			// Mark the touched tiles dirty.
			uint32_t x0, x1, z0, z1;
			smvk::placement::TerrainBrushBox(state.data, float(c.vec4.x),
				float(c.vec4.y), float(c.vec4.z), x0, x1, z0, z1);
			std::vector<uint32_t> touched;
			smvk::placement::TerrainBrushTouchedTiles(x0, x1, z0, z1,
				state.data.heights.width, state.data.tilesX,
				state.data.tilesZ, touched);
			for (uint32_t t : touched)
				state.dirty.insert(t);
			break;
		}
		case Verb::SetLightAttr: {
			uint32_t target = c.node;
			if ((target & kCreateLightRefBase) != 0) {
				const uint32_t k = target & ~kCreateLightRefBase;
				if (k >= createdLights.size()) {
					if (err)
						*err = "tour: setLightAttr ref out of range: " +
							std::to_string(k);
					if (!failCmd(c))
						return false;
					break;
				}
				target = createdLights[k];
			}
			else if (!checkNode(target, c)) {
				if (!failCmd(c))
					return false;
				break;
			}
			auto lit = scene.lightForNode.find(target);
			if (lit == scene.lightForNode.end()) {
				if (err)
					*err = "tour: setLightAttr on non-light node " +
						std::to_string(target);
				if (!failCmd(c))
					return false;
				break;
			}
			SceneLight* def = cluster.lights.get(
				cluster.lights.getHandle(lit->second));
			if (!def) {
				if (err)
					*err = "tour: setLightAttr bad light pool index";
				if (!failCmd(c))
					return false;
				break;
			}
			if (c.text == "color") {
				def->color = glm::vec3(c.vec4.x, c.vec4.y, c.vec4.z);
			}
			else if (c.text == "intensity") {
				def->intensity = std::max(float(c.number), 0.0f);
			}
			else if (c.text == "range") {
				def->range = std::max(float(c.number), 0.0f);
			}
			else if (c.text == "inner") {
				def->innerConeAngle = glm::radians(
					std::clamp(float(c.number), 0.0f, 89.0f));
			}
			else if (c.text == "outer") {
				def->outerConeAngle = glm::radians(
					std::clamp(float(c.number), 0.0f, 89.0f));
			}
			else if (c.text == "static") {
				if (c.flag)
					def->flags |= SceneLight::kSceneLightFlag_Static;
				else
					def->flags &= ~SceneLight::kSceneLightFlag_Static;
			}
			else if (c.text == "shadow") {
				if (c.flag)
					def->flags |= SceneLight::kSceneLightFlag_CastShadow;
				else
					def->flags &= ~SceneLight::kSceneLightFlag_CastShadow;
			}
			else if (c.text == "name") {
				def->name = c.name;
				const uint32_t id = uint32_t(scene.nodeNames.size());
				scene.nodeNames.push_back(c.name);
				scene.nameForNode[target] = id;
			}
			else if (c.text == "position" || c.text == "rotation") {
				// Rebuild the node's local TRS with one channel replaced
				// (editor TRS convention: Rz*Ry*Rx, degrees).
				glm::vec3 skew, t, s;
				glm::vec4 persp;
				glm::quat q;
				glm::decompose(scene.localTransform[target], s, q, t,
					skew, persp);
				glm::vec3 e = glm::degrees(glm::eulerAngles(q));
				if (c.text == "position")
					t = glm::vec3(c.vec4.x, c.vec4.y, c.vec4.z);
				else
					e = glm::vec3(c.vec4.x, c.vec4.y, c.vec4.z);
				glm::mat4 m(1.0f);
				m = glm::translate(m, t);
				m = glm::rotate(m, glm::radians(e.z), glm::vec3(0, 0, 1));
				m = glm::rotate(m, glm::radians(e.y), glm::vec3(0, 1, 0));
				m = glm::rotate(m, glm::radians(e.x), glm::vec3(1, 0, 0));
				m = glm::scale(m, s);
				scene.localTransform[target] = m;
			}
		else {
			warn("tour: unknown light attr '" + c.text + "' (skipped)");
		}
		break;
	}
	case Verb::SetCvolAttr: {
		uint32_t target = c.node;
		if ((target & kCreateCvolRefBase) != 0) {
			const uint32_t k = target & ~kCreateCvolRefBase;
			if (k >= createdCvols.size()) {
				if (err)
					*err = "tour: setCvolAttr ref out of range: " +
						std::to_string(k);
				if (!failCmd(c))
					return false;
				break;
			}
			target = createdCvols[k];
		}
		else if (!checkNode(target, c)) {
			if (!failCmd(c))
				return false;
			break;
		}
		auto cvit = scene.cvolForNode.find(target);
		if (cvit == scene.cvolForNode.end()) {
			if (err)
				*err = "tour: setCvolAttr on non-cvolume node " +
					std::to_string(target);
			if (!failCmd(c))
				return false;
			break;
		}
		SceneCvol& def = cvit->second;
		if (c.text == "priority") {
			def.priority = uint32_t(std::max(c.number, 0.0));
		}
		else if (c.text == "spacing") {
			def.spacing = std::max(float(c.number), 1e-3f);
		}
		else if (c.text == "inside") {
			def.insideFade = std::max(float(c.number), 0.0f);
		}
		else if (c.text == "outside") {
			def.outsideFade = std::max(float(c.number), 0.0f);
		}
		else if (c.text == "name") {
			const uint32_t id = uint32_t(scene.nodeNames.size());
			scene.nodeNames.push_back(c.name);
			scene.nameForNode[target] = id;
		}
		else if (c.text == "position" || c.text == "rotation"
			|| c.text == "size") {
			// Rebuild the node's local TRS with one channel
			// replaced (editor TRS convention: Rz*Ry*Rx, degrees;
			// size = full extents on the scale channel).
			glm::vec3 skew, t, s;
			glm::vec4 persp;
			glm::quat q;
			glm::decompose(scene.localTransform[target], s, q, t,
				skew, persp);
			glm::vec3 e = glm::degrees(glm::eulerAngles(q));
			if (c.text == "position")
				t = glm::vec3(c.vec4.x, c.vec4.y, c.vec4.z);
			else if (c.text == "rotation")
				e = glm::vec3(c.vec4.x, c.vec4.y, c.vec4.z);
			else
				s = glm::vec3(std::max(c.vec4.x, 1e-3f),
					std::max(c.vec4.y, 1e-3f),
					std::max(c.vec4.z, 1e-3f));
			glm::mat4 m(1.0f);
			m = glm::translate(m, t);
			m = glm::rotate(m, glm::radians(e.z), glm::vec3(0, 0, 1));
			m = glm::rotate(m, glm::radians(e.y), glm::vec3(0, 1, 0));
			m = glm::rotate(m, glm::radians(e.x), glm::vec3(1, 0, 0));
			m = glm::scale(m, s);
			scene.localTransform[target] = m;
		}
		else {
			warn("tour: unknown cvolume attr '" + c.text + "' (skipped)");
		}
		break;
	}
	case Verb::SetCubemapAttr: {
		uint32_t target = c.node;
		if ((target & kCreateCubemapRefBase) != 0) {
			const uint32_t k = target & ~kCreateCubemapRefBase;
			if (k >= createdCubemaps.size()) {
				if (err)
					*err = "tour: setCubemapAttr ref out of range: " +
						std::to_string(k);
				if (!failCmd(c))
					return false;
				break;
			}
			target = createdCubemaps[k];
		}
		else if (!checkNode(target, c)) {
			if (!failCmd(c))
				return false;
			break;
		}
		auto cmit = scene.cubemapForNode.find(target);
		if (cmit == scene.cubemapForNode.end()) {
			if (err)
				*err = "tour: setCubemapAttr on non-cubemap node " +
					std::to_string(target);
			if (!failCmd(c))
				return false;
			break;
		}
		SceneCubemap& def = cmit->second;
		if (c.text == "radius") {
			def.radius = std::max(float(c.number), 1e-2f);
		}
		else if (c.text == "name") {
			const uint32_t id = uint32_t(scene.nodeNames.size());
			scene.nodeNames.push_back(c.name);
			scene.nameForNode[target] = id;
		}
		else if (c.text == "position") {
			// Rebuild the node's local TRS with the translate channel
			// replaced (editor TRS convention).
			glm::vec3 skew, t, s;
			glm::vec4 persp;
			glm::quat q;
			glm::decompose(scene.localTransform[target], s, q, t,
				skew, persp);
			glm::vec3 e = glm::degrees(glm::eulerAngles(q));
			t = glm::vec3(c.vec4.x, c.vec4.y, c.vec4.z);
			glm::mat4 m(1.0f);
			m = glm::translate(m, t);
			m = glm::rotate(m, glm::radians(e.z), glm::vec3(0, 0, 1));
			m = glm::rotate(m, glm::radians(e.y), glm::vec3(0, 1, 0));
			m = glm::rotate(m, glm::radians(e.x), glm::vec3(1, 0, 0));
			m = glm::scale(m, s);
			scene.localTransform[target] = m;
		}
		else {
			warn("tour: unknown cubemap attr '" + c.text + "' (skipped)");
		}
		break;
	}
	case Verb::SetCamAttr: {
		uint32_t target = 0;
		if ((c.node & kCreateCameraRefBase) != 0) {
			const uint32_t k = c.node & ~kCreateCameraRefBase;
			if (k >= createdCameras.size()) {
				if (err)
					*err = "tour: setCamAttr ref out of range: " +
						std::to_string(k);
				if (!failCmd(c))
					return false;
				break;
			}
			target = createdCameras[k];
		}
		else if (!resolveNode(c.node, c, target)) {
			if (!failCmd(c))
				return false;
			break;
		}
		auto lit = scene.cameraForNode.find(target);
		if (lit == scene.cameraForNode.end()) {
			if (err)
				*err = "tour: setCamAttr on non-camera node " +
					std::to_string(target);
			if (!failCmd(c))
				return false;
			break;
		}
		SceneCamera* def = cluster.cameras.get(
			cluster.cameras.getHandle(lit->second));
		if (!def) {
			if (err)
				*err = "tour: setCamAttr bad camera pool index";
			if (!failCmd(c))
				return false;
			break;
		}
		if (c.text == "name") {
			def->name = c.name;
			const uint32_t id = uint32_t(scene.nodeNames.size());
			scene.nodeNames.push_back(c.name);
			scene.nameForNode[target] = id;
		}
		else if (c.text == "position" || c.text == "rotation") {
			// Rebuild the node's local TRS with one channel replaced
			// (editor TRS convention; mirrors the cubemap/cvolume path).
			glm::vec3 skew, t, s;
			glm::vec4 persp;
			glm::quat q;
			glm::decompose(scene.localTransform[target], s, q, t,
				skew, persp);
			glm::vec3 e = glm::degrees(glm::eulerAngles(q));
			if (c.text == "position")
				t = glm::vec3(c.vec4.x, c.vec4.y, c.vec4.z);
			else
				e = glm::vec3(c.vec4.x, c.vec4.y, c.vec4.z);
			glm::mat4 m(1.0f);
			m = glm::translate(m, t);
			m = glm::rotate(m, glm::radians(e.z), glm::vec3(0, 0, 1));
			m = glm::rotate(m, glm::radians(e.y), glm::vec3(0, 1, 0));
			m = glm::rotate(m, glm::radians(e.x), glm::vec3(1, 0, 0));
			m = glm::scale(m, s);
			scene.localTransform[target] = m;
		}
		else if (c.text == "projection") {
			def->projection = (c.number >= 0.5)
				? SceneCamera::Projection::Orthographic
				: SceneCamera::Projection::Perspective;
		}
		else if (c.text == "fovY") {
			def->fovYDeg = std::clamp(float(c.number), 1.0f, 179.0f);
		}
		else if (c.text == "orthoHalfH") {
			def->orthoHalfH = std::clamp(float(c.number), 1e-2f, 1e5f);
		}
		else if (c.text == "near") {
			def->nearClip = std::max(float(c.number), 1e-4f);
			def->farClip = std::max(def->farClip,
				def->nearClip * 1.1f + 1e-3f);
		}
		else if (c.text == "far") {
			def->farClip = std::max(float(c.number),
				def->nearClip * 1.1f + 1e-3f);
		}
		else {
			warn("tour: unknown camera attr '" + c.text + "' (skipped)");
		}
		break;
	}
		case Verb::CreateNode: {
			// System node types (material/file/image): parsed +
			// validated up front; replay defers to the editor — reported
			// here, never silently dropped. (DAG types lower to their
			// legacy verbs at parse time and never reach this case.)
			warn("tour: system node type '" + c.nodeType
				+ "' skipped at cook ('" + c.text + "' — editor-managed)");
			break;
		}
		}
	}
	// Terrain tile upload pass: every group's heights are final now (all
	// terrainPaint stamps applied); meshes must come from the baked grid.
	// Groups with no stamps upload every tile; painted groups upload the
	// dirty set only (identical result — unpainted tiles already match).
	for (TourTerrainGroup& state : terrainGroups) {
		const uint32_t tX = state.data.tilesX;
		const uint32_t tZ = state.data.tilesZ;
		for (uint32_t tz = 0; tz < tZ; ++tz)
			for (uint32_t tx = 0; tx < tX; ++tx) {
				const uint32_t packed = tx | (tz << 16);
				if (!state.dirty.empty() && !state.dirty.contains(packed))
					continue;
				smvk::StaticMeshUpload upload;
				if (!smvk::placement::BuildTerrainTileUpload(
						state.data, tx, tz, upload)) {
					if (strict) {
						if (err)
							*err = "tour: terrain tile mesh build failed";
						return false;
					}
					warn("tour: terrain tile mesh build failed (tile "
						+ std::to_string(tx) + "_" + std::to_string(tz)
						+ ")");
					continue;
				}
				const ClusterStaticMeshHandle mh =
					cluster.UploadStaticMesh(upload);
				if (!mh.valid()) {
					if (strict) {
						if (err)
							*err = "tour: terrain tile upload failed";
						return false;
					}
					warn("tour: terrain tile upload failed (tile "
						+ std::to_string(tx) + "_" + std::to_string(tz)
						+ ")");
					continue;
				}
				const uint32_t tileNode =
					state.tileNodes[size_t(tz) * tX + tx];
				cluster.RegisterMeshUri("terrain/" + std::to_string(
					state.groupNode) + "_" + std::to_string(tx) + "_"
					+ std::to_string(tz), mh);
				scene.meshForNode[tileNode] = mh.index();
				scene.materialForNode[tileNode] = state.material;
			}
	}
	// Transform pass: collect per-node TRS triples (tour order = last wins
	// per channel), then rebuild each touched node's local matrix.
	{
		struct Triple {
			bool hasT = false, hasR = false, hasS = false;
			glm::vec3 t{ 0 }, r{ 0 }, s{ 1 };
		};
		std::unordered_map<uint32_t, Triple> triples;
		for (const Command& c : doc.commands) {
			if (c.verb != Verb::SetTranslate && c.verb != Verb::SetRotate &&
				c.verb != Verb::SetScale)
				continue;
			uint32_t target = 0;
			if (!resolveNode(c.node, c, target)) {
				if (!failCmd(c))
					return false;
				break;
			}
			Triple& t = triples[target];
			if (c.verb == Verb::SetTranslate) {
				t.hasT = true;
				t.t = c.vec;
			}
			else if (c.verb == Verb::SetRotate) {
				t.hasR = true;
				t.r = c.vec;
			}
			else {
				t.hasS = true;
				t.s = c.vec;
			}
			if (!c.name.empty() && getNodeName(scene, int(target)) != c.name)
				warn("tour: node " + std::to_string(target) + " name mismatch");
		}
		for (auto& [node, t] : triples) {
			// Decompose current local, replace touched channels, rebuild.
			glm::vec3 skew;
			glm::vec4 persp;
			glm::vec3 curT, curS;
			glm::quat curQ;
			glm::decompose(scene.localTransform[node], curS, curQ, curT, skew, persp);
			if (t.hasT)
				curT = t.t;
			if (t.hasS)
				curS = t.s;
			glm::mat4 m(1.0f);
			m = glm::translate(m, curT);
			if (t.hasR) {
				m = glm::rotate(m, glm::radians(t.r.z), glm::vec3(0, 0, 1));
				m = glm::rotate(m, glm::radians(t.r.y), glm::vec3(0, 1, 0));
				m = glm::rotate(m, glm::radians(t.r.x), glm::vec3(1, 0, 0));
			}
			else {
				m *= glm::mat4_cast(curQ);
			}
			m = glm::scale(m, curS);
			scene.localTransform[node] = m;
			markAsChanged(scene, int(node));
		}
	}
	markAsChanged(scene, 0);
	recalculateGlobalTransforms(scene);
	// Wide override pass (Clarisse-style): regex rules in order, later
	// wins. Per-node local commands are MORE specific and win over any
	// wide rule touching the same node+attr (local wins).
	if (!doc.overrides.empty()) {
		std::unordered_set<uint32_t> localVis, localMat, localNoLM, localDither;
		for (const Command& c : doc.commands) {
			switch (c.verb) {
			case Verb::SetVisibility: localVis.insert(c.node); break;
			case Verb::AssignMaterial: localMat.insert(c.node); break;
			case Verb::SetNoLightmap: localNoLM.insert(c.node); break;
			case Verb::SetDitherCloseup: localDither.insert(c.node); break;
			default: break;
			}
		}
		for (const OverrideRule& r : doc.overrides) {
			if (!r.enabled || r.pattern.empty())
				continue;
			std::regex rx;
			try {
				rx.assign(r.pattern, std::regex::ECMAScript | std::regex::optimize);
			}
			catch (...) {
				warn(std::string("tour: bad override regex '") + r.pattern + "' (skipped)");
				continue;
			}
			if (r.attr == OverrideAttr::Material && !CookMaterial(cluster, r.material)) {
				warn("tour: override material out of range: " + std::to_string(r.material));
				continue;
			}
			for (uint32_t n = 0; n < uint32_t(scene.hierarchy.size()); ++n) {
				const std::string nm = getNodeName(scene, int(n));
				if (nm.empty())
					continue;
				bool hit = false;
				try {
					hit = std::regex_search(nm, rx);
				}
				catch (...) {
					continue;
				}
				if (!hit)
					continue;
				switch (r.attr) {
				case OverrideAttr::Visibility:
					if (localVis.contains(n))
						continue; // local wins
					if (scene.hidden.size() != scene.hierarchy.size())
						scene.hidden.assign(scene.hierarchy.size(), 0);
					scene.hidden[n] = r.flag ? 1 : 0;
					break;
				case OverrideAttr::Material:
					if (localMat.contains(n))
						continue;
					scene.materialForNode[n] = r.material;
					break;
				case OverrideAttr::NoLightmap:
					if (localNoLM.contains(n))
						continue;
					if (scene.noLightmap.size() != scene.hierarchy.size())
						scene.noLightmap.assign(scene.hierarchy.size(), 0);
					scene.noLightmap[n] = r.flag ? 1 : 0;
					break;
				case OverrideAttr::DitherCloseup:
					if (localDither.contains(n))
						continue;
					if (scene.ditherCloseup.size() != scene.hierarchy.size())
						scene.ditherCloseup.assign(scene.hierarchy.size(), 0);
					scene.ditherCloseup[n] = r.flag ? 1 : 0;
					break;
				}
			}
		}
	}
	// Authored environment (tour env blocks -> cluster scene): resolve the
	// ACTIVE block (the doc's env.current node, else the default block)
	// into the cluster scene + carry the default + node table (world
	// assembly keeps the first authored one; the pak v9 section persists
	// it for the App). Runs after command replay so node add/delete
	// settles first; node ids are cluster-space (dangling blocks warn +
	// drop, never fatal — same discipline as the command replay above).
	// NOTE: the only editorSettings keys the cook reads — everything else
	// in that section stays cook-ignored by contract.
	{
		TourEnvBlocks eb = ParseTourEnvBlocks(doc);
		if (eb.hasBlocks) {
			std::unordered_map<uint32_t, SceneEnv> table;
			for (const auto& [n, e] : eb.nodeEnvs) {
				if (n < scene.hierarchy.size())
					table[n] = e;
				else
					warn("tour: env node block #" + std::to_string(n) +
						" out of range (dropped)");
			}
			SceneEnv active = eb.def;
			if (eb.haveCurrent) {
				auto it = table.find(eb.currentCluster);
				if (it != table.end())
					active = it->second;
				else
					warn("tour: env.current node #" +
						std::to_string(eb.currentCluster) +
						" missing (default renders)");
			}
			scene.environment = active;
			scene.hasEnvironment = true;
			scene.defaultEnvironment = eb.def;
			scene.envForNode = std::move(table);
		}
	}
	return true;
}

std::string Describe(const Command& c) {
	char buf[256];
	// Created-terrain refs render readably (raw ids would print the
	// 0x40000000 base bit).
	char tgt[32], tgt2[32];
	auto target = [&](uint32_t n, char* out) {
		if ((n & kCreateTerrainRefBase) != 0)
			std::snprintf(out, 32, "terrain%u", n & ~kCreateTerrainRefBase);
		else
			std::snprintf(out, 32, "#%u", n);
	};
	switch (c.verb) {
	case Verb::Rename:
		target(c.node, tgt);
		std::snprintf(buf, sizeof(buf), "rename %s -> '%s'", tgt, c.text.c_str());
		break;
	case Verb::SetTranslate:
		target(c.node, tgt);
		std::snprintf(buf, sizeof(buf), "translate %s (%.2f %.2f %.2f)",
			tgt, c.vec.x, c.vec.y, c.vec.z);
		break;
	case Verb::SetRotate:
		target(c.node, tgt);
		std::snprintf(buf, sizeof(buf), "rotate %s (%.1f %.1f %.1f)",
			tgt, c.vec.x, c.vec.y, c.vec.z);
		break;
	case Verb::SetScale:
		target(c.node, tgt);
		std::snprintf(buf, sizeof(buf), "scale %s (%.2f %.2f %.2f)",
			tgt, c.vec.x, c.vec.y, c.vec.z);
		break;
	case Verb::AssignMaterial:
		target(c.node, tgt);
		std::snprintf(buf, sizeof(buf), "material %s -> mat.%u", tgt, c.material);
		break;
	case Verb::SetMatAttr:
		std::snprintf(buf, sizeof(buf), "mat.%u.%s", c.material, c.text.c_str());
		break;
	case Verb::SetNoLightmap:
		target(c.node, tgt);
		std::snprintf(buf, sizeof(buf), "noLightmap %s = %d", tgt, c.flag ? 1 : 0);
		break;
	case Verb::SetDitherCloseup:
		target(c.node, tgt);
		std::snprintf(buf, sizeof(buf), "ditherCloseup %s = %d", tgt, c.flag ? 1 : 0);
		break;
	case Verb::SetVisibility:
		target(c.node, tgt);
		std::snprintf(buf, sizeof(buf), "visibility %s = %s", tgt,
			c.flag ? "hidden" : "visible");
		break;
	case Verb::Parent:
		target(c.node, tgt);
		target(c.node2, tgt2);
		std::snprintf(buf, sizeof(buf), "parent %s under %s", tgt, tgt2);
		break;
	case Verb::CreateLight:
		std::snprintf(buf, sizeof(buf), "createLight '%s' (%s) under #%u",
			c.text.c_str(), c.vec4.x >= 1.5f ? "spot" : "point", c.node);
		break;
	case Verb::CreateTerrain:
		std::snprintf(buf, sizeof(buf),
			"createTerrain '%s' (%.0fm %.0f %.1fm @%.3f, %ux%u tiles) under #%u",
			c.text.c_str(), c.vec4.x, c.vec4.y, c.vec4.z, c.vec4.w,
			c.node2 & 0xFFFFu, (c.node2 >> 16) & 0xFFFFu, c.node);
		break;
	case Verb::CreateCVolume:
		std::snprintf(buf, sizeof(buf),
			"createCvolume '%s' (%.1fm box, p%u s%.2f) under #%u",
			c.text.c_str(), c.svec.x, uint32_t(c.vec4.x), c.vec4.y,
			c.node);
		break;
	case Verb::CreateCubemap:
		std::snprintf(buf, sizeof(buf),
			"createCubemap '%s' (r=%.1fm) under #%u",
			c.text.c_str(), float(c.number), c.node);
		break;
	case Verb::CreateCamera:
		std::snprintf(buf, sizeof(buf),
			"createCamera '%s' (%s fov=%.0f) under #%u",
			c.text.c_str(), c.vec4.x >= 0.5f ? "ortho" : "persp",
			c.vec4.y, c.node);
		break;
	case Verb::SetCamAttr: {
		char tgt[32];
		if ((c.node & kCreateCameraRefBase) != 0)
			std::snprintf(tgt, sizeof(tgt), "create%u", c.node & ~kCreateCameraRefBase);
		else
			std::snprintf(tgt, sizeof(tgt), "#%u", c.node);
		std::snprintf(buf, sizeof(buf), "cam.%s.%s", tgt, c.text.c_str());
		break;
	}
	case Verb::SetCubemapAttr: {
		char tgt[32];
		if ((c.node & kCreateCubemapRefBase) != 0)
			std::snprintf(tgt, sizeof(tgt), "create%u", c.node & ~kCreateCubemapRefBase);
		else
			std::snprintf(tgt, sizeof(tgt), "#%u", c.node);
		std::snprintf(buf, sizeof(buf), "cube.%s.%s", tgt, c.text.c_str());
		break;
	}
	case Verb::SetCvolAttr: {
		char tgt[32];
		if ((c.node & kCreateCvolRefBase) != 0)
			std::snprintf(tgt, sizeof(tgt), "create%u", c.node & ~kCreateCvolRefBase);
		else
			std::snprintf(tgt, sizeof(tgt), "#%u", c.node);
		std::snprintf(buf, sizeof(buf), "cvol.%s.%s", tgt, c.text.c_str());
		break;
	}
	case Verb::TerrainPaint:
		std::snprintf(buf, sizeof(buf), "terrainPaint.%s (r=%.1f s=%.2f)",
			c.text.c_str(), c.vec4.z, c.number);
		break;
	case Verb::SetLightAttr: {
		char tgt[32];
		if ((c.node & kCreateLightRefBase) != 0)
			std::snprintf(tgt, sizeof(tgt), "create%u", c.node & ~kCreateLightRefBase);
		else
			std::snprintf(tgt, sizeof(tgt), "#%u", c.node);
		std::snprintf(buf, sizeof(buf), "light.%s.%s", tgt, c.text.c_str());
		break;
	}
	case Verb::Delete: {
		if (!c.uuid.empty())
			std::snprintf(buf, sizeof(buf), "delete '%s' [uuid %.8s..]",
				c.name.empty() ? "?" : c.name.c_str(), c.uuid.c_str());
		else {
			target(c.node, tgt);
			std::snprintf(buf, sizeof(buf), "delete %s", tgt);
		}
		break;
	}
	case Verb::SetUuid: {
		std::snprintf(buf, sizeof(buf), "uuid #%u <- %.8s..", c.node,
			c.uuid.c_str());
		break;
	}
	case Verb::CreateNode:
		std::snprintf(buf, sizeof(buf), "createNode %s '%s'",
			c.nodeType.c_str(), c.text.c_str());
		break;
	}
	return buf;
}

std::string GenerateUuid() {
	std::random_device rd;
	std::mt19937_64 rng(rd());
	const auto hex64 = [](uint64_t v, char* dst) {
		static const char* k = "0123456789abcdef";
		for (int i = 15; i >= 0; --i) {
			dst[i] = k[v & 0xF];
			v >>= 4;
		}
	};
	char buf[33] = {};
	hex64(rng(), buf);
	hex64(rng(), buf + 16);
	return std::string(buf, 32);
}

bool SetNodeUuid(Scene& scene, uint32_t node, const std::string& uuid) {
	if (node >= scene.hierarchy.size() || uuid.empty())
		return false;
	// Maya setUuid parity: reject a uuid already held by ANOTHER live
	// node. Re-setting the holder's own uuid is a no-op true.
	for (const auto& [n, u] : scene.uuidForNode)
		if (u == uuid) {
			if (n == node)
				return true;
			return false;
		}
	scene.uuidForNode[node] = uuid;
	return true;
}

static std::vector<std::string> SplitDagPath(const std::string& path) {
	std::vector<std::string> comps;
	size_t start = 0;
	for (;;) {
		const size_t slash = path.find('/', start);
		std::string piece = path.substr(start,
			slash == std::string::npos ? std::string::npos
				: slash - start);
		if (!piece.empty())
			comps.push_back(piece);
		if (slash == std::string::npos)
			break;
		start = slash + 1;
	}
	return comps;
}

std::string DagFullPath(const Scene& scene, uint32_t node) {
	if (node >= scene.hierarchy.size())
		return {};
	// Root-to-node components (the unnamed root contributes nothing).
	std::vector<std::string> comps;
	for (int cur = int(node);
		cur >= 0 && size_t(cur) < scene.hierarchy.size();
		cur = scene.hierarchy[size_t(cur)].parent) {
		const std::string nm = getNodeName(scene, cur);
		if (!nm.empty() || comps.empty() || cur == 0)
			comps.push_back(nm);
	}
	std::string path;
	for (auto it = comps.rbegin(); it != comps.rend(); ++it) {
		if (!path.empty())
			path += '/';
		path += *it;
	}
	return path;
}

static bool NodeMatchesSuffix(const Scene& scene, uint32_t node,
	const std::vector<std::string>& comps) {
	if (comps.empty())
		return false;
	if (getNodeName(scene, int(node)) != comps.back())
		return false;
	int cur = int(node);
	for (int k = int(comps.size()) - 2; k >= 0; --k) {
		const int p = (cur >= 0 && size_t(cur) < scene.hierarchy.size())
			? scene.hierarchy[size_t(cur)].parent : -1;
		if (p < 0 || getNodeName(scene, p) != comps[size_t(k)])
			return false;
		cur = p;
	}
	return true;
}

std::string DagShortestUniquePath(const Scene& scene, uint32_t node) {
	// Maya partialPathName: shortest suffix that still names the node
	// uniquely; the bare name when it already does; empty when even the
	// full path is ambiguous.
	if (node >= scene.hierarchy.size())
		return {};
	const std::string leaf = getNodeName(scene, int(node));
	if (leaf.empty())
		return {};
	size_t unique = 0;
	for (uint32_t n = 0; n < scene.hierarchy.size(); ++n)
		if (getNodeName(scene, int(n)) == leaf && ++unique > 1)
			break;
	if (unique == 1)
		return leaf;
	const std::vector<std::string> comps =
		SplitDagPath(DagFullPath(scene, node));
	for (size_t len = 2; len <= comps.size(); ++len) {
		std::vector<std::string> suffix(comps.end() - len, comps.end());
		size_t count = 0;
		for (uint32_t n = 0; n < scene.hierarchy.size(); ++n)
			if (NodeMatchesSuffix(scene, n, suffix) && ++count > 1)
				break;
		if (count == 1) {
			std::string out;
			for (const std::string& c : suffix) {
				if (!out.empty())
					out += '/';
				out += c;
			}
			return out;
		}
	}
	return {};
}

static bool MatchDagPathSuffix(const Scene& scene, const std::string& path,
	uint32_t& out) {
	const std::vector<std::string> comps = SplitDagPath(path);
	if (comps.empty())
		return false;
	for (uint32_t n = 0; n < scene.hierarchy.size(); ++n)
		if (NodeMatchesSuffix(scene, n, comps)) {
			out = n;
			return true;
		}
	return false;
}

bool ResolveDagPath(const Scene& scene, const std::string& path,
	uint32_t& out) {
	if (path.empty() || path.find('/') == std::string::npos)
		return false;
	return MatchDagPathSuffix(scene, path, out);
}

bool ResolveParentTarget(const Scene& scene, const std::string& ref,
	uint32_t& out) {
	if (ref.empty())
		return false;
	// 32-hex reads as a uuid first (writer prefers it for unnamed
	// nodes); a short digit string can only be a legacy id (handled by
	// the caller, never here).
	if (ref.size() == 32) {
		bool hex = true;
		for (char ch : ref)
			hex = hex && ((ch >= '0' && ch <= '9')
				|| (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'));
		if (hex) {
			std::string lower = ref;
			for (char& ch : lower)
				ch = char(std::tolower((unsigned char)ch));
			for (const auto& [n, u] : scene.uuidForNode)
				if (u == lower && n < scene.hierarchy.size()) {
					out = n;
					return true;
				}
		}
	}
	// Name, then DAG path (both directions share the delete resolver's
	// layers; UINT32_MAX id means "no numeric fallback").
	return ResolveDeleteTarget(scene, {}, ref, UINT32_MAX, out);
}

bool ResolveDeleteTarget(const Scene& scene, const std::string& uuid,
	const std::string& name, uint32_t idFallback, uint32_t& out) {
	// 1. UUID: exact match (file-stable identity across re-imports).
	if (!uuid.empty()) {
		for (const auto& [n, u] : scene.uuidForNode)
			if (u == uuid && n < scene.hierarchy.size()) {
				out = n;
				return true;
			}
	}
	// 2. NAME: unique full-name match; else a DAG path suffix.
	if (!name.empty()) {
		std::vector<uint32_t> hits;
		for (uint32_t n = 0; n < scene.hierarchy.size(); ++n)
			if (getNodeName(scene, int(n)) == name)
				hits.push_back(n);
		if (hits.size() == 1) {
			out = hits[0];
			return true;
		}
		// MDagPath layer: full path or ancestor suffix ("grp/mesh",
		// matched from any depth).
		if (ResolveDagPath(scene, name, out))
			return true;
		// 3. id as disambiguator: valid and still naming this node.
		if (idFallback != UINT32_MAX
			&& idFallback < scene.hierarchy.size()
			&& getNodeName(scene, int(idFallback)) == name) {
			out = idFallback;
			return true;
		}
		return false;
	}
	// 4. Legacy nameless form: the recorded id.
	if (idFallback != UINT32_MAX && idFallback < scene.hierarchy.size()) {
		out = idFallback;
		return true;
	}
	return false;
}

} // namespace smvk::tour
