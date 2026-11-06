// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// Experimental native Win32 .tour authoring frontend. No SDL3 or Dear ImGui.
// This is NOT feature-equivalent to the existing Vulkan scene editor.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <CommCtrl.h>
#include <CommDlg.h>
#include <Richedit.h>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Shell32.lib")

namespace fs = std::filesystem;
namespace {
constexpr wchar_t kClass[] = L"TourmalineNativeAuthoringExperimental";
constexpr int kTreeWidth = 280;
constexpr int kStatusHeight = 26;
constexpr size_t kMaxFileBytes = 16 * 1024 * 1024;
constexpr unsigned kMaxTreeNodes = 5000;
enum Command : WORD {
    FileNew = 100, FileOpen, FileSave, FileSaveAs, FileReload, FileExit,
    ProjectOpen, ProjectRefresh, EditUndo, EditRedo, EditFindNext,
    ViewWordWrap, HelpAbout
};
enum Control : int { ProjectTree = 1000, SourceEditor, StatusBar, SearchBox };
constexpr wchar_t kFilters[] = L"Tourmaline Scenes (*.tour)\0*.tour\0All Files (*.*)\0*.*\0\0";

std::wstring FromUtf8(const std::string& bytes) {
    if (bytes.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (n == 0) return {};
    std::wstring wide(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
        static_cast<int>(bytes.size()), wide.data(), n);
    return wide;
}
std::string ToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(),
        static_cast<int>(wide.size()), bytes.data(), n, nullptr, nullptr);
    return bytes;
}
std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}
bool IsInterestingFile(const fs::path& path) {
    static constexpr const wchar_t* exts[] = {
        L".tour", L".touranim", L".tourrecipe", L".material", L".texture",
        L".gltf", L".glb", L".obj", L".json", L".hlsl", L".hlsli"
    };
    const auto ext = Lower(path.extension().wstring());
    for (auto* known : exts)
        if (ext == known) return true;
    return false;
}

class NativeEditor {
public:
    bool Create(HINSTANCE instance, const std::wstring& initialProject) {
        instance_ = instance;
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES};
        InitCommonControlsEx(&controls);
        richEditLibrary_ = LoadLibraryW(L"Msftedit.dll");
        if (!richEditLibrary_) return false;

