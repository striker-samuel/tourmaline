// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Portions Copyright (c) 2022 Packt
// Modified by Samuel Martin, 2026
// Licensed under the MIT License (the scene format derives from "Mastering
// Graphics Programming with Vulkan", Packt 2022). See the third-party
// notice in LICENSE for details.

#include "Engine/Scene/scene.h"
#include "Engine/Scene/scene_utils.h"

#include <cassert>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>



int addNode(Scene& scene, int parent, int level)
{
  const int nodeCount = (int)scene.hierarchy.size();
  // Parent must be a LIVE slot (not just in range): a retired slot is
  // a hole — linking into it orphans the node silently in release.
  assert(parent == -1
    || (parent >= 0 && parent < nodeCount
      && IsLiveSlot(scene, uint32_t(parent))));
  int node;
  if (!scene.nodeFreeList.empty()) {
    // Reuse a retired slot: everything the old life wrote is reset
    // here (the "addNode keeps every parallel array in sync" invariant
    // extends to reuse — the flag tables only where they cover the
    // slot, the lazy metadata/rowsForNode arrays likewise).
    node = (int)scene.nodeFreeList.back();
    scene.nodeFreeList.pop_back();
    assert(node >= 0 && node < nodeCount);
    scene.localTransform[size_t(node)] = glm::mat4(1.0f);
    scene.globalTransform[size_t(node)] = glm::mat4(1.0f);
    if (scene.staticRoot.size() > size_t(node)) scene.staticRoot[size_t(node)] = 0;
    if (scene.staticNode.size() > size_t(node)) scene.staticNode[size_t(node)] = 0;
    if (scene.noLightmap.size() > size_t(node)) scene.noLightmap[size_t(node)] = 0;
    if (scene.lightmap.size() > size_t(node)) scene.lightmap[size_t(node)] = 0;
    if (scene.castsVolumetricShadow.size() > size_t(node)) scene.castsVolumetricShadow[size_t(node)] = 0;
    if (scene.ditherCloseup.size() > size_t(node)) scene.ditherCloseup[size_t(node)] = 0;
    if (scene.hidden.size() > size_t(node)) scene.hidden[size_t(node)] = 0;
    scene.hierarchy[size_t(node)] = Hierarchy{};
    if (scene.metadata.size() > size_t(node)) scene.metadata[size_t(node)] = NodeMetadata{};
    if (scene.rowsForNode.size() > size_t(node)) scene.rowsForNode[size_t(node)] = NodeRows{};
    // The generation survived retirement (bumped there) — re-mark alive.
    scene.nodeGeneration[size_t(node)] |= kNodeSlotAliveBit;
  } else {
    // Append a fresh slot (the pre-6b dense path: a scene that never
    // deletes allocates 0,1,2,... — bit-identical ids).
    assert(nodeCount < int(kNodeSlotCountMax - 1) && "node slot space exhausted (2^20)");
    node = nodeCount;
    {
      // TODO: resize aux arrays (local/global etc.)
      scene.localTransform.push_back(glm::mat4(1.0f));
      scene.globalTransform.push_back(glm::mat4(1.0f));
      scene.staticRoot.push_back(0);
      scene.staticNode.push_back(0);
      scene.noLightmap.push_back(0);
      scene.lightmap.push_back(0);
      scene.castsVolumetricShadow.push_back(0);
      scene.ditherCloseup.push_back(0);
      scene.hidden.push_back(0);
    }
    // Generation word: grow-with-alive so a lagging array (wholesale
    // resizes never touched it) keeps its untracked slots LIVE.
    if (scene.nodeGeneration.size() < scene.hierarchy.size()) {
      const size_t base = scene.nodeGeneration.size();
      scene.nodeGeneration.resize(scene.hierarchy.size());
      for (size_t i = base; i < scene.nodeGeneration.size(); ++i)
        scene.nodeGeneration[i] = kNodeSlotAliveBit;
    }
    scene.hierarchy.push_back({ .parent = parent, .lastSibling = -1 });
  }
  // The linking block below only mutates the PARENT's entry (and
  // siblings'); the node's own .parent is set HERE for both branches
  // (the append branch's push_back initializer already carries it —
  // the reuse branch reset the entry to defaults above).
  scene.hierarchy[size_t(node)].parent = parent;
  if (parent > -1) {
    // find first item (sibling)
    const int s = scene.hierarchy[parent].firstChild;
    if (s == -1) {
      scene.hierarchy[parent].firstChild = node;
      scene.hierarchy[node].lastSibling  = node;
    } else {
      int dest = scene.hierarchy[s].lastSibling;
      if (dest <= -1) {
        // no cached lastSibling, iterate nextSibling indices
        for (dest = s; scene.hierarchy[dest].nextSibling != -1; dest = scene.hierarchy[dest].nextSibling)
          ;
      }
      scene.hierarchy[dest].nextSibling = node;
      scene.hierarchy[s].lastSibling    = node;
    }
  }
  scene.hierarchy[node].level       = level;
  scene.hierarchy[node].nextSibling = -1;
  scene.hierarchy[node].firstChild  = -1;
  return node;
}

// ---- stable node slots (Epoch 6b) ----------------------------------------

uint32_t LiveNodeCount(const Scene& scene)
{
  uint32_t live = 0;
  for (uint32_t s = 0; s < scene.hierarchy.size(); ++s)
    if (IsLiveSlot(scene, s))
      ++live;
  return live;
}

std::vector<uint32_t> BuildSlotToDenseMap(const Scene& scene)
{
  std::vector<uint32_t> map(scene.hierarchy.size(), UINT32_MAX);
  uint32_t dense = 0;
  for (uint32_t s = 0; s < scene.hierarchy.size(); ++s)
    if (IsLiveSlot(scene, s))
      map[s] = dense++;
  return map;
}

void UnlinkNodeFromParent(Scene& scene, int node)
{
  if (node < 0 || (size_t)node >= scene.hierarchy.size())
    return;
  Hierarchy& h = scene.hierarchy[size_t(node)];
  const int p = h.parent;
  if (p < 0 || (size_t)p >= scene.hierarchy.size()) {
    h.parent = -1;
    return;
  }
  Hierarchy& hp = scene.hierarchy[size_t(p)];
  if (hp.firstChild == node) {
    hp.firstChild = h.nextSibling;
  } else {
    int prev = hp.firstChild;
    while (prev >= 0 && scene.hierarchy[size_t(prev)].nextSibling != node)
      prev = scene.hierarchy[size_t(prev)].nextSibling;
    if (prev >= 0)
      scene.hierarchy[size_t(prev)].nextSibling = h.nextSibling;
  }
  // Refresh the cached tail on the (possibly new) first child.
  if (hp.firstChild >= 0) {
    int tailId = -1;
    for (int s = hp.firstChild; s >= 0; s = scene.hierarchy[size_t(s)].nextSibling)
      tailId = s;
    scene.hierarchy[size_t(hp.firstChild)].lastSibling = tailId;
  }
  h.parent = -1;
  h.nextSibling = -1;
}

