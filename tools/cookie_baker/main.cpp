// CookieBaker: offline baker for the light-cookie (gobo) textures.
//
//   CookieBaker [--out-dir engine/resources] [--size 256] [--mips on|off]
//             [--pak <host path>] [--verify on|off]
//
// Generates the procedural patterns with the engine's shared math
// (Engine/Renderer/cookies.h — same code as the runtime fallback, so baked
// files cannot drift from it), box-filters the mip chain, and writes
// uncompressed RGBA8 (VK_FORMAT_R8G8B8A8_UNORM, linear OETF) KTX2 files to
// <out-dir>/<cookie vfs path>, e.g. engine/resources/textures/cookies/.
// The engine CMake step copies <out-dir> to ${CMAKE_BINARY_DIR}/
// engine_resources, which apps mount as the lowest-priority VFS backend.
//
// Optional --pak additionally packs the baked bytes into a texture-only
// .pak (PakWriter texture blobs; PakBackend serves them by VFS path, so
// mounting that pak replaces the loose files with no code change).
//
// --verify re-opens every written file through libktx (format / level count
// / linear transfer / base-mip bytes) and checks both VFS serving paths
// (LooseBackend over the output dir, PakBackend over the pak when built).

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <volk.h>

#include "ktx.h"
#include <ktxVulkan.h>

#include <Engine/Renderer/cookies.h>
#include <Engine/Asset/pak.h>
#include <Engine/Vfs/vfs.h>

