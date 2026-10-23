// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Portions Copyright (c) 2022 Packt
// Modified by Samuel Martin, 2026
// Licensed under the MIT License. See external/Mastering-Graphics-Programming-with-Vulkan/LICENSE.txt for details.

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
  assert(parent == -1 || (parent >= 0 && parent < nodeCount));
  const int node = (int)scene.hierarchy.size();
  {
    // TODO: resize aux arrays (local/global etc.)
    scene.localTransform.push_back(glm::mat4(1.0f));
    scene.globalTransform.push_back(glm::mat4(1.0f));
  scene.staticRoot.push_back(0);
  scene.staticNode.push_back(0);
  scene.noLightmap.push_back(0);
  scene.lightmap.push_back(0);
  scene.ditherCloseup.push_back(0);
  scene.hidden.push_back(0);
  }
  scene.hierarchy.push_back({ .parent = parent, .lastSibling = -1 });
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
  for (size_t n = 0; n < scene.hierarchy.size(); ++n) {
    if (scene.hierarchy[n].parent == -1)
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

  for (size_t i = 0; i < scene.localTransform.size(); i++)
    if (scene.nameForNode.contains(i)) {
      int strID = scene.nameForNode.at(i);
      if (strID > -1)
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
  for (int c : scene.changedAtThisFrame[0]) {
    scene.globalTransform[c] = scene.localTransform[c];
    wasUpdated = true;
  }
  scene.changedAtThisFrame[0].clear();

  for (int i = 1; i < MAX_NODE_LEVEL; i++) {
    for (int c : scene.changedAtThisFrame[i]) {
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

void loadMap(FILE* f, std::unordered_map<uint32_t, uint32_t>& map)
{
  std::vector<uint32_t> ms;

  uint32_t sz = 0;
  fread(&sz, 1, sizeof(sz), f);

  ms.resize(sz);
  fread(ms.data(), sizeof(uint32_t), sz, f);

  for (size_t i = 0; i < (sz / 2); i++)
    map[ms[i * 2 + 0]] = ms[i * 2 + 1];
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
  scene.ditherCloseup.assign(sz, 0);
  scene.lightForNode.clear();
  // TODO: check > -1
  // TODO: recalculate changedAtThisLevel() - find max depth of a node [or save scene.maxLevel]
  fread(scene.localTransform.data(), sizeof(glm::mat4), sz, f);
  fread(scene.globalTransform.data(), sizeof(glm::mat4), sz, f);
  fread(scene.hierarchy.data(), sizeof(Hierarchy), sz, f);

  // MeshData for node [index to some list of buffers]
  loadMap(f, scene.materialForNode);
  loadMap(f, scene.meshForNode);

  if (!feof(f)) {
    loadMap(f, scene.nameForNode);
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
        std::unordered_map<NodeId, SceneEnv> table;
        for (uint32_t i = 0; i < nodeCount && ok; ++i) {
          uint32_t node = 0;
          SceneEnv block{};
          if (fread(&node, sizeof(node), 1, f) == 1 && loadEnvFloats(f, block))
            table[node] = block;
          else
            ok = false;
        }
        if (ok) {
          scene.environment = resolved;
          scene.envAuthored = envFlag != 0;
          scene.defaultEnvironment = def;
          scene.envForNode = std::move(table);
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
  {
    uint32_t zonesOn = 0;
    if (fread(&zonesOn, sizeof(zonesOn), 1, f) == 1)
      scene.envZonesEnabled = zonesOn != 0;
  }
  // Cooked punctual lights are NOT carried in this legacy format: they
  // are Cluster pool assets (see Scene::lightForNode) and round-trip
  // through paks only.

  fclose(f);

  MarkAllRootsChanged(scene);
  recalculateGlobalTransforms(scene);
}

void saveMap(FILE* f, const std::unordered_map<uint32_t, uint32_t>& map)
{
  std::vector<uint32_t> ms;
  ms.reserve(map.size() * 2);
  for (const auto& m : map) {
    ms.push_back(m.first);
    ms.push_back(m.second);
  }
  const uint32_t sz = static_cast<uint32_t>(ms.size());
  fwrite(&sz, sizeof(sz), 1, f);
  fwrite(ms.data(), sizeof(uint32_t), ms.size(), f);
}

void saveScene(const char* fileName, const Scene& scene)
{
  FILE* f = fopen(fileName, "wb");

  const uint32_t sz = (uint32_t)scene.hierarchy.size();
  fwrite(&sz, sizeof(sz), 1, f);

  fwrite(scene.localTransform.data(), sizeof(glm::mat4), sz, f);
  fwrite(scene.globalTransform.data(), sizeof(glm::mat4), sz, f);
  fwrite(scene.hierarchy.data(), sizeof(Hierarchy), sz, f);

  // MeshData for node [index to some list of buffers]
  saveMap(f, scene.materialForNode);
  saveMap(f, scene.meshForNode);

  if (!scene.nodeNames.empty() && !scene.nameForNode.empty()) {
    saveMap(f, scene.nameForNode);
    saveStringList(f, scene.nodeNames);
    saveStringList(f, scene.materialNames);
  }
  // Static-subtree section: always written (possibly all-zero) so new
  // loaders can rely on its presence; old loaders ignore trailing bytes.
  {
    const uint32_t staticCount = (uint32_t)scene.hierarchy.size();
    fwrite(&staticCount, sizeof(staticCount), 1, f);
    if (staticCount > 0) {
      if (scene.staticRoot.size() == scene.hierarchy.size()) {
        fwrite(scene.staticRoot.data(), 1, staticCount, f);
      } else {
        std::vector<uint8_t> zeros(staticCount, 0);
        fwrite(zeros.data(), 1, staticCount, f);
      }
    }
  }
  // Lightmap-exclusion section (v3): u32 count + count bytes, all-zero when
  // no exclusions. Old loaders stop after the static section; new loaders
  // treat a missing tail as "no exclusions".
  {
    const uint32_t lmCount = (uint32_t)scene.hierarchy.size();
    fwrite(&lmCount, sizeof(lmCount), 1, f);
    if (lmCount > 0) {
      if (scene.noLightmap.size() == scene.hierarchy.size()) {
        fwrite(scene.noLightmap.data(), 1, lmCount, f);
      } else {
        std::vector<uint8_t> zeros(lmCount, 0);
        fwrite(zeros.data(), 1, lmCount, f);
      }
    }
  }
  // Visibility section (v4): hidden flags, all-zero (visible) when unset.
  {
    const uint32_t hidCount = (uint32_t)scene.hierarchy.size();
    fwrite(&hidCount, sizeof(hidCount), 1, f);
    if (hidCount > 0) {
      if (scene.hidden.size() == scene.hierarchy.size()) {
        fwrite(scene.hidden.data(), 1, hidCount, f);
      } else {
        std::vector<uint8_t> zeros(hidCount, 0);
        fwrite(zeros.data(), 1, hidCount, f);
      }
    }
  }
  // Dither-on-closeup section (v5): all-zero when unused.
  {
    const uint32_t dCount = (uint32_t)scene.hierarchy.size();
    fwrite(&dCount, sizeof(dCount), 1, f);
    if (dCount > 0) {
      if (scene.ditherCloseup.size() == scene.hierarchy.size()) {
        fwrite(scene.ditherCloseup.data(), 1, dCount, f);
      } else {
        std::vector<uint8_t> zeros(dCount, 0);
        fwrite(zeros.data(), 1, dCount, f);
      }
    }
  }
  // Authored environment section (v6): u32 authored flag + resolved look
  // + default block + per-node blocks (u32 count + node-ordered (node,
  // record) pairs). Always written; old loaders stop after the dither
  // section (trailing bytes ignored).
  {
    const uint32_t envFlag = scene.envAuthored ? 1u : 0u;
    fwrite(&envFlag, sizeof(envFlag), 1, f);
    saveEnvFloats(f, scene.environment);
    saveEnvFloats(f, scene.defaultEnvironment);
    std::vector<std::pair<uint32_t, SceneEnv>> pairs(
      scene.envForNode.begin(), scene.envForNode.end());
    std::sort(pairs.begin(), pairs.end(),
      [](const auto& a, const auto& b) { return a.first < b.first; });
    const uint32_t n = (uint32_t)pairs.size();
    fwrite(&n, sizeof(n), 1, f);
    for (const auto& [node, env] : pairs) {
      fwrite(&node, sizeof(node), 1, f);
      saveEnvFloats(f, env);
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
  const auto vit = scene.envVolumeForNode.find(zone);
  const bool hasVol = vit != scene.envVolumeForNode.end();
  if (hasVol && !vit->second.enabled)
    return false;
  const uint32_t infMask = hasVol ? vit->second.infMask : 0u;
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
  const auto it = scene.envVolumeForNode.find(zone);
  if (it != scene.envVolumeForNode.end())
    return it->second.priority;
  return env.zonePriority;
}

NodeId ZoneAtPosition(const Scene& scene, const glm::vec3& p)
{
  NodeId best = UINT32_MAX;
  float bestPriority = 0.0f;
  double bestVolume = 0.0;
  for (const auto& [node, env] : scene.envForNode) {
    if (!ZoneCovers(scene, node, p))
      continue;
    // Bounded volume = the product of the finite half extents; a
    // legacy-INFINITE scale or ANY inf-mask face on an axis = unbounded
    // volume (loses ties against bounded zones — "innermost" wins
    // among equal priorities).
    float half[3];
    ZoneHalfExtents(scene, node, half);
    uint32_t infMask = 0u;
    if (const auto vit = scene.envVolumeForNode.find(node);
      vit != scene.envVolumeForNode.end())
      infMask = vit->second.infMask;
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

// Shift all hierarchy components in the nodes
void shiftNodes(Scene& scene, int startOffset, int nodeCount, int shiftAmount)
{
  auto shiftNode = [shiftAmount](Hierarchy& node) {
    if (node.parent > -1)
      node.parent += shiftAmount;
    if (node.firstChild > -1)
      node.firstChild += shiftAmount;
    if (node.nextSibling > -1)
      node.nextSibling += shiftAmount;
    if (node.lastSibling > -1)
      node.lastSibling += shiftAmount;
    // node->level does not require to be shifted
  };

  // If there are too many nodes, we can use std::execution::par with std::transform:
  //	 std::transform(scene.hierarchy_.begin() + startOffset,
  //                  scene.hierarchy_.begin() + nodeCount,
  //                  scene.hierarchy_.begin() + startOffset,
  //                  shiftNode);
  //	 for (auto i = scene.hierarchy_.begin() + startOffset ; i != scene.hierarchy_.begin() + nodeCount ; i++)
  //		 shiftNode(*i);

  for (int i = 0; i < nodeCount; i++)
    shiftNode(scene.hierarchy[i + startOffset]);
}

using ItemMap = std::unordered_map<uint32_t, uint32_t>;

// Add the items from otherMap shifting indices and values along the way
void mergeMaps(ItemMap& m, const ItemMap& otherMap, int indexOffset, int itemOffset)
{
  for (const auto& i : otherMap)
    m[i.first + indexOffset] = i.second + itemOffset;
}

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
  // Create new root node
  scene.hierarchy = {
    {
     .parent      = -1,
     .firstChild  = 1,
     .nextSibling = -1,
     .lastSibling = -1,
     .level       = 0,
     }
  };

  // I dont care about names for now at least.
  //scene.nameForNode[0] = 0;
  //scene.nodeNames      = { "NewRoot" };

  scene.localTransform.push_back(glm::mat4(1.f));
  scene.globalTransform.push_back(glm::mat4(1.f));
  scene.staticRoot.push_back(0);
  scene.staticNode.push_back(0);
  scene.noLightmap.push_back(0);
  scene.lightmap.push_back(0);
  scene.ditherCloseup.push_back(0);
  scene.hidden.push_back(0);
  // Cooked punctual lights are Cluster pool assets: their node links
  // (pool indices) cannot shift across scenes here — lights merge via
  // World::ResolveAndFixup_clusters instead.
  scene.lightForNode.clear();

  if (scenes.empty())
    return;

  int offs        = 1;
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
  for (const Scene* s : scenes) {
    mergeVectors(scene.localTransform, s->localTransform);
    mergeVectors(scene.globalTransform, s->globalTransform);

    mergeVectors(scene.hierarchy, s->hierarchy);

    if (s->staticRoot.size() == s->hierarchy.size())
      mergeVectors(scene.staticRoot, s->staticRoot);
    else
      scene.staticRoot.insert(scene.staticRoot.end(), s->hierarchy.size(), 0);

    if (s->noLightmap.size() == s->hierarchy.size())
      mergeVectors(scene.noLightmap, s->noLightmap);
    else
      scene.noLightmap.insert(scene.noLightmap.end(), s->hierarchy.size(), 0);

    if (s->lightmap.size() == s->hierarchy.size())
      mergeVectors(scene.lightmap, s->lightmap);
    else
      scene.lightmap.insert(scene.lightmap.end(), s->hierarchy.size(), 0);

    if (s->hidden.size() == s->hierarchy.size())
      mergeVectors(scene.hidden, s->hidden);
    else
      scene.hidden.insert(scene.hidden.end(), s->hierarchy.size(), 0);

    if (s->ditherCloseup.size() == s->hierarchy.size())
      mergeVectors(scene.ditherCloseup, s->ditherCloseup);
    else
      scene.ditherCloseup.insert(scene.ditherCloseup.end(), s->hierarchy.size(), 0);

    mergeVectors(scene.nodeNames, s->nodeNames);
    if (mergeMaterials)
      mergeVectors(scene.materialNames, s->materialNames);

    const int nodeCount = (int)s->hierarchy.size();

    shiftNodes(scene, offs, nodeCount, offs);

    mergeMaps(scene.meshForNode, s->meshForNode, offs, mergeMeshes ? meshOffs : 0);
    mergeMaps(scene.materialForNode, s->materialForNode, offs, mergeMaterials ? materialOfs : 0);
    mergeMaps(scene.nameForNode, s->nameForNode, offs, nameOffs);
    // Per-node authored env blocks: node keys shift like the other maps;
    // values are inline structs (no pool indirection, no value offset).
    for (const auto& [node, env] : s->envForNode)
      scene.envForNode[node + offs] = env;
    for (const auto& [node, vol] : s->envVolumeForNode)
      scene.envVolumeForNode[node + offs] = vol;
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

    offs += nodeCount;

    materialOfs += (int)s->materialNames.size();
    nameOffs += (int)s->nodeNames.size();

    if (mergeMeshes) {
      meshOffs += *meshCount;
      meshCount++;
    }
  }

  // fixing 'nextSibling' fields in the old roots (zero-index in all the scenes)
  offs    = 1;
  int idx = 0;
  for (const Scene* s : scenes) {
    const int nodeCount = (int)s->hierarchy.size();
    const bool isLast   = (idx == scenes.size() - 1);
    // calculate new next sibling for the old scene roots
    const int next = isLast ? -1 : offs + nodeCount;

    scene.hierarchy[offs].nextSibling = next;
    // attach to new root
    scene.hierarchy[offs].parent = 0;

    // transform old root nodes, if the transforms are given
    if (!rootTransforms.empty())
      scene.localTransform[offs] = rootTransforms[idx] * scene.localTransform[offs];

    offs += nodeCount;
    idx++;
  }

  // now, shift levels of all nodes below the root
  for (auto i = scene.hierarchy.begin() + 1; i != scene.hierarchy.end(); i++)
    i->level++;

  scene.staticNode.assign(scene.hierarchy.size(), 0);
  RebuildStaticCache(scene);
}

void dumpSceneToDot(const char* fileName, const Scene& scene, int* visited)
{
  FILE* f = fopen(fileName, "w");
  fprintf(f, "digraph G\n{\n");
  for (size_t i = 0; i < scene.globalTransform.size(); i++) {
    std::string name  = "";
    std::string extra = "";
    if (scene.nameForNode.contains(i)) {
      int strID = scene.nameForNode.at(i);
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

// A rather long algorithm (and the auxiliary routines) to delete a number of scene nodes from the hierarchy

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

int findLastNonDeletedItem(const Scene& scene, const std::vector<int>& newIndices, int node)
{
  // we have to be more subtle:
  //   if the (newIndices[firstChild_] == -1), we should follow the link and extract the last non-removed item
  //   ..
  if (node == -1)
    return -1;

  return (newIndices[node] == -1) ? findLastNonDeletedItem(scene, newIndices, scene.hierarchy[node].nextSibling) : newIndices[node];
}

void shiftMapIndices(std::unordered_map<uint32_t, uint32_t>& items, const std::vector<int>& newIndices)
{
  std::unordered_map<uint32_t, uint32_t> newItems;
  for (const auto& m : items) {
    int newIndex = newIndices[m.first];
    if (newIndex != -1)
      newItems[newIndex] = m.second;
  }
  items = newItems;
}

// Struct-value variant (per-node authored blocks like SceneEnv): keys
// remap like the index maps above; values ride unchanged (no pool).
void shiftEnvMapIndices(std::unordered_map<uint32_t, SceneEnv>& items, const std::vector<int>& newIndices)
{
  std::unordered_map<uint32_t, SceneEnv> newItems;
  for (const auto& m : items) {
    int newIndex = newIndices[m.first];
    if (newIndex != -1)
      newItems[newIndex] = m.second;
  }
  items = newItems;
}

// Approximately an O ( N * Log(N) * Log(M)) algorithm (N = scene.size, M = nodesToDelete.size) to delete a collection of nodes from scene
// graph
void deleteSceneNodes(Scene& scene, const std::vector<uint32_t>& nodesToDelete)
{
  // 0) Add all the nodes down below in the hierarchy
  auto indicesToDelete = nodesToDelete;
  for (uint32_t i : indicesToDelete)
    collectNodesToDelete(scene, i, indicesToDelete);

  // aux array with node indices to keep track of the moved ones [moved = [](node) { return (node != nodes[node]); ]
  std::vector<int> nodes(scene.hierarchy.size());
  std::iota(nodes.begin(), nodes.end(), 0);

  // 1.a) Move all the indicesToDelete to the end of 'nodes' array (and cut them off, a variation of swap'n'pop for multiple elements)
  const size_t oldSize = nodes.size();
  eraseSelected(nodes, indicesToDelete);

  // 1.b) Make a newIndices[oldIndex] mapping table
  std::vector<int> newIndices(oldSize, -1);
  for (int i = 0; i < nodes.size(); i++)
    newIndices[nodes[i]] = i;

  // 2) Replace all non-null parent/firstChild/nextSibling pointers in all the nodes by new positions
  auto nodeMover = [&scene, &newIndices](Hierarchy& h) {
    return Hierarchy{
      .parent      = (h.parent != -1) ? newIndices[h.parent] : -1,
      .firstChild  = findLastNonDeletedItem(scene, newIndices, h.firstChild),
      .nextSibling = findLastNonDeletedItem(scene, newIndices, h.nextSibling),
      .lastSibling = findLastNonDeletedItem(scene, newIndices, h.lastSibling),
    };
  };
  std::transform(scene.hierarchy.begin(), scene.hierarchy.end(), scene.hierarchy.begin(), nodeMover);

  // 3) Finally throw away the hierarchy items
  eraseSelected(scene.hierarchy, indicesToDelete);

  // 4) As in mergeScenes() routine we also have to adjust all the "components" (i.e., meshes, materials, names and transformations)

  // 4a) Transformations are stored in arrays, so we just erase the items as we did with the scene.hierarchy_
  eraseSelected(scene.localTransform, indicesToDelete);
  eraseSelected(scene.globalTransform, indicesToDelete);
  if (scene.staticRoot.size() == oldSize)
    eraseSelected(scene.staticRoot, indicesToDelete);
  else
    scene.staticRoot.assign(scene.hierarchy.size(), 0);
  if (scene.staticNode.size() == oldSize)
    eraseSelected(scene.staticNode, indicesToDelete);
  else
    scene.staticNode.assign(scene.hierarchy.size(), 0);
  if (scene.noLightmap.size() == oldSize)
    eraseSelected(scene.noLightmap, indicesToDelete);
  else
    scene.noLightmap.assign(scene.hierarchy.size(), 0);
  if (scene.lightmap.size() == oldSize)
    eraseSelected(scene.lightmap, indicesToDelete);
  else
    scene.lightmap.assign(scene.hierarchy.size(), 0);
  if (scene.hidden.size() == oldSize)
    eraseSelected(scene.hidden, indicesToDelete);
  else
    scene.hidden.assign(scene.hierarchy.size(), 0);
  if (scene.ditherCloseup.size() == oldSize)
    eraseSelected(scene.ditherCloseup, indicesToDelete);
  else
    scene.ditherCloseup.assign(scene.hierarchy.size(), 0);
  RebuildStaticCache(scene);

  // 4b) All the perFrame should change the key values with the newIndices[] array
  shiftMapIndices(scene.meshForNode, newIndices);
  shiftMapIndices(scene.materialForNode, newIndices);
  shiftMapIndices(scene.nameForNode, newIndices);
  shiftMapIndices(scene.lightForNode, newIndices);
  // Orphaned light definitions (no node left referencing them) are kept:
  // they are small and dropping them would require remapping the surviving
  // lightForNode values.
  // Per-node authored env blocks follow their nodes (dropped with them);
  // the scene-global resolved/default blocks are node-independent.
  shiftEnvMapIndices(scene.envForNode, newIndices);

  // 5) scene node names list is not modified, but in principle it can be (remove all non-used items and adjust the nameForNode_ map)
  // 6) Material names list is not modified also, but if some materials fell out of use
}
