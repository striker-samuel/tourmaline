#pragma once
#include <cstring>
#include <vector>

enum class ShaderStage
{
	Vertex,
	Fragment,
	Compute
};

std::vector<char> CompileHLSLToSPIRV(std::string_view source, ShaderStage stage, const wchar_t* entryPoint = L"main");
