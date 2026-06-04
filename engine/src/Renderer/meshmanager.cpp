#include <Engine/Renderer/meshmanager.h>


void destroy(MeshManager* man, MeshHandle handle) {
	auto mesh = man->meshes.get(handle);
};
