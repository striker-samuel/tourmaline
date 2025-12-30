// spak_vfs.h
#pragma once

// Option B: String table (path blob) + entry table + data blob.
// - No fixed 256-char path limit
// - VFS-style lookups by normalized path: "/scene1/meshes/mesh1.mesh"
// - Supports "mount points" by letting you prefix paths at mount time (optional)
//
// NOTE: This is intentionally "boring C++" and header-only-ish for easy integration.
//       You can split into .cpp later.

#include <cstdint>
#include <cstring>
#include <cassert>

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <algorithm>

namespace spak {

    // ----------------------------
    // Versioning / flags
    // ----------------------------
#ifndef SPAK_FILE_VERSION
#define SPAK_FILE_VERSION 1
#endif

    enum PAKFLAGS : uint32_t {
        SPAK_COMPRESSED = 1u << 0, // reserved (not implemented here)
        SPAK_STREAMED = 1u << 1, // reserved
    };

    enum ENTRYFLAGS : uint32_t {
        ENTRY_COMPRESSED = 1u << 0, // reserved
    };

    // ----------------------------
    // Low-level on-disk structs
    // ----------------------------
#pragma pack(push, 1)

    struct PakHeader
    {
        char     magic[4];        // "SPAK"
        uint8_t  pak_version;     // format version of the pak (SPAK_FILE_VERSION)
        uint8_t  content_version; // your pipeline "content version" (bump when runtime format changes)
        uint16_t reserved;

        uint32_t flags;           // pak-level flags
        uint32_t pak_id;          // optional ID (0 ok)

        uint32_t num_entries;

        uint64_t entry_table_offset; // file offset to entry table
        uint64_t path_blob_offset;   // file offset to path blob (string table)
        uint64_t data_blob_offset;   // file offset to data blob (file contents)
        uint64_t path_blob_size;     // bytes
        uint64_t data_blob_size;     // bytes
    };

    // Logical ID (optional)
    struct Uuid
    {
        uint8_t bytes[16];
    };

    struct PakEntry
    {
        Uuid     uuid;             // optional (all zeros allowed)
        uint32_t type;             // optional: Mesh, Texture, Audio, ...
        uint32_t flags;            // ENTRYFLAGS

        uint64_t data_offset;      // offset into data blob (NOT file offset)
        uint32_t uncompressed_size;
        uint32_t stored_size;      // == uncompressed_size if not compressed

        uint64_t path_offset;      // offset into path blob
        uint32_t path_size;        // bytes (not null-terminated)
        uint32_t path_hash;        // cheap hash to speed up comparisons (not security)
    };

#pragma pack(pop)

    // ----------------------------
    // Utilities
    // ----------------------------

    // Simple 32-bit FNV-1a hash for paths.
    // (Good enough for lookup acceleration; you still compare the actual string.)
    inline uint32_t fnv1a32(std::string_view s) {
        uint32_t h = 2166136261u;
        for (unsigned char c : s) {
            h ^= uint32_t(c);
            h *= 16777619u;
        }
        return h;
    }

