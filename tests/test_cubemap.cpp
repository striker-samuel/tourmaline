// cubemap_tests: CPU-side regression tests for cubemap support.
// No GPU required:
//  - FaceUvToDirection face layout (Engine/Renderer/cubemap.h, shared with
//    the CubemapBaker): face centers must map to the face axes and every
//    texel direction must be non-degenerate + dominated by its face axis
//    (what TextureCube face selection expects).
//  - GenerateCubemapFacePixels sizes per face.
//  - CubemapVfsPath naming.
//  - RHI helpers: IsCubemapLayerCount + MakeCubemapCreateInfo (smvk.h).

#include <cmath>
#include <iostream>

#include <Engine/Renderer/cubemap.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) {
		std::cout << "PASS " << name << "\n";
	}
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

void TestFaceCenters() {
	// Face centers (u=v=0.5) must point along the face axes.
	struct Expect {
		smvk::CubemapFace face;
		float x, y, z;
	};
	const Expect kExpects[6] = {
		{ smvk::CubemapFace::Px, 1, 0, 0 },
		{ smvk::CubemapFace::Nx, -1, 0, 0 },
		{ smvk::CubemapFace::Py, 0, 1, 0 },
		{ smvk::CubemapFace::Ny, 0, -1, 0 },
		{ smvk::CubemapFace::Pz, 0, 0, 1 },
		{ smvk::CubemapFace::Nz, 0, 0, -1 },
	};
	for (const Expect& e : kExpects) {
		const smvk::CubemapDir d = smvk::FaceUvToDirection(e.face, 0.5f, 0.5f);
		const bool ok = std::abs(d.x - e.x) < 1e-6f
			&& std::abs(d.y - e.y) < 1e-6f
			&& std::abs(d.z - e.z) < 1e-6f;
		char name[64];
		std::snprintf(name, sizeof(name), "cubemap.facecenter.%s",
			smvk::CubemapFaceName(e.face));
		Check(ok, name);
	}
}

void TestFaceDominance() {
	// Every texel of face F must have |axis_F| >= the other components
	// (major-axis face selection maps it back to F) and be non-degenerate.
	for (int f = 0; f < 6; ++f) {
		bool ok = true;
		for (int yi = 0; yi <= 8 && ok; ++yi) {
			for (int xi = 0; xi <= 8 && ok; ++xi) {
				const float u = (float(xi) + 0.5f) / 9.0f;
				const float v = (float(yi) + 0.5f) / 9.0f;
				const smvk::CubemapDir d = smvk::FaceUvToDirection(
					smvk::CubemapFace(f), u, v);
				const float ax = std::abs(d.x), ay = std::abs(d.y), az = std::abs(d.z);
				if (!(ax + ay + az > 1e-6f))
					ok = false;
				if (f == 0) ok = ok && (ax >= ay && ax >= az && d.x > 0);
				if (f == 1) ok = ok && (ax >= ay && ax >= az && d.x < 0);
				if (f == 2) ok = ok && (ay >= ax && ay >= az && d.y > 0);
				if (f == 3) ok = ok && (ay >= ax && ay >= az && d.y < 0);
				if (f == 4) ok = ok && (az >= ax && az >= ay && d.z > 0);
				if (f == 5) ok = ok && (az >= ax && az >= ay && d.z < 0);
			}
		}
		char name[64];
		std::snprintf(name, sizeof(name), "cubemap.facedominance.%s",
			smvk::CubemapFaceName(smvk::CubemapFace(f)));
		Check(ok, name);
	}
}

void TestProceduralFaces() {
	for (int f = 0; f < 6; ++f) {
		auto px = smvk::GenerateCubemapFacePixels(smvk::CubemapFace(f), 16);
		char name[64];
		std::snprintf(name, sizeof(name), "cubemap.fallback-size.%s",
			smvk::CubemapFaceName(smvk::CubemapFace(f)));
		Check(px.size() == size_t(16) * 16 * 4, name);
	}
	// Faces must differ (per-face tints) so order bugs are visible.
	auto px0 = smvk::GenerateCubemapFacePixels(smvk::CubemapFace::Px, 8);
	auto px1 = smvk::GenerateCubemapFacePixels(smvk::CubemapFace::Nx, 8);
	Check(px0 != px1, "cubemap.fallback-faces-differ");
}

void TestVfsPath() {
	Check(smvk::CubemapVfsPath("sky") == "textures/cubemaps/sky.ktx2",
		"cubemap.vfspath");
}

void TestRhiHelpers() {
	Check(smvk::IsCubemapLayerCount(6), "cubemap.rhi.layers6");
	Check(smvk::IsCubemapLayerCount(12), "cubemap.rhi.layers12");
	Check(!smvk::IsCubemapLayerCount(1), "cubemap.rhi.layers1");
	Check(!smvk::IsCubemapLayerCount(5), "cubemap.rhi.layers5");
	const smvk::ImageCreateInfo ci =
		smvk::MakeCubemapCreateInfo(256, smvk::Format::RGBA8_UNorm, 9, 1);
	Check(ci.isCubemap, "cubemap.rhi.make-flag");
	Check(ci.arrayLayers == 6, "cubemap.rhi.make-layers");
	Check(ci.extent.width == 256 && ci.extent.height == 256, "cubemap.rhi.make-extent");
	Check(ci.type == smvk::ImageType::Image2D, "cubemap.rhi.make-type");
	const smvk::SamplerCreateInfo sci = smvk::CubemapSampler();
	Check(sci.addressU == smvk::AddressMode::ClampToEdge
		&& sci.addressV == smvk::AddressMode::ClampToEdge
		&& sci.addressW == smvk::AddressMode::ClampToEdge,
		"cubemap.rhi.sampler-clamp");
}

} // namespace

int main() {
	TestFaceCenters();
	TestFaceDominance();
	TestProceduralFaces();
	TestVfsPath();
	TestRhiHelpers();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
