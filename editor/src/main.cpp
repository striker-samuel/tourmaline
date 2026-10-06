// GltfEditor entry point: inspect + cook glTF scenes.
//
// Usage:
//   Editor [--gltf <path>] [--out-pak <path>]
//
// With no --gltf the editor opens empty (File > Open glTF...).
// Camera: orbit-only (Alt+LMB tumble, Alt+MMB track, Alt+RMB dolly,
// wheel zoom, F / Shift+F framing).
// Selection: left-click a mesh, or pick it in the outliner.

#include "editor_app.h"

#include <Engine/Memory/memory.h>
#include <Engine/debug_log.h> // SetTraceEnabled (mute engine dashboards)

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Crash stack writer (Windows): a vectored exception handler that dumps the
// native call stack of fatal exceptions (AV etc.) to stderr AND
// vulkantoy_fatal.log in the working directory. The editor is a GUI app with
// no console — without this, a first-chance access violation's stack is only
// visible under an attached VS debugger (dev crash-chasing sessions kept
// losing it: the observed device-death crashes surface as CPU-side AVs at
// different offsets per run depending on timing). Symbols need the PDB
// (Debug builds have it); Release still prints module+offset frames.
// The handler only OBSERVES (EXCEPTION_CONTINUE_SEARCH) — the default WER
// crash / VS first-chance break behave exactly as before.
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

