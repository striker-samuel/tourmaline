// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// ContentRegistryTests: the CONSOLIDATED CONTENT ARCHITECTURE's first
// acceptance battery (Engine/Asset/content_registry.h — the ONE
// content-resolution authority). No GPU, no window: hand-built paks
// + a hand-written .tapp drive the registry end to end.
//
// THE CONVERGENCE CONTRACT (the reason this class exists): one
// texture, three providers (base pak + mod pak + loose dev override)
// — the compiled ASSET ID and the SERIALIZED PATH must resolve to the
// SAME effective provider, whatever the precedence state is, and
// disabling a layer must reveal the next one beneath it without
// breaking either lookup form. Both byte lanes are covered (the lazy
// PAGER lane first, then the adopted BACKEND lane — same bytes).

#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/content_registry.h>
#include <Engine/Asset/pak.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

namespace fs = std::filesystem;
namespace ta = tour::asset;

// The one ktx payload per provider (distinguished by a trailing tag
// byte — the envelope carries whatever we hand it, the comparisons
// are byte-exact).
std::vector<uint8_t> Ktx(uint8_t tag) {
	std::vector<uint8_t> b = { 0xAB, 'K', 'T', 'X', ' ', '2', '0',
		0x0D, 0x0A, 1, 2, 3, 4, 5 };
	b.push_back(tag);
	return b;
}

void WriteFile(const fs::path& p, const void* data, size_t size) {
	fs::create_directories(p.parent_path());
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	f.write(static_cast<const char*>(data), std::streamsize(size));
}

void WritePak(const fs::path& p, std::vector<uint8_t> image) {
	WriteFile(p, image.data(), image.size());
}

const std::string kStone = "content/models/s/stone.png";
const std::string kBell = "content/audio/bell.wav";

} // namespace