void RetireNodeSlot(Scene& scene, NodeId node)
{
  if (!IsNodeAlive(scene, node))
    return; // already retired (or never was): idempotent
  // Rows never outlive their node (the deletion discipline): drop every
  // association the slot holds. Survivors' rows keep their ids verbatim.
  RemoveNodeRows(scene, node);
  if (scene.metadata.size() > size_t(node)) scene.metadata[size_t(node)] = NodeMetadata{};
  if (scene.rowsForNode.size() > size_t(node)) scene.rowsForNode[size_t(node)] = NodeRows{};
  if (scene.staticRoot.size() > size_t(node)) scene.staticRoot[size_t(node)] = 0;
  if (scene.staticNode.size() > size_t(node)) scene.staticNode[size_t(node)] = 0;
  if (scene.noLightmap.size() > size_t(node)) scene.noLightmap[size_t(node)] = 0;
  if (scene.lightmap.size() > size_t(node)) scene.lightmap[size_t(node)] = 0;
  if (scene.castsVolumetricShadow.size() > size_t(node)) scene.castsVolumetricShadow[size_t(node)] = 0;
  if (scene.ditherCloseup.size() > size_t(node)) scene.ditherCloseup[size_t(node)] = 0;
  if (scene.hidden.size() > size_t(node)) scene.hidden[size_t(node)] = 0;
  scene.localTransform[size_t(node)] = glm::mat4(1.0f);
  scene.globalTransform[size_t(node)] = glm::mat4(1.0f);
  // level = -1 marks the hole for debug dumps (tree walks can't reach
  // it — it is unlinked — but flat iteration reads it as "not a node").
  scene.hierarchy[size_t(node)] = Hierarchy{ .parent = -1, .level = -1 };
  // Bump the generation (handles minted against the old life die here)
  // and push the slot to the free list. 12 bits: 4,096 lives per slot
  // before the counter wraps — see scene_node_id.h.
  if (scene.nodeGeneration.size() < scene.hierarchy.size()) {
    const size_t base = scene.nodeGeneration.size();
    scene.nodeGeneration.resize(scene.hierarchy.size());
    for (size_t i = base; i < scene.nodeGeneration.size(); ++i)
      scene.nodeGeneration[i] = kNodeSlotAliveBit;
  }
  uint32_t& g = scene.nodeGeneration[size_t(node)];
  // +1 bumps the generation; the kNodeGenMask also strips the alive bit.
  g = (g + 1u) & kNodeGenMask;
  scene.nodeFreeList.push_back(node);
}


void markAsChanged(Scene& scene, int node)
{
  const int level = scene.hierarchy[node].level;
  scene.changedAtThisFrame[level].push_back(node);

  // TODO: use non-recursive iteration with aux stack
  for (int s = scene.hierarchy[node].firstChild; s != -1; s = scene.hierarchy[s].nextSibling) {
    markAsChanged(scene, s);
  }
}

void MarkAllRootsChanged(Scene& scene)
{
  // See the header contract: every ROOT (parent == -1), not just node 0.
  // Retired slots reset to parent == -1 — they are holes, NOT roots; a
  // dead slot in the dirty set would be skipped by recalculate anyway,
  // but keeping them out keeps the lists honest.
  for (size_t n = 0; n < scene.hierarchy.size(); ++n) {
    if (scene.hierarchy[n].parent == -1 && IsLiveSlot(scene, uint32_t(n)))
      markAsChanged(scene, (int)n);
  }
}

void markAsChangedDynamic(Scene& scene, int node)
{
  // Frozen subtrees never re-enter the dirty set after the initial build.
  if (node >= 0 && (size_t)node < scene.staticNode.size() && scene.staticNode[(size_t)node] != 0)
    return;
  const int level = scene.hierarchy[node].level;
  scene.changedAtThisFrame[level].push_back(node);

  for (int s = scene.hierarchy[node].firstChild; s != -1; s = scene.hierarchy[s].nextSibling) {
    // A static child subtree stays frozen even when its dynamic parent moves.
    // (Its baked globals remain valid only while the static root itself is
    // static; reparenting under a moving node is a spec error — skip it.)
    if ((size_t)s < scene.staticNode.size() && scene.staticNode[(size_t)s] != 0)
      continue;
    markAsChangedDynamic(scene, s);
  }
}

void RebuildStaticCache(Scene& scene)
{
  scene.staticNode.assign(scene.hierarchy.size(), 0);
  if (scene.staticRoot.size() != scene.hierarchy.size())
    scene.staticRoot.assign(scene.hierarchy.size(), 0);
  std::vector<int> stack;
  for (size_t i = 0; i < scene.hierarchy.size(); ++i) {
    if (scene.staticRoot[i] == 0)
      continue;
    stack.clear();
    stack.push_back((int)i);
    while (!stack.empty()) {
      const int n = stack.back();
      stack.pop_back();
      if (n < 0 || (size_t)n >= scene.hierarchy.size())
        continue;
      if (scene.staticNode[(size_t)n] != 0 && n != (int)i)
        continue; // already covered by another root
      scene.staticNode[(size_t)n] = 1;
      for (int s = scene.hierarchy[(size_t)n].firstChild; s != -1; s = scene.hierarchy[(size_t)s].nextSibling)
        stack.push_back(s);
    }
  }
}

void MarkSubtreeStatic(Scene& scene, int node)
{
  if (node < 0 || (size_t)node >= scene.hierarchy.size())
    return;
  if (scene.staticRoot.size() != scene.hierarchy.size())
    scene.staticRoot.assign(scene.hierarchy.size(), 0);
  scene.staticRoot[(size_t)node] = 1;
  RebuildStaticCache(scene);
}

void ClearSubtreeStatic(Scene& scene, int node)
{
  if (node < 0 || (size_t)node >= scene.staticRoot.size())
    return;
  scene.staticRoot[(size_t)node] = 0;
  RebuildStaticCache(scene);
}

int findNodeByName(const Scene& scene, const std::string& name)
{
  // Extremely simple linear search without any hierarchy reference
  // To support DFS/BFS searches separate traversal routines are needed

  for (size_t i = 0; i < scene.localTransform.size(); i++) {
    const uint32_t strID = GetNodeNameIndex(scene, NodeId(i));
    if (strID != 0xFFFFFFFFu)
      if (scene.nodeNames[strID] == name)
        return (int)i;
  }

  return -1;
}

bool mat4IsIdentity(const glm::mat4& m);
void fprintfMat4(FILE* f, const glm::mat4& m);

// CPU version of global transform update []
bool recalculateGlobalTransforms(Scene& scene)
{
  bool wasUpdated = false;

  // Level 0: process EVERY changed entry (the old code took only [0] -
  // single-root assumption; a scene with several level-0 roots left the
  // rest identity). Root nodes: global = local (no parent to fold).
  // Retired slots (a deletion between mark and recalc) drop out here —
  // their transform data is reset but nobody may read it.
  for (int c : scene.changedAtThisFrame[0]) {
    if (!IsLiveSlot(scene, uint32_t(c)))
      continue;
    scene.globalTransform[c] = scene.localTransform[c];
    wasUpdated = true;
  }
  scene.changedAtThisFrame[0].clear();

  for (int i = 1; i < MAX_NODE_LEVEL; i++) {
    for (int c : scene.changedAtThisFrame[i]) {
      if (!IsLiveSlot(scene, uint32_t(c)))
        continue;
      const int p = scene.hierarchy[c].parent;
      // Root-level nodes at a deeper level (the cooker's WorldSun is
      // addNode(scene, -1, 1) - a deliberate root, see EnsureWorldSun):
      // global = local (the old code read globalTransform[-1] - UB that
      // showed up as an identity/horizontal sun: az=0/el=0).
      if (p <= -1)
        scene.globalTransform[c] = scene.localTransform[c];
      else
        scene.globalTransform[c] = scene.globalTransform[p] * scene.localTransform[c];
    }
    wasUpdated |= !scene.changedAtThisFrame[i].empty();
    scene.changedAtThisFrame[i].clear();
  }

  return wasUpdated;
}

bool recalculateGlobalTransformsDynamic(Scene& scene)
{
  const bool hasStatic =
    scene.staticNode.size() == scene.hierarchy.size() &&
    std::any_of(scene.staticNode.begin(), scene.staticNode.end(),
      [](uint8_t v) { return v != 0; });
  if (!hasStatic)
    return recalculateGlobalTransforms(scene);

  bool wasUpdated = false;
  if (!scene.changedAtThisFrame[0].empty()) {
    const int c = scene.changedAtThisFrame[0][0];
    const bool isStatic = (size_t)c < scene.staticNode.size() && scene.staticNode[(size_t)c] != 0;
    if (!isStatic) {
      scene.globalTransform[c] = scene.localTransform[c];
      wasUpdated = true;
    }
    scene.changedAtThisFrame[0].clear();
  }

  for (int i = 1; i < MAX_NODE_LEVEL; i++) {
    for (int c : scene.changedAtThisFrame[i]) {
      if (!IsLiveSlot(scene, uint32_t(c)))
        continue; // retired mid-frame: nothing to update
      const bool isStatic = (size_t)c < scene.staticNode.size() && scene.staticNode[(size_t)c] != 0;
      if (isStatic)
        continue;
      const int p              = scene.hierarchy[c].parent;
      scene.globalTransform[c] = scene.globalTransform[p] * scene.localTransform[c];
      wasUpdated = true;
    }
    scene.changedAtThisFrame[i].clear();
  }

  return wasUpdated;
}


