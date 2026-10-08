// spirv reflection tests: headless (no GPU/device required).
// Covers specialization payload semantics, push-constant merge/validate,
// ReflectSpirv error paths, and – when DXC is available – end-to-end
// readback of push ranges + spec constants from compiled HLSL.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

#include <Engine/Renderer/spirv_reflection.h>
#include <Engine/Renderer/dxc_integration.h>
#include <Engine/Rendergraph/bloom.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) {
		std::cout << "PASS " << name << "\n";
	} else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

void TestReflectRejectsBadInput() {
	{
		smvk::SpirvReflection r = smvk::ReflectSpirv(std::span<const uint32_t>{});
		Check(!r.valid && !r.error.empty(), "spirv.reject.empty");
	}
	{
		const uint32_t bad[] = { 0xDEADBEEFu, 0, 0, 0 };
		smvk::SpirvReflection r = smvk::ReflectSpirv(bad);
		Check(!r.valid && !r.error.empty(), "spirv.reject.magic");
	}
	{
		// Byte size not a multiple of 4.
		const char bytes[] = { 0x03, 0x02, 0x23, 0x07, 0x00 };
		smvk::SpirvReflection r = smvk::ReflectSpirvBytes(
			std::span<const char>(bytes, sizeof(bytes)));
		Check(!r.valid && !r.error.empty(), "spirv.reject.oddsize");
	}
	{
		// Correct magic but truncated header -> reflect must fail, not crash.
		const uint32_t trunc[] = { 0x07230203u };
		smvk::SpirvReflection r = smvk::ReflectSpirv(trunc);
		Check(!r.valid && !r.error.empty(), "spirv.reject.truncated");
	}
}

void TestMergePushRanges() {
	using smvk::PushConstantRange;
	using smvk::ShaderStage;
	// VS covers [0,64), FS covers [32,96): merged -> [0,32)V, [32,64)V|F, [64,96)F.
	std::vector<PushConstantRange> vs = { { 0, 64, ShaderStage::Vertex } };
	std::vector<PushConstantRange> fs = { { 32, 64, ShaderStage::Fragment } };
	auto merged = smvk::MergePushConstantRanges({ vs, fs });
	Check(merged.size() == 3, "push.merge.count");
	if (merged.size() == 3) {
		Check(merged[0].offset == 0 && merged[0].size == 32 &&
				merged[0].stages == ShaderStage::Vertex, "push.merge.first");
		Check(merged[1].offset == 32 && merged[1].size == 32 &&
				merged[1].stages == (ShaderStage::Vertex | ShaderStage::Fragment),
			"push.merge.mid");
		Check(merged[2].offset == 64 && merged[2].size == 32 &&
				merged[2].stages == ShaderStage::Fragment, "push.merge.last");
	}
	Check(smvk::PushConstantFootprint(merged) == 96, "push.footprint");

	// Identical ranges across stages collapse to one range with unioned stages.
	std::vector<PushConstantRange> a = { { 0, 32, ShaderStage::Vertex } };
	std::vector<PushConstantRange> b = { { 0, 32, ShaderStage::Fragment } };
	auto m2 = smvk::MergePushConstantRanges({ a, b });
	Check(m2.size() == 1 && m2[0].size == 32 &&
			m2[0].stages == (ShaderStage::Vertex | ShaderStage::Fragment),
		"push.merge.union");

	// Empty input -> empty output.
	auto m3 = smvk::MergePushConstantRanges({});
	Check(m3.empty(), "push.merge.empty");
}

void TestValidatePushRanges() {
	using smvk::PushConstantRange;
	using smvk::ShaderStage;
	std::vector<PushConstantRange> ok = { { 0, 128, ShaderStage::All } };
	Check(smvk::ValidatePushConstants(ok, 256), "push.validate.ok");

	std::vector<PushConstantRange> big = { { 0, 257, ShaderStage::All } };
	std::string err;
	Check(!smvk::ValidatePushConstants(big, 256, &err) && !err.empty(),
		"push.validate.overflow");

	std::vector<PushConstantRange> noStage = { { 0, 16, ShaderStage::None } };
	Check(!smvk::ValidatePushConstants(noStage, 256), "push.validate.nostage");

	std::vector<PushConstantRange> zero = { { 0, 0, ShaderStage::Vertex } };
	Check(!smvk::ValidatePushConstants(zero, 256), "push.validate.zerosize");

	// 126 bytes is not a multiple of 4 -> unsupported (VUID-01795); 128 is OK.
	std::vector<PushConstantRange> odd = { { 0, 126, ShaderStage::All } };
	std::string oddErr;
	Check(!smvk::ValidatePushConstants(odd, 256, &oddErr) && !oddErr.empty(),
		"push.validate.odd126");
	std::vector<PushConstantRange> aligned128 = { { 0, 128, ShaderStage::All } };
	Check(smvk::ValidatePushConstants(aligned128, 128), "push.validate.fits128");
}