namespace {

FILE* g_crashLog = nullptr;

void CrashLogOpen() {
	if (!g_crashLog)
		g_crashLog = std::fopen("vulkantoy_fatal.log", "a");
}

void CrashLogPrintf(const char* fmt, ...) {
	va_list a1, a2;
	va_start(a1, fmt);
	va_copy(a2, a1);
	std::vfprintf(stderr, fmt, a1);
	if (g_crashLog)
		std::vfprintf(g_crashLog, fmt, a2);
	va_end(a1);
	va_end(a2);
}

// No C++ objects with destructors here (SEH + unwinding discipline).
// A VEH runs ON the faulting thread with the faulting stack still in
// place, so CaptureStackBackTrace sees the faulting call chain below a
// few handler frames (skipped).
void DumpStack() {
	void* frames[64]{};
	const WORD n = CaptureStackBackTrace(4, 60, frames, nullptr);
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
	SymInitialize(GetCurrentProcess(), nullptr, TRUE);
	CrashLogOpen();
	CrashLogPrintf("[Crash] native stack (%u frames):\n", (unsigned)n);
	for (WORD i = 0; i < n; ++i) {
		DWORD64 off = 0;
		char symBuf[sizeof(SYMBOL_INFO) + 256]{};
		SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		const char* name = "???";
		if (SymFromAddr(GetCurrentProcess(), (DWORD64)frames[i], &off, sym) && sym->Name[0])
			name = sym->Name;
		DWORD lineOff = 0;
		IMAGEHLP_LINE64 line{};
		line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
		char where[96] = "";
		if (SymGetLineFromAddr64(GetCurrentProcess(), (DWORD64)frames[i], &lineOff, &line))
			std::snprintf(where, sizeof(where), " %s(%lu)", line.FileName, (unsigned long)line.LineNumber);
		HMODULE mod = nullptr;
		char modName[64] = "?";
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				(LPCWSTR)frames[i], &mod) && mod) {
			char buf[64];
			if (GetModuleFileNameA(mod, buf, sizeof(buf))) {
				const char* slash = std::strrchr(buf, '\\');
				std::snprintf(modName, sizeof(modName), "%s", slash ? slash + 1 : buf);
			}
		}
		CrashLogPrintf("[Crash]   #%02u %s!%s+0x%llx%s\n",
			(unsigned)i, modName, name, (unsigned long long)off, where);
	}
	if (g_crashLog) {
		std::fflush(g_crashLog);
		std::fclose(g_crashLog);
		g_crashLog = nullptr;
	}
	std::fflush(stderr);
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
	if (!ep || !ep->ExceptionRecord)
		return EXCEPTION_CONTINUE_SEARCH;
	const DWORD code = ep->ExceptionRecord->ExceptionCode;
	// Only fatal faults (not C++ exceptions 0xE06D7363, not breakpoints
	// under a debugger, not MSVC asserts' fastfail which never get here).
	if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR
		&& code != EXCEPTION_ILLEGAL_INSTRUCTION
		&& code != EXCEPTION_PRIV_INSTRUCTION
		&& code != EXCEPTION_STACK_OVERFLOW
		&& code != 0xC0000409 /* stack buffer overrun / fastfail */
		&& code != 0x80000004 /* SINGLE_STEP: the Dr0 graph write watchpoint */)
		return EXCEPTION_CONTINUE_SEARCH;
	// Hardware watchpoint report (see App::StartGraphWatchdog): Dr0 watches
	// an 8-byte WRITE on RenderGraph+32 (topoHeap_._Myproxy) during the
	// hosted first frame — a trap here is the STOMPER ITSELF: dump the
	// writer's RIP + registers + the written value + the writer's full
	// stack. Dr6 bit 0 (B0) confirms it is the Dr0 breakpoint.
	if (code == 0x80000004 && ep->ContextRecord && (ep->ContextRecord->Dr6 & 1)) {
		const uintptr_t wpAddr = static_cast<uintptr_t>(ep->ContextRecord->Dr0);
		uint64_t written = 0;
		__try { written = *reinterpret_cast<uint64_t*>(wpAddr); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		CrashLogOpen();
		CrashLogPrintf("[Crash] HW WATCHPOINT: WRITE to graph+32 (Dr0=%p) RIP=%p value=0x%llx\n",
			(void*)wpAddr, (void*)ep->ContextRecord->Rip,
			(unsigned long long)written);
		CrashLogPrintf("[Crash] writer context: rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n",
			(unsigned long long)ep->ContextRecord->Rax,
			(unsigned long long)ep->ContextRecord->Rbx,
			(unsigned long long)ep->ContextRecord->Rcx,
			(unsigned long long)ep->ContextRecord->Rdx,
			(unsigned long long)ep->ContextRecord->Rsi,
			(unsigned long long)ep->ContextRecord->Rdi,
			(unsigned long long)ep->ContextRecord->R8,
			(unsigned long long)ep->ContextRecord->R9,
			(unsigned long long)ep->ContextRecord->R10,
			(unsigned long long)ep->ContextRecord->R11,
			(unsigned long long)ep->ContextRecord->R12,
			(unsigned long long)ep->ContextRecord->R13,
			(unsigned long long)ep->ContextRecord->R14,
			(unsigned long long)ep->ContextRecord->R15);
		CrashLogPrintf("[Crash] THE STOMPER'S STACK:\n");
		DumpStack();
		// Keep the graph head evidence: also dump the first 48 bytes.
		{
			uint64_t head[6]{};
			uintptr_t graphBase = wpAddr - 32;
			__try {
				std::memcpy(head, (void*)graphBase, sizeof(head));
				CrashLogPrintf("[Crash] graph head after write: [%llx %llx %llx %llx %llx %llx]\n",
					(unsigned long long)head[0], (unsigned long long)head[1],
					(unsigned long long)head[2], (unsigned long long)head[3],
					(unsigned long long)head[4], (unsigned long long)head[5]);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		if (g_crashLog) {
			std::fflush(g_crashLog);
			std::fclose(g_crashLog);
			g_crashLog = nullptr;
		}
		std::fflush(stderr);
		std::abort();
	}
	CrashLogOpen();
	CrashLogPrintf("[Crash] exception 0x%08lX at %p",
		(unsigned long)code, ep->ExceptionRecord->ExceptionAddress);
	if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
		CrashLogPrintf(" (AV %s at %p)",
			ep->ExceptionRecord->ExceptionInformation[0] == 8 ? "execute"
			: ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
			(void*)ep->ExceptionRecord->ExceptionInformation[1]);
	CrashLogPrintf("\n");
	// Null-call forensics: an EXECUTE at 0 (a call through a null/garbage
	// function pointer) leaves the CALLER's return address at [Rsp] in the
	// faulting context — CaptureStackBackTrace sees almost nothing there
	// (the skip eats the shallow frames), so dump the context's raw stack
	// words with symbols: the first module frame below the fault is the
	// caller of the null call.
	if (ep->ContextRecord) {
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
		SymInitialize(GetCurrentProcess(), nullptr, TRUE);
		uintptr_t* sp = reinterpret_cast<uintptr_t*>(ep->ContextRecord->Rsp);
		for (int i = 0; i < 24; ++i) {
			uintptr_t v = 0;
			__try { v = sp[i]; }
			__except (EXCEPTION_EXECUTE_HANDLER) { break; }
			if (v < 0x10000ull || v > 0x7FFFFFFFFFFFull)
				continue; // not a code-looking address
			DWORD64 off = 0;
			char symBuf[sizeof(SYMBOL_INFO) + 256]{};
			SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
			sym->SizeOfStruct = sizeof(SYMBOL_INFO);
			sym->MaxNameLen = 255;
			const char* name = "???";
			if (SymFromAddr(GetCurrentProcess(), (DWORD64)v, &off, sym) && sym->Name[0])
				name = sym->Name;
			HMODULE mod = nullptr;
			char modName[64] = "?";
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					(LPCWSTR)v, &mod) && mod) {
				char buf[64];
				if (GetModuleFileNameA(mod, buf, sizeof(buf))) {
					const char* slash = std::strrchr(buf, '\\');
					std::snprintf(modName, sizeof(modName), "%s", slash ? slash + 1 : buf);
				}
			}
			CrashLogPrintf("[Crash]   ctx[rsp+%02d] %p %s!%s+0x%llx\n",
				i * 8, (void*)v, modName, name, (unsigned long long)off);
		}
	}
	DumpStack();
	return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashHandler() {
	AddVectoredExceptionHandler(1, &CrashHandler);
}

} // namespace
#endif // _WIN32

