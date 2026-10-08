#pragma once
#include <cstring>
#include <vector>

enum class DxcShaderStage
{
	Vertex,
	Fragment,
	Compute,
	Task, // amplification (as_6_7, entry mainAS)
	Mesh, // mesh (ms_6_7, entry mainMS)
};
std::vector<char> CompileHLSLToSPIRV(std::string_view source, DxcShaderStage stage, const wchar_t* entryPoint = L"main");
