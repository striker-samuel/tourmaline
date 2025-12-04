#pragma once

#include <string>
#include <strstream>
#include <fStream>

// .
#include <filesystem>


std::filesystem::path ExpandPath(const std::string& path)
{
	std::filesystem::path p(path);

	// If it's relative, make it absolute relative to EXECUTABLE directory
	if (!p.is_absolute())
	{
		try {
			p = std::filesystem::absolute(p);
		}
		catch (...) {
			std::cerr << "[ERROR] Failed to make absolute path\n";
		}
	}

	return p;
}

#ifndef PAK_FILE_VERSION
#define PAK_FILE_VERSION 0 // Just implemented Thats the reason for 0
#endif

// Lifted from https://simoncoenen.com/blog/programming/PakFiles // Changes will come
typedef enum PAKFLAGS {
	SPAK_COMPRESSED = 1 << 0,
	SPAK_UNCOMPRESSED = 1 << 1,
	SPAK_MESH = 1 << 2, // Doesnt make sense to specialize pak-level content type like this with a flag.
} PAKFLAGS;

#pragma pack(push, 1)
struct PakHeader
{
	char     magic[4];        // "SPAK"
	uint8_t  pak_version;     // format version of the pak
	uint8_t  content_version; // or uint32_t if you want
	uint16_t reserved;        // padding / future use

	uint32_t flags;           // e.g. encrypted, platform, etc.
	uint32_t pak_id;          // unique ID per PAK (optional but useful)
	uint32_t num_entries;     // how many entries

	uint64_t file_table_offset; // where the table starts (if you want flexibility)
};
#pragma pack(pop)


#pragma pack(push, 1)
struct Uuid
{
	uint8_t bytes[16];
};

struct PakFileTableEntry
{
	Uuid     uuid;             // Logical ID
	uint32_t type;             // Mesh, Texture, Audio, etc. (optional)
	uint32_t flags;            // compressed, streamed, etc.

	uint64_t offset;           // offset to data
	uint32_t uncompressed_size;
	uint32_t compressed_size;  // == uncompressed_size if not compressed

	// (optional) original path or name, only for tools/debug:
	char original_path[256];
};
#pragma pack(pop)

struct ResourceLocation
{
	uint32_t pak_id;   // or index into an array of open pak handles
	uint64_t offset;   // where data starts in that pak
	uint32_t size;     // compressed_size
	uint32_t flags;
};

struct UuidHash
{
	std::size_t operator()(const Uuid& u) const noexcept {
		// simple hash: treat first 8 bytes as uint64_t, etc.
		uint64_t v;
		std::memcpy(&v, u.bytes, sizeof(v));
		return std::hash<uint64_t>{}(v);
	}
};

struct UuidEq
{
	bool operator()(const Uuid& a, const Uuid& b) const noexcept {
		return std::memcmp(a.bytes, b.bytes, 16) == 0;
	}
};


struct PakWriter
{
	std::vector<char> dataBuffer;
	std::vector<PakFileTableEntry> entries;

	uint32_t addEntryFromMemory(const void* data,
		uint32_t size,
		const Uuid& uuid,
		const std::string& originalPath,
		uint32_t flags = 0)
	{
		PakFileTableEntry e{};
		e.uuid = uuid;
		e.flags = flags;
		e.uncompressed_size = size;
		e.compressed_size = size;
		e.offset = static_cast<uint32_t>(dataBuffer.size());

		// Copy original path (truncate if needed)
		std::strncpy(e.original_path, originalPath.c_str(), sizeof(e.original_path) - 1);
		e.original_path[sizeof(e.original_path) - 1] = '\0';

		const char* p = static_cast<const char*>(data);
		dataBuffer.insert(dataBuffer.end(), p, p + size);

		entries.push_back(e);
		return static_cast<uint32_t>(entries.size() - 1);
	}


	// optional: helper for std::vector<char>
	uint32_t addEntryFromVector(const std::vector<char>& v,
		uint32_t flags = 0)
	{
		/*	return addEntryFromMemory(
				v.data(),
				static_cast<uint32_t>(v.size()),
				Uuid(0),

				flags
			);*/

		return 0;
	}
};


struct PakReader {
public:
	explicit PakReader(const std::string& path)
		: m_path(path)
	{
		m_stream.open(path, std::ios::binary);
		if (!m_stream)
			throw std::runtime_error("Failed to open pak: " + path);

		readHeader();
		readTable();
	}

	const PakHeader& header() const { return m_header; }
	const std::vector<PakFileTableEntry>& entries() const { return m_entries; }