// Typed-table readers (the saveMeshRows/saveMaterialRows mirror):
// u32 rowCount + pairs, routed through the table mutators (which
// validate node range — a corrupt file's out-of-range rows drop
// instead of materializing as garbage entries).
void loadMeshRows(FILE* f, Scene& scene)
{
  std::vector<uint32_t> ms;
  uint32_t count = 0;
  fread(&count, 1, sizeof(count), f);
  ms.resize(size_t(count) * 2);
  fread(ms.data(), sizeof(uint32_t), ms.size(), f);
  for (size_t i = 0; i < count; ++i)
    SetMesh(scene, ms[i * 2 + 0], ms[i * 2 + 1]);
}

void loadMaterialRows(FILE* f, Scene& scene)
{
  std::vector<uint32_t> ms;
  uint32_t count = 0;
  fread(&count, 1, sizeof(count), f);
  ms.resize(size_t(count) * 2);
  fread(ms.data(), sizeof(uint32_t), ms.size(), f);
  for (size_t i = 0; i < count; ++i)
    SetMaterial(scene, ms[i * 2 + 0], ms[i * 2 + 1]);
}

// The saveNameRows mirror (out-of-range nodes drop via the mutators).
void loadNameRows(FILE* f, Scene& scene)
{
  std::vector<uint32_t> ms;
  uint32_t count = 0;
  fread(&count, 1, sizeof(count), f);
  ms.resize(size_t(count) * 2);
  fread(ms.data(), sizeof(uint32_t), ms.size(), f);
  for (size_t i = 0; i < count; ++i)
    SetNodeNameIndex(scene, ms[i * 2 + 0], ms[i * 2 + 1]);
}

// Authored-environment record I/O (see SceneEnv): kSceneEnvFloatCount
// floats in fixed order + a length-prefixed NUL-terminated string
// (saveStringList encoding). Returns false on any short read (caller
// keeps defaults; the env section is always last, so a short read simply
// ends parsing). Bools/modes ride as 0/1 floats (see kSceneEnvFloatCount
// for the postfx order).
static void saveEnvFloats(FILE* f, const SceneEnv& e)
{
  const float floats[kSceneEnvFloatCount] = {
    e.skyColor.x, e.skyColor.y, e.skyColor.z, e.skyIntensity,
    e.sunColor.x, e.sunColor.y, e.sunColor.z,
    e.sunAzimuth, e.sunElevation, e.sunIntensity,
    e.exposure, e.fogDensity,
    e.fogColor.x, e.fogColor.y, e.fogColor.z,
    e.volumetrics,
    e.zonePriority,
  };
  static_assert(sizeof(floats) / sizeof(float) == kSceneEnvFloatCount,
    "env float order drifted from kSceneEnvFloatCount");
  fwrite(floats, sizeof(float), kSceneEnvFloatCount, f);
  const uint32_t len = (uint32_t)e.skyboxUri.length();
  fwrite(&len, sizeof(len), 1, f);
  if (len > 0)
    fwrite(e.skyboxUri.c_str(), len + 1, 1, f);
}

static bool loadEnvFloats(FILE* f, SceneEnv& e)
{
  float floats[kSceneEnvFloatCount] = {};
  if (fread(floats, sizeof(float), kSceneEnvFloatCount, f)
      != kSceneEnvFloatCount)
    return false;
  e.skyColor = glm::vec3(floats[0], floats[1], floats[2]);
  e.skyIntensity = floats[3];
  e.sunColor = glm::vec3(floats[4], floats[5], floats[6]);
  e.sunAzimuth = floats[7];
  e.sunElevation = floats[8];
  e.sunIntensity = floats[9];
  e.exposure = floats[10];
  e.fogDensity = floats[11];
  e.fogColor = glm::vec3(floats[12], floats[13], floats[14]);
  e.volumetrics = floats[15];
  e.zonePriority = floats[16];
  uint32_t len = 0;
  if (fread(&len, sizeof(len), 1, f) != 1 || len > 1024 * 1024)
    return false;
  if (len > 0) {
    std::vector<char> bytes(len + 1, 0);
    if (fread(bytes.data(), len + 1, 1, f) != 1)
      return false;
    e.skyboxUri = std::string(bytes.data());
  }
  else {
    e.skyboxUri.clear();
  }
  return true;
}

// Render-settings record (see SceneRenderSettings): the postfx look's
// 27 floats in fixed order (bloom 3 + tonemap 1 + exposure 7 + AO 9 +
// SSR 2 + motionblur/dof/aa 3 + cubemap 2). Bools/modes ride as 0/1
// floats. Split out of the env record (old files fail loudly — the
// sanctioned no-conversions break, AGENTS.md).
static void saveRenderFloats(FILE* f, const SceneRenderSettings& r)
{
  const float floats[kSceneRenderFloatCount] = {
    r.bloomEnabled ? 1.0f : 0.0f, r.bloomIntensity, r.bloomThreshold,
    float(r.tonemapMode),
    r.autoExposure ? 1.0f : 0.0f, r.manualExposure, r.exposureEvBias,
    r.exposureSpeedToDark, r.exposureSpeedToBright,
    r.exposureMin, r.exposureMax,
    float(r.aoMode), r.aoRadius, r.aoBias, r.aoIntensity,
    r.aoMaxDistance, float(r.aoSsaoSamples), r.aoBlurSharpness,
    float(r.aoSaoSamples), float(r.aoSaoSpiralTurns),
    r.ssrEnabled ? 1.0f : 0.0f, r.ssrIntensity,
    r.motionBlurEnabled ? 1.0f : 0.0f, r.dofEnabled ? 1.0f : 0.0f,
    float(r.aaMode),
    r.cubemapEnabled ? 1.0f : 0.0f, r.cubemapIntensity,
  };
  static_assert(sizeof(floats) / sizeof(float) == kSceneRenderFloatCount,
    "render float order drifted from kSceneRenderFloatCount");
  fwrite(floats, sizeof(float), kSceneRenderFloatCount, f);
}

static bool loadRenderFloats(FILE* f, SceneRenderSettings& r)
{
  float floats[kSceneRenderFloatCount] = {};
  if (fread(floats, sizeof(float), kSceneRenderFloatCount, f)
      != kSceneRenderFloatCount)
    return false;
  r.bloomEnabled = floats[0] != 0.0f;
  r.bloomIntensity = floats[1];
  r.bloomThreshold = floats[2];
  r.tonemapMode = uint32_t(floats[3]);
  r.autoExposure = floats[4] != 0.0f;
  r.manualExposure = floats[5];
  r.exposureEvBias = floats[6];
  r.exposureSpeedToDark = floats[7];
  r.exposureSpeedToBright = floats[8];
  r.exposureMin = floats[9];
  r.exposureMax = floats[10];
  r.aoMode = int(floats[11]);
  r.aoRadius = floats[12];
  r.aoBias = floats[13];
  r.aoIntensity = floats[14];
  r.aoMaxDistance = floats[15];
  r.aoSsaoSamples = int(floats[16]);
  r.aoBlurSharpness = floats[17];
  r.aoSaoSamples = int(floats[18]);
  r.aoSaoSpiralTurns = int(floats[19]);
  r.ssrEnabled = floats[20] != 0.0f;
  r.ssrIntensity = floats[21];
  r.motionBlurEnabled = floats[22] != 0.0f;
  r.dofEnabled = floats[23] != 0.0f;
  r.aaMode = int(floats[24]);
  r.cubemapEnabled = floats[25] != 0.0f;
  r.cubemapIntensity = floats[26];
  return true;
}

