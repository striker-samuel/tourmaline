// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Native Windows file-open dialog (IFileOpenDialog): the single "Open
// file" entry point for scenes (.gltf/.glb), cooked paks (.pak) and
// tours (.tour). Starts in C:/ with an extension filter; the picked
// path feeds OpenAnySceneFile, which dispatches by extension (or
// ImportAsset for project imports). Replaces the old custom ImGui
// browser (one dialog for every open type now).

// Native file dialogs: Windows uses the shell's IFileOpenDialog/
// IFileSaveDialog (COM); Linux shells out to zenity when installed (the
// de-facto desktop file dialog) and falls back to a plain terminal
// prompt - headless/SSH editor sessions still work.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shobjidl.h> // IFileOpenDialog
#else
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#endif

#include <string>

#ifdef _WIN32
namespace editor {

bool NativeOpenFile(std::string& outPath, bool images) {
	// COM for the shell dialog (the editor otherwise avoids COM).
	const HRESULT coInit = CoInitializeEx(nullptr,
		COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	const bool coOwns = SUCCEEDED(coInit);
	if (FAILED(coInit) && coInit != RPC_E_CHANGED_MODE)
		return false;
	bool ok = false;
	IFileOpenDialog* dlg = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
			CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
		static const COMDLG_FILTERSPEC kFilters[] = {
			{ L"Scene files", L"*.gltf;*.glb;*.tour" },
			{ L"glTF scenes", L"*.gltf;*.glb" },
			{ L"Tour files", L"*.tour" },
			{ L"All files", L"*.*" },
		};
		static const COMDLG_FILTERSPEC kImageFilters[] = {
			{ L"Image files", L"*.ktx2;*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.exr;*.hdr" },
			{ L"KTX2 textures", L"*.ktx2" },
			{ L"PNG", L"*.png" },
			{ L"All files", L"*.*" },
		};
		const COMDLG_FILTERSPEC* filters = images ? kImageFilters : kFilters;
		UINT filterCount = images
			? static_cast<UINT>(sizeof(kImageFilters) / sizeof(kImageFilters[0]))
			: static_cast<UINT>(sizeof(kFilters) / sizeof(kFilters[0]));
		dlg->SetFileTypes(filterCount, filters);
		dlg->SetFileTypeIndex(1);
		// Start in C:/; fall back to the default folder on failure.
		IShellItem* startDir = nullptr;
		if (SUCCEEDED(SHCreateItemFromParsingName(L"C:\\", nullptr,
				IID_PPV_ARGS(&startDir)))) {
			dlg->SetFolder(startDir);
			startDir->Release();
		}
		if (SUCCEEDED(dlg->Show(nullptr))) {
			IShellItem* item = nullptr;
			if (SUCCEEDED(dlg->GetResult(&item))) {
				PWSTR path = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,
						&path))) {
					const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1,
						nullptr, 0, nullptr, nullptr);
					if (n > 1) {
						std::string utf8(size_t(n) - 1, '\0');
						WideCharToMultiByte(CP_UTF8, 0, path, -1,
							utf8.data(), n, nullptr, nullptr);
						outPath = std::move(utf8);
						ok = true;
					}
					CoTaskMemFree(path);
				}
				item->Release();
			}
		}
		dlg->Release();
	}
	if (coOwns)
		CoUninitialize();
	return ok;
}

// Native Windows file-SAVE dialog (IFileSaveDialog): for choosing an
// output path (e.g., cook output .pak). Starts in content/paks/ with
// a .pak extension filter and a caller-supplied suggested filename.
bool NativeSaveFile(std::string& outPath, const std::string& suggestedName) {
	const HRESULT coInit = CoInitializeEx(nullptr,
		COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	const bool coOwns = SUCCEEDED(coInit);
	if (FAILED(coInit) && coInit != RPC_E_CHANGED_MODE)
		return false;
	bool ok = false;
	IFileSaveDialog* dlg = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
			CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
		static const COMDLG_FILTERSPEC kFilters[] = {
			{ L"Pak files", L"*.pak" },
			{ L"All files", L"*.*" },
		};
		dlg->SetFileTypes(ARRAYSIZE(kFilters), kFilters);
		dlg->SetFileTypeIndex(1);
		dlg->SetDefaultExtension(L"pak");
		// Start in content/paks/ if it exists
		IShellItem* startDir = nullptr;
		wchar_t cwd[MAX_PATH];
		GetCurrentDirectoryW(MAX_PATH, cwd);
		std::wstring startPath = std::wstring(cwd) + L"\\content\\paks";
		if (SUCCEEDED(SHCreateItemFromParsingName(startPath.c_str(), nullptr,
				IID_PPV_ARGS(&startDir)))) {
			dlg->SetFolder(startDir);
			startDir->Release();
		}
		// Suggest the caller's filename (UTF-8 -> wide), defaulting to
		// the historic "scene.pak".
		std::wstring suggest = L"scene.pak";
		if (!suggestedName.empty()) {
			const int wn = MultiByteToWideChar(CP_UTF8, 0,
				suggestedName.c_str(), -1, nullptr, 0);
			if (wn > 1) {
				suggest.resize(size_t(wn));
				MultiByteToWideChar(CP_UTF8, 0, suggestedName.c_str(), -1,
					suggest.data(), wn);
				suggest.pop_back(); // drop the terminator WideCharToMultiByte wrote
			}
		}
		dlg->SetFileName(suggest.c_str());
		if (SUCCEEDED(dlg->Show(nullptr))) {
			IShellItem* item = nullptr;
			if (SUCCEEDED(dlg->GetResult(&item))) {
				PWSTR path = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
					const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1,
						nullptr, 0, nullptr, nullptr);
					if (n > 1) {
						std::string utf8(size_t(n) - 1, '\0');
						WideCharToMultiByte(CP_UTF8, 0, path, -1,
							utf8.data(), n, nullptr, nullptr);
						outPath = std::move(utf8);
						ok = true;
					}
					CoTaskMemFree(path);
				}
				item->Release();
			}
		}
		dlg->Release();
	}
	if (coOwns)
		CoUninitialize();
	return ok;
}