namespace {

struct Args {
	std::string outDir = "engine/resources";
	uint32_t size = smvk::kCookieTextureSize;
	bool mips = true;
	std::string pak; // empty = no pak
	bool verify = true;
};

bool ParseArgs(int argc, char** argv, Args& out) {
	for (int i = 1; i < argc; ++i) {
		const std::string a = argv[i];
		auto need = [&](std::string& dst) {
			if (i + 1 >= argc) {
				std::cerr << "Missing value for " << a << std::endl;
				return false;
			}
			dst = argv[++i];
			return true;
		};
		if (a == "--out-dir") {
			if (!need(out.outDir)) return false;
		}
		else if (a == "--size") {
			std::string v;
			if (!need(v)) return false;
			out.size = static_cast<uint32_t>(std::stoul(v));
			if (out.size < 4 || out.size > 4096) {
				std::cerr << "--size must be within [4, 4096]" << std::endl;
				return false;
			}
		}
		else if (a == "--mips") {
			std::string v;
			if (!need(v)) return false;
			out.mips = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--pak") {
			if (!need(out.pak)) return false;
		}
		else if (a == "--verify") {
			std::string v;
			if (!need(v)) return false;
			out.verify = (v == "on" || v == "1" || v == "true");
		}
		else if (a == "--help" || a == "-h") {
			return false;
		}
		else {
			std::cerr << "Unknown arg: " << a << std::endl;
			return false;
		}
	}
	return true;
}

void Usage() {
	std::cout << "CookieBaker [--out-dir engine/resources] [--size 256]\n"
		<< "          [--mips on|off] [--pak <host path>] [--verify on|off]\n";
}

struct KtxGuard {
	ktxTexture2* tex = nullptr;
	explicit KtxGuard(ktxTexture2* t) : tex(t) {}
	~KtxGuard() { if (tex) ktxTexture2_Destroy(tex); }
	KtxGuard(const KtxGuard&) = delete;
	KtxGuard& operator=(const KtxGuard&) = delete;
};

// Box-filtered mip chain: level 0 = base, then halved dims (clamped 2x2
// average, odd edges sample the last texel) down to 1x1.
std::vector<std::vector<uint8_t>> BuildMipChain(
	const std::vector<uint8_t>& base, uint32_t w, uint32_t h)
{
	std::vector<std::vector<uint8_t>> chain;
	chain.push_back(base);
	uint32_t pw = w, ph = h;
	while (pw > 1 || ph > 1) {
		const uint32_t cw = std::max(1u, pw / 2);
		const uint32_t ch = std::max(1u, ph / 2);
		std::vector<uint8_t> dst(size_t(cw) * ch * 4);
		for (uint32_t y = 0; y < ch; ++y) {
			for (uint32_t x = 0; x < cw; ++x) {
				uint32_t acc[4] = { 0, 0, 0, 0 };
				uint32_t n = 0;
				for (uint32_t dy = 0; dy < 2; ++dy) {
					for (uint32_t dx = 0; dx < 2; ++dx) {
						const uint32_t sx = std::min(x * 2 + dx, pw - 1);
						const uint32_t sy = std::min(y * 2 + dy, ph - 1);
						const uint8_t* s =
							&chain.back()[(size_t(sy) * pw + sx) * 4];
						for (int c = 0; c < 4; ++c)
							acc[c] += s[c];
						++n;
					}
				}
				uint8_t* d = &dst[(size_t(y) * cw + x) * 4];
				for (int c = 0; c < 4; ++c)
					d[c] = uint8_t((acc[c] + n / 2) / n);
			}
		}
		chain.push_back(std::move(dst));
		pw = cw;
		ph = ch;
	}
	return chain;
}

std::filesystem::path HostPath(const std::string& outDir, const std::string& vfsPath) {
	return std::filesystem::path(outDir) / std::filesystem::path(vfsPath);
}

bool WriteCookieKtx2(const std::filesystem::path& file,
	const std::vector<std::vector<uint8_t>>& mips, uint32_t w, uint32_t h,
	std::string& err)
{
	ktxTextureCreateInfo ci{};
	ci.vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
	ci.baseWidth = w;
	ci.baseHeight = h;
	ci.baseDepth = 1;
	ci.numDimensions = 2;
	ci.numLevels = static_cast<ktx_uint32_t>(mips.size());
	ci.numLayers = 1;
	ci.numFaces = 1;
	ci.isArray = KTX_FALSE;
	ci.generateMipmaps = KTX_FALSE; // mips are pre-baked into the file

	ktxTexture2* tex = nullptr;
	KTX_error_code res = ktxTexture2_Create(
		&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex);
	if (res != KTX_SUCCESS || !tex) {
		err = std::string("ktxTexture2_Create: ") + ktxErrorString(res);
		return false;
	}
	KtxGuard guard(tex);

	// Linear OETF: cookie pixels are linear multipliers on the light color,
	// the runtime must not sample them through an sRGB view.
	res = ktxTexture2_SetOETF(tex, KHR_DF_TRANSFER_LINEAR);
	if (res != KTX_SUCCESS) {
		err = std::string("ktxTexture2_SetOETF: ") + ktxErrorString(res);
		return false;
	}

	for (ktx_uint32_t level = 0; level < mips.size(); ++level) {
		res = ktxTexture_SetImageFromMemory(ktxTexture(tex),
			level, 0, 0, mips[level].data(), mips[level].size());
		if (res != KTX_SUCCESS) {
			err = std::string("ktxTexture_SetImageFromMemory: ")
				+ ktxErrorString(res);
			return false;
		}
	}

	std::error_code ec;
	std::filesystem::create_directories(file.parent_path(), ec);
	res = ktxTexture2_WriteToNamedFile(tex, file.string().c_str());
	if (res != KTX_SUCCESS) {
		err = std::string("ktxTexture2_WriteToNamedFile: ") + ktxErrorString(res);
		return false;
	}
	return true;
}

std::optional<std::vector<uint8_t>> ReadFileBytes(const std::filesystem::path& file) {
	std::ifstream f(file, std::ios::binary | std::ios::ate);
	if (!f)
		return std::nullopt;
	const auto size = f.tellg();
	f.seekg(0);
	std::vector<uint8_t> bytes(static_cast<size_t>(size));
	if (!f.read(reinterpret_cast<char*>(bytes.data()), size))
		return std::nullopt;
	return bytes;
}

bool WriteFileBytes(const std::filesystem::path& file,
	const std::vector<uint8_t>& bytes)
{
	std::error_code ec;
	std::filesystem::create_directories(file.parent_path(), ec);
	FILE* f = std::fopen(file.string().c_str(), "wb");
	if (!f)
		return false;
	const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	std::fclose(f);
	return ok;
}

// Re-opens one baked file and checks everything the runtime depends on:
// RGBA8 UNORM format, expected level count, linear transfer function and
// base-mip bytes identical to a fresh generation.
bool VerifyCookieKtx2(const std::filesystem::path& file, int pattern,
	uint32_t expectedLevels, std::string& err)
{
	ktxTexture2* tex = nullptr;
	KTX_error_code res = ktxTexture2_CreateFromNamedFile(
		file.string().c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &tex);
	if (res != KTX_SUCCESS || !tex) {
		err = std::string("open: ") + ktxErrorString(res);
		return false;
	}
	KtxGuard guard(tex);

	if (ktxTexture2_GetVkFormat(tex) != VK_FORMAT_R8G8B8A8_UNORM) {
		err = "vkFormat is not VK_FORMAT_R8G8B8A8_UNORM";
		return false;
	}
	if (tex->numLevels != expectedLevels) {
		err = "unexpected level count " + std::to_string(tex->numLevels)
			+ " (want " + std::to_string(expectedLevels) + ")";
		return false;
	}
	if (ktxTexture2_GetTransferFunction_e(tex) == KHR_DF_TRANSFER_SRGB) {
		err = "transfer function is sRGB (cookies must stay linear)";
		return false;
	}

	ktx_size_t offset = 0;
	res = ktxTexture2_GetImageOffset(tex, 0, 0, 0, &offset);
	if (res != KTX_SUCCESS) {
		err = std::string("ktxTexture2_GetImageOffset: ") + ktxErrorString(res);
		return false;
	}
	const std::vector<uint8_t> fresh = smvk::GenerateCookiePixels(pattern);
	const ktx_uint8_t* base = ktxTexture_GetData(ktxTexture(tex)) + offset;
	if (std::memcmp(base, fresh.data(), fresh.size()) != 0) {
		err = "base-mip bytes differ from a fresh generation";
		return false;
	}
	return true;
}

} // namespace