void loadScene(const char* fileName, Scene& scene)
{
  FILE* f = fopen(fileName, "rb");

  uint32_t sz = 0;
  fread(&sz, sizeof(sz), 1, f);

  scene.hierarchy.resize(sz);
  scene.globalTransform.resize(sz);
  scene.localTransform.resize(sz);
  scene.staticRoot.assign(sz, 0);
  scene.staticNode.assign(sz, 0);
  scene.noLightmap.assign(sz, 0);
  scene.lightmap.assign(sz, 0);
  scene.hidden.assign(sz, 0);
  scene.castsVolumetricShadow.assign(sz, 0);
  scene.ditherCloseup.assign(sz, 0);
  // Epoch 6b: the file is DENSE (the writer compacts) — the loaded
  // scene's slots are 0..sz-1, all alive at generation 0, free list
  // empty. A stale free list would hand addNode recycled slots whose
  // parallel-array entries belong to the pre-load graph.
  scene.nodeGeneration.assign(sz, kNodeSlotAliveBit);
  scene.nodeFreeList.clear();
  scene.tables = {};
  scene.rowsForNode.clear();
  scene.metadata.clear();
  // TODO: check > -1
  // TODO: recalculate changedAtThisLevel() - find max depth of a node [or save scene.maxLevel]
  fread(scene.localTransform.data(), sizeof(glm::mat4), sz, f);
  fread(scene.globalTransform.data(), sizeof(glm::mat4), sz, f);
  fread(scene.hierarchy.data(), sizeof(Hierarchy), sz, f);

  // Mesh/material associations (the typed tables — row-native
  // sections; material first like always).
  loadMaterialRows(f, scene);
  loadMeshRows(f, scene);

  if (!feof(f)) {
    loadNameRows(f, scene);
    loadStringList(f, scene.nodeNames);
    loadStringList(f, scene.materialNames);
  }

  // Static-subtree section (new files only): u32 count + count bytes.
  // Old files simply hit EOF here; leave the spec empty in that case.
  {
    uint32_t staticCount = 0;
    if (fread(&staticCount, sizeof(staticCount), 1, f) == 1 &&
        staticCount > 0 && staticCount <= 1024 * 1024) {
      std::vector<uint8_t> roots(staticCount, 0);
      if (fread(roots.data(), 1, staticCount, f) == staticCount && staticCount == sz) {
        scene.staticRoot = std::move(roots);
        RebuildStaticCache(scene);
      }
    }
  }

  // Lightmap-exclusion section (v3): u32 count + count bytes. Absent on
  // older files (EOF) — the all-zero default stands.
  {
    uint32_t lmCount = 0;
    if (fread(&lmCount, sizeof(lmCount), 1, f) == 1 &&
        lmCount > 0 && lmCount <= 1024 * 1024) {
      std::vector<uint8_t> flags(lmCount, 0);
      if (fread(flags.data(), 1, lmCount, f) == lmCount && lmCount == sz) {
        scene.noLightmap = std::move(flags);
      }
    }
  }
  // Visibility section (v4): u32 count + count bytes (hidden flags).
  // Absent on older files (EOF) — the all-visible default stands.
  {
    uint32_t hidCount = 0;
    const long pos = ftell(f);
    if (fread(&hidCount, sizeof(hidCount), 1, f) == 1 &&
        hidCount > 0 && hidCount <= 1024 * 1024) {
      std::vector<uint8_t> flags(hidCount, 0);
      if (fread(flags.data(), 1, hidCount, f) == hidCount && hidCount == sz) {
        scene.hidden = std::move(flags);
      }
      else {
        // Not a hidden section (or truncated): rewind so a dither-only
        // tail still parses below; hidden stays all-visible.
        fseek(f, pos, SEEK_SET);
      }
    }
  }
  // Dither-on-closeup section (v5): u32 count + count bytes. Absent on
  // older files (EOF) — the all-off default stands.
  {
    uint32_t dCount = 0;
    if (fread(&dCount, sizeof(dCount), 1, f) == 1 &&
        dCount > 0 && dCount <= 1024 * 1024) {
      std::vector<uint8_t> flags(dCount, 0);
      if (fread(flags.data(), 1, dCount, f) == dCount && dCount == sz) {
        scene.ditherCloseup = std::move(flags);
      }
    }
  }
  // Authored environment section (v6): u32 authored flag + resolved-look
  // record + default-block record + per-node blocks (u32 count + (node,
  // record) pairs in node order). Absent on older files (EOF) — the
  // struct defaults stand and envAuthored stays false.
  {
    uint32_t envFlag = 0;
    if (fread(&envFlag, sizeof(envFlag), 1, f) == 1) {
      SceneEnv resolved{}, def{};
      uint32_t nodeCount = 0;
      bool ok = loadEnvFloats(f, resolved) && loadEnvFloats(f, def);
      if (ok && fread(&nodeCount, sizeof(nodeCount), 1, f) == 1 &&
          nodeCount <= 1024 * 1024) {
        // All-or-nothing like the map era: a truncated pair list
        // discards the whole section (globals + rows together).
        std::vector<std::pair<uint32_t, SceneEnv>> rows;
        rows.reserve(nodeCount);
        for (uint32_t i = 0; i < nodeCount && ok; ++i) {
          uint32_t node = 0;
          SceneEnv block{};
          if (fread(&node, sizeof(node), 1, f) == 1 && loadEnvFloats(f, block))
            rows.emplace_back(node, block);
          else
            ok = false;
        }
        if (ok) {
          scene.environment = resolved;
          scene.envAuthored = envFlag != 0;
          scene.defaultEnvironment = def;
          for (const auto& [node, block] : rows)
            SetEnv(scene, node, block);
        }
      }
    }
  }
  // Render-settings section (last; absent in old files — a short read
  // keeps the defaults): u32 authored flag + the record.
  {
    uint32_t renderFlag = 0;
    if (fread(&renderFlag, sizeof(renderFlag), 1, f) == 1) {
      SceneRenderSettings rs{};
      if (loadRenderFloats(f, rs)) {
        scene.renderSettings = rs;
        scene.defaultRenderSettings = rs;
        scene.hasRenderSettings = renderFlag != 0;
      }
    }
  }
  // Env-zone master switch (appended; absent = the default ON stands).
  // RETIRED with the always-on zones rework (the default env packs as the
  // infinite base zone): the trailing u32 stays read for format
  // compatibility, the value is forced true.
  {
    uint32_t zonesOn = 0;
    if (fread(&zonesOn, sizeof(zonesOn), 1, f) == 1) {
      (void)zonesOn;
      scene.envZonesEnabled = true;
    }
  }
  // Volumetric-shadow caster section (appended LAST): u32 count + count
  // bytes (the castsVolumetricShadow self flags - the coarse fog tile's
  // caster marks). Absent on older files (EOF) - the all-off default
  // stands.
  {
    uint32_t vCount = 0;
    if (fread(&vCount, sizeof(vCount), 1, f) == 1 &&
        vCount > 0 && vCount <= 1024 * 1024) {
      std::vector<uint8_t> flags(vCount, 0);
      if (fread(flags.data(), 1, vCount, f) == vCount && vCount == sz)
        scene.castsVolumetricShadow = std::move(flags);
    }
  }
  // Cooked punctual lights are NOT carried in this legacy format: they
  // are Cluster pool assets (see the light table) and round-trip
  // through paks only.

  fclose(f);

  MarkAllRootsChanged(scene);
  recalculateGlobalTransforms(scene);
}


// Typed-table sections (row-native: u32 rowCount + (node, value)
// pairs in TABLE order — the canonical association format; the old
// saveMap layout is gone with the maps, old .scene files fail loudly,
// content re-saves). Epoch 6b: nodes go through the slot->dense
// compaction map (the file stays DENSE — see BuildSlotToDenseMap);
// rows pointing at retired slots drop (they cannot exist through the
// mutators — belt and suspenders for hand-tampered scenes).
void saveMeshRows(FILE* f, const Scene& scene, const std::vector<uint32_t>& slotToDense)
{
  uint32_t count = 0;
  for (const MeshRow& row : scene.tables.meshes)
    if (row.node < slotToDense.size() && slotToDense[row.node] != UINT32_MAX)
      ++count;
  fwrite(&count, sizeof(count), 1, f);
  for (const MeshRow& row : scene.tables.meshes) {
    if (row.node >= slotToDense.size() || slotToDense[row.node] == UINT32_MAX)
      continue;
    const uint32_t node = slotToDense[row.node];
    fwrite(&node, sizeof(uint32_t), 1, f);
    fwrite(&row.mesh, sizeof(uint32_t), 1, f);
  }
}

