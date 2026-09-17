// particles_tests: CPU-side tests for the GPU particle system builder
// (Engine/Rendergraph/particles.h). No GPU required: the struct ABI mirrors
// (C++ <-> particles.hlsli, sizes asserted on the C++ side), the free-list
// init image, the sim dispatch math and the emitter spawn-quota arithmetic
// (float-exact CPU mirror of the emit CS carry loop) are all pure code.
//
// Hand-rolled runner (no test framework dependency): each check prints
// PASS/FAIL, the process exits nonzero on any failure.

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include <Engine/Rendergraph/particles.h>

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

// ---------------------------------------------------------------------------
// T1: struct ABI mirrors (particles.hlsli). The HLSL side cannot
// static_assert (DXC 2018 mode rejects it at global scope), so the C++
// sizes/offsets are the contract.
// ---------------------------------------------------------------------------
void TestStructAbi() {
	Check(sizeof(smvk::ParticleGpu) == 64, "pt.abi.particle-size");
	Check(sizeof(smvk::ParticleGpu) % 16 == 0, "pt.abi.particle-align");
	Check(sizeof(smvk::ParticleEmitterGpu) == 128, "pt.abi.emitter-size");
	Check(sizeof(smvk::ParticleEmitterGpu) % 16 == 0, "pt.abi.emitter-align");
	Check(offsetof(smvk::ParticleEmitterGpu, tail) == 112,
		"pt.abi.emitter-tail-offset");

	// Default rows must carry a zero GPU carry (uploaded rows leave
	// scheduling to the emitterState buffer).
	smvk::ParticleEmitterGpu def{};
	Check(def.tail.x == 0.0f, "pt.abi.emitter-default-carry-zero");

	// Push layouts (must match particles_emit/sim/draw.hlsl mirrors).
	Check(sizeof(smvk::ParticlesEmitPush) == 48, "pt.abi.emit-push-size");
	Check(sizeof(smvk::ParticlesSimPush) == 48, "pt.abi.sim-push-size");
	Check(sizeof(smvk::ParticlesDrawPush) == 80, "pt.abi.draw-push-size");
	Check(offsetof(smvk::ParticlesDrawPush, fpTileCountX) == 48,
		"pt.abi.draw-push-tilecountx");
	Check(offsetof(smvk::ParticlesDrawPush, maxParticles) == 64,
		"pt.abi.draw-push-maxparticles");
	Check(offsetof(smvk::ParticlesEmitPush, frameSeed) == 44,
		"pt.abi.emit-push-frameseed");
}

// ---------------------------------------------------------------------------
// T2: free-list init image (Particles::Create primes the stack from this;
// the emit CS pops slots[top-1], the sim CS pushes at slots[top]).
// ---------------------------------------------------------------------------
void TestFreeListInit() {
	{
		const auto data = smvk::Particles::BuildFreeListInitData(8);
		Check(data.size() == 9, "pt.freelist.size");
		Check(data[0] == 8, "pt.freelist.count-full");
		bool slotsOk = data.size() == 9;
		for (uint32_t i = 0; i < 8; ++i)
			slotsOk = slotsOk && data[i + 1] == i;
		Check(slotsOk, "pt.freelist.identity-slots");
	}
	{
		const auto data = smvk::Particles::BuildFreeListInitData(1);
		Check(data.size() == 2 && data[0] == 1 && data[1] == 0,
			"pt.freelist.single-slot");
	}
}

// ---------------------------------------------------------------------------
// T3: sim dispatch group count (ceil; the CS guards the tail).
// ---------------------------------------------------------------------------
void TestSimGroups() {
	using smvk::Particles;
	Check(Particles::SimGroupsFor(0, 64) == 0, "pt.groups.zero");
	Check(Particles::SimGroupsFor(1, 64) == 1, "pt.groups.one-slot");
	Check(Particles::SimGroupsFor(64, 64) == 1, "pt.groups.exact");
	Check(Particles::SimGroupsFor(65, 64) == 2, "pt.groups.tail");
	Check(Particles::SimGroupsFor(65536, 64) == 1024, "pt.groups.pool");
	// Thread-count guard: a zero/invalid threads argument falls back to 64
	// rather than dividing by zero.
	Check(Particles::SimGroupsFor(128, 0) == 2, "pt.groups.thread-guard");
}