    // Normalize to a VFS-ish path:
    // - backslashes -> forward slashes
    // - ensure leading '/'
    // - collapse repeated slashes
    // - resolve '.' and '..' segments (purely lexically)
    // - do NOT touch case by default (case-sensitive VFS)
    inline std::string NormalizeVfsPath(std::string_view in)
    {
        std::string p;
        p.reserve(in.size() + 1);

        // 1) Copy + slash normalize
        for (char ch : in) {
            p.push_back(ch == '\\' ? '/' : ch);
        }

        // 2) Trim whitespace edges (optional; keep minimal)
        while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
        while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();

        // 3) Ensure leading slash
        if (p.empty() || p.front() != '/') p.insert(p.begin(), '/');

        // 4) Split into segments and resolve '.'/'..'
        std::vector<std::string_view> segs;
        segs.reserve(16);

        // We’ll scan p and create views, then build output from a stack of views.
        auto is_sep = [](char c) { return c == '/'; };

        size_t i = 0;
        while (i < p.size()) {
            while (i < p.size() && is_sep(p[i])) i++;         // skip separators
            size_t start = i;
            while (i < p.size() && !is_sep(p[i])) i++;        // read segment
            size_t end = i;
            if (end > start) {
                segs.emplace_back(p.data() + start, end - start);
            }
        }

        // stack of "kept" segments
        std::vector<std::string_view> stack;
        stack.reserve(segs.size());

        for (auto s : segs) {
            if (s == ".") continue;
            if (s == "..") {
                if (!stack.empty()) stack.pop_back();
                continue;
            }
            stack.push_back(s);
        }

        // 5) Rebuild normalized
        std::string out;
        out.reserve(p.size());

        out.push_back('/');
        for (size_t si = 0; si < stack.size(); ++si) {
            if (si) out.push_back('/');
            out.append(stack[si].data(), stack[si].size());
        }

        // 6) Remove trailing slash (except root)
        if (out.size() > 1 && out.back() == '/') out.pop_back();

        return out;
    }

    // Optional helper: combine mount point and asset-relative path.
    // mount="/scene1", path="meshes/a.mesh" => "/scene1/meshes/a.mesh"
    inline std::string JoinMount(std::string_view mount, std::string_view path)
    {
        std::string m = NormalizeVfsPath(mount);
        std::string p = NormalizeVfsPath(path);

        if (m == "/") return p;
        if (p == "/") return m;

        // p begins with '/', m ends without '/'
        return m + p;
    }

    // Optional UUID from string (non-cryptographic).
    inline Uuid MakeUuidFromString(const std::string& s)
    {
        Uuid u{};
        // Fill 16 bytes using two 64-bit std::hash values.
        std::hash<std::string> h;
        uint64_t h1 = (uint64_t)h(s);
        uint64_t h2 = (uint64_t)h(s + "#2");
        std::memcpy(u.bytes + 0, &h1, 8);
        std::memcpy(u.bytes + 8, &h2, 8);
        return u;
    }

    // ----------------------------
    // Writer
    // ----------------------------
    struct WriterEntry
    {
        PakEntry entry{};
        std::string path_norm; // we keep a copy while building
    };

    class PakWriter
    {
    public:
        // Add a blob with a VFS path.
        // - vfs_path: "/scene1/meshes/mesh1.mesh" (anything; we normalize)
        // - type: optional type ID
        // - uuid: optional (use zeros or generate from path)
        // - flags: ENTRYFLAGS
        void addFromMemory(std::string_view vfs_path,
            const void* data,
            uint32_t size,
            uint32_t type = 0,
            uint32_t flags = 0
        )
        {
            if (!data && size > 0) throw std::runtime_error("addFromMemory: data is null");

            WriterEntry we{};
            we.path_norm = NormalizeVfsPath(vfs_path);
            we.entry.uuid = MakeUuidFromString(we.path_norm);
            we.entry.type = type;
            we.entry.flags = flags;

            we.entry.data_offset = (uint64_t)m_data_blob.size();
            we.entry.uncompressed_size = size;
            we.entry.stored_size = size; // no compression in this sample

            // Copy bytes to data blob
            const char* p = reinterpret_cast<const char*>(data);
            m_data_blob.insert(m_data_blob.end(), p, p + size);

            // path_offset/path_size/path_hash filled during finalize
            m_entries.push_back(std::move(we));
        }

        // Convenience: load file from disk and store under vfs_path (or same path)
        void addFile(std::string_view vfs_path, const std::filesystem::path& disk_path, uint32_t type = 0, uint32_t flags = 0)
        {
            std::ifstream in(disk_path, std::ios::binary | std::ios::ate);
            if (!in) throw std::runtime_error("addFile: failed to open " + disk_path.string());

            const std::streamsize size = in.tellg();
            in.seekg(0, std::ios::beg);

            std::vector<char> buf((size_t)size);
            if (size > 0) {
                in.read(buf.data(), size);
                if (!in) throw std::runtime_error("addFile: failed read " + disk_path.string());
            }

            addFromMemory(vfs_path, buf.data(), (uint32_t)buf.size(), type, flags);
        }