void saveMaterialRows(FILE* f, const Scene& scene, const std::vector<uint32_t>& slotToDense)
{
  uint32_t count = 0;
  for (const MaterialRow& row : scene.tables.materials)
    if (row.node < slotToDense.size() && slotToDense[row.node] != UINT32_MAX)
      ++count;
  fwrite(&count, sizeof(count), 1, f);
  for (const MaterialRow& row : scene.tables.materials) {
    if (row.node >= slotToDense.size() || slotToDense[row.node] == UINT32_MAX)
      continue;
    const uint32_t node = slotToDense[row.node];
    fwrite(&node, sizeof(uint32_t), 1, f);
    fwrite(&row.material, sizeof(uint32_t), 1, f);
  }
}

// Node-name section (row-native, node-sorted for byte-stable saves):
// u32 rowCount + (node, nodeNames index) pairs; UINT32_MAX name = skip
// (should not occur — named nodes only).
void saveNameRows(FILE* f, const Scene& scene, const std::vector<uint32_t>& slotToDense)
{
  std::vector<std::pair<uint32_t, uint32_t>> pairs;
  for (uint32_t n = 0; n < scene.metadata.size(); ++n)
    if (scene.metadata[n].name != 0xFFFFFFFFu
        && n < slotToDense.size() && slotToDense[n] != UINT32_MAX)
      pairs.emplace_back(slotToDense[n], scene.metadata[n].name);
  std::sort(pairs.begin(), pairs.end());
  const uint32_t count = static_cast<uint32_t>(pairs.size());
  fwrite(&count, sizeof(count), 1, f);
  for (const auto& [node, nameIdx] : pairs) {
    fwrite(&node, sizeof(uint32_t), 1, f);
    fwrite(&nameIdx, sizeof(uint32_t), 1, f);
  }
}

void saveScene(const char* fileName, const Scene& scene)
{
  FILE* f = fopen(fileName, "wb");

  // Epoch 6b: the file stays DENSE — live slots compact in ascending
  // slot order (a never-deleted scene is byte-identical to the pre-6b
  // save: ascending-slot == the old array order). Holes never
  // serialize; a save/load round trip renumbers ids densely (stable
  // identity across a round trip is the uuid's job, never the id's).
  const std::vector<uint32_t> slotToDense = BuildSlotToDenseMap(scene);
  const uint32_t sz = LiveNodeCount(scene);
  fwrite(&sz, sizeof(sz), 1, f);

  // Transforms + hierarchy, compacted; links remap through the map (a
  // live node's links point at live nodes by the deletion discipline).
  {
    std::vector<glm::mat4> local;
    std::vector<glm::mat4> global;
    std::vector<Hierarchy> hierarchy;
    local.reserve(sz);
    global.reserve(sz);
    hierarchy.reserve(sz);
    const auto mapLink = [&slotToDense](int link) -> int {
      if (link < 0 || size_t(link) >= slotToDense.size())
        return -1;
      const uint32_t d = slotToDense[size_t(link)];
      return (d != UINT32_MAX) ? int(d) : -1;
    };
    for (uint32_t s = 0; s < scene.hierarchy.size(); ++s) {
      if (slotToDense[s] == UINT32_MAX)
        continue;
      local.push_back(scene.localTransform[s]);
      global.push_back(scene.globalTransform[s]);
      const Hierarchy& h = scene.hierarchy[s];
      hierarchy.push_back(Hierarchy{
        .parent      = mapLink(h.parent),
        .firstChild  = mapLink(h.firstChild),
        .nextSibling = mapLink(h.nextSibling),
        .lastSibling = mapLink(h.lastSibling),
        .level       = h.level,
        });
    }
    if (sz > 0) {
      fwrite(local.data(), sizeof(glm::mat4), sz, f);
      fwrite(global.data(), sizeof(glm::mat4), sz, f);
      fwrite(hierarchy.data(), sizeof(Hierarchy), sz, f);
    }
  }

  // Mesh/material associations (the typed tables; material section
  // first, matching the historical order).
  saveMaterialRows(f, scene, slotToDense);
  saveMeshRows(f, scene, slotToDense);

  const bool anyNamed = !scene.nodeNames.empty() && scene.metadata.end()
    != std::find_if(scene.metadata.begin(), scene.metadata.end(),
      [](const NodeMetadata& m) { return m.name != 0xFFFFFFFFu; });
  if (anyNamed) {
    saveNameRows(f, scene, slotToDense);
    saveStringList(f, scene.nodeNames);
    saveStringList(f, scene.materialNames);
  }
  // Per-node byte section writer (the flag tables): live slots only,
  // compacted through the map, zeros where the source table was short
  // (the "missing == default" contract unchanged).
  const auto saveFlagBytes = [&f, &scene, &slotToDense, sz](const std::vector<uint8_t>& flags) {
    fwrite(&sz, sizeof(sz), 1, f);
    if (sz == 0)
      return;
    std::vector<uint8_t> bytes;
    bytes.reserve(sz);
    for (uint32_t s = 0; s < scene.hierarchy.size(); ++s)
      if (slotToDense[s] != UINT32_MAX)
        bytes.push_back(flags.size() > s ? flags[s] : uint8_t(0));
    fwrite(bytes.data(), 1, sz, f);
  };

  // Static-subtree section: always written (possibly all-zero) so new
  // loaders can rely on its presence; old loaders ignore trailing bytes.
  saveFlagBytes(scene.staticRoot);
  // Lightmap-exclusion section (v3): all-zero when no exclusions. Old
  // loaders stop after the static section; new loaders treat a missing
  // tail as "no exclusions".
  saveFlagBytes(scene.noLightmap);
  // Visibility section (v4): hidden flags, all-zero (visible) when unset.
  saveFlagBytes(scene.hidden);
  // Dither-on-closeup section (v5): all-zero when unused.
  saveFlagBytes(scene.ditherCloseup);
  // Authored environment section (v6): u32 authored flag + resolved look
  // + default block + per-node blocks (u32 count + node-ordered (node,
  // record) pairs). Always written; old loaders stop after the dither
  // section (trailing bytes ignored).
  {
    const uint32_t envFlag = scene.envAuthored ? 1u : 0u;
    fwrite(&envFlag, sizeof(envFlag), 1, f);
    saveEnvFloats(f, scene.environment);
    saveEnvFloats(f, scene.defaultEnvironment);
    // Node-ordered (byte-stable) through the compaction map; rows of
    // retired slots drop (cannot exist through the mutators).
    std::vector<std::pair<uint32_t, const EnvRow*>> rows;
    rows.reserve(scene.tables.envs.size());
    for (const EnvRow& row : scene.tables.envs)
      if (row.node < slotToDense.size() && slotToDense[row.node] != UINT32_MAX)
        rows.emplace_back(slotToDense[row.node], &row);
    std::sort(rows.begin(), rows.end(),
      [](const auto& a, const auto& b) { return a.first < b.first; });
    const uint32_t n = (uint32_t)rows.size();
    fwrite(&n, sizeof(n), 1, f);
    for (const auto& [node, row] : rows) {
      fwrite(&node, sizeof(node), 1, f);
      saveEnvFloats(f, row->environment);
    }
  }
  // Render-settings section (old loaders stop after the env section —
  // trailing bytes ignored): u32 authored flag + the record.
  {
    const uint32_t renderFlag = scene.hasRenderSettings ? 1u : 0u;
    fwrite(&renderFlag, sizeof(renderFlag), 1, f);
    saveRenderFloats(f, scene.renderSettings);
  }
  // Env-zone master switch (appended; old loaders ignore the tail).
  {
    const uint32_t zonesOn = scene.envZonesEnabled ? 1u : 0u;
    fwrite(&zonesOn, sizeof(zonesOn), 1, f);
  }
  // Volumetric-shadow caster section (appended LAST): count + count
  // bytes, all-zero when unused. Old loaders stop earlier - trailing
  // bytes ignored.
  saveFlagBytes(scene.castsVolumetricShadow);
  fclose(f);
}