void TestExpandPushStagesForLayout() {	using smvk::PushConstantRange;
	using smvk::ShaderStage;
	// Engine global layout: single ALL range [0, 256). A narrow compute-only
	// push (the ForwardPlusCull case: offset 0, size 64) must expand to ALL
	// per VUID-vkCmdPushConstants-offset-01796.
	const PushConstantRange engineRange{ .offset = 0, .size = smvk::kEnginePushConstantSize, .stages = ShaderStage::All };
	const std::span<const PushConstantRange> layout(&engineRange, 1);
	Check(smvk::ExpandPushStagesForLayout(ShaderStage::Compute, layout, 0, 64) == ShaderStage::All,
		"push.expand.compute-to-all");
	Check(smvk::ExpandPushStagesForLayout(ShaderStage::Vertex, layout, 0, 16) == ShaderStage::All,
		"push.expand.vertex-to-all");
	Check(smvk::ExpandPushStagesForLayout(ShaderStage::All, layout, 0, 256) == ShaderStage::All,
		"push.expand.all-stable");
	// Non-overlapping byte range leaves the mask untouched.
	Check(smvk::ExpandPushStagesForLayout(ShaderStage::Compute, layout, 512, 64) == ShaderStage::Compute,
		"push.expand.nooverlap");
	// Narrow layout range unions only its own stages.
	const PushConstantRange fragOnly{ .offset = 0, .size = 32, .stages = ShaderStage::Fragment };
	const std::span<const PushConstantRange> fragLayout(&fragOnly, 1);
	Check(smvk::ExpandPushStagesForLayout(ShaderStage::Compute, fragLayout, 0, 16) ==
			(ShaderStage::Compute | ShaderStage::Fragment),
		"push.expand.union");
}

void TestSpecializationPayload() {
	smvk::ShaderSpecialization s0;
	uint32_t iv = 7;
	float fv = 1.5f;
	s0.Set(0, iv);
	s0.Set(1, fv);

	smvk::ShaderSpecialization s1;
	s1.Set(0, iv);
	s1.Set(1, fv);
	Check(s0 == s1, "spec.equal");

	smvk::ShaderSpecialization s2;
	uint32_t other = 8;
	s2.Set(0, other);
	s2.Set(1, fv);
	Check(!(s0 == s2), "spec.notequal.value");
	Check(s0.entries.size() == 2 && s0.entries[0].constantID == 0 &&
			s0.entries[0].offset == 0 && s0.entries[0].size == 4,
		"spec.entry.layout");
	Check(s0.data.size() == 8, "spec.data.size");

	smvk::ShaderSpecialization sb;
	sb.SetBool(3, true);
	Check(sb.entries.size() == 1 && sb.entries[0].size == 4, "spec.bool.size");
	uint32_t back = 0;
	std::memcpy(&back, sb.data.data(), 4);
	Check(back == 1u, "spec.bool.value");
}