int main(int argc, char** argv) {
	Args args;
	if (!ParseArgs(argc, argv, args)) {
		Usage();
		return 2;
	}

	const uint32_t S = args.size;
	uint32_t expectedLevels = 1;
	if (args.mips) {
		uint32_t w = S;
		while (w > 1) {
			w = std::max(1u, w / 2);
			++expectedLevels;
		}
	}

	struct Baked {
		int pattern;
		std::string vfsPath;
		std::filesystem::path file;
		size_t bytes = 0;
	};
	std::vector<Baked> baked;
	baked.reserve(smvk::kCookiePatternCount);

	for (int p = 0; p < smvk::kCookiePatternCount; ++p) {
		const std::string vfsPath = smvk::CookieVfsPath(p);
		const std::filesystem::path file = HostPath(args.outDir, vfsPath);

		std::vector<std::vector<uint8_t>> mips;
		mips.push_back(smvk::GenerateCookiePixels(p));
		if (args.mips)
			mips = BuildMipChain(mips[0], S, S);

		std::string err;
		if (!WriteCookieKtx2(file, mips, S, S, err)) {
			std::cerr << "FAILED " << vfsPath << ": " << err << std::endl;
			return 1;
		}
		const auto sizeOnDisk = std::filesystem::file_size(file);
		baked.push_back(Baked{ p, vfsPath, file, sizeOnDisk });
		std::cout << "baked " << vfsPath << "  " << S << "x" << S
			<< "  levels=" << mips.size()
			<< "  " << sizeOnDisk << " B" << std::endl;
	}

	// Optional texture-only pak over the baked bytes (capability demo:
	// mount the pak at runtime and PakBackend serves the same VFS paths).
	if (!args.pak.empty()) {
		smvk::PakWriter writer;
		for (const Baked& b : baked) {
			auto bytes = ReadFileBytes(b.file);
			if (!bytes) {
				std::cerr << "FAILED pak read-back: " << b.file << std::endl;
				return 1;
			}
			std::vector<uint8_t> payload = smvk::BuildTexturePayload(
				b.vfsPath, -1, -1, -1, -1, std::span<const uint8_t>(*bytes));
			if (!writer.AddBlob(smvk::PakAssetKind::Texture, b.vfsPath,
				std::move(payload))) {
				std::cerr << "FAILED pak AddBlob: " << b.vfsPath << std::endl;
				return 1;
			}
		}
		const std::vector<uint8_t> pak = writer.Finalize();
		if (!WriteFileBytes(args.pak, pak)) {
			std::cerr << "FAILED writing pak: " << args.pak << std::endl;
			return 1;
		}
		std::cout << "paked " << args.pak << "  " << pak.size()
			<< " B (" << baked.size() << " textures)" << std::endl;
	}

	if (args.verify) {
		size_t failures = 0;

		// 1. Written files decode + round-trip through the production
		//    KTX read path (same calls as LoadKtxTextureMemory).
		for (const Baked& b : baked) {
			std::string err;
			if (!VerifyCookieKtx2(b.file, b.pattern, expectedLevels, err)) {
				std::cerr << "verify FAILED " << b.vfsPath << ": "
					<< err << std::endl;
				++failures;
			}
		}

		// 2. Loose serving: the output dir mounted as the app mounts
		//    engine_resources must serve every cookie VFS path.
		{
			smvk::OverlayVfs vfs;
			vfs.Mount(std::make_shared<smvk::LooseBackend>(args.outDir), 0, "baked");
			for (const Baked& b : baked) {
				auto bytes = vfs.TryRead(b.vfsPath);
				if (!bytes) {
					std::cerr << "verify FAILED loose read: " << b.vfsPath << std::endl;
					++failures;
					continue;
				}
				auto disk = ReadFileBytes(b.file);
				if (!disk || disk->size() != bytes->size() ||
					std::memcmp(disk->data(), bytes->data(), bytes->size()) != 0) {
					std::cerr << "verify FAILED loose bytes differ: "
						<< b.vfsPath << std::endl;
					++failures;
				}
			}
		}

		// 3. Pak serving: PakBackend must serve the same paths from the pak.
		if (!args.pak.empty()) {
			auto image = ReadFileBytes(args.pak);
			std::string pakErr;
			auto pakImage = std::make_shared<const std::vector<uint8_t>>(
				image ? std::move(*image) : std::vector<uint8_t>{});
			auto backend = smvk::PakBackend::Open(pakImage, args.pak, pakErr);
			if (!backend) {
				std::cerr << "verify FAILED pak open: " << pakErr << std::endl;
				++failures;
			}
			else {
				for (const Baked& b : baked) {
					auto bytes = backend->TryRead(b.vfsPath);
					if (!bytes) {
						std::cerr << "verify FAILED pak read: "
							<< b.vfsPath << std::endl;
						++failures;
					}
				}
			}
		}

		if (failures) {
			std::cerr << "verify: " << failures << " failure(s)" << std::endl;
			return 1;
		}
		std::cout << "verify: " << baked.size() << " file(s) OK"
			<< (args.pak.empty() ? "" : " + pak OK") << std::endl;
	}

	return 0;
}
