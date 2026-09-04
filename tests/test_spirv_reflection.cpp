// spirv reflection tests: headless (no GPU/device required).
// Covers specialization payload semantics, push-constant merge/validate,
// ReflectSpirv error paths, and – when DXC is available – end-to-end
// readback of push ranges + spec constants from compiled HLSL.

#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

#include <Engine/Renderer/spirv_reflection.h>
#include <Engine/Renderer/dxc_integration.h>

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
}

void TestExpandPushStagesForLayout() {
	using smvk::PushConstantRange;
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

// End-to-end: compile HLSL with a push block + spec constants, reflect it.
// Skipped gracefully when DXC (dxcompiler.dll) is unavailable.
void TestDxcRoundTrip() {
	static constexpr const char* kSrc = R"(
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

} // namespace

int main() {
	TestReflectRejectsBadInput();
	TestMergePushRanges();
	TestValidatePushRanges();
	TestExpandPushStagesForLayout();
	TestSpecializationPayload();
	TestShaderEqualityWithSpec();
	TestDxcRoundTrip();
	std::cout << g_checks - g_failures << "/" << g_checks << " checks passed\n";
	return g_failures == 0 ? 0 : 1;
}
