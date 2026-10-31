// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

enum class DxcShaderStage
{
	Vertex,
	Fragment,
	Compute,
	Task, // amplification (as_6_7, entry mainAS)
	Mesh, // mesh (ms_6_7, entry mainMS)
};

// The legacy self-contained form (no includes, no defines).
std::vector<char> CompileHLSLToSPIRV(std::string_view source,
	DxcShaderStage stage, const wchar_t* entryPoint = L"main");

// The full-control form (the shader hot-reload path): an INCLUDE
// directory (DXC's default include handler resolves #include against
// it — the engine's shaders pull common.hlsli & co), optional -D
// defines (the variant twins' baked constants), and CAPTURED
// diagnostics — the compiler's error text lands in `outDiagnostics`
// (never just stderr) so the reload service can surface WHY a
// recompile failed. Throws std::runtime_error on failure (the
// diagnostics string rides the throw when non-empty).
std::vector<char> CompileHLSLToSPIRV(std::string_view source,
	DxcShaderStage stage, const wchar_t* entryPoint,
	const std::wstring& includeDir,
	const std::vector<std::wstring>& defines,
	std::string* outDiagnostics);
