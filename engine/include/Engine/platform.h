#pragma once

#include <fstream>
#include <vector>
#include <string>
#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

// Read file with exe-relative fallback (for shader loading when CWD != exe dir)
static std::vector<char> readFile(const std::string& filename) {
	// First try the path as-is (CWD-relative)
	std::ifstream file(filename, std::ios::ate | std::ios::binary);
	if (file.is_open()) {
		size_t fileSize = (size_t)file.tellg();
		std::vector<char> buffer(fileSize);
		file.seekg(0);
		file.read(buffer.data(), fileSize);
		file.close();
		return buffer;
	}

	// Fallback: exe-relative path
#ifdef _WIN32
	char exePath[1024] = {};
	DWORD n = GetModuleFileNameA(nullptr, exePath, DWORD(sizeof(exePath)));
	if (n > 0 && n < sizeof(exePath)) {
		std::string exeDir = std::string(exePath).substr(0, std::string(exePath).find_last_of("\\/"));
		std::string exeRelativePath = exeDir + "\\" + filename;
		// Normalize slashes
		for (char& c : exeRelativePath) if (c == '/') c = '\\';
		std::cerr << "[readFile] Trying exe-relative: " << exeRelativePath << std::endl;
		std::ifstream file2(exeRelativePath, std::ios::ate | std::ios::binary);
		if (file2.is_open()) {
			size_t fileSize = (size_t)file2.tellg();
			std::vector<char> buffer(fileSize);
			file2.seekg(0);
			file2.read(buffer.data(), fileSize);
			file2.close();
			return buffer;
		}
	}
#endif

	throw std::runtime_error("failed to open file! " + filename + " (tried CWD-relative and exe-relative)");
}