	// Read entry by index into a vector<char>
	std::vector<char> readEntry(std::size_t index)
	{
		if (index >= m_entries.size())
			throw std::out_of_range("Pak entry index out of range");

		const auto& e = m_entries[index];

		// Compute absolute file offset
		const std::uint64_t dataRegionStart =
			sizeof(PakHeader) +
			static_cast<std::uint64_t>(m_entries.size()) * sizeof(PakFileTableEntry);

		const std::uint64_t absOffset =
			dataRegionStart + static_cast<std::uint64_t>(e.offset);

		// Seek and read compressed/uncompressed data
		m_stream.seekg(static_cast<std::streamoff>(absOffset), std::ios::beg);
		if (!m_stream)
			throw std::runtime_error("seekg failed in pak");

		std::vector<char> buffer(e.compressed_size);
		m_stream.read(buffer.data(), e.compressed_size);
		if (!m_stream)
			throw std::runtime_error("read failed in pak");

		//// If you add compression:
		//if (e.flags & SPAK_COMPRESSED)
		//{
		//	std::vector<char> decompressed(e.uncompressed_size);
		//	if (!DecompressLZ4(buffer.data(), e.compressed_size,
		//		decompressed.data(), e.uncompressed_size))
		//	{
		//		throw std::runtime_error("Failed to decompress entry");
		//	}
		//	return decompressed;
		//}

		return buffer;
	}

	std::vector<char> readData(const ResourceLocation& loc)
	{

		// ======================
		// Compute absolute offset
		// ======================
		const std::uint64_t dataRegionStart =
			sizeof(PakHeader) +
			static_cast<std::uint64_t>(entries().size()) * sizeof(PakFileTableEntry);

		const std::uint64_t absOffset = dataRegionStart + loc.offset;

		// ======================
		// Seek + read
		// ======================
		std::vector<char> buffer(loc.size);

		m_stream.seekg(static_cast<std::streamoff>(absOffset), std::ios::beg);
		if (!m_stream)
			throw std::runtime_error("seekg failed in readData");

		m_stream.read(buffer.data(), loc.size);
		if (!m_stream)
			throw std::runtime_error("read failed in readData");



		// ======================
		// (Optional) decompress
		// ======================
		// if (loc.flags & SPAK_COMPRESSED)
		// {
		//     std::vector<char> decompressed(e.uncompressed_size);
		//     if (!DecompressLZ4(buffer.data(), loc.size,
		//                         decompressed.data(), e.uncompressed_size))
		//         throw std::runtime_error("Failed to decompress");
		//     return decompressed;
		// }

		return buffer;
	}

	void readHeader()
	{
		m_stream.read(reinterpret_cast<char*>(&m_header), sizeof(m_header));
		if (!m_stream)
			throw std::runtime_error("Failed to read pak header");

		if (std::memcmp(m_header.magic, "SPAK", 4) != 0)
			throw std::runtime_error("Invalid pak magic in " + m_path);

		if (m_header.file_table_offset == 0)
			throw std::runtime_error("Pak has file_table_offset = 0 (unsupported/old format?)");
	}

	void readTable()
	{
		// Seek to table
		m_stream.seekg(static_cast<std::streamoff>(m_header.file_table_offset), std::ios::beg);
		if (!m_stream)
			throw std::runtime_error("Failed to seek to pak file table");

		m_entries.resize(m_header.num_entries);
		m_stream.read(reinterpret_cast<char*>(m_entries.data()),
			m_entries.size() * sizeof(PakFileTableEntry));
		if (!m_stream)
			throw std::runtime_error("Failed to read pak file table");
	}


	std::string m_path;
	std::ifstream m_stream;
	PakHeader m_header{};
	std::vector<PakFileTableEntry> m_entries;
};


void Pack(std::istream& files, const std::string& output_pak_filepath)
{
	PakWriter writer;

	PakHeader header{};
	std::memcpy(header.magic, "SPAK", 4);
	header.pak_version = static_cast<uint8_t>(PAK_FILE_VERSION);
	header.content_version = 1;
	header.flags = 0;
	header.num_entries = 0;

	std::string filePath;

	while (std::getline(files, filePath))
	{
		if (filePath.empty())
			continue;

		std::ifstream fileStream(filePath, std::ios::binary | std::ios::ate);
		if (!fileStream)
		{
			std::cout << filePath + "	: couldn't be reached!" << std::endl;
			assert(false);

			// In a real tool, log and continue or throw
			continue;
		}

		auto size = static_cast<uint32_t>(fileStream.tellg());
		fileStream.seekg(0, std::ios::beg);

		std::vector<char> fileData(size);
		fileStream.read(fileData.data(), size);
		if (!fileStream)
			continue;

		// optional: detect type, set flags (MESH, TEXTURE, etc)
		uint32_t flags = 0;
		writer.addEntryFromVector(fileData, flags);
	}

	header.num_entries = static_cast<uint32_t>(writer.entries.size());
	header.file_table_offset = sizeof(PakHeader);

	std::ofstream out(output_pak_filepath, std::ios::binary);
	if (!out)
		throw std::runtime_error("Failed to open pak output: " + output_pak_filepath);

	// Write header
	out.write(reinterpret_cast<const char*>(&header), sizeof(header));

	// Write table at header.file_table_offset
	out.write(reinterpret_cast<const char*>(writer.entries.data()),
		writer.entries.size() * sizeof(PakFileTableEntry));

	// Write data region after the table
	out.write(writer.dataBuffer.data(), writer.dataBuffer.size());

}
