// motionblur_tests: CPU-side contract test for the tile-dilated velocity
// gather + interleaved gradient noise tap jitter
// (engine/shaders/motionblur.frag.hlsl + the two compute prepasses
// motionblur_tiles.cs.hlsl / motionblur_tiledilate.cs.hlsl).
//
// What it pins down:
//   1. Tile reduction (phase A): each 16x16 tile reduces to its
//      MAX-LENGTH velocity (jitter-stripped + shutter-scaled, the same
//      transform as the gather); partial edge tiles ignore out-of-source
//      pixels; a jittered-but-still camera reduces to zero.
//   2. Tile dilation (phase B): the 5x5 neighborhood max propagates the
//      mover's velocity two tiles out, no further; the longest neighbor
//      wins.
//   3. Gather: a still background pixel inside a dilated fast tile
//      adopts the tile velocity (the CoD:AW fix for sharp halos around
//      moving silhouettes), while a pixel whose own velocity beats the
//      tile keeps its own (clamped to maxRadius).
//   4. IGN (Jimenez 2014, CoD:AW): deterministic, in [0,1), well
//      distributed; the (i + ign)/n - 0.5 tap phase keeps every tap
//      inside [-0.5, +0.5) and the box average unbiased on flat fields.
//
// The port uses point taps (the shader bilinear-samples HDR); the
// properties under test are velocity ADOPTION, clamping and tap PHASE,
// which are identical under either reconstruction.
//
// Hand-rolled runner (no test framework dependency).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
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

constexpr uint32_t kTile = 16; // kMotionBlurTilePixels
constexpr uint32_t kDilateRings = 2; // 5x5 neighborhood

struct Vec2 {
	float x = 0.0f, y = 0.0f;
	float Len() const { return std::sqrt(x * x + y * y); }
};

// Interleaved gradient noise (Jimenez 2014, "Next Generation Post
// Processing in Call of Duty: Advanced Warfare"). fp32 mirror of the
// shader's InterleavedGradientNoise.
float Ign(float px, float py) {
	const float f = 0.06711056f * px + 0.00583715f * py;
	const float frac1 = f - std::floor(f);
	const float g = 52.9829189f * frac1;
	return g - std::floor(g);
}

// Per-pixel velocity field (UV domain, like the RG16F motion texture).
struct MotionField {
	uint32_t w = 0, h = 0;
	std::vector<Vec2> v;
};

struct TileMap {
	uint32_t w = 0, h = 0;
	std::vector<Vec2> v; // one texel per tile
	const Vec2& At(uint32_t x, uint32_t y) const { return v[size_t(y) * w + x]; }
};

// Phase A mirror (motionblur_tiles.cs.hlsl): argmax over the tile's
// pixels of the jitter-stripped, shutter-scaled velocity.
TileMap ReduceTiles(const MotionField& m, Vec2 jitterDelta, float velocityScale) {
	TileMap t;
	t.w = (m.w + kTile - 1) / kTile;
	t.h = (m.h + kTile - 1) / kTile;
	t.v.assign(size_t(t.w) * t.h, Vec2{});
	for (uint32_t ty = 0; ty < t.h; ++ty)
		for (uint32_t tx = 0; tx < t.w; ++tx) {
			Vec2 best{};
			float bestLen2 = -1.0f;
			for (uint32_t y = ty * kTile; y < (ty + 1) * kTile && y < m.h; ++y)
				for (uint32_t x = tx * kTile; x < (tx + 1) * kTile && x < m.w; ++x) {
					Vec2 vel = m.v[size_t(y) * m.w + x];
					vel.x = (vel.x - jitterDelta.x) * velocityScale;
					vel.y = (vel.y - jitterDelta.y) * velocityScale;
					const float l2 = vel.x * vel.x + vel.y * vel.y;
					if (l2 > bestLen2) {
						bestLen2 = l2;
						best = vel;
					}
				}
			t.v[size_t(ty) * t.w + tx] = (bestLen2 > 0.0f) ? best : Vec2{};
		}
	return t;
}

