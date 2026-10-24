#!/usr/bin/env bash
# install_dependencies.sh - fetches the non-submodule third-party trees into
# external/. Same pins as install_dependencies.ps1 (see that file for the full
# commentary) EXCEPT the platform binaries, which each script owns its side
# of: here SDL3 arrives as the 3.4.16 SOURCE zip (the root CMakeLists builds
# it as a subproject when no system SDL3 is found) and DXC as the official
# Linux package (bin/dxc + lib/libdxcompiler.so); the Windows prebuilts live
# in the .ps1. The only skipped package is Streamline (the DLSS/Reflex
# runtime set is Windows-only; the engine reports DLSS unavailable here).
#
# Every dep is attempted even if an earlier one fails; failed deps are
# summarized at the end and the script exits non-zero. (The install functions
# carry explicit error returns for this - `|| fail` wrapping disables `set -e`
# inside the callee, so nothing may rely on errexit to abort mid-dep and
# write a .depstamp for a half-installed tree.)
#
# Usage:
#   ./install_dependencies.sh
#   ./install_dependencies.sh --force
#   ./install_dependencies.sh --include-unused   # box3d + zlib (not wired in)

set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
EXT="$ROOT/external"
DEPS="$ROOT/tools/deps"
CACHE="$EXT/.downloads"
FORCE=0
INCLUDE_UNUSED=0
FAILED_DEPS=""

for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        --include-unused) INCLUDE_UNUSED=1 ;;
        *) echo "unknown option: $arg (use --force / --include-unused)"; exit 1 ;;
    esac
done

mkdir -p "$CACHE"

sha_of() { sha256sum "$1" | cut -d' ' -f1; }

download() { # url dst expected_sha
    local url="$1" dst="$2" expected="$3"
    if [ -f "$dst" ] && [ "$(sha_of "$dst")" = "$expected" ]; then
        echo "  cached: $(basename "$dst")"
        return 0
    fi
    echo "  downloading: $url"
    # -sS: silent except errors (matches the ps1's quiet Invoke-WebRequest)
    curl -fL -sS --retry 3 -o "$dst" "$url"
    local actual
    actual="$(sha_of "$dst")"
    if [ "$actual" != "$expected" ]; then
        echo "  sha256 mismatch for $(basename "$dst"): got $actual" >&2
        return 1
    fi
}

fail() { # name  - record a failed dep; the summary at the end exits non-zero
    echo "[$1] FAILED (see above)" >&2
    FAILED_DEPS="$FAILED_DEPS $1"
}