void TestShaderEqualityWithSpec() {
	smvk::Shader a{ .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
	smvk::Shader b{ .entryPoint = "mainVS", .stage = smvk::ShaderStage::Vertex };
	Check(a == b, "shader.equal.nospec");

	auto spec1 = std::make_shared<smvk::ShaderSpecialization>();
	uint32_t v = 4;
	spec1->Set(0, v);
	auto spec2 = std::make_shared<smvk::ShaderSpecialization>();
	spec2->Set(0, v);
	a.specialization = spec1;
	b.specialization = spec2;
	Check(a == b, "shader.equal.samespec");

	auto spec3 = std::make_shared<smvk::ShaderSpecialization>();
	uint32_t w = 5;
	spec3->Set(0, w);
	b.specialization = spec3;
	Check(!(a == b), "shader.notequal.difspec");

	b.specialization.reset();
	Check(!(a == b), "shader.notequal.nullspec");
}

void TestDemoteSanitizeUnit() {
	// Hand-built minimal instruction stream: header + Shader cap + Demote
	// cap + MemoryModel + an OpConstant whose literal value mimics the demote
	// word (walker must NOT touch operands) + one real OpDemote instruction.
	const uint32_t kKillWord = (1u << 16) | 252u;
	const uint32_t kDemoteWord = (1u << 16) | 5380u;
	std::vector<uint32_t> words = {
		0x07230203u, 0x00010000u, 0u, 10u, 0u, // header
		0x00020011u, 1u, // OpCapability Shader
		0x00020011u, 5379u, // OpCapability DemoteToHelperInvocation
		0x0003000Eu, 0u, 1u, // OpMemoryModel Logical GLSL450
		0x0004002Bu, 7u, 8u, kDemoteWord, // OpConstant ... <demote-word-as-literal>
		kDemoteWord, // OpDemoteToHelperInvocation (real instruction)
	};
	std::vector<char> bytes(words.size() * 4, 0);
	std::memcpy(bytes.data(), words.data(), bytes.size());

	Check(smvk::SanitizeDemoteToKill(bytes) == 1, "sanitize.unit.count");
	Check(bytes.size() == (words.size() - 2) * 4, "sanitize.unit.shrink");
	if (bytes.size() == (words.size() - 2) * 4) {
		auto* w = reinterpret_cast<const uint32_t*>(bytes.data());
		Check(w[0] == 0x07230203u, "sanitize.unit.magic");
		// Capability erased: words[7..8] are now MemoryModel, literal intact.
		Check(w[7] == 0x0003000Eu, "sanitize.unit.caperased");
		Check(w[13] == kDemoteWord, "sanitize.unit.literalintact");
		Check(w[14] == kKillWord, "sanitize.unit.demotetokill");
	}
	// Idempotent: second run finds nothing.
	Check(smvk::SanitizeDemoteToKill(bytes) == 0, "sanitize.unit.idempotent");

	// Malformed input is left untouched.
	std::vector<char> empty;
	Check(smvk::SanitizeDemoteToKill(empty) == 0, "sanitize.unit.empty");
	std::vector<char> odd = { 0x03, 0x02, 0x23 };
	Check(smvk::SanitizeDemoteToKill(odd) == 0, "sanitize.unit.oddsize");
}

void TestDemoteSanitizeBranchUnit() {
	// DXC's real `discard` pattern: demote does NOT terminate its block, so
	// it is followed by OpBranch %merge. A bare demote->kill opcode swap
	// strands that branch after a terminator (spirv-val: "Branch must appear
	// in a block"); the sanitizer must erase the dead tail too.
	const uint32_t kKillWord = (1u << 16) | 252u;
	const uint32_t kDemoteWord = (1u << 16) | 5380u;
	std::vector<uint32_t> words = {
		0x07230203u, 0x00010000u, 0u, 10u, 0u, // header
		0x00020011u, 1u, // OpCapability Shader
		0x00020011u, 5379u, // OpCapability DemoteToHelperInvocation
		0x0003000Eu, 0u, 1u, // OpMemoryModel Logical GLSL450
		0x000200F8u, 1u, // OpLabel %1
		kDemoteWord, // OpDemoteToHelperInvocation
		0x000200F9u, 2u, // OpBranch %2 (dead once demote becomes kill)
		0x000200F8u, 2u, // OpLabel %2
	};
	std::vector<char> bytes(words.size() * 4, 0);
	std::memcpy(bytes.data(), words.data(), bytes.size());

	Check(smvk::SanitizeDemoteToKill(bytes) == 1, "sanitize.branch.count");
	// -2 words capability, -2 words dead branch.
	Check(bytes.size() == (words.size() - 4) * 4, "sanitize.branch.shrink");
	if (bytes.size() == (words.size() - 4) * 4) {
		auto* w = reinterpret_cast<const uint32_t*>(bytes.data());
		Check(w[7] == 0x0003000Eu, "sanitize.branch.caperased");
		Check(w[12] == kKillWord, "sanitize.branch.demotetokill");
		// The merge label must immediately follow the kill: no stranded branch.
		Check(w[13] == 0x000200F8u && w[14] == 2u, "sanitize.branch.tailerased");
		// No demote opcode or capability left anywhere.
		bool clean = true;
		for (size_t i = 5; i + 1 < bytes.size() / 4 && clean; ++i) {
			if (w[i] == kDemoteWord)
				clean = false;
			if (w[i] == 0x00020011u && w[i + 1] == 5379u)
				clean = false;
		}
		Check(clean, "sanitize.branch.clean");
	}
	Check(smvk::SanitizeDemoteToKill(bytes) == 0, "sanitize.branch.idempotent");
}

// End-to-end: compile HLSL with a push block + spec constants, reflect it.
// Skipped gracefully when DXC (dxcompiler.dll) is unavailable.
void TestDxcRoundTrip() {	static constexpr const char* kSrc = R"(
struct TestPush { uint a; uint b; float c; uint d; };
[[vk::push_constant]] ConstantBuffer<TestPush> pc;
[[vk::constant_id(0)]] const uint kIters = 4;
[[vk::constant_id(1)]] const float kScale = 1.0f;
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> outBuf;
[numthreads(1, 1, 1)]
void mainCS(uint3 id : SV_DispatchThreadID) {
  float s = kScale;
  uint n = kIters + pc.a + pc.b;
  outBuf[0] = (uint)(s * (float)n + pc.c) + pc.d;
}
)";
	std::vector<char> spv;
	try {
		spv = CompileHLSLToSPIRV(kSrc, DxcShaderStage::Compute, L"mainCS");
	} catch (const std::exception& e) {
		std::cout << "SKIP spirv.dxc (DXC unavailable: " << e.what() << ")\n";
		return;
	}
	Check(!spv.empty() && spv.size() % 4 == 0, "spirv.dxc.code");
	if (spv.empty() || spv.size() % 4 != 0)
		return;

	smvk::SpirvReflection r =
		smvk::ReflectSpirvBytes(std::span<const char>(spv.data(), spv.size()));
	Check(r.valid, "spirv.dxc.valid");
	if (!r.valid) {
		std::cout << "  reflect error: " << r.error << "\n";
		return;
	}
	// Push block: 4 members x 4 bytes = 16 bytes at offset 0.
	bool foundPush = false;
	for (const auto& pc : r.pushConstants) {
		if (pc.offset == 0 && pc.size >= 16 &&
			(pc.stages & smvk::ShaderStage::Compute) != smvk::ShaderStage::None)
			foundPush = true;
	}
	Check(foundPush, "spirv.dxc.push");

	// Two spec constants with ids 0 and 1.
	bool has0 = false, has1 = false;
	for (const auto& s : r.specConstants) {
		if (s.specId == 0)
			has0 = true;
		if (s.specId == 1)
			has1 = true;
	}
	Check(has0 && has1, "spirv.dxc.specids");
	Check(r.specConstants.size() >= 2, "spirv.dxc.speccount");

	// Merged program helper input shape: single module merge is identity-ish.
	auto merged = smvk::MergePushConstantRanges({ r.pushConstants });
	Check(!merged.empty() &&
			smvk::PushConstantFootprint(merged) >= 16,
		"spirv.dxc.merge");
	Check(smvk::ValidatePushConstants(merged, 256), "spirv.dxc.fits256");
}