// Phase B mirror (motionblur_tiledilate.cs.hlsl): max-length velocity
// over the 5x5 tile neighborhood (clamped at the map edges).
TileMap DilateTiles(const TileMap& raw) {
	TileMap t;
	t.w = raw.w;
	t.h = raw.h;
	t.v.assign(size_t(t.w) * t.h, Vec2{});
	for (uint32_t ty = 0; ty < t.h; ++ty)
		for (uint32_t tx = 0; tx < t.w; ++tx) {
			Vec2 best{};
			float bestLen2 = -1.0f;
			for (int32_t dy = -int32_t(kDilateRings); dy <= int32_t(kDilateRings); ++dy)
				for (int32_t dx = -int32_t(kDilateRings); dx <= int32_t(kDilateRings); ++dx) {
					const int32_t sx = std::clamp(int32_t(tx) + dx, 0, int32_t(raw.w) - 1);
					const int32_t sy = std::clamp(int32_t(ty) + dy, 0, int32_t(raw.h) - 1);
					const Vec2 v = raw.At(uint32_t(sx), uint32_t(sy));
					const float l2 = v.x * v.x + v.y * v.y;
					if (l2 > bestLen2) {
						bestLen2 = l2;
						best = v;
					}
				}
			t.v[size_t(ty) * t.w + tx] = (bestLen2 > 0.0f) ? best : Vec2{};
		}
	return t;
}

// Scalar HDR image (red channel) + the gather mirror
// (motionblur.frag.hlsl mainPS). Velocities are in PIXELS here (the
// shader divides UV by texelSize first - same numbers).
struct Image {
	uint32_t w = 0, h = 0;
	std::vector<float> px;
	float At(int x, int y) const {
		x = std::clamp(x, 0, int(w) - 1);
		y = std::clamp(y, 0, int(h) - 1);
		return px[size_t(y) * w + size_t(x)];
	}
};

// hasTile: the dilated tile-map velocity for the pixel's tile (the
// shader's tileVel lookup); the pair (hasTile=false) mirrors
// tileVelTextureIndex == UINT32_MAX.
float Gather(const Image& img, float cx, float cy, Vec2 ownPx, bool hasTile,
	Vec2 tilePx, uint32_t n, float maxRadius) {
	Vec2 v = ownPx;
	float len = v.Len();
	if (hasTile) {
		const float tileLen = tilePx.Len();
		if (tileLen > len) {
			v = tilePx;
			len = tileLen;
		}
	}
	if (len < 0.5f || n == 1 || maxRadius <= 0.0f)
		return img.At(int(std::lround(cx)), int(std::lround(cy)));
	const float maxLen = std::min(maxRadius, len);
	const float dx = v.x / std::max(len, 1e-5f) * maxLen;
	const float dy = v.y / std::max(len, 1e-5f) * maxLen;
	const float ign = Ign(cx, cy);
	float acc = 0.0f;
	for (uint32_t i = 0; i < n; ++i) {
		const float t = (float(i) + ign) / float(n) - 0.5f;
		acc += img.At(int(std::lround(cx + dx * t)), int(std::lround(cy + dy * t)));
	}
	return acc / float(n);
}

void TestTileReduce() {
	MotionField m;
	m.w = m.h = 64;
	m.v.assign(64 * 64, Vec2{});
	m.v[32 * 64 + 40] = Vec2{ 8.0f / 64.0f, 0.0f }; // one fast pixel in tile (2,2)
	m.v[32 * 64 + 20] = Vec2{ 2.0f / 64.0f, 0.0f }; // slower one in tile (1,2)
	const TileMap t = ReduceTiles(m, Vec2{}, 1.0f);
	Check(t.w == 4 && t.h == 4, "motionblur.tile-grid-extent");
	Check(std::abs(t.At(2, 2).x - 8.0f / 64.0f) < 1e-6f, "motionblur.tile-reduce-picks-mover");
	Check(t.At(1, 2).x > 1.9f / 64.0f && t.At(1, 2).x < 2.1f / 64.0f,
		"motionblur.tile-reduce-picks-slower-in-own-tile");
	Check(t.At(0, 0).Len() == 0.0f, "motionblur.tile-reduce-empty-zero");
}

void TestTileReduceJitterStripped() {
	// Jittered-but-still camera: every motion vector == jitterDelta; the
	// tile map must carry TRUE velocity (zero), matching the gather's
	// (sample - jitterDelta) * scale.
	MotionField m;
	m.w = m.h = 64;
	m.v.assign(64 * 64, Vec2{ 0.0015f, -0.0025f });
	const TileMap t = ReduceTiles(m, Vec2{ 0.0015f, -0.0025f }, 1.0f);
	Check(t.At(1, 1).Len() == 0.0f, "motionblur.tile-reduce-jitter-stripped");
}