// ---------------------------------------------------------------------------
// T4: spawn quota arithmetic — float-exact CPU mirror of the emit CS
// (carry += rate*dt; quota = floor(carry); carry -= quota). The GPU owns the
// carry; this pins the schedule semantics: no burst, no lost particles.
// ---------------------------------------------------------------------------
void TestSpawnQuota() {
	using smvk::Particles;
	// Exact whole frame: 4/s over a quarter second spawns exactly 1.
	float carry = 0.0f;
	const uint32_t q = Particles::SpawnQuotaForFrame(4.0f, 0.25f, carry, &carry);
	Check(q == 1 && carry == 0.0f, "pt.quota.whole");

	// Fractional rate: 60/s at 60 fps — each frame accumulates 60*dt ~ 1.0
	// (float drift can wobble the floor boundary), so over 60 frames the
	// total must be rate * seconds within +/- 1. No burst, no loss.
	{
		float carry = 0.0f;
		uint32_t spawned = 0;
		const float dt = 1.0f / 60.0f;
		for (int f = 0; f < 60; ++f)
			spawned += Particles::SpawnQuotaForFrame(60.0f, dt, carry, &carry);
		Check(spawned >= 59 && spawned <= 61, "pt.quota.sixty-fps-conserves");
	}
	// Rate 1/s over 10 s (600 frames): the floor boundary can wobble a
	// single spawn either way (carry never lost, just deferred a frame),
	// but the long-window total is conserved: rate * seconds +/- 1.
	{
		float carry = 0.0f;
		uint32_t spawned = 0;
		const float dt = 1.0f / 60.0f;
		for (int f = 0; f < 600; ++f)
			spawned += Particles::SpawnQuotaForFrame(1.0f, dt, carry, &carry);
		Check(spawned >= 9 && spawned <= 11, "pt.quota.one-per-second-conserves");
	}
	// Degenerate inputs: zero rate / zero dt / negative rate never spawn
	// and never poison the carry.
	{
		float carry = 0.5f;
		Check(Particles::SpawnQuotaForFrame(0.0f, 1.0f, carry, &carry) == 0,
			"pt.quota.zero-rate");
		Check(Particles::SpawnQuotaForFrame(10.0f, 0.0f, carry, &carry) == 0,
			"pt.quota.zero-dt");
		Check(Particles::SpawnQuotaForFrame(-10.0f, 1.0f, carry, &carry) == 0,
			"pt.quota.negative-rate");
		Check(carry == 0.5f, "pt.quota.carry-preserved");
	}
	// Hibernation spike: dt is clamped by the app (1/30 s), so a 5 s pause
	// spawns at most the clamped quota — this pins the clamp rationale.
	{
		float carry = 0.0f;
		const uint32_t q = Particles::SpawnQuotaForFrame(90.0f, 1.0f / 30.0f, carry, &carry);
		Check(q == 3, "pt.quota.dt-clamp-burst");
	}
}

// ---------------------------------------------------------------------------
// T5: free-list stack invariants under the GPU access pattern (emit pops
// with atomicAdd -1; sim pushes with atomicAdd +1). A slot can never be
// handed out while alive; the count stays inside [0, max].
// ---------------------------------------------------------------------------
void TestFreeListInvariants() {
	constexpr uint32_t kMax = 16;
	auto init = smvk::Particles::BuildFreeListInitData(kMax);
	std::vector<uint32_t> count{ init[0] };
	std::vector<uint32_t> slots(init.begin() + 1, init.end());
	std::vector<bool> alive(kMax, false);

	bool invariantsOk = true;
	uint32_t rng = 12345;
	auto pop = [&](uint32_t& slot) {
		if (count[0] < 1 || count[0] > kMax) {
			count[0] = count[0] < 1 ? 0 : count[0]; // GPU restores the miss
			return false;
		}
		count[0] -= 1;
		slot = slots[count[0]];
		if (slot >= kMax || alive[slot]) {
			invariantsOk = false;
			return false;
		}
		alive[slot] = true;
		return true;
	};
	auto push = [&](uint32_t slot) {
		if (!alive[slot]) {
			invariantsOk = false;
			return;
		}
		if (count[0] >= kMax) {
			invariantsOk = false; // overflow guard (unreachable per design)
			return;
		}
		alive[slot] = false;
		slots[count[0]] = slot;
		count[0] += 1;
	};
	// Randomized pop/push churn: quota-batched spawns, staggered deaths.
	for (int frame = 0; frame < 256; ++frame) {
		rng = rng * 1664525u + 1013904223u;
		const uint32_t quota = (rng >> 24) % 5;
		for (uint32_t k = 0; k < quota; ++k) {
			uint32_t slot;
			(void)pop(slot);
		}
		// Kill a few random alive slots (simulate lifetime expiry).
		rng = rng * 1664525u + 1013904223u;
		for (uint32_t i = 0; i < kMax; ++i) {
			if (alive[i] && ((rng >> (i % 31)) & 7) == 0)
				push(i);
		}
	}
	Check(invariantsOk, "pt.freelist.invariants");
	Check(count[0] <= kMax, "pt.freelist.count-bounded");

	// Drain + full death sweep returns the stack to its initial state.
	for (uint32_t i = 0; i < kMax; ++i)
		if (alive[i])
			push(i);
	Check(count[0] == kMax, "pt.freelist.drains-full");
}

} // namespace

int main() {
	TestStructAbi();
	TestFreeListInit();
	TestSimGroups();
	TestSpawnQuota();
	TestFreeListInvariants();
	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