// float3x4 push packing (motionvector prevModel): the DXC SPIR-V declares
// the member RowMajor stride 12 and lowers mul() to v4 x M, so result[j] =
// sum_i v[i] * blob[3i+j]. That equals M*v exactly when the blob holds the
// matrix COLUMNS (glm columns xyz + translation). These checks pin the
// mapping with hand-built matrices (no glm needed) by emulating the SPV
// op on the packed blob; a flat 12-float copy fails them (it drops
// translation and leaks the projection row).
void TestPushFloat3x4() {
	auto emulateSpvMul = [](const float* blob12, const float v[4], float out[3]) {
		for (int j = 0; j < 3; ++j)
			out[j] = v[0] * blob12[0 + j] + v[1] * blob12[3 + j]
				+ v[2] * blob12[6 + j] + v[3] * blob12[9 + j];
	};
	auto matMul = [](const float* colMajor16, const float v[4], float out[3]) {
		for (int r = 0; r < 3; ++r)
			out[r] = colMajor16[0 * 4 + r] * v[0] + colMajor16[1 * 4 + r] * v[1]
				+ colMajor16[2 * 4 + r] * v[2] + colMajor16[3 * 4 + r] * v[3];
	};
	auto close = [](const float a[3], const float b[3]) {
		for (int i = 0; i < 3; ++i)
			if (std::abs(a[i] - b[i]) > 1e-5f)
				return false;
		return true;
	};

	// Translation-only (identity rotation, t = (10,20,30)).
	const float kT[16] = {
		1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 1, 0,
		10, 20, 30, 1,
	};
	float packed[12] = {};
	smvk::PackPushFloat3x4(kT, packed);
	Check(packed[0] == 1.0f && packed[4] == 1.0f && packed[8] == 1.0f,
		"push.f3x4.translation.rotpart");
	Check(packed[9] == 10.0f && packed[10] == 20.0f && packed[11] == 30.0f,
		"push.f3x4.translation.kept");
	// The old flat copy drops translation: dst[9..11] would be (0,0,1).
	Check(!(kT[9] == 10.0f && kT[10] == 20.0f && kT[11] == 30.0f),
		"push.f3x4.flatcopy.loses.translation");

	// Y-90 rotation + translation, several points incl. off-axis.
	const float kR[16] = {
		0, 0, -1, 0,
		0, 1, 0, 0,
		1, 0, 0, 0,
		10, 20, 30, 1,
	};
	float packedR[12] = {};
	smvk::PackPushFloat3x4(kR, packedR);
	const float kPts[][4] = {
		{ 1, 2, 3, 1 }, { 0, 0, 0, 1 }, { -5, 7, -9, 1 }, { 0.5f, -0.25f, 2.0f, 1 },
	};
	bool allMatch = true;
	for (const auto& p : kPts) {
		float want[3], got[3];
		matMul(kR, p, want);
		emulateSpvMul(packedR, p, got);
		allMatch = allMatch && close(want, got);
	}
	Check(allMatch, "push.f3x4.spvop.matches.matmul");
	// Spot value: Ry(90) maps (1,2,3) -> (3,2,-1), plus t -> (13,22,29).
	{
		const float p[4] = { 1, 2, 3, 1 };
		float got[3];
		emulateSpvMul(packedR, p, got);
		const float want[3] = { 13, 22, 29 };
		Check(close(want, got), "push.f3x4.spot");
	}
}