        // Finalize and write pak to disk.
        // content_version: your pipeline version for the runtime content
        void writeToFile(const std::filesystem::path& out_path,
            uint8_t content_version = 1,
            uint32_t pak_flags = 0,
            uint32_t pak_id = 0)
        {
            // Build path blob + fill per-entry path offsets
            buildPathBlobAndFinalizeEntries();

            // Pack header + tables layout:
            // [Header]
            // [EntryTable: PakEntry[num_entries]]
            // [PathBlob: bytes]
            // [DataBlob: bytes]
            PakHeader hdr{};
            std::memcpy(hdr.magic, "SPAK", 4);
            hdr.pak_version = (uint8_t)SPAK_FILE_VERSION;
            hdr.content_version = content_version;
            hdr.reserved = 0;

            hdr.flags = pak_flags;
            hdr.pak_id = pak_id;
            hdr.num_entries = (uint32_t)m_entries.size();

            hdr.entry_table_offset = sizeof(PakHeader);
            hdr.path_blob_offset = hdr.entry_table_offset + uint64_t(hdr.num_entries) * sizeof(PakEntry);
            hdr.path_blob_size = (uint64_t)m_path_blob.size();
            hdr.data_blob_offset = hdr.path_blob_offset + hdr.path_blob_size;
            hdr.data_blob_size = (uint64_t)m_data_blob.size();

            std::ofstream out(out_path, std::ios::binary);
            if (!out) throw std::runtime_error("Failed to open pak output: " + out_path.string());

            // Write header
            out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

            // Write entry table
            for (const auto& we : m_entries) {
                out.write(reinterpret_cast<const char*>(&we.entry), sizeof(PakEntry));
            }

            // Write path blob
            if (!m_path_blob.empty()) {
                out.write(m_path_blob.data(), (std::streamsize)m_path_blob.size());
            }

            // Write data blob
            if (!m_data_blob.empty()) {
                out.write(m_data_blob.data(), (std::streamsize)m_data_blob.size());
            }

            if (!out) throw std::runtime_error("Failed while writing pak: " + out_path.string());
        }

    private:
        void buildPathBlobAndFinalizeEntries()
        {
            // We’ll store each path as raw bytes + '\0' for convenience.
            // entry.path_size excludes the '\0'.
            m_path_blob.clear();
            m_path_blob.reserve(m_entries.size() * 64);

            // Optional: stable order.
            // Keeping entries sorted by path helps debugging and makes directory listing easier.
            std::sort(m_entries.begin(), m_entries.end(),
                [](const WriterEntry& a, const WriterEntry& b) { return a.path_norm < b.path_norm; });

            for (auto& we : m_entries) {
                const uint64_t off = (uint64_t)m_path_blob.size();
                const uint32_t len = (uint32_t)we.path_norm.size();

                we.entry.path_offset = off;
                we.entry.path_size = len;
                we.entry.path_hash = fnv1a32(std::string_view(we.path_norm));

                m_path_blob.insert(m_path_blob.end(), we.path_norm.begin(), we.path_norm.end());
                m_path_blob.push_back('\0');
            }
        }

    private:
        std::vector<WriterEntry> m_entries;
        std::vector<char>        m_path_blob;
        std::vector<char>        m_data_blob;
    };

    // ----------------------------
    // Reader
    // ----------------------------
    class PakReader
    {
    public:
        explicit PakReader(const std::filesystem::path& path)
            : m_path(path)
        {
            m_stream.open(path, std::ios::binary);
            if (!m_stream) throw std::runtime_error("Failed to open pak: " + path.string());

            readHeader();
            readEntries();
            readPathBlob();
            buildIndex(); // build path->entry index
        }

        const PakHeader& header() const { return m_header; }

        // Existence check by VFS path
        bool exists(std::string_view vfs_path) const {
            auto key = NormalizeVfsPath(vfs_path);
            return m_index.find(key) != m_index.end();
        }

