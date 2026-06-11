#include <Engine/Renderer/ToyResourceManager.h>


void destroy(ToyResourceManager* man, MeshHandle handle) {
	auto mesh = man->meshes.get(handle);
};