# copy_tree <src> <dst> <subset(space-sep, may be empty)> <drop(space-sep)> <exclude-manifest> <overlay-manifest>
copy_tree() {
    local src="$1" dst="$2" subset="$3" drop="$4" exclude_file="$5" overlay_file="$6"
    local copied=0 skipped=0
    if [ ! -d "$src" ]; then
        echo "  source tree missing: $src" >&2
        return 1
    fi
    while IFS= read -r rel; do
        # rel path (forward slashes) of every file in the source tree
        local excluded=0 dropped=0
        if [ -n "$exclude_file" ] && [ -f "$exclude_file" ]; then
            while IFS= read -r ex; do
                [ "$rel" = "$ex" ] && excluded=1
                case "$rel" in "$ex"/*) excluded=1 ;; esac
                [ "$excluded" = 1 ] && break
            done < "$exclude_file"
        fi
        for d in $drop; do
            [ "$rel" = "$d" ] && dropped=1
            case "$rel" in "$d"/*) dropped=1 ;; esac
        done
        if [ "$excluded" = 1 ] || [ "$dropped" = 1 ]; then skipped=$((skipped+1)); continue; fi
        # tracked overlay file - git owns it; the fetch NEVER writes it, even
        # when absent (git checkout restores a wiped tree, the fetch must not
        # re-import upstream over our pinned edits)
        if [ -n "$overlay_file" ] && [ -f "$overlay_file" ] && grep -qxF "$rel" "$overlay_file"; then
            skipped=$((skipped+1)); continue
        fi
        if [ "$subset" != "*" ]; then
            local top="${rel%%/*}"
            case " $subset " in
                *" $rel "*) ;;                       # exact file entry
                *" $top "*) ;;                        # top-level dir entry
                *) skipped=$((skipped+1)); continue ;;  # not in the subset
            esac
        fi
        mkdir -p "$dst/$(dirname "$rel")" || return 1
        cp "$src/$rel" "$dst/$rel" || return 1
        copied=$((copied+1))
    done < <(cd "$src" && find . -type f | sed 's|^\./||')
    echo "  copied $copied file(s), skipped $skipped (excluded/overlay/subset)"
}

install_archive() { # name target type url sha subset drop exclude-manifest overlay-manifest into
    local name="$1" target="$2" type="$3" url="$4" sha="$5" subset="$6" drop="$7" excl="$8" over="$9" into="${10}"
    local target_dir="$EXT/$target"
    local stamp="$target_dir/.depstamp-$name"
    local stamp_id="$url|$sha"
    if [ "$FORCE" = 0 ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$stamp_id" ]; then
        echo "[$name] up to date"
        return 0
    fi
    echo "[$name] installing into external/$target/"
    mkdir -p "$target_dir"

    local archive="$CACHE/$name.archive"
    download "$url" "$archive" "$sha" || return 1

    local extract="$CACHE/extract-$name"
    rm -rf "$extract"; mkdir -p "$extract"

    if [ "$type" = tar ]; then
        mkdir -p "$extract/t"
        # exit codes 1/2 are warnings: upstream tarballs carry symlinks that
        # some tar builds cannot create - those trees are usually excluded
        # from the copy anyway. Anything above 2 is a real failure.
        tar -xzf "$archive" -C "$extract/t" --strip-components=1
        local rc=$?
        if [ "$rc" -gt 2 ]; then
            echo "  tar extraction failed for $name (exit $rc)" >&2
            return 1
        fi
        local tree="$extract/t"
    else
        if command -v unzip >/dev/null 2>&1; then
            unzip -q "$archive" -d "$extract/x" || return 1
        elif command -v python3 >/dev/null 2>&1; then
            python3 -m zipfile -e "$archive" "$extract/x" || return 1
        else
            echo "  need unzip or python3 to extract $name" >&2
            return 1
        fi
        # single wrapper dir (e.g. SDL3-3.4.16/) -> strip it
        local tree="$extract/x"
        local entries
        entries="$(ls -A "$extract/x")"
        if [ "$(echo "$entries" | wc -l)" = 1 ] && [ -d "$extract/x/$entries" ]; then
            tree="$extract/x/$entries"
        fi
    fi

    # a missing subset entry means the pin is stale (wrong archive variant,
    # upstream renamed something) - fail the dep instead of stamping a
    # partial tree as installed
    if [ "$subset" != "*" ]; then
        local e
        for e in $subset; do
            if [ ! -e "$tree/$e" ]; then
                echo "  subset entry missing in archive: $e (stale pin?)" >&2
                return 1
            fi
        done
    fi

    local dst="$target_dir"
    [ -n "$into" ] && dst="$target_dir/$into"
    copy_tree "$tree" "$dst" "$subset" "$drop" "$excl" "$over" || return 1
    echo "$stamp_id" > "$stamp"
    echo "[$name] done"
}

install_raw() { # name target file url sha
    local name="$1" target="$2" file="$3" url="$4" sha="$5"
    local target_dir="$EXT/$target"
    local stamp="$target_dir/.depstamp-$name"
    local stamp_id="$url|$sha"
    # the stamp alone is not enough - the target file must also exist (a
    # manually deleted file with a valid stamp would otherwise report
    # "up to date" forever and starve the build)
    if [ "$FORCE" = 0 ] && [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$stamp_id" ] && [ -f "$target_dir/$file" ]; then
        echo "[$name] up to date"
        return 0
    fi
    echo "[$name] installing external/$target/$file"
    # the file may land in a subdir of the target (e.g. dxc/include/dxc/)
    mkdir -p "$target_dir/$(dirname "$file")"
    download "$url" "$target_dir/$file" "$sha" || return 1
    echo "$stamp_id" > "$stamp"
    echo "[$name] done"
}

MAN="$DEPS"   # manifests dir

# --- raw single-file deps ---
install_raw VMA         VulkanMemoryAllocator vk_mem_alloc.h \
    'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/68d1c9733f56f681613d6b3d728c1af4dca8c2b6/include/vk_mem_alloc.h' \
    af2e8ed9cabc33bd5a027d480aaac14309aad741763427fa39ffdcf98561c52b || fail VMA
install_raw stb-image   stb stb_image.h \
    'https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h' \
    594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3 || fail stb-image
install_raw stb-write   stb stb_image_write.h \
    'https://raw.githubusercontent.com/nothings/stb/1ee679ca2ef753a528db5ba6801e1067b40481b8/stb_image_write.h' \
    cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05 || fail stb-write
install_raw tinygltf   tinygltf tiny_gltf.h \
    'https://raw.githubusercontent.com/syoyo/tinygltf/v2.8.10/tiny_gltf.h' \
    777b94a0d7b4876b521cea53630a5bbd55aff693ef49586a55fb273278e96bf5 || fail tinygltf
install_raw lz4-h       lz4 lz4.h \
    'https://raw.githubusercontent.com/lz4/lz4/v1.10.0/lib/lz4.h' \
    26b82efc53d1570f3b54eef02e9c4764c1ad374ff03cac04e2ced5ea4d4c552f || fail lz4-h
install_raw lz4-c       lz4 lz4.c \
    'https://raw.githubusercontent.com/lz4/lz4/v1.10.0/lib/lz4.c' \
    9396f7de527bc8435de9c7569fb7998e56545a84b4f3c2d808c0235c01774539 || fail lz4-c

# --- sqlite3 amalgamation into external/SQLiteCpp/sqlite3/ (the editor DB) ---
install_archive sqlite-amalgamation SQLiteCpp zip \
    'https://sqlite.org/2026/sqlite-amalgamation-3530400.zip' \
    1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d \
    'sqlite3.c sqlite3.h' '' '' '' sqlite3 || fail sqlite-amalgamation

# --- source-tree deps (tarballs at pinned commits, exclude + overlay manifests) ---
install_archive SQLiteCpp SQLiteCpp tar \
    'https://codeload.github.com/SRombauts/SQLiteCpp/tar.gz/8a63915c06988988b8f20b434fcb34666ab72e38' \
    ae81b10191346114fbeb4b937943f65accf8647050426ae71dd0e33973acb144 \
    '*' '' "$MAN/SQLiteCpp.exclude" "$MAN/SQLiteCpp.overlay" '' || fail SQLiteCpp
install_archive embree embree tar \
    'https://codeload.github.com/embree/embree/tar.gz/3d9cb89b9ea099c630e6272d37767e7dd4e78e74' \
    65a7d6014f546844a69201ff1ed0e892c49aa74171db8a7eeffa86f6ef0be78f \
    '*' '' "$MAN/embree.exclude" "$MAN/embree.overlay" '' || fail embree
install_archive optick optick tar \
    'https://codeload.github.com/bombomby/optick/tar.gz/362d3952d35982f2037e4a74df4706486d590cfe' \
    771582d71050630c76aecb1a759443eeb902cef3a16620b8167e5223cd892466 \
    '*' '' "$MAN/optick.exclude" "$MAN/optick.overlay" '' || fail optick
install_archive imgui imgui tar \
    'https://codeload.github.com/ocornut/imgui/tar.gz/1c544ee941' \
    d56a25a7b0594e391b79e7d3cb47a46cb87387de20a8dd1b6dd28b3b7def4dc7 \
    '*' '' "$MAN/imgui.exclude" "$MAN/imgui.overlay" '' || fail imgui
install_archive xatlas xatlas tar \
    'https://codeload.github.com/jpcy/xatlas/tar.gz/1f101e2983b0bb91f5cebff80306d0ee8710f11e' \
    5c7c3dfff9e40e32c31f1ecc3e8f6af376545008a03dfdb1fddbf5491f1b4dcb \
    '*' '' "$MAN/xatlas.exclude" "$MAN/xatlas.overlay" '' || fail xatlas

# --- Vulkan-Headers (Linux): the full modern set --------------------------------
# Several engine symbols are 1.4-era while distro libvulkan-dev packages
# ship older trimmed headers (Ubuntu 24.04: 1.3.275). This pins the same
# Vulkan-Headers the Windows SDK uses (1.4.357); the root CMakeLists puts
# them ahead of the system headers (the LOADER stays the distro's -
# backwards compatible). Windows uses its SDK's own headers.
install_archive vulkan-headers vulkan-headers tar \
    'https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/v1.4.357' \
    7dc0dbcf1d49dd3d7da3761c251c6097dfbaac475321a4a8a99269d3d5abecdc \
    'include' '' '' '' '' || fail vulkan-headers

# --- SDL3 (Linux): the 3.4.16 SOURCE tree ----------------------------------
# A system/distro SDL3 install wins in the build (find_package; see the SDL3
# block in CMakeLists.txt) - this fetched tree is the fallback the root
# CMakeLists builds as a subproject when no SDL3 is found. The .exclude
# manifest trims the non-CMake platform folders out of the copy.
install_archive SDL3 SDL3-3.4.16 zip \
    'https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.zip' \
    5f399fdfbc040169ef4418a3cb99ccc5a0641681f7ccc1da66eeec3c97fa4076 \
    '*' '' "$MAN/SDL3.exclude" '' '' || fail SDL3

# --- DXC (Linux): the official linux package at the same release as the
# Windows pin. The subset keeps only what the build uses - bin/dxc (the
# build-time HLSL->SPIR-V compiler; it rpath-resolves ../lib) and the two
# .so's the engine links (libdxcompiler.so is the in-process compiler,
# libdxil.so its validator companion). The archive also carries the whole
# LLVM build tree under lib/ (CMakeFiles/, .o intermediates, static libs)
# which nothing here links - the subset skips it. The two API headers come
# from the release tag (the linux package ships no include/dxc).
install_raw dxc-api-h  dxc include/dxc/dxcapi.h \
    'https://raw.githubusercontent.com/microsoft/DirectXShaderCompiler/v1.9.2607/include/dxc/dxcapi.h' \
    a8d409642ded485ea4fd4c055f1199a13538f0393755c2d958ade281afed4ca0 || fail dxc-api-h
install_raw dxc-api-wa dxc include/dxc/WinAdapter.h \
    'https://raw.githubusercontent.com/microsoft/DirectXShaderCompiler/v1.9.2607/include/dxc/WinAdapter.h' \
    f5688a1408a8de8c0c35176bc900f21d7679d492215da94da4ab643cb66867f4 || fail dxc-api-wa
install_archive dxc dxc tar \
    'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/linux_dxc_2026_07_29.x86_x64.tar.gz' \
    55665c87824051ed4774ff3280a79ccbbb7d39243b9736ca5e98222134112d54 \
    'bin/dxc lib/libdxcompiler.so lib/libdxil.so LICENCE-MIT.txt LICENSE-LLVM.txt LICENSE-MS.txt' \
    '' '' '' '' || fail dxc

# --- DirectX-Headers (Linux): DirectXTex's non-Win32 path includes
# microsoft/DirectX-Headers' wsl/winadapter.h + directx/d3d12.h (the same
# headers Texconv's Linux build consumes). Windows takes DirectXTex's own
# _WIN32 path, so the pin lives here only.
install_archive directx-headers directx-headers tar \
    'https://codeload.github.com/microsoft/DirectX-Headers/tar.gz/v1.619.5' \
    24a0b7d8079a2dbbc90753c0d8bc812040d052acce2302e69a97c5d873b313b8 \
    'include' '' '' '' '' || fail directx-headers
# DirectXMath (DirectXTex's non-Win32 path also includes DirectXPackedVector.h;
# the headers live under Inc/ in the tarball)
install_archive directxmath directxmath tar \
    'https://codeload.github.com/microsoft/DirectXMath/tar.gz/jun2026' \
    cb46002679b16a25fd11d0a2a13f6a7c7bf9c51bf04258fb5485e5b839da8474 \
    'Inc' '' '' '' '' || fail directxmath
# SAL header for DirectXMath on non-Windows: the same source Microsoft's
# own DirectXMath WSL CI uses (dotnet/runtime's PAL sal.h - see their
# .azuredevops WSL pipeline).
install_raw sal directxmath Inc/sal.h \
    'https://raw.githubusercontent.com/dotnet/runtime/v8.0.1/src/coreclr/pal/inc/rt/sal.h' \
    7dae281adc3a09a691291fb90526f05e4f9ef8b16d7f33d716ba690f7241a492 || fail sal

# --- Windows-only binary packages (skipped here) ---
echo "[streamline] skipped (Windows-only DLSS/Reflex runtimes; the engine reports DLSS unavailable on this platform)"

# --- intentionally unused, opt-in ---
if [ "$INCLUDE_UNUSED" = 1 ]; then
    install_archive box3d box3d tar \
        'https://codeload.github.com/erincatto/box3d/tar.gz/954cf879e717334eb96da9a5c255788fdc9f5f87' \
        d9f15a080708120d4c9dfcc589bfb629f999646402c604fe27d2d4d1f8266504 \
        '*' '' '' '' '' || fail box3d
    install_archive zlib zlib tar \
        'https://codeload.github.com/madler/zlib/tar.gz/51b2667cb4e02a583a8be55abd995421dccbfec0' \
        bbe3653640497bde28c86c99ca33bbdb38a3b3ef15bd99905b97c8b2863ac80a \
        '*' '' '' '' '' || fail zlib
else
    echo "[box3d] skipped (not wired into the build; pass --include-unused)"
    echo "[zlib]  skipped (not wired into the build; pass --include-unused)"
fi

if [ -n "$FAILED_DEPS" ]; then
    echo >&2
    echo "Failed deps:$FAILED_DEPS" >&2
    exit 1
fi

echo "All fetchable dependencies installed."
echo "System prerequisites NOT fetched here: the Vulkan loader/headers"
echo "(libvulkan-dev or the Vulkan SDK), glslc for any GLSL sources, and KTX"
echo "(KTX-Software - build/install it and point KTX_ROOT at the prefix)."