        WNDCLASSEXW klass{};
        klass.cbSize = sizeof(klass);
        klass.hInstance = instance;
        klass.lpszClassName = kClass;
        klass.lpfnWndProc = WindowProc;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassExW(&klass)) return false;

        window_ = CreateWindowExW(0, kClass,
            L"Tourmaline Native Editor [EXPERIMENTAL]",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900,
            nullptr, nullptr, instance, this);
        if (!window_) return false;
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        if (!initialProject.empty()) SetProject(fs::path(initialProject));
        return true;
    }

    int Run() {
        ACCEL shortcuts[] = {
            { FCONTROL | FVIRTKEY, 'N', FileNew },
            { FCONTROL | FVIRTKEY, 'O', FileOpen },
            { FCONTROL | FVIRTKEY, 'S', FileSave },
            { FCONTROL | FVIRTKEY, 'Z', EditUndo },
            { FCONTROL | FVIRTKEY, 'Y', EditRedo },
            { FVIRTKEY, VK_F3, EditFindNext },
            { FVIRTKEY, VK_F5, ProjectRefresh }
        };
        HACCEL accelerators = CreateAcceleratorTableW(shortcuts, ARRAYSIZE(shortcuts));
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (search_ && !IsWindow(search_)) search_ = nullptr;
            if (search_ && IsDialogMessageW(search_, &msg)) continue;
            if (accelerators && TranslateAcceleratorW(window_, accelerators, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (accelerators) DestroyAcceleratorTable(accelerators);
        return static_cast<int>(msg.wParam);
    }
    ~NativeEditor() {
        if (font_) DeleteObject(font_);
        if (richEditLibrary_) FreeLibrary(richEditLibrary_);
    }

private:
    HINSTANCE instance_ = nullptr;
    HMODULE richEditLibrary_ = nullptr;
    HWND window_ = nullptr, tree_ = nullptr, edit_ = nullptr, status_ = nullptr, search_ = nullptr;
    HFONT font_ = nullptr;
    fs::path project_, current_;
    bool dirty_ = false, loading_ = false, wordWrap_ = false;
    std::vector<fs::path> treePaths_;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<NativeEditor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<NativeEditor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->Handle(hwnd, message, wp, lp)
                    : DefWindowProcW(hwnd, message, wp, lp);
    }

    void AddMenu(HMENU menu, UINT id, const wchar_t* label) {
        AppendMenuW(menu, MF_STRING, id, label);
    }
    void BuildMenu() {
        HMENU root = CreateMenu();
        HMENU file = CreatePopupMenu();
        AddMenu(file, FileNew, L"&New Scene\tCtrl+N");
        AddMenu(file, FileOpen, L"&Open Scene...\tCtrl+O");
        AddMenu(file, FileSave, L"&Save\tCtrl+S");
        AddMenu(file, FileSaveAs, L"Save &As...");
        AddMenu(file, FileReload, L"&Reload from Disk");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AddMenu(file, FileExit, L"E&xit");
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
        HMENU project = CreatePopupMenu();
        AddMenu(project, ProjectOpen, L"&Open Project Directory...");
        AddMenu(project, ProjectRefresh, L"&Refresh Asset Browser\tF5");
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(project), L"&Project");
        HMENU edit = CreatePopupMenu();
        AddMenu(edit, EditUndo, L"&Undo\tCtrl+Z");
        AddMenu(edit, EditRedo, L"&Redo\tCtrl+Y");
        AddMenu(edit, EditFindNext, L"&Find Next\tF3");
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"&Edit");
        HMENU view = CreatePopupMenu();
        AddMenu(view, ViewWordWrap, L"Toggle &Word Wrap");
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
        HMENU help = CreatePopupMenu();
        AddMenu(help, HelpAbout, L"&About");
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
        SetMenu(window_, root);
    }
    void Status(const std::wstring& message) {
        if (status_) SetWindowTextW(status_, message.c_str());
    }
    void Title() {
        std::wstring title = L"Tourmaline Native Editor [EXPERIMENTAL] - ";
        title += current_.empty() ? L"(untitled)" : current_.wstring();
        if (dirty_) title += L" *";
        SetWindowTextW(window_, title.c_str());
    }
    void SetDirty(bool value) {
        dirty_ = value;
        Title();
    }
    void Layout() {
        if (!tree_ || !edit_) return;
        RECT client{};
        GetClientRect(window_, &client);
        const int width = std::max(0L, client.right - client.left);
        const int height = std::max(0L, client.bottom - client.top);
        const int status = std::min(kStatusHeight, height);
        const int sidebar = std::min(kTreeWidth, width / 3);
        MoveWindow(tree_, 0, 0, sidebar, height - status, TRUE);
        MoveWindow(edit_, sidebar, 0, width - sidebar, height - status, TRUE);
        MoveWindow(status_, 0, height - status, width, status, TRUE);
    }
    void InitControls() {
        BuildMenu();
        tree_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES |
            TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
            0, 0, 1, 1, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ProjectTree)), instance_, nullptr);
        edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_HSCROLL | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
            0, 0, 1, 1, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(SourceEditor)), instance_, nullptr);
        status_ = CreateWindowExW(0, L"STATIC",
            L"Experimental native text authoring - no Vulkan viewport yet",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            0, 0, 1, 1, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(StatusBar)), instance_, nullptr);
        font_ = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");
        if (font_) {
            SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            SendMessageW(tree_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        }
        SendMessageW(edit_, EM_EXLIMITTEXT, 0, static_cast<LPARAM>(kMaxFileBytes));
        SendMessageW(edit_, EM_SETEVENTMASK, 0, ENM_CHANGE);
        SetDirty(false);
        Layout();
    }
    bool ConfirmDiscard() {
        if (!dirty_) return true;
        const int result = MessageBoxW(window_, L"Save changes before continuing?",
            L"Unsaved Tourmaline scene", MB_YESNOCANCEL | MB_ICONWARNING);
        if (result == IDCANCEL) return false;
        if (result == IDYES) return Save(false);
        return true;
    }
    bool PickFile(bool save, fs::path& result) {
        wchar_t path[32768]{};
        if (!current_.empty()) {
            const auto w = current_.wstring();
            if (w.size() < 32768) wcscpy_s(path, w.c_str());
        }
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = window_;
        ofn.lpstrFilter = kFilters;
        ofn.lpstrFile = path;
        ofn.nMaxFile = 32768;
        ofn.lpstrDefExt = L"tour";
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
            (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return false;
        result = fs::path(path);
        return true;
    }
    bool ReadFile(const fs::path& path, std::wstring& output) {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) return false;
        const auto size = input.tellg();
        if (size < 0 || static_cast<uint64_t>(size) > kMaxFileBytes) return false;
        std::string data(static_cast<size_t>(size), '\0');
        input.seekg(0);
        if (!data.empty() && !input.read(data.data(), static_cast<std::streamsize>(data.size())))
            return false;
        if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
            static_cast<unsigned char>(data[1]) == 0xBB &&
            static_cast<unsigned char>(data[2]) == 0xBF) data.erase(0, 3);
        if (!data.empty() && data.find('\0') != std::string::npos) return false;
        output = FromUtf8(data);
        return data.empty() || !output.empty();
    }
    bool OpenFile(const fs::path& path) {
        if (!ConfirmDiscard()) return false;
        std::wstring contents;
        if (!ReadFile(path, contents)) {
            MessageBoxW(window_, L"Unable to read UTF-8 text (or file is too large).",
                L"Open failed", MB_ICONERROR);
            return false;
        }
        loading_ = true;
        SetWindowTextW(edit_, contents.c_str());
        SendMessageW(edit_, EM_EMPTYUNDOBUFFER, 0, 0);
        loading_ = false;
        current_ = path;
        SendMessageW(edit_, EM_SETMODIFY, FALSE, 0);
        SetDirty(false);
        Status(L"Opened " + path.wstring());
        return true;
    }
    bool Save(bool as) {
        fs::path target = current_;
        if (as || target.empty()) {
            if (!PickFile(true, target)) return false;
        }
        const int n = GetWindowTextLengthW(edit_);
        std::wstring contents(static_cast<size_t>(n) + 1, L'\0');
        GetWindowTextW(edit_, contents.data(), n + 1);
        contents.resize(wcslen(contents.c_str()));
        std::string bytes = ToUtf8(contents);
        if (!contents.empty() && bytes.empty()) return false;
        fs::path temp = target;
        temp += L".tourmaline-writing";
        {
            std::ofstream output(temp, std::ios::binary | std::ios::trunc);
            if (!output) {
                MessageBoxW(window_, L"Unable to create temporary save file.",
                    L"Save failed", MB_ICONERROR);
                return false;
            }
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.flush();
            if (!output) {
                output.close();
                std::error_code ec;
                fs::remove(temp, ec);
                MessageBoxW(window_, L"Unable to write the complete scene.",
                    L"Save failed", MB_ICONERROR);
                return false;
            }
        }
        if (!MoveFileExW(temp.c_str(), target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code ec;
            fs::remove(temp, ec);
            MessageBoxW(window_, L"Unable to replace the target file.",
                L"Save failed", MB_ICONERROR);
            return false;
        }
        current_ = target;
        SendMessageW(edit_, EM_SETMODIFY, FALSE, 0);
        SetDirty(false);
        Status(L"Saved " + target.wstring());
        RefreshTree();
        return true;
    }
    void NewFile() {
        if (!ConfirmDiscard()) return;
        current_.clear();
        loading_ = true;
        SetWindowTextW(edit_, L"// Tourmaline scene\nfileInfo -k \"application\" -v \"tourmaline-tour\";\nfileInfo -k \"product\" -v \"Tourmaline\";\nfileInfo -k \"version\" -v \"1\";\n");
        SendMessageW(edit_, EM_EMPTYUNDOBUFFER, 0, 0);
        loading_ = false;
        SetDirty(true);
        Status(L"New unsaved .tour scene");
    }
    void SetProject(const fs::path& path) {
        std::error_code ec;
        if (!fs::is_directory(path, ec)) {
            MessageBoxW(window_, L"Not a readable directory.", L"Project", MB_ICONERROR);
            return;
        }
        project_ = fs::absolute(path, ec);
        if (ec) project_ = path;
        RefreshTree();
        Status(L"Project: " + project_.wstring());
    }
    void ChooseProject() {
        BROWSEINFOW info{};
        info.hwndOwner = window_;
        info.lpszTitle = L"Select Tourmaline project directory";
        info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&info);
        if (!item) return;
        wchar_t path[MAX_PATH]{};
        const bool ok = SHGetPathFromIDListW(item, path) != FALSE;
        CoTaskMemFree(item);
        if (ok) SetProject(fs::path(path));
    }
    HTREEITEM AddTree(HTREEITEM parent, const fs::path& path, const std::wstring& label) {
        treePaths_.push_back(path);
        const auto index = treePaths_.size() - 1;
        TVINSERTSTRUCTW insert{};
        insert.hParent = parent;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        insert.item.pszText = const_cast<wchar_t*>(label.c_str());
        insert.item.lParam = static_cast<LPARAM>(index);
        return TreeView_InsertItem(tree_, &insert);
    }
    void Populate(const fs::path& dir, HTREEITEM parent, unsigned depth, unsigned& count) {
        if (depth > 12 || count >= kMaxTreeNodes) return;
        std::error_code ec;
        std::vector<fs::directory_entry> entries;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
            !ec && it != end && count < kMaxTreeNodes; it.increment(ec)) {
            const auto& entry = *it;
            if (entry.is_symlink(ec)) continue;
            const bool directory = entry.is_directory(ec);
            if (directory || IsInterestingFile(entry.path())) entries.push_back(entry);
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            std::error_code ecA, ecB;
            const bool ad = a.is_directory(ecA), bd = b.is_directory(ecB);
            if (ad != bd) return ad;
            return Lower(a.path().filename().wstring()) < Lower(b.path().filename().wstring());
        });
        for (const auto& item : entries) {
            if (count >= kMaxTreeNodes) break;
            ++count;
            const auto path = item.path();
            auto node = AddTree(parent, path, path.filename().wstring());
            std::error_code typeError;
            if (item.is_directory(typeError)) Populate(path, node, depth + 1, count);
        }
    }
    void RefreshTree() {
        if (!tree_) return;
        TreeView_DeleteAllItems(tree_);
        treePaths_.clear();
        if (project_.empty()) return;
        auto root = AddTree(TVI_ROOT, project_, project_.filename().wstring());
        unsigned count = 0;
        Populate(project_, root, 0, count);
        TreeView_Expand(tree_, root, TVE_EXPAND);
    }
    void SearchNext() {
        // Simple native text find, deliberately not routed through ImGui.
        wchar_t term[256]{};
        if (!search_) {
            search_ = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"",
                WS_OVERLAPPED | WS_CAPTION | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                CW_USEDEFAULT, CW_USEDEFAULT, 320, 70, window_, nullptr, instance_, nullptr);
            if (search_) {
                SetFocus(search_);
                Status(L"Type search text, then press F3 in the editor");
            }
            return;
        }
        GetWindowTextW(search_, term, 256);
        if (!term[0]) return;
        CHARRANGE selection{};
        SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        FINDTEXTEXW query{};
        query.chrg.cpMin = selection.cpMax;
        query.chrg.cpMax = -1;
        query.lpstrText = term;
        LRESULT found = SendMessageW(edit_, EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&query));
        if (found == -1) {
            query.chrg.cpMin = 0;
            found = SendMessageW(edit_, EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&query));
        }
        if (found != -1) {
            SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&query.chrgText));
            SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
            SetFocus(edit_);
        } else {
            Status(L"No match for: " + std::wstring(term));
        }
    }
    void Execute(WORD command) {
        fs::path path;
        switch (command) {
        case FileNew: NewFile(); break;
        case FileOpen: if (PickFile(false, path)) OpenFile(path); break;
        case FileSave: Save(false); break;
        case FileSaveAs: Save(true); break;
        case FileReload:
            if (!current_.empty() && ConfirmDiscard()) {
                const auto old = current_;
                // Prompt already resolved; OpenFile must not prompt again.
                SetDirty(false);
                OpenFile(old);
            }
            break;
        case ProjectOpen: ChooseProject(); break;
        case ProjectRefresh: RefreshTree(); break;
        case EditUndo: SendMessageW(edit_, EM_UNDO, 0, 0); break;
        case EditRedo: SendMessageW(edit_, EM_REDO, 0, 0); break;
        case EditFindNext: SearchNext(); break;
        case ViewWordWrap:
            wordWrap_ = !wordWrap_;
            SendMessageW(edit_, EM_SETTARGETDEVICE, 0, wordWrap_ ? 0 : 1);
            Status(wordWrap_ ? L"Word wrap enabled" : L"Word wrap disabled");
            break;
        case HelpAbout:
            MessageBoxW(window_,
                L"Native Win32 .tour source editor and project browser.\n"
                L"EXPERIMENTAL: no live Vulkan scene viewport, inspector, or gizmos.\n"
                L"No SDL3 and no Dear ImGui are used by this executable.",
                L"Tourmaline experimental editor", MB_OK | MB_ICONINFORMATION);
            break;
        case FileExit: SendMessageW(window_, WM_CLOSE, 0, 0); break;
        default: break;
        }
    }
    LRESULT Handle(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        switch (message) {
        case WM_CREATE: InitControls(); return 0;
        case WM_SIZE: Layout(); return 0;
        case WM_SETFOCUS: if (edit_) SetFocus(edit_); return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == SourceEditor && HIWORD(wp) == EN_CHANGE) {
                if (!loading_) SetDirty(true);
                return 0;
            }
            if (HIWORD(wp) == 0 || HIWORD(wp) == 1) {
                Execute(LOWORD(wp));
                return 0;
            }
            return 0;
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (hdr && hdr->idFrom == ProjectTree && hdr->code == NM_DBLCLK) {
                const auto selected = TreeView_GetSelection(tree_);
                if (selected) {
                    TVITEMW item{};
                    item.hItem = selected;
                    item.mask = TVIF_PARAM;
                    if (TreeView_GetItem(tree_, &item) && item.lParam >= 0 &&
                        static_cast<size_t>(item.lParam) < treePaths_.size()) {
                        const auto& path = treePaths_[static_cast<size_t>(item.lParam)];
                        std::error_code ec;
                        if (fs::is_regular_file(path, ec) &&
                            Lower(path.extension().wstring()) == L".tour") OpenFile(path);
                    }
                }
                return 0;
            }
            break;
        }
        case WM_KEYDOWN:
            if (wp == VK_F5) { RefreshTree(); return 0; }
            if (wp == VK_F3) { SearchNext(); return 0; }
            break;
        case WM_CLOSE:
            if (!ConfirmDiscard()) return 0;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // Common file dialogs and shell folder pickers are STA clients.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    {
        NativeEditor editor;
        std::wstring project;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; ++i) {
                if (std::wstring_view(argv[i]) == L"--project" && i + 1 < argc)
                    project = argv[++i];
            }
            LocalFree(argv);
        }
        if (editor.Create(instance, project)) result = editor.Run();
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return result;
}
