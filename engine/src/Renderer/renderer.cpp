//#include <Engine/primitives.h>
#include <vector>

struct Material {};
struct Mesh {};


enum MaterialType {
  BASIC,
  TRANSPARENT,
};


class Renderer {
 private:
  int test_;

  std::vector<Material> Materials;
  std::vector<Mesh> Meshes;

 public:
  Renderer() {}
  ~Renderer() {}
};

int CreateTexture(Renderer* renderer, char* file) {
  // allocate

  // register

  return 0;
};

int CreateMaterial(Renderer* renderer, char* name, MaterialType mat_type) {
  return 0;
};