// Native Windows file-open dialog (IFileOpenDialog): the single "Open
// file" entry point for scenes (.gltf/.glb), cooked paks (.pak) and
// tours (.tour). Starts in C:/ with an extension filter; the picked
// path feeds OpenAnySceneFile, which dispatches by extension (or
// ImportAsset for project imports). Replaces the old custom ImGui
// browser (one dialog for every open type now).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shobjidl.h> // IFileOpenDialog

#include <string>

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
			{ L"Scene files", L"*.gltf;*.glb;*.pak;*.tour" },
			{ L"glTF scenes", L"*.gltf;*.glb" },
			{ L"Cooked paks", L"*.pak" },
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
// a .pak extension filter.
bool NativeSaveFile(std::string& outPath) {
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
		// Suggest a default name
		dlg->SetFileName(L"scene.pak");
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
