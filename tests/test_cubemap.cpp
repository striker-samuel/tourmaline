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
#include <Engine/Renderer/cubemap_prefilter.h>

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
	Check(smvk::CubemapManifestVfsPath("sponza") == "textures/cubemaps/sponza.cubemanifest",
		"cubemap.manifest-vfspath");
}

void TestManifestRoundtrip() {
	// Serialize -> parse must recover every row (cook -> runtime path).
	std::vector<smvk::CubemapManifestEntry> in(2);
	in[0].vfsPath = "textures/cubemaps/sponza_0.ktx2";
	in[0].position = glm::vec3(1.5f, 2.0f, -3.25f);
	in[0].influenceRadius = 6.0f;
	in[0].fadeRadius = 3.0f;
	in[1].vfsPath = "textures/cubemaps/sponza_1.ktx2";
	in[1].position = glm::vec3(-4.0f, 2.0f, 8.0f);
	in[1].influenceRadius = 6.0f;
	in[1].fadeRadius = 3.0f;
	const std::string text = smvk::CubemapManifestSerialize(in);
	std::vector<smvk::CubemapManifestEntry> out;
	std::string err;
	Check(smvk::CubemapManifestParse(text, out, err), "cubemap.manifest-parse");
	bool ok = out.size() == 2 && err.empty();
	for (size_t i = 0; ok && i < 2; ++i) {
		ok = out[i].vfsPath == in[i].vfsPath
			&& std::abs(out[i].position.x - in[i].position.x) < 1e-4f
			&& std::abs(out[i].position.y - in[i].position.y) < 1e-4f
			&& std::abs(out[i].position.z - in[i].position.z) < 1e-4f
			&& std::abs(out[i].influenceRadius - in[i].influenceRadius) < 1e-4f
			&& std::abs(out[i].fadeRadius - in[i].fadeRadius) < 1e-4f;
	}
	Check(ok, "cubemap.manifest-roundtrip");
	// Malformed lines + bad radii must be rejected, never half-loaded.
	std::vector<smvk::CubemapManifestEntry> bad;
	Check(!smvk::CubemapManifestParse("not-a-row\n", bad, err)
		&& bad.empty(), "cubemap.manifest-reject-garbage");
	Check(!smvk::CubemapManifestParse(
		"textures/cubemaps/a.ktx2 0 0 0 0 1\n", bad, err)
		&& bad.empty(), "cubemap.manifest-reject-zero-influence");
	Check(!smvk::CubemapManifestParse("", bad, err)
		&& bad.empty(), "cubemap.manifest-reject-empty");
}

void TestDirectionRoundtrip() {
	// FaceUvToDirection -> DirectionToFaceUv must recover the face and
	// the uv (what the prefilter's bilinear source fetch depends on).
	bool ok = true;
	for (int f = 0; f < 6; ++f) {
		for (int yi = 0; yi <= 8 && ok; ++yi) {
			for (int xi = 0; xi <= 8 && ok; ++xi) {
				const float u = (float(xi) + 0.5f) / 9.0f;
				const float v = (float(yi) + 0.5f) / 9.0f;
				const smvk::CubemapDir d = smvk::FaceUvToDirection(
					smvk::CubemapFace(f), u, v);
				smvk::CubemapFace rf = smvk::CubemapFace::Px;
				float ru = 0.0f, rv = 0.0f;
				if (!smvk::DirectionToFaceUv(glm::vec3(d.x, d.y, d.z), rf, ru, rv))
					ok = false;
				if (int(rf) != f)
					ok = false;
				if (std::abs(ru - u) > 1e-4f || std::abs(rv - v) > 1e-4f)
					ok = false;
			}
		}
	}
	Check(ok, "cubemap.dir-roundtrip");
	{
		smvk::CubemapFace rf = smvk::CubemapFace::Px;
		float ru = 0.0f, rv = 0.0f;
		Check(!smvk::DirectionToFaceUv(glm::vec3(0.0f), rf, ru, rv),
			"cubemap.dir-degenerate-guarded");
	}
}