// End-to-end for the demote policy: a `discard` pixel shader compiled for
// vulkan1.3 must come out of SanitizeDemoteToKill with no demote capability
// and a still-reflectable module. Skipped when DXC is unavailable.
void TestDemoteSanitizeDxc() {
	static constexpr const char* kSrc = R"(
struct PSIn { float4 pos : SV_Position; };
float4 mainPS(PSIn i) : SV_Target0 {
  if (i.pos.x < 0.0f) discard;
  return float4(1.0f, 0.0f, 0.0f, 1.0f);
}
)";
	std::vector<char> spv;
	try {
		spv = CompileHLSLToSPIRV(kSrc, DxcShaderStage::Fragment, L"mainPS");
	} catch (const std::exception& e) {
		std::cout << "SKIP spirv.sanitize.dxc (DXC unavailable: " << e.what() << ")\n";
		return;
	}
	auto hasDemoteCap = [](const std::vector<char>& code) {
		if (code.size() % 4 != 0 || code.size() < 20)
			return false;
		auto* w = reinterpret_cast<const uint32_t*>(code.data());
		const size_t n = code.size() / 4;
		for (size_t i = 5; i + 1 < n; ++i) {
			if (w[i] == 0x00020011u && w[i + 1] == 5379u)
				return true;
		}
		return false;
	};
	Check(hasDemoteCap(spv), "spirv.sanitize.dxc.emitsdemote");
	const uint32_t fixed = smvk::SanitizeDemoteToKill(spv);
	Check(fixed >= 1, "spirv.sanitize.dxc.count");
	Check(!hasDemoteCap(spv), "spirv.sanitize.dxc.capgone");
	// No rewritten kill may be followed by a stranded branch (the device-lost
	// bug: demote is not a terminator, kill is).
	{
		bool noStrandedBranch = true;
		if (spv.size() % 4 == 0 && spv.size() >= 20) {
			auto* w = reinterpret_cast<const uint32_t*>(spv.data());
			const size_t n = spv.size() / 4;
			size_t i = 5;
			while (i < n) {
				const uint32_t wc = w[i] >> 16;
				const uint32_t op = w[i] & 0xFFFF;
				if (wc == 0 || i + wc > n)
					break;
				if (op == 252u && wc == 1 && i + 1 < n) {
					const uint32_t nop = w[i + 1] & 0xFFFF;
					if (nop == 249u)
						noStrandedBranch = false;
				}
				i += wc;
			}
		}
		Check(noStrandedBranch, "spirv.sanitize.dxc.nostrandedbranch");
	}
	// Module must still parse + reflect after the rewrite.
	smvk::SpirvReflection r =
		smvk::ReflectSpirvBytes(std::span<const char>(spv.data(), spv.size()));
	Check(r.valid, "spirv.sanitize.dxc.stillvalid");
}