// Native Windows FOLDER picker (IFileOpenDialog + FOS_PICKFOLDERS):
// for choosing a project folder (New / Open Project...). An existing
// project dir just opens; anything else gets scaffolded.
bool NativePickFolder(std::string& outPath) {
	const HRESULT coInit = CoInitializeEx(nullptr,
		COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	const bool coOwns = SUCCEEDED(coInit);
	if (FAILED(coInit) && coInit != RPC_E_CHANGED_MODE)
		return false;
	bool ok = false;
	IFileOpenDialog* dlg = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
			CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
		DWORD opts = 0;
		if (SUCCEEDED(dlg->GetOptions(&opts)))
			dlg->SetOptions(opts | FOS_PICKFOLDERS
				| FOS_FORCEFILESYSTEM);
		dlg->SetTitle(L"Select project folder");
		IShellItem* startDir = nullptr;
		if (SUCCEEDED(SHCreateItemFromParsingName(L"F:\\", nullptr,
				IID_PPV_ARGS(&startDir)))) {
			dlg->SetFolder(startDir);
			startDir->Release();
		}
		if (SUCCEEDED(dlg->Show(nullptr))) {
			IShellItem* item = nullptr;
			if (SUCCEEDED(dlg->GetResult(&item))) {
				PWSTR path = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,
						&path))) {
					const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1,
						nullptr, 0, nullptr, nullptr);
					if (n > 1) {
						std::string utf8(size_t(n) - 1, '\0');
						WideCharToMultiByte(CP_UTF8, 0, path, -1,
							utf8.data(), n, nullptr, nullptr);
						outPath = std::move(utf8);
						ok = true;
					}
					CoTaskMemFree(path);
				}
				item->Release();
			}
		}
		dlg->Release();
	}
	if (coOwns)
		CoUninitialize();
	return ok;
}

} // namespace editor

#else // !_WIN32: zenity + terminal-prompt fallbacks
// ---------------------------------------------------------------------------
// Linux dialogs: `zenity --file-selection` covers the three modes (open /
// save / folder). zenity prints the picked absolute path on stdout (UTF-8,
// already in the engine's string convention) and exits non-zero on cancel,
// so the popen contract maps 1:1. When zenity is missing (server installs,
// minimal containers) the functions degrade to a terminal prompt on stdin.
// ---------------------------------------------------------------------------

namespace {

bool HaveZenity() {
	return std::system("command -v zenity >/dev/null 2>&1") == 0;
}

// Runs zenity with `args` appended to --file-selection; returns false on
// cancel/failure, else the picked path in outPath (trailing newline
// stripped, quotes already handled by zenity itself).
bool RunZenity(std::string& outPath, const char* args) {
	// popen only takes a shell line: the args are repo-controlled
	// constants (no caller-supplied text reaches the command line
	// verbatim; suggested filenames go through the quoted form below).
	std::string cmd = std::string("zenity --file-selection ") + args + " 2>/dev/null";
	FILE* p = popen(cmd.c_str(), "r");
	if (!p)
		return false;
	char buf[4096] = {};
	const size_t n = std::fread(buf, 1, sizeof(buf) - 1, p);
	const int rc = pclose(p);
	if (rc != 0 || n == 0)
		return false;
	buf[n] = '\0';
	std::string path(buf);
	while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
		path.pop_back();
	if (path.empty())
		return false;
	outPath = std::move(path);
	return true;
}

// Single-quote a suggested filename for the shell (zenity --filename=).
std::string ShellQuote(const std::string& s) {
	std::string q = "'";
	for (char c : s) {
		if (c == '\'')
			q += "'\\''";
		else
			q += c;
	}
	q += "'";
	return q;
}

// Terminal fallback (no zenity): type the path by hand.
bool PromptPath(const char* what, std::string& outPath) {
	std::cout << what << " (type path, empty cancels): " << std::flush;
	std::string line;
	if (!std::getline(std::cin, line))
		return false;
	while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
		line.pop_back();
	if (line.empty())
		return false;
	outPath = std::move(line);
	return true;
}

} // namespace (zenity helpers)

namespace editor {

bool NativeOpenFile(std::string& outPath, bool images) {
	const char* filter = images
		? "--file-filter='Images | *.ktx2 *.png *.jpg *.jpeg *.tga *.bmp *.exr *.hdr' "
		  "--file-filter='All files | *'"
		: "--file-filter='Scene files | *.gltf *.glb *.tour' "
		  "--file-filter='All files | *'";
	if (HaveZenity())
		return RunZenity(outPath, filter);
	return PromptPath("Open scene/image file", outPath);
}

bool NativeSaveFile(std::string& outPath, const std::string& suggestedName) {
	const std::string suggest = suggestedName.empty() ? "scene.pak" : suggestedName;
	// --save --confirm-overwrite + the suggested filename pre-filled.
	const std::string args = std::string("--save --confirm-overwrite ")
		+ "--file-filter='Pak files | *.pak' --file-filter='All files | *' "
		+ "--filename=" + ShellQuote(suggest);
	if (HaveZenity())
		return RunZenity(outPath, args.c_str());
	return PromptPath("Save pak", outPath);
}

bool NativePickFolder(std::string& outPath) {
	if (HaveZenity())
		return RunZenity(outPath, "--directory");
	return PromptPath("Project folder", outPath);
}

} // namespace editor
#endif // _WIN32 (native dialogs: Windows shell / Linux zenity)