        // Get metadata (pointer to PakEntry) or nullptr
        const PakEntry* find(std::string_view vfs_path) const {
            auto key = NormalizeVfsPath(vfs_path);
            auto it = m_index.find(key);
            if (it == m_index.end()) return nullptr;
            return &m_entries[it->second];
        }

        // Read raw stored bytes (compressed bytes if you later enable compression).
        // For now stored_size == uncompressed_size.
        std::vector<char> readAll(std::string_view vfs_path) const
        {
            auto key = NormalizeVfsPath(vfs_path);
            auto it = m_index.find(key);
            if (it == m_index.end())
                throw std::runtime_error("PakReader::readAll: not found: " + std::string(key));

            const PakEntry& e = m_entries[it->second];
            return readEntryBytes(e);
        }

        // Optional: get the path string for an entry index (debug/tools)
        std::string_view entryPath(size_t entry_index) const
        {
            if (entry_index >= m_entries.size()) return {};
            const PakEntry& e = m_entries[entry_index];
            if (e.path_offset + e.path_size > m_path_blob.size()) return {};
            return std::string_view(m_path_blob.data() + (size_t)e.path_offset, (size_t)e.path_size);
        }

        size_t entryCount() const { return m_entries.size(); }

    private:
        void readHeader()
        {
            m_stream.seekg(0, std::ios::beg);
            m_stream.read(reinterpret_cast<char*>(&m_header), sizeof(m_header));
            if (!m_stream) throw std::runtime_error("Failed to read pak header");

            if (std::memcmp(m_header.magic, "SPAK", 4) != 0)
                throw std::runtime_error("Invalid pak magic: " + m_path.string());

            if (m_header.pak_version != SPAK_FILE_VERSION) {
                // You can relax this if you want backward compat.
                throw std::runtime_error("Unsupported pak_version in " + m_path.string());
            }

            // Sanity
            if (m_header.entry_table_offset == 0 || m_header.path_blob_offset == 0 || m_header.data_blob_offset == 0)
                throw std::runtime_error("Pak header has zero offsets (corrupt?): " + m_path.string());
        }

        void readEntries()
        {
            m_entries.resize(m_header.num_entries);

            m_stream.seekg((std::streamoff)m_header.entry_table_offset, std::ios::beg);
            if (!m_stream) throw std::runtime_error("seekg failed to entry table");

            m_stream.read(reinterpret_cast<char*>(m_entries.data()),
                (std::streamsize)m_entries.size() * (std::streamsize)sizeof(PakEntry));
            if (!m_stream) throw std::runtime_error("Failed to read entry table");
        }

        void readPathBlob()
        {
            m_path_blob.resize((size_t)m_header.path_blob_size);

            m_stream.seekg((std::streamoff)m_header.path_blob_offset, std::ios::beg);
            if (!m_stream) throw std::runtime_error("seekg failed to path blob");

            if (!m_path_blob.empty()) {
                m_stream.read(m_path_blob.data(), (std::streamsize)m_path_blob.size());
                if (!m_stream) throw std::runtime_error("Failed to read path blob");
            }
        }

        void buildIndex()
        {
            // Build a normalized string->entry_index map.
            // We store keys as std::string (owned) to keep the map safe.
            // If you want zero allocations, store string_view keys into m_path_blob
            // and use a custom hashing strategy (more advanced).
            m_index.clear();
            m_index.reserve(m_entries.size() * 2);

            for (size_t i = 0; i < m_entries.size(); ++i) {
                const PakEntry& e = m_entries[i];

                if (e.path_offset + e.path_size > m_path_blob.size()) {
                    throw std::runtime_error("Corrupt path offset/size in pak: " + m_path.string());
                }

                std::string_view raw(m_path_blob.data() + (size_t)e.path_offset, (size_t)e.path_size);

                // Paths in pak are assumed already normalized by writer.
                // But we normalize again defensively (and so paks from other tools still work).
                std::string key = NormalizeVfsPath(raw);

                // If duplicates exist, later entries overwrite earlier ones.
                m_index[key] = i;
            }
        }