void TestPrefilterUniform() {
	// Uniform capture: every prefiltered level must reproduce the
	// constant (the NdotL-weighted average of a constant field), and
	// mip 0 must be an exact copy of the base.
	const uint32_t S = 8;
	smvk::CubemapBaseFaces base;
	for (int f = 0; f < 6; ++f) {
		base[f].assign(size_t(S) * S * 4, 0.0f);
		for (size_t i = 0; i < size_t(S) * S; ++i) {
			base[f][i * 4 + 0] = 2.0f;
			base[f][i * 4 + 1] = 1.0f;
			base[f][i * 4 + 2] = 0.5f;
			base[f][i * 4 + 3] = 1.0f;
		}
	}
	smvk::CubemapMipChains chains;
	smvk::PrefilterCubemapGGX(base, S, 0, 32, chains);
	Check(chains.size() == 6, "cubemap.prefilter.faces");
	Check(chains[0].size() == smvk::CubemapFullMipCount(S), "cubemap.prefilter.levels");
	bool ok = true;
	for (int f = 0; f < 6 && ok; ++f) {
		uint32_t w = S;
		for (size_t m = 0; m < chains[f].size() && ok; ++m) {
			if (chains[f][m].size() != size_t(w) * w * 4)
				ok = false;
			for (size_t i = 0; i < size_t(w) * w && ok; ++i) {
				const float* p = &chains[f][m][i * 4];
				if (std::abs(p[0] - 2.0f) > 1e-3f
					|| std::abs(p[1] - 1.0f) > 1e-3f
					|| std::abs(p[2] - 0.5f) > 1e-3f
					|| std::abs(p[3] - 1.0f) > 1e-6f)
					ok = false;
			}
			w = std::max(1u, w / 2);
		}
	}
	Check(ok, "cubemap.prefilter.uniform-exact");
}

void TestPrefilterDeterminism() {
	// Checker capture: two runs must produce bit-identical chains
	// (bake determinism across thread schedules) and the roughest mip
	// must differ from the sharp one (convolution actually spreads).
	const uint32_t S = 8;
	smvk::CubemapBaseFaces base;
	for (int f = 0; f < 6; ++f) {
		base[f].assign(size_t(S) * S * 4, 0.0f);
		for (uint32_t y = 0; y < S; ++y)
			for (uint32_t x = 0; x < S; ++x) {
				float* p = &base[f][(size_t(y) * S + x) * 4];
				const float c = ((x + y + f) % 2 == 0) ? 4.0f : 0.25f;
				p[0] = p[1] = p[2] = c;
				p[3] = 1.0f;
			}
	}
	smvk::CubemapMipChains a, b;
	smvk::PrefilterCubemapGGX(base, S, 0, 64, a);
	smvk::PrefilterCubemapGGX(base, S, 0, 64, b);
	bool same = a.size() == b.size();
	for (size_t f = 0; same && f < a.size(); ++f) {
		if (a[f].size() != b[f].size())
			same = false;
		for (size_t m = 0; same && m < a[f].size(); ++m)
			same = a[f][m] == b[f][m];
	}
	Check(same, "cubemap.prefilter.deterministic");
	// Sharp vs roughest: the checker contrast must wash out.
	double sharpVar = 0.0, roughVar = 0.0, sharpMean = 0.0, roughMean = 0.0;
	{
		const auto& s0 = a[0][0];
		size_t n0 = s0.size() / 4;
		for (size_t i = 0; i < n0; ++i)
			sharpMean += s0[i * 4];
		sharpMean /= double(n0);
		for (size_t i = 0; i < n0; ++i) {
			const double d = s0[i * 4] - sharpMean;
			sharpVar += d * d;
		}
		sharpVar /= double(n0);
		const auto& rL = a[0].back();
		size_t nL = rL.size() / 4;
		for (size_t i = 0; i < nL; ++i)
			roughMean += rL[i * 4];
		roughMean /= double(std::max<size_t>(nL, 1));
		for (size_t i = 0; i < nL; ++i) {
			const double d = rL[i * 4] - roughMean;
			roughVar += d * d;
		}
		roughVar /= double(std::max<size_t>(nL, 1));
	}
	Check(roughVar < sharpVar, "cubemap.prefilter.rough-blurs");
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
	TestManifestRoundtrip();
	TestDirectionRoundtrip();
	TestPrefilterUniform();
	TestPrefilterDeterminism();
	TestRhiHelpers();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
