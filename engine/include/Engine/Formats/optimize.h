//#include <Engine/Utils/gltf.h>
//#include <meshoptimizer.h>
//
//struct Meshlet
//{
//    uint32_t vertex_offset;
//    uint32_t vertex_count;
//    uint32_t triangle_offset;
//    uint32_t triangle_count;
//};
//
//struct GPUMeshletDesc
//{
//    uint32_t vertOffset;
//    uint32_t vertCount;
//    uint32_t triOffset;     // offset into triIndex buffer (in "triangles", not bytes)
//    uint32_t triCount;
//
//    // Culling data
//    glm::vec3 aabbMin;
//    float     sphereRadius;
//    glm::vec3 aabbMax;
//    float     spherePad;    // or sphereCenter.w if you want vec4 center
//    glm::vec3 sphereCenter;
//    float     coneCutoff;   // optional
//    glm::vec3 coneAxis;     // optional
//    float     conePad;
//};
//
//void OptimizeMeshData(MeshData& mesh)
//{
//    const size_t index_count = mesh.indices.size();
//    const size_t vertex_count = mesh.vertices.size();
//
//    if (index_count == 0 || vertex_count == 0)
//        return;
//
//    // 1) Generate remap based on *indexed* geometry
//    std::vector<unsigned int> remap(vertex_count);
//
//    const size_t new_vertex_count = meshopt_generateVertexRemap(
//        remap.data(),
//        mesh.indices.data(),   // <-- use your indices
//        index_count,
//        mesh.vertices.data(),
//        vertex_count,
//        sizeof(Vertex)
//    );
//
//    // 2) Apply remap to index buffer (in-place)
//    meshopt_remapIndexBuffer(
//        mesh.indices.data(),
//        mesh.indices.data(),
//        index_count,
//        remap.data()
//    );
//
//    // 3) Apply remap to vertex buffer (needs source copy if doing in-place)
//    std::vector<Vertex> old_vertices = mesh.vertices;
//
//    // resize destination first so data() is valid for new_vertex_count writes
//    mesh.vertices.resize(new_vertex_count);
//
//    meshopt_remapVertexBuffer(
//        mesh.vertices.data(),
//        old_vertices.data(),
//        new_vertex_count,            // <-- FIX: destination count
//        sizeof(Vertex),
//        remap.data()
//    );
//
//
//    // 4) Shrink vertices to the actually-used count
//    mesh.vertices.resize(new_vertex_count);
//
//    // 5) (Optional but recommended) Run the three common optimizations
//    meshopt_optimizeVertexCache(mesh.indices.data(), mesh.indices.data(), index_count, new_vertex_count);
//
//    // If you have positions inside Vertex, pass a pointer to them here:
//    // Example assumes Vertex begins with glm::vec3 pos; adjust accordingly!
//    const float* positions = reinterpret_cast<const float*>(mesh.vertices.data()); // ONLY correct if pos is first and tightly packed!
//    // Safer: use a separate positions array or offsetof(Vertex, position)
//    //const float* positions = &mesh.vertices[0].pos.x;
//    //meshopt_optimizeOverdraw(mesh.indices.data(), mesh.indices.data(), index_count,
//    //    positions, new_vertex_count, sizeof(Vertex), 1.05f);
//
//    std::vector<Vertex> fetch_optimized(new_vertex_count);
//
//    meshopt_optimizeVertexFetch(
//        fetch_optimized.data(),
//        mesh.indices.data(),
//        index_count,
//        mesh.vertices.data(),
//        new_vertex_count,
//        sizeof(Vertex)
//    );
//
//    mesh.vertices.swap(fetch_optimized);
//
//}
//
//struct GpuMeshletCull
//{
//    glm::vec3 aabbMin;
//    float     sphereRadius;
//
//    glm::vec3 aabbMax;
//    float     coneCutoff;    // or padding if you skip cone
//
//    glm::vec3 sphereCenter;
//    float     pad0;
//
//    glm::vec3 coneAxis;
//    float     pad1;
//};
//
//struct MeshletBuildResult
//{
//    std::vector<meshopt_Meshlet> meshlets;
//    std::vector<unsigned int>    vertices;
//    std::vector<unsigned char>   triangles;
//
//    std::vector<GpuMeshletCull>  cull; // one per meshlet
//};
//
//struct MeshletMeshUploadData{
//    std::vector<Vertex> vertices;
//    std::vector<uint32_t> indices;
//    uint32_t vertexCount;
//    uint32_t triCount;
//
//
//};
//
//MeshletBuildResult BuildMeshletsForMesh(const MeshData& mesh)
//{
//    const size_t max_vertices = 64;
//    const size_t max_triangles = 126;
//    const float  cone_weight = 0.0f;
//
//    const size_t max_meshlets =
//        meshopt_buildMeshletsBound(mesh.indices.size(), max_vertices, max_triangles);
//
//    MeshletBuildResult out;
//    out.meshlets.resize(max_meshlets);
//
//    // Safe bounds for the backing buffers:
//    out.vertices.resize(max_meshlets * max_vertices);
//    out.triangles.resize(max_meshlets * max_triangles * 3);
//
//    const size_t meshlet_count = meshopt_buildMeshlets(
//        out.meshlets.data(),
//        out.vertices.data(),
//        out.triangles.data(),
//        mesh.indices.data(),
//        mesh.indices.size(),
//        &mesh.vertices[0].pos.x,
//        mesh.vertices.size(),
//        sizeof(Vertex),
//        max_vertices,
//        max_triangles,
//        cone_weight
//    );
//
//    out.meshlets.resize(meshlet_count);
//
//    // Optional: trim backing buffers to actually used ranges
//    size_t used_vertices = 0;
//    size_t used_triangles = 0;
//    for (size_t i = 0; i < meshlet_count; ++i)
//    {
//        used_vertices = std::max(used_vertices,
//            size_t(out.meshlets[i].vertex_offset + out.meshlets[i].vertex_count));
//        used_triangles = std::max(used_triangles,
//            size_t(out.meshlets[i].triangle_offset + out.meshlets[i].triangle_count * 3));
//    }
//    out.vertices.resize(used_vertices);
//    out.triangles.resize(used_triangles);
//    out.cull.resize(meshlet_count);
//
//    for (size_t i = 0; i < meshlet_count; ++i)
//    {
//        const meshopt_Meshlet& m = out.meshlets[i];
//
//        const unsigned int* mv = out.vertices.data() + m.vertex_offset;
//        const unsigned char* mt = out.triangles.data() + m.triangle_offset;
//
//        // 1) bounds from meshoptimizer (sphere + cone)
//        meshopt_Bounds b = meshopt_computeMeshletBounds(
//            mv, mt, m.triangle_count,
//            &mesh.vertices[0].pos.x,
//            mesh.vertices.size(),
//            sizeof(Vertex)
//        );
//
//        // 2) AABB from referenced vertices
//        glm::vec3 aabbMin(+FLT_MAX), aabbMax(-FLT_MAX);
//        for (uint32_t v = 0; v < m.vertex_count; ++v)
//        {
//            const glm::vec3 p = mesh.vertices[mv[v]].pos;
//            aabbMin = glm::min(aabbMin, p);
//            aabbMax = glm::max(aabbMax, p);
//        }
//
//        auto& c = out.cull[i];
//        c.aabbMin = aabbMin;
//        c.aabbMax = aabbMax;
//        c.sphereCenter = glm::vec3(b.center[0], b.center[1], b.center[2]);
//        c.sphereRadius = b.radius;
//        c.coneAxis = glm::vec3(b.cone_axis[0], b.cone_axis[1], b.cone_axis[2]);
//        c.coneCutoff = b.cone_cutoff;
//        c.pad0 = 0.0f;
//        c.pad1 = 0.0f;
//    }
//
//
//    return out;
//}