        std::vector<char> readEntryBytes(const PakEntry& e) const
        {
            // Compute absolute file offset into data blob
            const uint64_t abs = m_header.data_blob_offset + e.data_offset;

            std::vector<char> buf(e.stored_size);

            m_stream.seekg((std::streamoff)abs, std::ios::beg);
            if (!m_stream) throw std::runtime_error("seekg failed in readEntryBytes");

            if (!buf.empty()) {
                m_stream.read(buf.data(), (std::streamsize)buf.size());
                if (!m_stream) throw std::runtime_error("read failed in readEntryBytes");
            }

            // If you later add compression:
            // if (e.flags & ENTRY_COMPRESSED) { decompress here into uncompressed_size }

            return buf;
        }

    private:
        std::filesystem::path m_path;
        mutable std::ifstream m_stream;

        PakHeader m_header{};
        std::vector<PakEntry> m_entries;
        std::vector<char> m_path_blob;

        std::unordered_map<std::string, size_t> m_index;
    };

    // ----------------------------
    // VFS mounting multiple packs
    // ----------------------------

    struct MountedPak
    {
        std::string mount;    // normalized mountpoint ("/", "/scene1", "/dlc1")
        PakReader* pak;      // non-owning; lifetime managed by you
        int         priority; // higher wins (simple override mechanism)
    };

    class PakVfs
    {
    public:
        void mount(PakReader& pak, std::string_view mount_point = "/", int priority = 0)
        {
            MountedPak m{};
            m.mount = NormalizeVfsPath(mount_point);
            m.pak = &pak;
            m.priority = priority;

            m_mounts.push_back(std::move(m));

            // Highest priority first; stable so equal-priority preserves order.
            std::stable_sort(m_mounts.begin(), m_mounts.end(),
                [](const MountedPak& a, const MountedPak& b) {
                    return a.priority > b.priority;
                });
        }

        bool exists(std::string_view path) const
        {
            std::string p = NormalizeVfsPath(path);

            for (const auto& m : m_mounts) {
                // Try mount: "/scene1" + "/meshes/a.mesh" => "/scene1/meshes/a.mesh"
                std::string mounted = (m.mount == "/") ? p : (m.mount + p);
                if (m.pak->exists(mounted)) return true;
            }
            return false;
        }

        std::vector<char> readAll(std::string_view path) const
        {
            std::string p = NormalizeVfsPath(path);

            for (const auto& m : m_mounts) {
                std::string mounted = (m.mount == "/") ? p : (m.mount + p);
                if (m.pak->exists(mounted)) {
                    return m.pak->readAll(mounted);
                }
            }

            throw std::runtime_error("PakVfs::readAll: not found: " + std::string(p));
        }

    private:
        std::vector<MountedPak> m_mounts;
    };

}


// namespace spak

/*
-----------------------------------------
Usage Example
-----------------------------------------

#include "spak_vfs.h"

void build_pak()
{
    spak::PakWriter w;

    // Add a mesh blob
    std::vector<char> meshBytes = ...;
    w.addFromMemory("/scene1/meshes/mesh1.mesh", meshBytes.data(), (uint32_t)meshBytes.size(), /*type=* / 1);

    // Add a texture file from disk under a VFS path
    w.addFile("/scene1/textures/albedo.ktx2", "C:/assets/albedo.ktx2", /*type=* / 2);

    w.writeToFile("scene1.spak", /*content_version=* / 1);
}

void runtime()
{
    spak::PakReader pak("scene1.spak");

    spak::PakVfs vfs;
    vfs.mount(pak, "/", /*priority=* / 0);

    auto bytes = vfs.readAll("/scene1/textures/albedo.ktx2");
}

-----------------------------------------
tinygltf FsCallbacks mapping idea:
-----------------------------------------
- Your FsCallbacks::ReadWholeFile just calls vfs.readAll(path)
- ExpandFilePath can normalize paths to VFS style (NormalizeVfsPath)
*/