void TestTileReducePartialEdge() {
	// 70px source: tile (4,4) covers pixels 64..69 only; the fast pixel
	// at (69,69) must land there (out-of-source lanes contribute zero).
	MotionField m;
	m.w = m.h = 70;
	m.v.assign(70 * 70, Vec2{});
	m.v[69 * 70 + 69] = Vec2{ 5.0f / 70.0f, 0.0f };
	const TileMap t = ReduceTiles(m, Vec2{}, 1.0f);
	Check(t.w == 5 && t.h == 5, "motionblur.tile-partial-edge-grid");
	Check(std::abs(t.At(4, 4).x - 5.0f / 70.0f) < 1e-6f,
		"motionblur.tile-partial-edge-fast-pixel");
	Check(t.At(3, 3).Len() == 0.0f, "motionblur.tile-partial-edge-no-bleed");
}

void TestTileDilate() {
	// 128px source = 8x8 tile grid, so the 2-ring dilation bound is
	// observable (a 4x4 grid has no tile 3 away from (2,2)).
	MotionField m;
	m.w = m.h = 128;
	m.v.assign(128 * 128, Vec2{});
	m.v[32 * 128 + 32] = Vec2{ 8.0f / 128.0f, 0.0f }; // tile (2,2) fast
	TileMap raw = ReduceTiles(m, Vec2{}, 1.0f);
	Check(raw.w == 8 && raw.h == 8, "motionblur.dilate-grid-extent");
	const TileMap d = DilateTiles(raw);
	Check(std::abs(d.At(0, 0).x - 8.0f / 128.0f) < 1e-6f,
		"motionblur.dilate-propagates-two-tiles");
	Check(d.At(5, 2).Len() == 0.0f, "motionblur.dilate-bounded-three-tiles");
	Check(d.At(2, 5).Len() == 0.0f, "motionblur.dilate-bounded-vertical");

	// Two movers in the neighborhood: the longer velocity wins.
	raw.v[size_t(1) * raw.w + 1] = Vec2{ 3.0f / 128.0f, 0.0f }; // tile (1,1)
	const TileMap d2 = DilateTiles(raw);
	Check(std::abs(d2.At(2, 2).x - 8.0f / 128.0f) < 1e-6f,
		"motionblur.dilate-picks-longest");
}

void TestGatherBackgroundAdoptsTile() {
	// Still background pixel next to a fast mover: without dilation the
	// gather is the sharp center tap; with the dilated tile velocity it
	// smears along the mover's streak (the reported sharp-halo artifact).
	Image img;
	img.w = img.h = 64;
	img.px.resize(64 * 64);
	for (uint32_t y = 0; y < 64; ++y)
		for (uint32_t x = 0; x < 64; ++x)
			img.px[size_t(y) * 64 + x] = (x >= 32) ? 1.0f : 0.0f;
	constexpr uint32_t kTaps = 12;
	constexpr float kRadius = 24.0f;
	const Vec2 tilePx{ 16.0f, 0.0f };
	// Pixel just left of the step, tile velocity carries it across.
	const float blurred = Gather(img, 31, 32, Vec2{}, true, tilePx, kTaps, kRadius);
	const float sharp = Gather(img, 31, 32, Vec2{}, false, Vec2{}, kTaps, kRadius);
	Check(sharp == 0.0f, "motionblur.no-tile-still-pixel-sharp");
	Check(blurred > 0.1f && blurred < 0.9f, "motionblur.background-adopts-tile-velocity");
	// Adoption == running the same gather with the tile velocity as own.
	const float asOwn = Gather(img, 31, 32, tilePx, false, Vec2{}, kTaps, kRadius);
	Check(std::abs(blurred - asOwn) < 1e-6f, "motionblur.tile-adoption-equivalent");
}

