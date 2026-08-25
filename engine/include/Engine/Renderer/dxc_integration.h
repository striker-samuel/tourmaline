#pragma once
#include <cstring>
#include <vector>

enum class DxcShaderStage
{
	Vertex,
	Fragment,
	Compute
};
std::vector<char> CompileHLSLToSPIRV(std::string_view source, DxcShaderStage stage, const wchar_t* entryPoint = L"main");