bool mat4IsIdentity(const glm::mat4& m)
{
  return (
      m[0][0] == 1 && m[0][1] == 0 && m[0][2] == 0 && m[0][3] == 0 && m[1][0] == 0 && m[1][1] == 1 && m[1][2] == 0 && m[1][3] == 0 &&
      m[2][0] == 0 && m[2][1] == 0 && m[2][2] == 1 && m[2][3] == 0 && m[3][0] == 0 && m[3][1] == 0 && m[3][2] == 0 && m[3][3] == 1);
}

void fprintfMat4(FILE* f, const glm::mat4& m)
{
  if (mat4IsIdentity(m)) {
    fprintf(f, "Identity\n");
  } else {
    fprintf(f, "\n");
    for (int i = 0; i < 4; i++) {
      for (int j = 0; j < 4; j++)
        fprintf(f, "%f ;", m[i][j]);
      fprintf(f, "\n");
    }
  }
}

// ---- Env zones (see scene.h for the coverage contract) ------------------

// Zone half extents from the node's global transform columns (column
// lengths). An INFINITE component = unbounded on that local axis.
static inline void ZoneHalfExtents(const Scene& scene, NodeId zone,
  float out[3])
{
  const glm::mat4& g = scene.globalTransform[zone];
  out[0] = glm::length(glm::vec3(g[0]));
  out[1] = glm::length(glm::vec3(g[1]));
  out[2] = glm::length(glm::vec3(g[2]));
}

bool ZoneCovers(const Scene& scene, NodeId zone, const glm::vec3& p)
{
  if (zone >= scene.globalTransform.size())
    return false;
  // Disabled volumes cover nothing (Unreal bEnabled).
  const EnvVolumeRow* volRow = FindEnvVolume(scene, zone);
  const bool hasVol = volRow != nullptr;
  if (hasVol && !volRow->volume.enabled)
    return false;
  const uint32_t infMask = hasVol ? volRow->volume.infMask : 0u;
  const uint32_t negBit[3] = { kEnvInfNegX, kEnvInfNegY, kEnvInfNegZ };
  const uint32_t posBit[3] = { kEnvInfPosX, kEnvInfPosY, kEnvInfPosZ };
  const glm::mat4& g = scene.globalTransform[zone];
  float half[3];
  ZoneHalfExtents(scene, zone, half);
  // The transform IS the extents: g maps the unit cube [-1,1]^3 onto
  // the zone box, so the coverage test is the UNIT cube per axis in
  // g^-1 * p space — except per-face infinity (a set bit pushes that
  // face out) and degenerate flat axes without bits (an empty zone).
  // A degenerate axis WITH either bit unbounds the whole axis (faces
  // coincide at the node origin; same rule as the map raster).
  for (int a = 0; a < 3; ++a) {
    const bool flat = std::isfinite(half[a]) && !(half[a] > 0.0f);
    const bool faceInf = (infMask & negBit[a]) != 0
      || (infMask & posBit[a]) != 0;
    if (flat && !faceInf)
      return false; // flat on this axis: an empty zone
  }
  // Per-face infinity: a set inf bit pushes that face to -/+infinity;
  // a legacy INFINITE (or NaN) scale unbounds the whole axis. INF entries
  // poison the inverse (INF/INF = NaN in the cofactor arithmetic), so
  // unbounded axes get unit columns BEFORE inverting — those axes are
  // skipped by the test anyway.
  glm::mat4 frame = g;
  for (int a = 0; a < 3; ++a) {
    // Unbounded axes get unit columns BEFORE inverting (INF entries
    // poison the inverse); a degenerate (zero-length) finite axis with
    // either inf bit unbounds the whole axis the same way. Tested axes
    // keep their real columns.
    const bool flat = std::isfinite(half[a]) && !(half[a] > 0.0f);
    if (!std::isfinite(half[a])
      || (flat
        && ((infMask & negBit[a]) != 0 || (infMask & posBit[a]) != 0)))
      frame[a] = glm::vec4(a == 0, a == 1, a == 2, 0.0f);
  }
  const glm::vec3 local = glm::vec3(
    glm::inverse(frame) * glm::vec4(p, 1.0f));
  for (int a = 0; a < 3; ++a) {
    const bool flat = std::isfinite(half[a]) && !(half[a] > 0.0f);
    const bool faceInf = (infMask & negBit[a]) != 0
      || (infMask & posBit[a]) != 0;
    const bool negInf = !std::isfinite(half[a])
      || (infMask & negBit[a]) != 0 || (flat && faceInf);
    const bool posInf = !std::isfinite(half[a])
      || (infMask & posBit[a]) != 0 || (flat && faceInf);
    if (!negInf && !(local[a] >= -1.0f))
      return false;
    if (!posInf && !(local[a] <= 1.0f))
      return false;
  }
  return true;
}

float ZonePriority(const Scene& scene, NodeId zone, const SceneEnv& env)
{
  if (const EnvVolumeRow* volRow = FindEnvVolume(scene, zone))
    return volRow->volume.priority;
  return env.zonePriority;
}

NodeId ZoneAtPosition(const Scene& scene, const glm::vec3& p)
{
  NodeId best = UINT32_MAX;
  float bestPriority = 0.0f;
  double bestVolume = 0.0;
  for (const EnvRow& row : scene.tables.envs) {
    const NodeId node = row.node;
    const SceneEnv& env = row.environment;
    if (!ZoneCovers(scene, node, p))
      continue;
    // Bounded volume = the product of the finite half extents; a
    // legacy-INFINITE scale or ANY inf-mask face on an axis = unbounded
    // volume (loses ties against bounded zones — "innermost" wins
    // among equal priorities).
    float half[3];
    ZoneHalfExtents(scene, node, half);
    uint32_t infMask = 0u;
    if (const EnvVolumeRow* volRow = FindEnvVolume(scene, node))
      infMask = volRow->volume.infMask;
    const uint32_t faceBits[3] = { kEnvInfNegX | kEnvInfPosX,
      kEnvInfNegY | kEnvInfPosY, kEnvInfNegZ | kEnvInfPosZ };
    double volume = 1.0;
    bool unbounded = false;
    for (int a = 0; a < 3; ++a) {
      if (!std::isfinite(half[a]) || (infMask & faceBits[a]) != 0) {
        unbounded = true;
        break;
      }
      volume *= double(half[a]);
    }
    if (unbounded)
      volume = std::numeric_limits<double>::infinity();
    const float priority = ZonePriority(scene, node, env);
    const bool better = best == UINT32_MAX
      || priority > bestPriority
      || (priority == bestPriority && volume < bestVolume)
      || (priority == bestPriority && volume == bestVolume
        && node < best); // deterministic last-resort tiebreak
    if (better) {
      best = node;
      bestPriority = priority;
      bestVolume = volume;
    }
  }
  return best;
}

void dumpTransforms(const char* fileName, const Scene& scene)
{
  FILE* f = fopen(fileName, "a+");
  for (size_t i = 0; i < scene.localTransform.size(); i++) {
    fprintf(f, "Node[%d].localTransform: ", (int)i);
    fprintfMat4(f, scene.localTransform[i]);
    fprintf(f, "Node[%d].globalTransform: ", (int)i);
    fprintfMat4(f, scene.globalTransform[i]);
    fprintf(
        f, "Node[%d].globalDet = %f; localDet = %f\n", (int)i, glm::determinant(scene.globalTransform[i]),
        glm::determinant(scene.localTransform[i]));
  }
  fclose(f);
}

