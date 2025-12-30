#pragma once
//#include <Engine/Formats/pak.h>
//
//using ResourceMap = std::unordered_map<Uuid, ResourceLocation, UuidHash, UuidEq>;
//std::vector<std::unique_ptr<PakReader>> g_paks;
//ResourceMap g_resourceIndex;
//
//void RegisterPak(
//	const std::string& pakPath)
//{
//	auto pak = std::make_unique<PakReader>(pakPath);
//	uint32_t pakIndex = static_cast<uint32_t>(g_paks.size());
//
//	const auto& header = pak->header();
//	pak->readTable(); // returns vector<PakFileTableEntry>
//
//	auto entries = pak->m_entries;
//
//	for (const auto& e : entries)
//	{
//		ResourceLocation loc;
//		loc.pak_id = pakIndex;
//		loc.offset = e.offset;
//		loc.size = e.compressed_size;
//		loc.flags = e.flags;
//
//		g_resourceIndex.emplace(e.uuid, loc);
//	}
//
//	g_paks.push_back(std::move(pak));
//}
//
//std::vector<char> LoadResource(const Uuid& id)
//{
//	auto it = g_resourceIndex.find(id);
//	if (it == g_resourceIndex.end())
//		throw std::runtime_error("Resource not found");
//
//	const auto& loc = it->second;
//	auto& pak = g_paks[loc.pak_id];
//
//	return pak->readData(loc);
//}
//
//void WritePak() {
//
//	//std::stringstream ss;
//	//ss << "models/sponza/Sponza.gltf\n";
//	//ss << "models/sponza/b.ogg\n";
//	//auto model = GltfModel("models/sponza/Sponza.gltf");
//	//for (auto m : model.mesh_data) {
//	//	OptimizeMeshData(m);
//	//}
//	//Pack(ss, "paks/data.pak");
//
//}
//
//void RegisterPaks() {
//
//	RegisterPak("paks/data.pak");
//	//RegisterPak("paks/data.pak");
//	//RegisterPak("paks/data.pak");
//	//RegisterPak("paks/data.pak");
//	//RegisterPak("paks/data.pak");
//
//}
//