// SPD contract: the HLSL SpdPush block in engine/shaders/spd_downsample.cs.hlsl
// must stay layout-identical to smvk::SpdPush in Engine/Rendergraph/bloom.h.
// The shader below mirrors the real block member-for-member (keep in sync by
// hand — the real file needs common.hlsli includes the test compiler has no
// search path for); every member is consumed so DXC cannot trim the range.
// Skipped gracefully when DXC is unavailable.
void TestSpdPushContract() {
	static constexpr const char* kSrc = R"(
struct SpdPush {
  uint srcIndex;
  uint dstIndex0;
  uint dstIndex1;
  uint dstIndex2;
  uint dstIndex3;
  uint dstIndex4;
  uint baseWidth;
  uint baseHeight;
  uint hdrWidth;
  uint hdrHeight;
  float threshold;
  float knee;
};
[[vk::push_constant]] ConstantBuffer<SpdPush> spd;
[[vk::binding(2, 0)]] RWTexture2D<float4> spdImages[];
[numthreads(16, 16, 1)]
void mainCS(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
  uint s = spd.srcIndex + spd.dstIndex0 + spd.dstIndex1 + spd.dstIndex2
    + spd.dstIndex3 + spd.dstIndex4 + spd.baseWidth + spd.baseHeight
    + spd.hdrWidth + spd.hdrHeight;
  float f = spd.threshold + spd.knee;
  spdImages[spd.dstIndex4][gtid.xy + gid.xy] = float4((float)s, f, 0.0f, 1.0f);
}
)";
	static_assert(sizeof(smvk::SpdPush) == 48, "SpdPush must stay 48 bytes");
	std::vector<char> spv;
	try {
		spv = CompileHLSLToSPIRV(kSrc, DxcShaderStage::Compute, L"mainCS");
	} catch (const std::exception& e) {
		std::cout << "SKIP spirv.spd (DXC unavailable: " << e.what() << ")\n";
		return;
	}
	Check(!spv.empty() && spv.size() % 4 == 0, "spirv.spd.code");
	if (spv.empty() || spv.size() % 4 != 0)
		return;
	smvk::SpirvReflection r =
		smvk::ReflectSpirvBytes(std::span<const char>(spv.data(), spv.size()));
	Check(r.valid, "spirv.spd.valid");
	if (!r.valid) {
		std::cout << "  reflect error: " << r.error << "\n";
		return;
	}
	bool hasMainCS = false;
	for (const auto& e : r.entryPoints) {
		if (e == "mainCS")
			hasMainCS = true;
	}
	Check(hasMainCS, "spirv.spd.entry");
	// The reflected push footprint must equal the C++ struct: any drift
	// (added/removed/reordered member) fails here before it can corrupt
	// the bloom chain's dispatch.
	auto merged = smvk::MergePushConstantRanges({ r.pushConstants });
	Check(smvk::PushConstantFootprint(merged) == sizeof(smvk::SpdPush),
		"spirv.spd.footprint48");
	bool computeStage = false;
	for (const auto& pc : r.pushConstants) {
		if ((pc.stages & smvk::ShaderStage::Compute) != smvk::ShaderStage::None)
			computeStage = true;
	}
	Check(computeStage, "spirv.spd.compute");
	Check(smvk::ValidatePushConstants(merged, 256), "spirv.spd.fits256");
}

} // namespace

int main() {
	TestReflectRejectsBadInput();
	TestMergePushRanges();
	TestValidatePushRanges();
	TestExpandPushStagesForLayout();
	TestDemoteSanitizeUnit();
	TestDemoteSanitizeBranchUnit();
	TestDemoteSanitizeDxc();
	TestSpecializationPayload();
	TestShaderEqualityWithSpec();
	TestDxcRoundTrip();
	TestPushFloat3x4();
	TestSpdPushContract();
	std::cout << g_checks - g_failures << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