void printChangedNodes(const Scene& scene)
{
  for (int i = 0; i < MAX_NODE_LEVEL && (!scene.changedAtThisFrame[i].empty()); i++) {
    printf("Changed at level(%d):\n", i);

    for (const int& c : scene.changedAtThisFrame[i]) {
      int p = scene.hierarchy[c].parent;
      // scene.globalTransform_[c] = scene.globalTransform_[p] * scene.localTransform_[c];
      printf(" Node %d. Parent = %d; LocalTransform: ", c, p);
      fprintfMat4(stdout, scene.localTransform[i]);
      if (p > -1) {
        printf(" ParentGlobalTransform: ");
        fprintfMat4(stdout, scene.globalTransform[p]);
      }
    }
  }
}

// (shiftNodes died with Epoch 6b: the merge path remaps through explicit
// slot-association maps now — see mergeScenes — and deletion retires
// slots instead of compacting, so there is nothing left to shift.)


/**
  There are different use cases for scene merging.
  The simplest one is the direct "gluing" of multiple scenes into one [all the material lists and mesh lists are merged and indices in all
  scene nodes are shifted appropriately] The second one is creating a "grid" of objects (or scenes) with the same material and mesh sets.
  For the second use case we need two flags: 'mergeMeshes' and 'mergeMaterials' to avoid shifting mesh indices
*/
void mergeScenes(
    Scene& scene,
    const std::vector<Scene*>& scenes,
    const std::vector<glm::mat4>& rootTransforms,
    const std::vector<uint32_t>& meshCounts,
    bool mergeMeshes,
    bool mergeMaterials,
    bool mergeLights
)
{
  // Create new root node (slot 0 of the reset destination).
  scene.hierarchy.assign(1, Hierarchy{
      .parent      = -1,
      .firstChild  = -1,
      .nextSibling = -1,
      .lastSibling = -1,
      .level       = 0,
      });
  // The fresh-root reset also resets the slot machinery (Epoch 6b): a
  // stale free list would hand addNode recycled slots whose parallel-
  // array entries belong to the pre-reset graph.
  scene.nodeGeneration.assign(1, kNodeSlotAliveBit);
  scene.nodeFreeList.clear();

  // I dont care about names for now at least.
  //scene.metadata[0].name = 0;
  //scene.nodeNames      = { "NewRoot" };

  scene.localTransform.assign(1, glm::mat4(1.f));
  scene.globalTransform.assign(1, glm::mat4(1.f));
  scene.staticRoot.assign(1, 0);
  scene.staticNode.assign(1, 0);
  scene.noLightmap.assign(1, 0);
  scene.lightmap.assign(1, 0);
  scene.castsVolumetricShadow.assign(1, 0);
  scene.ditherCloseup.assign(1, 0);
  scene.hidden.assign(1, 0);
  // The hierarchy reset above makes EVERY prior association row dead
  // (node ids no longer resolve) — clear all tables with it. Lights
  // especially: cooked punctual lights are Cluster pool assets (pool
  // indices cannot shift across scenes here — lights merge via
  // World::ResolveAndFixup_clusters instead).
  scene.tables = {};
  scene.rowsForNode.clear();
  scene.metadata.clear();

  if (scenes.empty())
    return;

  int meshOffs    = 0;
  int nameOffs    = (int)scene.nodeNames.size();
  int materialOfs = 0;
  auto meshCount  = meshCounts.begin();

  if (!mergeMaterials)
    scene.materialNames = scenes[0]->materialNames;

  // FIXME: too much logic (for all the components in a scene, though mesh data and materials go separately - they're dedicated data lists)
  // Authored environment: first authored scene wins (resolved look +
  // default block travel together; per-node blocks offset-merge).
  bool envTaken = false;
  // Per-source slot-association map (Epoch 6b: the dense +offset remap
  // became an explicit map — correct for holed sources, numerically
  // identical for dense ones). srcSlot -> dst NodeId, UINT32_MAX = hole.
  std::vector<NodeId> nodeMap;
  // Each source's root in the DESTINATION (its first live slot — slot 0
  // for a dense source, matching the old offs arithmetic exactly).
  std::vector<NodeId> srcRoots;
  srcRoots.reserve(scenes.size());
  for (const Scene* s : scenes) {
    const uint32_t srcSlotCount = (uint32_t)s->hierarchy.size();
    nodeMap.assign(srcSlotCount, UINT32_MAX);

    // Pass 1: allocate a destination slot per LIVE source node (level
    // +1: everything sits one rung lower under the merged root) and
    // copy its payload. Links stay default; pass 2 maps them.
    for (uint32_t src = 0; src < srcSlotCount; ++src) {
      if (!IsLiveSlot(*s, src))
        continue;
      const int dst = addNode(scene, -1, s->hierarchy[src].level + 1);
      assert(dst >= 0);
      nodeMap[src] = (NodeId)dst;
      scene.localTransform[size_t(dst)] = s->localTransform[src];
      scene.globalTransform[size_t(dst)] = s->globalTransform[src];
      if (s->staticRoot.size() > size_t(src))
        scene.staticRoot[size_t(dst)] = s->staticRoot[src];
      if (s->noLightmap.size() > size_t(src))
        scene.noLightmap[size_t(dst)] = s->noLightmap[src];
      if (s->lightmap.size() > size_t(src))
        scene.lightmap[size_t(dst)] = s->lightmap[src];
      if (s->hidden.size() > size_t(src))
        scene.hidden[size_t(dst)] = s->hidden[src];
      if (s->castsVolumetricShadow.size() > size_t(src))
        scene.castsVolumetricShadow[size_t(dst)] = s->castsVolumetricShadow[src];
      if (s->ditherCloseup.size() > size_t(src))
        scene.ditherCloseup[size_t(dst)] = s->ditherCloseup[src];
    }

    // Pass 2: hierarchy links through the map (a live node's links point
    // at live nodes by the deletion discipline; UINT32_MAX falls back to
    // -1 for safety).
    auto mapLink = [&nodeMap](int link) -> int {
      if (link < 0 || size_t(link) >= nodeMap.size())
        return -1;
      const NodeId mapped = nodeMap[size_t(link)];
      return (mapped != UINT32_MAX) ? int(mapped) : -1;
    };
    for (uint32_t src = 0; src < srcSlotCount; ++src) {
      if (nodeMap[src] == UINT32_MAX)
        continue;
      Hierarchy& h = scene.hierarchy[nodeMap[src]];
      const Hierarchy& sh = s->hierarchy[src];
      h.parent      = mapLink(sh.parent);
      h.firstChild  = mapLink(sh.firstChild);
      h.nextSibling = mapLink(sh.nextSibling);
      h.lastSibling = mapLink(sh.lastSibling);
    }

    // Name pool first (metadata offsets into it below).
    mergeVectors(scene.nodeNames, s->nodeNames);
    if (mergeMaterials)
      mergeVectors(scene.materialNames, s->materialNames);

    // The source's root for the relink pass: first live slot (slot 0 of
    // a dense source — the old code's `offs` node).
    NodeId srcRoot = UINT32_MAX;
    for (uint32_t src = 0; src < srcSlotCount; ++src)
      if (nodeMap[src] != UINT32_MAX) {
        srcRoot = nodeMap[src];
        break;
      }
    srcRoots.push_back(srcRoot);

    // Typed tables: append the source rows through the map with the
    // resource offsets (one row at a time — SetMesh/SetMaterial keep
    // the derived index correct as we go).
    // Dead rows (a hole where a row still points — a tampered scene)
    // drop silently like every other row validator.
    for (const MeshRow& row : s->tables.meshes)
      if (row.node < nodeMap.size() && nodeMap[row.node] != UINT32_MAX)
        SetMesh(scene, nodeMap[row.node],
          row.mesh + (mergeMeshes ? uint32_t(meshOffs) : 0u));
    for (const MaterialRow& row : s->tables.materials)
      if (row.node < nodeMap.size() && nodeMap[row.node] != UINT32_MAX)
        SetMaterial(scene, nodeMap[row.node],
          row.material + (mergeMaterials ? uint32_t(materialOfs) : 0u));
    // Node metadata: names offset into the merged pool, uuids verbatim.
    for (uint32_t src = 0; src < srcSlotCount; ++src) {
      if (nodeMap[src] == UINT32_MAX)
        continue;
      if (src < s->metadata.size()) {
        if (s->metadata[src].name != 0xFFFFFFFFu)
          SetNodeNameIndex(scene, nodeMap[src],
            s->metadata[src].name + uint32_t(nameOffs));
        SetNodeUuidRaw(scene, nodeMap[src], s->metadata[src].uuid);
      }
    }
    // Per-node authored env blocks: node keys map like the tables;
    // values are inline structs (no pool indirection, no value offset).
    for (const EnvRow& row : s->tables.envs)
      if (row.node < nodeMap.size() && nodeMap[row.node] != UINT32_MAX)
        SetEnv(scene, nodeMap[row.node], row.environment);
    for (const EnvVolumeRow& row : s->tables.envVolumes)
      if (row.node < nodeMap.size() && nodeMap[row.node] != UINT32_MAX)
        SetEnvVolume(scene, nodeMap[row.node], row.volume);
    if (!envTaken && s->envAuthored) {
      scene.environment = s->environment;
      scene.envAuthored = true;
      scene.defaultEnvironment = s->defaultEnvironment;
      envTaken = true;
    }
    // Render settings: first authored look wins (same rule as the env;
    // the look is scene-global, never node-shifted).
    if (!scene.hasRenderSettings && s->hasRenderSettings) {
      scene.renderSettings = s->renderSettings;
      scene.defaultRenderSettings = s->defaultRenderSettings;
      scene.hasRenderSettings = true;
    }

    materialOfs += (int)s->materialNames.size();
    nameOffs += (int)s->nodeNames.size();

    if (mergeMeshes) {
      meshOffs += *meshCount;
      meshCount++;
    }
  }

  // Attach each source's root under the merged root (the old
  // 'fixing nextSibling of the old roots' pass, map-based): a chain in
  // source order, rootTransforms pre-multiplied onto the roots.
  {
    NodeId prevRoot = UINT32_MAX;
    for (size_t idx = 0; idx < scenes.size(); ++idx) {
      const NodeId root = srcRoots[idx];
      if (root == UINT32_MAX)
        continue; // empty source: contributes no hierarchy
      scene.hierarchy[root].parent = 0;
      if (!rootTransforms.empty())
        scene.localTransform[root] =
          rootTransforms[idx] * scene.localTransform[root];
      if (prevRoot == UINT32_MAX)
        scene.hierarchy[0].firstChild = int(root);
      else
        scene.hierarchy[prevRoot].nextSibling = int(root);
      prevRoot = root;
    }
    if (prevRoot != UINT32_MAX)
      scene.hierarchy[prevRoot].nextSibling = -1;
    else
      scene.hierarchy[0].firstChild = -1; // no live nodes at all
  }

  scene.staticNode.assign(scene.hierarchy.size(), 0);
  RebuildStaticCache(scene);
}