int main() {
	const fs::path tmp = fs::temp_directory_path() / "tourmaline_cr";
	std::error_code ec;
	fs::remove_all(tmp, ec);
	fs::create_directories(tmp, ec);

	// ---- the content universe -----------------------------------------
	// base.pak: stone (BASE) + bell (the sound lane's proof).
	// mod.pak: stone (MOD) — the override pak.
	// dev tree: stone as a LOOSE file (DEV) — the loose override.
	// broken.pak: garbage bytes — the corrupt-owner diagnostic.
	const std::vector<uint8_t> ktxBase = Ktx(0x01);
	const std::vector<uint8_t> ktxMod = Ktx(0x02);
	const std::vector<uint8_t> ktxDev = Ktx(0x03);
	const std::vector<uint8_t> wav = { 'R', 'I', 'F', 'F', 'b', 'e',
		'l', 'l' };
	{
		smvk::PakWriter w;
		w.AddBlob(smvk::PakAssetKind::Texture, kStone,
			smvk::BuildTexturePayload(kStone, -1, -1, -1, -1,
				std::span<const uint8_t>(ktxBase)));
		w.AddBlob(smvk::PakAssetKind::Sound, kBell, wav);
		WritePak(tmp / "base.pak", w.Finalize());
	}
	{
		smvk::PakWriter w;
		w.AddBlob(smvk::PakAssetKind::Texture, kStone,
			smvk::BuildTexturePayload(kStone, -1, -1, -1, -1,
				std::span<const uint8_t>(ktxMod)));
		WritePak(tmp / "mod.pak", w.Finalize());
	}
	{
		// The loose dev override: the RAW file bytes at the logical
		// path (a loose tree carries source files, not envelopes).
		WriteFile(tmp / "dev" / fs::path(kStone),
			ktxDev.data(), ktxDev.size());
	}
	{
		const std::vector<uint8_t> garbage(16, 0xEEu);
		WriteFile(tmp / "broken.pak", garbage.data(), garbage.size());
	}
	// The manifest (the identity source — [ids] rows over the paks).
	const fs::path tapp = tmp / "game.tapp";
	{
		std::ofstream f(tapp, std::ios::binary | std::ios::trunc);
		f << "# Tourmaline Runtime Project\n"
			<< "appmanifest 1\n"
			<< "entry base.pak Church\n"
			<< "\n[paks]\nbase.pak\nbroken.pak\n"
			<< "\n[ids]\n"
			<< "0x1000 scene base.pak content/scenes/church.tour Church\n"
			<< "0x2000 texture base.pak " << kStone << "\n"
			<< "0x2500 texture broken.pak content/models/ghost.png\n"
			<< "0x3000 mesh base.pak content/models/tree.geometry\n"
			<< "0x4000 sound base.pak " << kBell << "\n";
	}

	// ---- adoption -------------------------------------------------------
	smvk::AppManifest manifest;
	std::string mErr;
	if (!smvk::LoadAppManifest(tapp.generic_string(), manifest, mErr)) {
		Check(false, "cr.manifest-load");
		return (g_failures != 0) ? EXIT_FAILURE : EXIT_SUCCESS;
	}
	ta::ContentRegistry reg;
	std::string err;
	Check(reg.AdoptManifest(manifest,
			(tapp.parent_path()).generic_string(), err) && err.empty()
		&& reg.RecordCount() == 5,
		"cr.manifest-adopt");
	{
		// The reverse lookup + the join fields survive adoption.
		const ta::ContentRecord* stone = reg.Find(0x2000);
		const ta::ContentRecord* church = reg.Find(0x1000);
		Check(stone && stone->kind == smvk::kCatalogKindTexture
			&& stone->pak == "base.pak",
			"cr.find-by-id");
		Check(reg.Find(smvk::kCatalogKindTexture, kStone) == stone,
			"cr.find-by-path-joins-id");
		Check(church && church->sceneName == "Church"
			&& church->kind == smvk::kCatalogKindScene,
			"cr.scene-record-fields");
		Check(reg.Find(0x9999) == nullptr, "cr.unknown-id-null");
	}
	// The loud adoption refusals (identity invariants).
	{
		smvk::AppManifest bad = manifest;
		bad.assets[1].pak = "unlisted.pak"; // a row over an unlisted pak
		std::string aErr;
		Check(!reg.AdoptManifest(bad,
				(tapp.parent_path()).generic_string(), aErr)
			&& aErr.find("unlisted") != std::string::npos,
			"cr.unlisted-pak-refused");
		// A duplicate identity (the same id twice) never adopts.
		smvk::AppManifest dup = manifest;
		dup.assets.push_back(manifest.assets[1]);
		std::string dErr;
		Check(!reg.AdoptManifest(dup,
				(tapp.parent_path()).generic_string(), dErr)
			&& dErr.find("twice") != std::string::npos,
			"cr.duplicate-id-refused");
		// A duplicate kind+path never adopts (one identity per pair).
		smvk::AppManifest dup2 = manifest;
		smvk::AppManifestAsset ghost = manifest.assets[1];
		ghost.id = 0x2100;
		dup2.assets.push_back(ghost);
		std::string d2Err;
		Check(!reg.AdoptManifest(dup2,
				(tapp.parent_path()).generic_string(), d2Err)
			&& d2Err.find("twice") != std::string::npos,
			"cr.duplicate-path-refused");
		// The failed adoptions LEFT NO STATE: re-adopt the good
		// manifest for the rest of the battery.
		std::string rErr;
		Check(reg.AdoptManifest(manifest,
				(tapp.parent_path()).generic_string(), rErr),
			"cr.adopt-recovers");
	}

	// ---- overrides ------------------------------------------------------
	// Precedence by registration order: dev (loose) FIRST — it wins;
	// mod second; base beneath both.
	std::string oErr;
	Check(reg.AddLooseOverride("dev",
			(tmp / "dev").generic_string(), oErr) && oErr.empty(),
		"cr.add-loose-override");
	Check(reg.AddPakOverride("mod",
			(tmp / "mod.pak").generic_string(), oErr) && oErr.empty(),
		"cr.add-pak-override");
	{
		// The name-uniqueness gate.
		std::string dErr;
		Check(!reg.AddPakOverride("mod", "whatever.pak", dErr)
			&& dErr.find("already exists") != std::string::npos,
			"cr.duplicate-provider-name-refused");
	}

	// ---- THE CONVERGENCE CONTRACT (the headline) ------------------------
	// Both forms see the SAME effective provider — the loose override
	// (registered first) wins, the id form and the path form AGREE,
	// and the pager/backend split never shows (base bytes only once
	// the overrides above it are gone).
	{
		std::string idErr;
		auto idBytes = reg.Read(0x2000, idErr);
		std::string pErr;
		auto pathBytes = reg.Read(smvk::kCatalogKindTexture, kStone,
			pErr);
		Check(idBytes && *idBytes == ktxDev && idErr.empty(),
			"cr.id-read-sees-loose-override");
		Check(pathBytes && *pathBytes == ktxDev && pErr.empty(),
			"cr.path-read-sees-loose-override");
		Check(idBytes && pathBytes && *idBytes == *pathBytes,
			"cr.id-and-path-agree");
	}
	// The resolution carries the provider (the diagnostics form).
	{
		std::string rErr;
		ta::ContentRegistry::Resolution r = reg.Resolve(0x2000, rErr);
		Check(r.record && r.provider && r.provider->name == "dev"
			&& r.providerIsOverride,
			"cr.resolve-names-winner");
		const uint64_t gen = reg.ProviderGeneration("dev");
		Check(gen == 1 && reg.ProviderEnabled("dev"),
			"cr.provider-generation-baseline");
	}
	// The shadowed list: EVERY provider serving the path, precedence
	// order (the winner first) — the collision report.
	{
		std::vector<std::string> provs = reg.ProvidersFor(
			smvk::kCatalogKindTexture, kStone);
		Check(provs.size() == 3 && provs[0] == "dev"
			&& provs[1] == "mod" && provs[2] == "base.pak",
			"cr.providers-for-precedence");
	}

	// ---- the mod ladder (toggles reveal the layer beneath) --------------
	// Disable the loose override -> the MOD pak wins (both forms).
	Check(reg.SetProviderEnabled("dev", false), "cr.disable-loose");
	{
		std::string idErr;
		auto idBytes = reg.Read(0x2000, idErr);
		std::string pErr;
		auto pathBytes = reg.Read(smvk::kCatalogKindTexture, kStone,
			pErr);
		Check(idBytes && *idBytes == ktxMod,
			"cr.mod-wins-after-loose-disabled");
		Check(pathBytes && *pathBytes == ktxMod,
			"cr.mod-wins-path-form");
		Check(idBytes && pathBytes && *idBytes == *pathBytes,
			"cr.id-and-path-agree-mod");
	}
	// A toggle bumps the generation (a cached resolution's
	// invalidation signal); a same-value toggle is a no-op.
	Check(reg.ProviderGeneration("dev") == 2
		&& !reg.SetProviderEnabled("dev", false),
		"cr.toggle-bumps-generation");
	// Disable the mod -> the BASE pak wins. NOTE: base.pak has NO
	// adopted backend yet — this read rides THE PAGER LANE (the SMP8
	// light mount + one positional page read; no whole-image preload).
	Check(reg.SetProviderEnabled("mod", false), "cr.disable-mod");
	{
		std::string idErr;
		auto idBytes = reg.Read(0x2000, idErr);
		Check(idBytes && *idBytes == ktxBase && idErr.empty(),
			"cr.base-wins-via-pager-lane");
	}
	// The adopted BACKEND lane: open the whole image, adopt it —
	// same bytes, and the provider's pager is released (the worker
	// + handle do not outlive the adoption).
	{
		std::vector<uint8_t> image;
		std::ifstream f(tmp / "base.pak", std::ios::binary);
		f.seekg(0, std::ios::end);
		image.resize(size_t(f.tellg()));
		f.seekg(0);
		f.read(reinterpret_cast<char*>(image.data()),
			std::streamsize(image.size()));
		auto img = std::make_shared<const std::vector<uint8_t>>(
			std::move(image));
		std::string bErr;
		auto backend = smvk::PakBackend::Open(img, "base.pak", bErr);
		Check(bool(backend), "cr.backend-open");
		Check(reg.AdoptBackend("base.pak", backend),
			"cr.adopt-backend");
		Check(reg.HasBackend("base.pak"), "cr.has-backend");
		std::string rErr;
		auto bytes = reg.Read(0x2000, rErr);
		Check(bytes && *bytes == ktxBase && rErr.empty(),
			"cr.base-wins-via-backend-lane");
		// A path-only read through the same selection (a path with
		// NO catalog identity — the loose/mod files of the world).
		auto raw = reg.ReadPath(kStone, rErr);
		Check(raw && *raw == ktxBase && rErr.empty(),
			"cr.readpath-sees-winner");
		std::string nErr;
		Check(!reg.ReadPath("content/nothing.png", nErr)
			&& nErr.find("no provider serves") != std::string::npos,
			"cr.readpath-unknown-refused");
	}
	// Re-enable everything: the ladder stands again (the loose
	// override's generation moved on — the resolution is FRESH).
	Check(reg.SetProviderEnabled("dev", true)
		&& reg.SetProviderEnabled("mod", true),
		"cr.re-enable");
	{
		std::string rErr;
		auto bytes = reg.Read(0x2000, rErr);
		Check(bytes && *bytes == ktxDev, "cr.loose-wins-again");
	}

	// ---- kinds ---------------------------------------------------------
	{
		// The sound lane: id and path agree, raw .wav bytes.
		std::string idErr;
		auto idBytes = reg.Read(0x4000, idErr);
		std::string pErr;
		auto pathBytes = reg.Read(smvk::kCatalogKindSound, kBell,
			pErr);
		Check(idBytes && *idBytes == wav && idErr.empty(),
			"cr.sound-by-id");
		Check(pathBytes && *pathBytes == wav && pErr.empty(),
			"cr.sound-by-path");
		// The identity-addressed kinds refuse byte reads loudly.
		std::string sErr;
		Check(!reg.Read(0x1000, sErr)
			&& sErr.find("identity") != std::string::npos,
			"cr.scene-bytes-refused");
		std::string mErr2;
		Check(!reg.Read(0x3000, mErr2)
			&& mErr2.find("identity") != std::string::npos,
			"cr.mesh-bytes-refused");
		// A scene/mesh RESOLUTION answers the owning provider (the
		// identity join stands — no provider selection applies).
		std::string rErr;
		ta::ContentRegistry::Resolution r = reg.Resolve(0x1000, rErr);
		Check(r.record && r.provider
			&& r.provider->name == "base.pak" && !r.providerIsOverride,
			"cr.scene-resolves-owner");
		// A kind with no record at that path resolves path-only
		// (no refusal — a texture at a sound's path is the sound
		// record's own join, this lookup simply has no row).
		std::string kErr;
		Check(!reg.Read(smvk::kCatalogKindShader,
				"content/shaders/x.spv", kErr)
			&& kErr.find("no provider serves") != std::string::npos,
			"cr.no-record-path-only-refused");
		// An unknown id refuses loudly (the version-skew shape).
		std::string uErr;
		Check(!reg.Read(0x9999, uErr)
			&& uErr.find("version skew") != std::string::npos,
			"cr.unknown-id-refused");
	}

	// ---- the corrupt owner ----------------------------------------------
	// broken.pak (garbage bytes) owns a texture row: resolution
	// reports THE PAK'S OWN FAILURE — actionable, never a vague miss
	// and never a silent skip.
	{
		std::string cErr;
		Check(!reg.Read(0x2500, cErr)
			&& cErr.find("broken.pak") != std::string::npos
			&& cErr.find("failed") != std::string::npos,
			"cr.corrupt-owner-named");
	}
	// ---- a disabled base pak answers nothing -----------------------------
	{
		Check(reg.SetProviderEnabled("broken.pak", false),
			"cr.disable-broken-base");
		std::string dErr;
		Check(!reg.Read(0x2500, dErr) && !dErr.empty(),
			"cr.disabled-base-refused");
		reg.SetProviderEnabled("broken.pak", true);
	}

	// ---- re-adoption (the between-launches rebuild) ----------------------
	// A fresh manifest adoption REPLACES the state: overrides do not
	// carry forward (a new launch builds its provider stack anew).
	{
		std::string rErr;
		Check(reg.AdoptManifest(manifest,
				(tapp.parent_path()).generic_string(), rErr),
			"cr.readopt");
		auto infos = reg.ProviderInfos();
		Check(infos.size() == 2 && infos[0].base && infos[1].base
			&& infos[0].enabled && infos[1].enabled,
			"cr.readopt-clears-overrides");
		std::string bErr;
		auto bytes = reg.Read(0x2000, bErr);
		// base.pak carries NO adopted backend after the re-adoption
		// — the pager lane answers again.
		Check(bytes && *bytes == ktxBase && bErr.empty(),
			"cr.readopt-base-reads-pager");
	}

	std::cout << "checks=" << g_checks << " failures=" << g_failures
		<< "\n";
	return g_failures != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