namespace {

void PrintUsage(const char* prog) {
	std::cout << "Usage: " << (prog ? prog : "Editor")
		<< " [--gltf <path.gltf>] [--out-pak <path.pak>]\n"
		<< "  Source scenes only: cooked paks run in the App (the editor\n"
		<< "  is the WYSIWYG authoring tool; pak opens are refused).\n"
		<< "  --wysiwyg-smoke: headless-ish crash repro — cooks the scene\n"
		<< "  minimal, exercises the auto-preview + the in-place rebind, then\n"
		<< "  exits (dev tool).\n"
		<< "  --project-smoke <dir>: with --gltf <asset>: create/open the\n"
		<< "  project at <dir>, import the asset, cook it (product step),\n"
		<< "  then run the wysiwyg-smoke stages over it (dev tool).\n";
}

} // namespace

   	int main(int argc, const char* argv[]) {
	// Reserve linear-allocator virtual address space FIRST (see
	// Engine/Memory/memory.h) — before any subsystem init.
	smvk::mem::InitMemory();
	// Mute the periodic engine dashboards ([GfxProbe]/[GfxFilter]
	// view + dispatch, [exposure] record/download — SMVK_TRACE_LOG):
	// the editor's terminal is interactive and these every-N-frame
	// status lines are noise there. The App / bench flows keep them
	// (they are the only per-run visibility into the live pipeline);
	// fault/warning lines stay unconditional here too.
	smvk::SetTraceEnabled(false);
	// Disk pipeline cache OFF in the editor (see impl.cpp's
	// VULKANTOY_PIPELINE_CACHE contract — "off" = no disk seed or save;
	// the in-process pipeline-object dedup still runs): compute-pipeline
	// creation against a driver-seeded cache still crashes the driver on
	// some configurations (the blob validates — same driver/GPU — but
	// the driver faults on its own cached data anyway). The shipped App
	// keeps the cache (startup speed + bench determinism). An explicit
	// user setting wins — this only defaults when unset, so the cache
	// can be re-enabled for A/B runs without a rebuild.
	if (!std::getenv("VULKANTOY_PIPELINE_CACHE")) {
#ifdef _WIN32
		_putenv("VULKANTOY_PIPELINE_CACHE=off");
#else
		setenv("VULKANTOY_PIPELINE_CACHE", "off", 0);
#endif
	}
#ifdef _WIN32
	// Crash stack writer (see the comment block above): FIRST, before any
	// subsystem init, so any boot-time fault is captured too.
	InstallCrashHandler();
#endif
	const char* scenePath = nullptr;
	const char* outPak = nullptr;
	const char* projectSmoke = nullptr;
	bool wysiwygSmoke = false;
	for (int i = 1; i < argc; ++i) {
		const char* a = argv[i];
		if (std::strcmp(a, "--gltf") == 0 && i + 1 < argc) {
			scenePath = argv[++i];
		}
		else if (std::strncmp(a, "--gltf=", 7) == 0) {
			scenePath = a + 7;
		}
		else if (std::strcmp(a, "--out-pak") == 0 && i + 1 < argc) {
			outPak = argv[++i];
		}
		else if (std::strncmp(a, "--out-pak=", 10) == 0) {
			outPak = a + 10;
		}
		else if (std::strcmp(a, "--wysiwyg-smoke") == 0) {
			wysiwygSmoke = true;
		}
		else if (std::strcmp(a, "--project-smoke") == 0 && i + 1 < argc) {
			projectSmoke = argv[++i];
		}
		else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
			PrintUsage(argv[0]);
			return EXIT_SUCCESS;
		}
		else {
			std::cerr << "Unknown argument: " << a << "\n";
			PrintUsage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	editor::EditorApp app;
	if (projectSmoke) {
		// Project flow owns the --gltf path as its import source (it
		// is copied into the project, not opened in place).
		if (!scenePath) {
			std::cerr << "--project-smoke <dir> needs --gltf <asset>\n";
			return EXIT_FAILURE;
		}
		app.SetProjectSmoke(projectSmoke, scenePath);
	}
	else if (scenePath) {
		// Same dispatch as the open dialog (OpenAnySceneFile): .tour
		// files replay through the tour-open path instead of failing the
		// glTF import; also gives headless smoke runs the exact dialog
		// flow (crash repros must not diverge by entry point).
		if (!app.OpenAnySceneFile(scenePath)) {
			std::cerr << "Editor: failed to queue scene '" << scenePath << "'\n";
			return EXIT_FAILURE;
		}
	}
	if (outPak)
		app.SetOutPak(outPak);
	if (wysiwygSmoke)
		app.SetWysiwygSmoke();
	try {
		if (!app.Run())
			return EXIT_FAILURE;
	}
	catch (const std::exception& e) {
		std::cerr << "\nERROR: " << e.what() << std::endl;
		std::cerr << "\nPress Enter to exit...";
		std::cin.get();
		return EXIT_FAILURE;
	}
	smvk::mem::ShutdownMemory(); // footprint report (used/committed/reserved)
	return EXIT_SUCCESS;
}