void dumpSceneToDot(const char* fileName, const Scene& scene, int* visited)
{
  FILE* f = fopen(fileName, "w");
  fprintf(f, "digraph G\n{\n");
  for (size_t i = 0; i < scene.globalTransform.size(); i++) {
    if (!IsLiveSlot(scene, uint32_t(i)))
      continue; // retired slot (Epoch 6b): a hole, not a node
    std::string name  = "";
    std::string extra = "";
    if (const uint32_t strID = GetNodeNameIndex(scene, NodeId(i));
      strID != 0xFFFFFFFFu) {
      name      = scene.nodeNames[strID];
    }
    if (visited) {
      if (visited[i])
        extra = ", color = red";
    }
    fprintf(f, "n%d [label=\"%s\" %s]\n", (int)i, name.c_str(), extra.c_str());
  }
  for (size_t i = 0; i < scene.hierarchy.size(); i++) {
    int p = scene.hierarchy[i].parent;
    if (p > -1)
      fprintf(f, "\t n%d -> n%d\n", p, (int)i);
  }
  fprintf(f, "}\n");
  fclose(f);
}

int getNodeLevel(const Scene& scene, int n) {
  int level = -1;
  for (int p = 0; p != -1; p = scene.hierarchy[p].parent, level++);
  return level;
}

// ---- node deletion (Epoch 6b: RETIREMENT, not compaction) ----------------
// The pre-6b compaction machinery — the newIndices remap table,
// findLastNonDeletedItem, the ten shift*RowNodes helpers,
// shiftNodeMetadata, eraseSelected over the parallel arrays — is
// DELETED: retirement keeps every survivor's id put, so there is no
// remap to build and nothing to shift. What replaces it lives above:
// UnlinkNodeFromParent + RetireNodeSlot (O(1) per slot), plus this
// O(N) subtree collect.

// Add an index to a sorted index array
static void addUniqueIdx(std::vector<uint32_t>& v, uint32_t index)
{
  if (!std::binary_search(v.begin(), v.end(), index))
    v.push_back(index);
}

// Recurse down from a node and collect all nodes which are already marked for deletion
static void collectNodesToDelete(const Scene& scene, int node, std::vector<uint32_t>& nodes)
{
  for (int n = scene.hierarchy[node].firstChild; n != -1; n = scene.hierarchy[n].nextSibling) {
    addUniqueIdx(nodes, n);
    collectNodesToDelete(scene, n, nodes);
  }
}

void deleteSceneNodes(Scene& scene, const std::vector<uint32_t>& nodesToDelete)
{
  // 0) Collect every subtree under the requested roots. The input is
  // normalized FIRST (sorted-unique — addUniqueIdx's binary_search and
  // the doomed-parent skip below both need it; the old code trusted
  // callers to hand it sorted) and again after collection (DFS pushes
  // are not order-guaranteed on hand-built graphs).
  auto indicesToDelete = nodesToDelete;
  std::sort(indicesToDelete.begin(), indicesToDelete.end());
  indicesToDelete.erase(std::unique(indicesToDelete.begin(), indicesToDelete.end()), indicesToDelete.end());
  for (uint32_t i : indicesToDelete)
    if (i < scene.hierarchy.size() && IsLiveSlot(scene, i))
      collectNodesToDelete(scene, int(i), indicesToDelete);
  std::sort(indicesToDelete.begin(), indicesToDelete.end());
  indicesToDelete.erase(std::unique(indicesToDelete.begin(), indicesToDelete.end()), indicesToDelete.end());

  // 1) Detach each deleted node whose parent SURVIVES (a node doomed
  // together with its parent takes its links to the grave — the
  // parent's child list dies with it). Surviving parents' sibling
  // lists are repaired inside UnlinkNodeFromParent
  // (firstChild/nextSibling/lastSibling surgery).
  for (uint32_t n : indicesToDelete) {
    if (n >= scene.hierarchy.size() || !IsLiveSlot(scene, n))
      continue; // stale input id or already retired: no-op
    const int p = scene.hierarchy[n].parent;
    if (p < 0)
      continue; // root: nothing to detach from
    if (std::binary_search(indicesToDelete.begin(), indicesToDelete.end(), uint32_t(p)))
      continue; // the parent dies too: its child list is moot
    UnlinkNodeFromParent(scene, int(n));
  }

  // 2) Retire each slot: every association row drops, the slot's
  // per-node state resets, the generation bumps (handles minted
  // against the old life die here), and the slot re-enters
  // circulation through addNode. Every OTHER node's id stays put —
  // that is the whole epoch.
  for (uint32_t n : indicesToDelete)
    RetireNodeSlot(scene, n);

  // 3) Reconcile the derived index once (RetireNodeSlot cleared the
  // retired slots' entries and dropped their rows; the rebuild is the
  // discipline backstop — it also compacts out any row that still
  // points at a dead slot).
  RebuildNodeRows(scene);

  RebuildStaticCache(scene);
  // Orphaned light/camera/decal pool definitions are kept (pre-6b
  // behavior): they are small and dropping them would require remapping
  // the surviving table values' pool indices.
}
