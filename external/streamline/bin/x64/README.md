# Streamline runtime staging dir

The Streamline 2.14.1 runtime DLLs vendored here (staged next to `App.exe`
by CMake's `tourmaline_stage_streamline`, see the root `CMakeLists.txt`):

- `sl.interposer.dll` - the Streamline interposer (loaded dynamically from
  the exe dir, `engine/src/Renderer/dlss.cpp`)
- `sl.common.dll` - Streamline common/plugin infrastructure
- `sl.dlss.dll` - the DLSS-SR feature plugin
- `NvLowLatencyVk.dll` - Reflex low-latency (PCL markers)
- NVIDIA license texts: `reflex.license.txt`, `nvngx_dlss.license.txt`

## NOT here: `nvngx_dlss.dll`

The NGX DLSS snippet binary (~56 MB) is deliberately **not tracked by git**
(size + NVIDIA redistribution terms - see `THIRD_PARTY_NOTICES.md`).

To enable DLSS-SR locally, fetch `nvngx_dlss.dll` from the NVIDIA
Streamline SDK / DLSS SDK (NVIDIA Developer Program, free login -
developer.nvidia.com) and drop it into this directory. The next build
stages it next to `App.exe` automatically.

**Missing it is fine** (verified headless, sponza bench with
`TOURMALINE_DLSS=perf`): `[DLSS] feature not loaded (result=31 loaded=0)`,
the engine renders at native extent (`render extent 1920x1080 -> swap
1920x1080`), Reflex/PCL markers stay active, clean exit 0. CMake configure
just warns: "Streamline binary 'nvngx_dlss.dll' not found ... DLSS will be
unavailable at runtime".
