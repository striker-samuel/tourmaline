#include <primitives.h>

struct GpuMesh {
	
	uint32_t index_start;
	uint32_t index_count;
	uint32_t vertexOffset;
	
};

class Context {


public:

	// Reserve meemory. 
	bool initialize();
	bool uploadMesh(Mesh mesh);
	bool createInstance(int mesh_handle, InstanceData data);

private:
	
	GpuMesh meshes_[MAX_MESHES];

};

bool Context::uploadMesh(Mesh mesh) {


	// Upload mesh



};


bool Context::createInstance(Mesh mesh, InstanceData data) {
}
