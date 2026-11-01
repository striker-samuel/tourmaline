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

	// ---- THE DISCOVERY LANE (the .tapp-free form) ------------------------
	// A FRESH registry with NO manifest: the paks' own "#ids" registry
	// blobs build the record table + the provider set. This is the
	// per-pak identity metadata the .tapp retirement builds on —
	// one pak's discovery is a light mount + ONE page read, never a
	// whole image, never a manifest.
	{
		const std::vector<uint8_t> ktxDisc = Ktx(0x04);
		const fs::path discPak = tmp / "disc.pak";
		const fs::path plainPak = tmp / "plain.pak";
		const fs::path clashPak = tmp / "clash.pak";
		const fs::path badPak = tmp / "badids.pak";
		const std::vector<tour::asset::PakAssetIdRow> discRows = {
			{ 0x1000, smvk::kCatalogKindScene,
				"content/scenes/d.tour", "" },
			{ 0x2000, smvk::kCatalogKindTexture, kStone, "" },
			{ 0x4000, smvk::kCatalogKindSound, kBell, "" },
		};
		{
			smvk::PakWriter w;
			w.AddBlob(smvk::PakAssetKind::Texture, kStone,
				smvk::BuildTexturePayload(kStone, -1, -1, -1, -1,
					std::span<const uint8_t>(ktxDisc)));
			w.AddBlob(smvk::PakAssetKind::Sound, kBell, wav);
			w.AddBlob(smvk::PakAssetKind::Identity,
				tour::asset::kPakAssetIdsPath,
				tour::asset::WriteAssetIdsBlob(discRows));
			WritePak(discPak, w.Finalize());
		}
		{
			// A well-formed pak WITHOUT the identity blob (the
			// refusal case — discovery never guesses).
			smvk::PakWriter w;
			w.AddBlob(smvk::PakAssetKind::Texture, kStone,
				smvk::BuildTexturePayload(kStone, -1, -1, -1, -1,
					std::span<const uint8_t>(ktxDisc)));
			WritePak(plainPak, w.Finalize());
		}
		{
			// A pak whose identity blob claims disc.pak's texture id
			// (the deployment-conflict case).
			smvk::PakWriter w;
			const std::vector<tour::asset::PakAssetIdRow> clashRows = {
				{ 0x2000, smvk::kCatalogKindTexture,
					"content/models/s/other.png", "" },
			};
			w.AddBlob(smvk::PakAssetKind::Texture,
				"content/models/s/other.png",
				smvk::BuildTexturePayload("content/models/s/other.png",
					-1, -1, -1, -1,
					std::span<const uint8_t>(ktxDisc)));
			w.AddBlob(smvk::PakAssetKind::Identity,
				tour::asset::kPakAssetIdsPath,
				tour::asset::WriteAssetIdsBlob(clashRows));
			WritePak(clashPak, w.Finalize());
		}
		{
			// A pak whose identity blob is garbage bytes (the
			// malformed-table refusal).
			smvk::PakWriter w;
			const std::vector<uint8_t> garbage = { 1, 2, 3, 4, 5, 6 };
			w.AddBlob(smvk::PakAssetKind::Identity,
				tour::asset::kPakAssetIdsPath, garbage);
			WritePak(badPak, w.Finalize());
		}
		ta::ContentRegistry reg2;
		std::string dErr;
		Check(reg2.DiscoverPak("disc", discPak.generic_string(), dErr)
			&& dErr.empty() && reg2.RecordCount() == 3,
			"disc.discovers");
		// The convergent forms over DISCOVERED records (the pager lane
		// — the discovery's own cached pager serves; no backend).
		{
			std::string idErr;
			auto idBytes = reg2.Read(0x2000, idErr);
			std::string pErr;
			auto pathBytes = reg2.Read(smvk::kCatalogKindTexture,
				kStone, pErr);
			Check(idBytes && *idBytes == ktxDisc && idErr.empty(),
				"disc.id-read");
			Check(pathBytes && *pathBytes == ktxDisc && pErr.empty(),
				"disc.path-read");
			Check(idBytes && pathBytes && *idBytes == *pathBytes,
				"disc.forms-agree");
		}
		{
			// The record fields + the scene join's shape (an empty
			// name rides the single-scene inference at load time).
			const ta::ContentRecord* scene = reg2.Find(0x1000);
			Check(scene && scene->kind == smvk::kCatalogKindScene
				&& scene->pak == "disc" && scene->sceneName.empty(),
				"disc.scene-record");
			std::string rErr;
			ta::ContentRegistry::Resolution r = reg2.Resolve(0x1000,
				rErr);
			Check(r.record && r.provider
				&& r.provider->name == "disc",
				"disc.scene-resolves-owner");
			std::string sErr;
			auto sound = reg2.Read(0x4000, sErr);
			Check(sound && *sound == wav, "disc.sound-read");
		}
		// Idempotent re-discovery (same name, same archive).
		{
			std::string rErr;
			Check(reg2.DiscoverPak("disc", discPak.generic_string(),
					rErr)
				&& reg2.RecordCount() == 3,
				"disc.idempotent");
		}
		// A pak without identity metadata refuses loudly (recook —
		// never a guess, never silent).
		{
			std::string pErr;
			Check(!reg2.DiscoverPak("plain", plainPak.generic_string(),
					pErr)
				&& pErr.find("identity metadata") != std::string::npos,
				"disc.no-blob-refused");
		}
		// The malformed table refuses loudly.
		{
			std::string bErr;
			Check(!reg2.DiscoverPak("badids", badPak.generic_string(),
					bErr)
				&& bErr.find("malformed") != std::string::npos,
				"disc.malformed-refused");
		}
		// The deployment conflict refuses ATOMICALLY: no records, no
		// provider (the count + the provider table are unchanged).
		{
			std::string cErr;
			Check(!reg2.DiscoverPak("clash", clashPak.generic_string(),
					cErr)
				&& cErr.find("conflict") != std::string::npos
				&& cErr.find("disc") != std::string::npos,
				"disc.conflict-refused");
			Check(reg2.RecordCount() == 3
				&& reg2.ProviderInfos().size() == 1,
				"disc.conflict-atomic");
		}
		// The LENIENT form (the viewer's legacy lane,
		// requireIdentityBlob=false): a blob-less pak joins as a
		// PATH-SERVING provider — bytes, never identity rows (a
		// pre-identity pak in a multi-pak launch still resolves
		// cross-pak paths).
		{
			ta::ContentRegistry reg3;
			std::string lErr;
			Check(reg3.DiscoverPak("plain",
					plainPak.generic_string(), lErr, false)
				&& lErr.empty() && reg3.RecordCount() == 0
				&& reg3.ProviderInfos().size() == 1,
				"disc.lenient-joins-provider");
			std::string pErr;
			auto bytes = reg3.ReadPath(kStone, pErr);
			Check(bytes && *bytes == ktxDisc && pErr.empty(),
				"disc.lenient-serves-paths");
			std::string iErr;
			Check(!reg3.Read(0x2000, iErr) && !iErr.empty(),
				"disc.lenient-no-identity-rows");
		}
		// Overrides compose over discovered base providers: the loose
		// dev tree wins for BOTH forms, the toggle reveals the
		// discovered pak beneath.
		{
			std::string oErr;
			Check(reg2.AddLooseOverride("dev",
					(tmp / "dev").generic_string(), oErr),
				"disc.override-registers");
			std::string rErr;
			auto bytes = reg2.Read(0x2000, rErr);
			Check(bytes && *bytes == ktxDev,
				"disc.override-wins");
			reg2.SetProviderEnabled("dev", false);
			std::string d2Err;
			auto base = reg2.Read(0x2000, d2Err);
			Check(base && *base == ktxDisc && d2Err.empty(),
				"disc.toggle-reveals-discovered");
		}
	}

	std::cout << "checks=" << g_checks << " failures=" << g_failures
		<< "\n";
	return g_failures != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