void TestGatherOwnVelocityWins() {
	Image img;
	img.w = img.h = 64;
	img.px.resize(64 * 64);
	for (uint32_t y = 0; y < 64; ++y)
		for (uint32_t x = 0; x < 64; ++x)
			img.px[size_t(y) * 64 + x] = float(x) / 63.0f;
	constexpr uint32_t kTaps = 12;
	constexpr float kRadius = 24.0f;
	// Own 40px beats the tile's 8px; the radius clamp caps the span at 24.
	const float mixed = Gather(img, 32, 32, Vec2{ 40.0f, 0.0f }, true, Vec2{ 8.0f, 0.0f },
		kTaps, kRadius);
	const float ownOnly = Gather(img, 32, 32, Vec2{ 40.0f, 0.0f }, false, Vec2{},
		kTaps, kRadius);
	Check(std::abs(mixed - ownOnly) < 1e-6f, "motionblur.own-velocity-dominant");
	// And the clamp: 40px with maxRadius 24 spans the same taps as a
	// literal 24px velocity.
	const float clamped = Gather(img, 32, 32, Vec2{ 24.0f, 0.0f }, false, Vec2{},
		kTaps, kRadius);
	Check(std::abs(mixed - clamped) < 1e-6f, "motionblur.radius-clamp-span");
}

void TestIgn() {
	bool inRange = true, deterministic = true;
	float prev[4] = { -1, -1, -1, -1 };
	for (int i = 0; i < 4; ++i) {
		const float v = Ign(7.0f + 13.0f * i, 3.0f + 29.0f * i);
		if (!(v >= 0.0f && v < 1.0f))
			inRange = false;
		if (prev[0] >= 0.0f && Ign(7.0f + 13.0f * i, 3.0f + 29.0f * i) != v)
			deterministic = false;
	}
	Check(inRange, "motionblur.ign-in-unit-range");
	Check(deterministic, "motionblur.ign-deterministic");

	// Well distributed: mean over a 16x16 block near 0.5, spread across
	// the unit interval (the property IGN is engineered for).
	double sum = 0.0;
	float lo = 1.0f, hi = 0.0f;
	std::vector<float> block;
	for (uint32_t y = 0; y < 16; ++y)
		for (uint32_t x = 0; x < 16; ++x) {
			const float v = Ign(float(x), float(y));
			sum += v;
			lo = std::min(lo, v);
			hi = std::max(hi, v);
			block.push_back(v);
		}
	const double mean = sum / 256.0;
	Check(mean > 0.35 && mean < 0.65, "motionblur.ign-block-mean-near-half");
	Check(lo < 0.2f && hi > 0.8f, "motionblur.ign-block-spread");
	std::sort(block.begin(), block.end());
	block.erase(std::unique(block.begin(), block.end()), block.end());
	Check(block.size() >= 200, "motionblur.ign-block-distinct");
}

void TestGatherTapPhase() {
	// Tap phase (i + ign)/n - 0.5: always inside [-0.5, +0.5) for any
	// ign in [0,1) - the jitter rotates the window, never widens it.
	bool inBounds = true;
	for (uint32_t n = 2; n <= 32; ++n)
		for (uint32_t i = 0; i < n; ++i) {
			for (float ign = 0.0f; ign < 1.0f; ign += 0.03125f) {
				const float t = (float(i) + ign) / float(n) - 0.5f;
				if (!(t >= -0.5f && t < 0.5f))
					inBounds = false;
			}
		}
	Check(inBounds, "motionblur.ign-taps-in-bounds");

	// Flat field: any phase rotation averages to the same constant
	// (unbiased box) - the gather never invents contrast on flat input.
	Image img;
	img.w = img.h = 64;
	img.px.assign(64 * 64, 0.37f);
	bool flatExact = true;
	for (uint32_t y = 0; y < 64; y += 7)
		for (uint32_t x = 0; x < 64; x += 5) {
			const float v = Gather(img, float(x), float(y), Vec2{ 9.0f, 5.0f },
				false, Vec2{}, 12, 24.0f);
			if (std::abs(v - 0.37f) > 1e-6f)
				flatExact = false;
		}
	Check(flatExact, "motionblur.flat-field-unbiased");

	// Sub-pixel own motion (no tile): sharp center tap, IGN never
	// engages below the 0.5px threshold.
	const float subpx = Gather(img, 32, 32, Vec2{ 0.3f, 0.4f }, false, Vec2{}, 12, 24.0f);
	Check(std::abs(subpx - 0.37f) < 1e-6f, "motionblur.subpixel-sharp");
}

} // namespace

int main() {
	TestTileReduce();
	TestTileReduceJitterStripped();
	TestTileReducePartialEdge();
	TestTileDilate();
	TestGatherBackgroundAdoptsTile();
	TestGatherOwnVelocityWins();
	TestIgn();
	TestGatherTapPhase();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
