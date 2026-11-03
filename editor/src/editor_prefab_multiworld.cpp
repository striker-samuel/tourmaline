// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// A separate editable .tour definition is a separate WORLD, not a
// different namespace/selection mode in EditorApp's parent world.
// The host's ResourceManager and GPU device remain shared.
#include "editor_app.h"
#include "editor_prefab_sync.h"
#include <gltf_import/gltf.h>
#include <Engine/Asset/tour.h>
#include <Engine/Scene/scene.h>
#include <glm/gtx/matrix_decompose.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace editor {
namespace {
namespace fs = std::filesystem;
bool Finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
bool Near(const glm::mat4& a, const glm::mat4& b) {
    for (int i=0;i<4;++i) for (int j=0;j<4;++j)
        if (std::abs(a[i][j]-b[i][j])>0.0005f) return false;
    return true;
}
bool DescendantOf(const Scene& scene, uint32_t n, uint32_t root) {
    for (size_t guard=0; guard<scene.hierarchy.size(); ++guard) {
        if (n==root) return true;
        if (n>=scene.hierarchy.size()) return false;
        const int p=scene.hierarchy[n].parent;
        if (p<0) return false;
        n=uint32_t(p);
    }
    return false;
}
bool ReadTrs(const glm::mat4& mat, glm::vec3& t,
    glm::vec3& r, glm::vec3& s) {
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::quat quaternion;
    if (!glm::decompose(mat,s,quaternion,t,skew,perspective))
        return false;
    r=glm::degrees(glm::eulerAngles(quaternion));
    return Finite(t) && Finite(r) && Finite(s);
}
bool SameSource(const std::string& a, const std::string& b) {
    std::error_code ec;
    const fs::path x=fs::weakly_canonical(fs::path(a),ec);
    if (ec) return fs::path(a).lexically_normal()==
        fs::path(b).lexically_normal();
    const fs::path y=fs::weakly_canonical(fs::path(b),ec);
    return !ec && x==y;
}
} // namespace

// Entering full editor mode is deliberately a frame-boundary operation:
// the normal editor owns the entire scene toolset, viewport and history.
// A temporary .tour holds the PARENT's unsaved state without modifying it.
void EditorApp::RequestFullPrefabEdit(const std::string& path) {
    if (path.empty() || !m_fullPrefabParentSnapshot.empty())
        return;
    if (m_prefab.dirty) {
        m_prefab.error = "Save or Revert pane edits before full-scene editing.";
        return;
    }
    m_fullPrefabRequestedPath = path;
}

void EditorApp::RequestReturnFromPrefab(bool save) {
    if (m_fullPrefabParentSnapshot.empty())
        return;
    m_fullPrefabReturnRequested = true;
    m_fullPrefabReturnSave = save;
}

void EditorApp::ProcessFullPrefabNavigation() {
    if (m_fullPrefabReturnRequested) {
        m_fullPrefabReturnRequested = false;
        if (m_fullPrefabParentSnapshot.empty())
            return;
        if (m_fullPrefabReturnSave && !SaveTour()) {
            Log("Prefab: save failed; staying in prefab edit mode.");
            return;
        }
        const std::string snapshot = m_fullPrefabParentSnapshot;
        const std::string target = m_fullPrefabParentTarget;
        const bool dirty = m_fullPrefabParentDirty;
        const std::vector<EditorCam> parentCameras = m_fullPrefabParentCameras;
        const std::vector<uint32_t> parentSelection = m_fullPrefabParentSelection;
        const uint32_t parentSelected = m_fullPrefabParentSelected;
        if (!OpenTourFile(snapshot)) {
            Log("Prefab: could not restore parent snapshot: " + snapshot);
            return;
        }
        m_tourPath = target;
        m_tourDirty = dirty;
        if (!parentCameras.empty())
            m_editorCams = parentCameras;
        if (m_world && parentSelected < m_world->scene.hierarchy.size()) {
            m_selected = parentSelected;
            m_selection.clear();
            for (uint32_t n : parentSelection)
                if (n < m_world->scene.hierarchy.size())
                    m_selection.push_back(n);
        }
        std::error_code ec;
        fs::remove(snapshot, ec);
        m_fullPrefabParentSnapshot.clear();
        m_fullPrefabParentTarget.clear();
        m_fullPrefabParentCameras.clear();
        m_fullPrefabParentSelection.clear();
        m_fullPrefabParentSelected = UINT32_MAX;
        Log("Prefab: returned to parent scene (working edits restored).");
        return;
    }
    if (m_fullPrefabRequestedPath.empty())
        return;
    const std::string prefabPath = std::move(m_fullPrefabRequestedPath);
    m_fullPrefabRequestedPath.clear();
    if (!m_world || TourTarget().empty() || m_cookRunning) {
        Log("Prefab: open a scene and finish cooking before editing a prefab.");
        return;
    }
    if (!InProjectDir(prefabPath)) {
        Log("Prefab: source must be inside the current project.");
        return;
    }
    if (SameSource(prefabPath, TourTarget())) {
        Log("Prefab: this scene is already open in the main editor.");
        return;
    }
    if (m_prefab.dirty) {
        Log("Prefab: save/revert pending pane changes first.");
        return;
    }
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec) {
        Log("Prefab: cannot obtain temporary directory.");
        return;
    }
    tmp /= "tourmaline_parent_" + smvk::tour::GenerateUuid() + ".tour";
    // Save a copy of the live document, preserving original path and
    // dirty flag. Never call CloseScene (which writes the parent file).
    const std::string previousTarget = TourTarget();
    const bool previousDirty = m_tourDirty;
    const std::vector<EditorCam> previousCameras = m_editorCams;
    const std::vector<uint32_t> previousSelection = m_selection;
    const uint32_t previousSelected = m_selected;
    if (!SaveTourTo(tmp.generic_string(), false, true)) {
        Log("Prefab: parent snapshot failed; nothing switched.");
        return;
    }
    if (m_prefab.world)
        ClosePrefabWorld();
    m_showPrefabWindow = false;
    m_prefabRequestedPath.clear();
    if (!OpenTourFile(prefabPath)) {
        Log("Prefab: failed to open source; recovering parent snapshot.");
        (void)OpenTourFile(tmp.generic_string());
        m_tourPath = previousTarget;
        m_tourDirty = previousDirty;
        fs::remove(tmp, ec);
        return;
    }
    m_fullPrefabParentSnapshot = tmp.generic_string();
    m_fullPrefabParentTarget = previousTarget;
    m_fullPrefabParentDirty = previousDirty;
    m_fullPrefabParentCameras = previousCameras;
    m_fullPrefabParentSelection = previousSelection;
    m_fullPrefabParentSelected = previousSelected;
    Log("Prefab: full editor mode for '" + prefabPath
        + "' (Save & Return or Discard & Return in File).");
}

void EditorApp::RequestOpenPrefab(const std::string& path) {
    if (path.empty()) return;
    m_showPrefabWindow=true;
    if (!m_prefab.path.empty() && SameSource(m_prefab.path,path))
        return;
    if (m_prefab.dirty) {
        m_prefab.error="The current prefab has unsaved changes. "
            "Save or Revert before opening another.";
        return;
    }
    m_prefabRequestedPath=path; // actual load at safe frame boundary
}

bool EditorApp::LoadPrefabWorld(const std::string& path) {
    std::error_code ec;
    if (fs::path(path).extension()!=".tour"
        || !fs::is_regular_file(fs::path(path),ec) || ec) {
        m_prefab.error="Prefab sources must be existing .tour files.";
        return false;
    }
    smvk::tour::Doc doc;
    std::string err;
    if (!smvk::tour::LoadTour(path,doc,err)) {
        m_prefab.error="Cannot parse prefab: "+err;
        return false;
    }
    auto newWorld=tour::mem::MakeProcess<tour::World>();
    tour::Cluster& cluster=newWorld->LoadCluster(*m_context);
    cluster.SetRetainUploads(true);
    addNode(cluster.scene,-1,0);
    std::vector<std::string> issues;
    smvk::MeshOptConfig opt{};
    smvk::MeshletConfig meshlets{};
    meshlets.enabled=true;
    (void)ApplySceneReferences(cluster,doc,
        fs::path(path).parent_path().generic_string(),
        m_context.get(),nullptr,nullptr,opt,meshlets,0,
        nullptr,&issues,nullptr,true,nullptr,
        m_texCacheAdapter.get(),&m_guidResolver,
        nullptr,m_projectRoot);
    // Top-level .tour commands are separate from the recursively inlined
    // references. The same two-phase application is used by editor/cooker.
    std::string applyErr;
    if (!smvk::tour::ApplyTourToCluster(
            cluster,doc,&issues,&applyErr,false)) {
        m_prefab.error="Prefab replay failed: "+applyErr;
        return false;
    }
    m_context->FlushUploads();
    newWorld->ResolveAndFixup_clusters();
    if (newWorld->scene.hierarchy.size()<2) {
        m_prefab.error="Prefab has no drawable or editable scene nodes.";
        return false;
    }
    ClosePrefabWorld();
    m_prefab.path=path;
    m_prefab.document=doc;
    m_prefab.saved=doc;
    m_prefab.world=std::move(newWorld);
    m_prefab.savedLocals=m_prefab.world->scene.localTransform;
    m_prefab.selected=UINT32_MAX;
    m_prefab.parentWitness=nullptr;
    m_prefab.parentOriginal.clear();
    m_prefab.parentApplied.clear();
    m_prefab.undo.clear();
    m_prefab.redo.clear();
    m_prefab.dirty=false;
    m_prefab.dragNode=UINT32_MAX;
    m_prefab.dragField=-1;
    m_prefab.error.clear();
    m_prefab.distance=7.0f;
    m_prefab.focus=glm::vec3(0.0f);
    // Fit the independent prefab camera to the source definition,
    // not to the parent world's overall bounds.
    const ::tour::Aabb bounds=m_prefab.world->ComputeSceneBounds();
    if (bounds.mx.x>=bounds.mn.x) {
        m_prefab.focus=(bounds.mn+bounds.mx)*0.5f;
        m_prefab.distance=std::max(
            glm::length(bounds.mx-bounds.mn)*1.35f,1.5f);
    }
    m_showPrefabWindow=true;
    // The actual renderer is created by the boundary step, not by
    // DrawUI. A viewport never uploads/destroys GPU resources mid-record.
    return true;
}

void EditorApp::RestorePrefabPreview() {
    if (!m_world || m_prefab.parentWitness!=m_world.get()) {
        m_prefab.parentOriginal.clear();
        m_prefab.parentApplied.clear();
        m_prefab.parentWitness=nullptr;
        return;
    }
    Scene& scene=m_world->scene;
    bool changed=false;
    std::vector<uint32_t> nodes;
    nodes.reserve(m_prefab.parentOriginal.size());
    for (const auto& [node, original] : m_prefab.parentOriginal) {
        (void)original;
        nodes.push_back(node);
    }
    for (uint32_t node : nodes) {
        if (node>=scene.localTransform.size()) continue;
        if (prefab_sync::RestoreOne(
            m_prefab.parentOriginal,m_prefab.parentApplied,
            node,scene.localTransform[node])) {
            markAsChanged(scene,int(node));
            changed=true;
        }
    }
    if (changed) recalculateGlobalTransforms(scene);
    m_prefab.parentOriginal.clear();
    m_prefab.parentApplied.clear();
    m_prefab.parentWitness=nullptr;
}
void EditorApp::ClosePrefabWorld() {
    // Unsaved temporary source edits NEVER enter the parent .tour.
    RestorePrefabPreview();
    m_prefabRendererRenderable=false;
    if (!m_prefabComposeLogical.empty()) {
        m_rendergraph->FreeImageView(m_prefabComposeLogical);
        m_prefabComposeLogical={};
        m_prefabComposePhysical={};
    }
    if (m_prefabRenderer) {
        if (m_prefabRendererValid) m_prefabRenderer->onDetach();
        m_prefabRenderer.reset();
    }
    m_prefabRendererValid=false;
    m_prefab.world.reset();
    m_prefab.path.clear();
    m_prefabRectFresh=false;
    m_prefabCanvasSize=ImVec2(0,0);
    m_prefabRequestClose=false;
}

bool EditorApp::ParentHasTransformOverride(uint32_t node) const {
    for (size_t i=0;i<m_historyCursor && i<m_history.size();++i) {
        const EditCmd& c=m_history[i];
        if (c.node!=node) continue;
        if (c.verb==EditVerb::Translate || c.verb==EditVerb::Rotate
            || c.verb==EditVerb::Scale || c.verb==EditVerb::Parent)
            return true;
    }
    return false;
}

void EditorApp::SyncPrefabPreview() {
    if (!m_prefab.world || !m_world) return;
    Scene& parent=m_world->scene;
    const Scene& source=m_prefab.world->scene;
    if (m_prefab.parentWitness!=m_world.get()) {
        m_prefab.parentWitness=m_world.get();
        m_prefab.parentOriginal.clear();
        m_prefab.parentApplied.clear();
    }
    bool changed=false;
    // A name is a safe source->instance address only when it is unique.
    // Never guess between duplicate siblings: a future UUID remapper
    // can support that case without accidentally moving the wrong node.
    std::unordered_map<std::string,uint32_t> sourceCounts;
    for (uint32_t n=1;n<source.hierarchy.size();++n)
        if (IsLiveSlot(source,n))
            ++sourceCounts[getNodeName(source,int(n))];
    for (size_t ri=0;ri<m_references.size();++ri) {
        const auto& ref=m_references[ri];
        if (ref.root || !ref.enabled) continue;
        const std::string resolved=ri<m_refResolved.size()
            && !m_refResolved[ri].empty() ? m_refResolved[ri] :
            (fs::path(m_projectRoot)/ref.source).string();
        if (!SameSource(resolved,m_prefab.path)) continue;
        const uint32_t placeholder=RefPlaceholderNode(ri);
        if (placeholder==UINT32_MAX) continue;
        std::unordered_map<std::string,uint32_t> parentCounts;
        for (uint32_t p=0;p<parent.hierarchy.size();++p)
            if (IsLiveSlot(parent,p) &&
                DescendantOf(parent,p,placeholder))
                ++parentCounts[getNodeName(parent,int(p))];
        for (uint32_t n=1;n<source.hierarchy.size();++n) {
            if (!IsLiveSlot(source,n) ||
                n>=source.localTransform.size() ||
                n>=m_prefab.savedLocals.size()) continue;
            const auto& wanted=source.localTransform[n];
            const auto& saved=m_prefab.savedLocals[n];
            const std::string sourceName=getNodeName(source,int(n));
            if (sourceCounts[sourceName]!=1) continue;
            const std::string expected=ref.name+":"+sourceName;
            if (parentCounts[expected]!=1) continue;
            for (uint32_t p=0;p<parent.hierarchy.size();++p) {
                if (!IsLiveSlot(parent,p) || p>=parent.localTransform.size()
                    || !DescendantOf(parent,p,placeholder)
                    || getNodeName(parent,int(p))!=expected)
                    continue;
                if (prefab_sync::Apply(
                    m_prefab.parentOriginal,m_prefab.parentApplied,
                    p,saved,wanted,parent.localTransform[p],
                    ParentHasTransformOverride(p))) {
                    markAsChanged(parent,int(p));
                    changed=true;
                }
            }
        }
    }
    if (changed) recalculateGlobalTransforms(parent);
}

void EditorApp::CapturePrefabUndo() {
    if (!m_prefab.world) return;
    if (m_prefab.undo.size()>=96)
        m_prefab.undo.erase(m_prefab.undo.begin());
    m_prefab.undo.push_back({m_prefab.document,
        m_prefab.world->scene.localTransform});
    m_prefab.redo.clear();
}
void EditorApp::PrefabUndo() {
    if (!m_prefab.world || m_prefab.undo.empty()) return;
    m_prefab.redo.push_back({m_prefab.document,
        m_prefab.world->scene.localTransform});
    auto previous=std::move(m_prefab.undo.back());
    m_prefab.undo.pop_back();
    m_prefab.document=std::move(previous.doc);
    m_prefab.world->scene.localTransform=std::move(previous.locals);
    MarkAllRootsChanged(m_prefab.world->scene);
    recalculateGlobalTransforms(m_prefab.world->scene);
    m_prefab.dirty=true;
    SyncPrefabPreview();
}
void EditorApp::PrefabRedo() {
    if (!m_prefab.world || m_prefab.redo.empty()) return;
    m_prefab.undo.push_back({m_prefab.document,
        m_prefab.world->scene.localTransform});
    auto next=std::move(m_prefab.redo.back());
    m_prefab.redo.pop_back();
    m_prefab.document=std::move(next.doc);
    m_prefab.world->scene.localTransform=std::move(next.locals);
    MarkAllRootsChanged(m_prefab.world->scene);
    recalculateGlobalTransforms(m_prefab.world->scene);
    m_prefab.dirty=true;
    SyncPrefabPreview();
}
void EditorApp::RevertPrefab() {
    if (!m_prefab.world) return;
    m_prefab.document=m_prefab.saved;
    m_prefab.world->scene.localTransform=m_prefab.savedLocals;
    MarkAllRootsChanged(m_prefab.world->scene);
    recalculateGlobalTransforms(m_prefab.world->scene);
    m_prefab.undo.clear();
    m_prefab.redo.clear();
    m_prefab.dragNode=UINT32_MAX;
    m_prefab.dragField=-1;
    m_prefab.dirty=false;
    SyncPrefabPreview();
}

bool EditorApp::SavePrefab() {
    if (!m_prefab.world || m_prefab.path.empty()) return false;
    std::string err;
    // Never truncate an authored .tour on an interrupted save.
    // The same-directory staging file makes replacement atomic.
    const fs::path destination(m_prefab.path);
    fs::path staging=destination;
    staging+=std::string(".prefab-save-")+
        std::to_string(std::chrono::steady_clock::now()
            .time_since_epoch().count());
    if (!smvk::tour::SaveTour(staging.string(),m_prefab.document,err)) {
        std::error_code cleanup;
        fs::remove(staging,cleanup);
        m_prefab.error="Saving prefab: "+err;
        return false;
    }
    std::error_code replaceError;
#ifdef _WIN32
    if (!::MoveFileExW(staging.c_str(),destination.c_str(),
            MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        replaceError=std::make_error_code(std::errc::io_error);
#else
    fs::rename(staging,destination,replaceError);
#endif
    if (replaceError) {
        std::error_code cleanup;
        fs::remove(staging,cleanup);
        m_prefab.error="Replacing prefab source failed: "+
            replaceError.message();
        return false;
    }
    m_prefab.saved=m_prefab.document;
    m_prefab.savedLocals=m_prefab.world->scene.localTransform;
    // A successful source save means that preview poses are now the
    // durable source state. Future Revert must restore THIS version.
    prefab_sync::Commit(
        m_prefab.parentOriginal,m_prefab.parentApplied);
    m_prefab.dirty=false;
    m_prefab.error.clear();
    return true;
}
bool EditorApp::PrefabSetTRS(uint32_t node,
    const glm::vec3& translation, const glm::vec3& rotation,
    const glm::vec3& scale) {
    if (!m_prefab.world || !Finite(translation) || !Finite(rotation)
        || !Finite(scale)) return false;
    Scene& source=m_prefab.world->scene;
    if (node==0 || node>=source.localTransform.size()
        || !IsLiveSlot(source,node)) return false;
    source.localTransform[node]=LocalMatrixFromTRS(
        translation,rotation,scale);
    markAsChanged(source,int(node));
    recalculateGlobalTransforms(source);
    m_prefab.dirty=true;
    SyncPrefabPreview();
    return true;
}
void EditorApp::UpdatePrefabBoundary(float dt) {
    ProcessFullPrefabNavigation();
    m_prefabRendererRenderable=false;
    if (m_prefabRequestClose) {
        if (m_prefab.dirty) {
            m_prefab.error="Save or Revert unsaved prefab changes before closing.";
            m_showPrefabWindow=true;
            m_prefabRequestClose=false;
        } else {
            ClosePrefabWorld();
            return;
        }
    }
    if (!m_prefabRequestedPath.empty()) {
        const std::string path=std::move(m_prefabRequestedPath);
        m_prefabRequestedPath.clear();
        if (!m_prefab.dirty) (void)LoadPrefabWorld(path);
    }
    if (!m_prefab.world || !m_showPrefabWindow) return;
    if (!m_prefabRenderer) {
        auto renderer=tour::mem::MakeProcess<engine::WorldRenderer>(*this);
        engine::WorldRendererConfig cfg=engine::WorldRendererConfig::All();
        cfg.ui=false;
        renderer->Configure(cfg);
        renderer->SetBorrowedWorld(m_prefab.world.get());
        renderer->SetInlineOutput(true);
        if (!renderer->onAttach()) {
            m_prefab.error="Prefab viewport renderer attach failed.";
            return;
        }
        m_prefabRenderer=std::move(renderer);
        m_prefabRendererValid=true;
        // The RHI exposes one active TLAS per ResourceManager. The main
        // WYSIWYG and prefab views share that manager, so enabling ray queries
        // in both would let the prefab overwrite the main world's TLAS.
        // Keep the prefab on the existing contact/CSM fallback until TLAS
        // ownership becomes per-world/per-view.
        auto prefabQuality = m_renderQuality;
        prefabQuality.rayTracedSunShadowsEnabled = false;
        prefabQuality.opacityMicromapsEnabled = false;
        m_prefabRenderer->PushRenderQuality(prefabQuality);
    }
    SyncPrefabPreview(); // recovers after the parent world was rebound
    const glm::vec3 offset(
        std::cos(m_prefab.pitch)*std::sin(m_prefab.yaw),
        std::sin(m_prefab.pitch),
        std::cos(m_prefab.pitch)*std::cos(m_prefab.yaw));
    const glm::vec3 eye=m_prefab.focus+
        offset*std::max(m_prefab.distance,0.15f);
    m_prefabRenderer->CameraPositioner().lookAt(
        eye,m_prefab.focus,glm::vec3(0,1,0));
    m_prefabRenderer->SetSceneFovY(45.0f);
    m_prefabRenderer->SetSelectedNode(m_prefab.selected);
    m_prefabRenderer->Tick(dt);
    const float width=m_prefabRectMax.x-m_prefabRectMin.x;
    const float height=m_prefabRectMax.y-m_prefabRectMin.y;
    if (width<48 || height<48) return;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)
        && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        if (m_prefabCanvasSize.x<8 ||
            std::abs(width-m_prefabCanvasSize.x)>16 ||
            std::abs(height-m_prefabCanvasSize.y)>16)
            m_prefabCanvasSize=ImVec2(width,height);
    }
    const auto before=m_prefabRenderer->InlineTargetExtent();
    const auto swap=m_swapchain->GetExtent();
    const smvk::Extent2D target{
        std::min(std::max(uint32_t(m_prefabCanvasSize.x),64u),
            std::max(64u,swap.width)),
        std::min(std::max(uint32_t(m_prefabCanvasSize.y),64u),
            std::max(64u,swap.height))};
    if (!m_prefabRenderer->EnsureInlineExtent(target)) return;
    const auto after=m_prefabRenderer->InlineTargetExtent();
    if (before.width!=after.width || before.height!=after.height)
        m_wysiwygBoundaryQuiesce=true; // after-present safety gate
    m_prefabRendererRenderable=true;
}

void EditorApp::RenderPrefabLayer(smvk::Framebuffer& fb,
    smvk::Extent2D extent) {
    if (!m_prefabRendererRenderable || !m_prefabRectFresh
        || !m_prefabRendererValid || !m_prefabRenderer)
        return;
    engine::ElementFrame frame{};
    frame.fb=&fb;
    frame.swapExtent=extent;
    frame.layer=engine::LayerRect::Full();
    m_prefabRenderer->RenderLayerInto(frame);
}

void EditorApp::RecordPrefabComposePass(
    smvk::LogicalImageViewHandle swapView) {
    if (!m_prefabRenderer || !m_prefabRendererValid
        || m_prefabRenderer->InlineTargetView().empty()
        || !m_prefabRectFresh) return;
    const ImVec2 fb=ImGui::GetIO().DisplaySize;
    const float x0=std::clamp(m_prefabRectMin.x,0.0f,fb.x);
    const float y0=std::clamp(m_prefabRectMin.y,0.0f,fb.y);
    const float x1=std::clamp(m_prefabRectMax.x,0.0f,fb.x);
    const float y1=std::clamp(m_prefabRectMax.y,0.0f,fb.y);
    if (x1-x0<8 || y1-y0<8) return;
    if (!m_prefabComposite.Ensure(*m_context)) return;
    if (m_prefabComposePhysical!=m_prefabRenderer->InlineTargetView()) {
        if (!m_prefabComposeLogical.empty())
            m_rendergraph->FreeImageView(m_prefabComposeLogical);
        m_prefabComposeLogical=m_rendergraph->AddExternalImageView(
            m_prefabRenderer->InlineTargetView(),
            m_prefabRenderer->InlineTargetFormat(),
            "prefabInlineSource");
        m_prefabComposePhysical=m_prefabRenderer->InlineTargetView();
    }
    if (m_prefabComposeLogical.empty()) return;
    m_prefabComposite.AddCompositePass(*m_rendergraph,
        m_prefabComposeLogical,
        m_prefabRenderer->InlineTargetBindlessIndex(),0,
        {swapView},
        smvk::Extent2D{uint32_t(x1-x0),uint32_t(y1-y0)},
        false,"PrefabWorldCompose",uint32_t(x0),uint32_t(y0),
        true);
}

void EditorApp::DrawPrefabWindow() {
    if (!m_showPrefabWindow) { m_prefabRectFresh=false; return; }
    // Default to a real two-world side-by-side authoring layout.
    // Floating mode remains available (e.g. when editing on 2 monitors
    // or when the host is showing a quad world viewport).
    const ImVec2 screen=ImGui::GetIO().DisplaySize;
    const float stageX=m_uiMode?m_outlinerW:0.0f;
    const float stageW=screen.x-stageX-(m_uiMode?m_inspectorW:0.0f);
    const bool dock=m_prefabDocked && m_vpLayout==0 && stageW>=800.0f;
    if (dock) {
        const float offset=stageW*.54f;
        ImGui::SetNextWindowPos(ImVec2(stageX+offset,m_menuH),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(stageW-offset,
            screen.y-m_menuH-(m_uiMode?
                BottomDockH()+kEditorStatusH:0.0f)),
            ImGuiCond_Always);
    } else
        ImGui::SetNextWindowSize(ImVec2(960,575),ImGuiCond_FirstUseEver);
    bool showing=m_showPrefabWindow;
    const std::string title=m_prefab.path.empty()
        ? "Prefab World" :
        "Prefab World - "+fs::path(m_prefab.path).filename().string();
    if (!ImGui::Begin(title.c_str(),&showing,
        dock ? (ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse) : 0)) {
        ImGui::End();
        m_showPrefabWindow=showing;
        m_prefabRectFresh=false;
        if (!showing) {
            if (m_prefab.dirty) {
                m_prefab.error="Save or Revert unsaved prefab changes before closing.";
                m_showPrefabWindow=true;
            } else m_prefabRequestClose=true;
        }
        return;
    }
    m_showPrefabWindow=showing;
    if (!showing) {
        if (m_prefab.dirty) {
            m_showPrefabWindow=true;
            m_prefab.error="Save or Revert unsaved prefab changes before closing.";
        } else m_prefabRequestClose=true;
    }
    if (m_prefab.world) {
        if (ImGui::SmallButton(m_prefabDocked?"Float pane":"Dock pane"))
            m_prefabDocked=!m_prefabDocked;
        ImGui::SameLine();
        ImGui::TextDisabled("%s",fs::path(m_prefab.path).filename().string().c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Editing source: %s",m_prefab.path.c_str());
        if (m_prefab.dirty) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(.96f,.73f,.3f,1),"UNSAVED");
        }
        if (ImGui::Button("Edit in main editor")) {
            RequestFullPrefabEdit(m_prefab.path);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Use the full outliner, inspector, gizmos and scene tools. "
                "The current scene is snapshotted; File > Save prefab & return "
                "restores it without losing pending changes.");
        ImGui::SameLine();
        if (ImGui::Button("Save prefab")) SavePrefab();
        ImGui::SameLine();
        if (ImGui::Button("Undo")) PrefabUndo();
        ImGui::SameLine();
        if (ImGui::Button("Redo")) PrefabRedo();
        ImGui::SameLine();
        if (ImGui::Button("Revert unsaved")) RevertPrefab();
        ImGui::Separator();
        const float side=std::clamp(
            ImGui::GetContentRegionAvail().x*0.42f,175.0f,360.0f);
        const float panelH=std::max(140.0f,ImGui::GetContentRegionAvail().y);
        ImGui::BeginChild("##prefabView",
            ImVec2(-side-6.0f,panelH),true,
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 origin=ImGui::GetCursorScreenPos();
        const ImVec2 size=ImGui::GetContentRegionAvail();
        if (size.x>8 && size.y>8) {
            m_prefabRectMin=origin;
            m_prefabRectMax=ImVec2(origin.x+size.x,origin.y+size.y);
            m_prefabRectFresh=true;
        } else m_prefabRectFresh=false;
        ImGui::InvisibleButton("##prefabOrbit",ImVec2(
            std::max(1.0f,size.x),std::max(1.0f,size.y)));
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
                m_prefab.yaw-=ImGui::GetIO().MouseDelta.x*.008f;
                m_prefab.pitch=std::clamp(m_prefab.pitch+
                    ImGui::GetIO().MouseDelta.y*.008f,-1.4f,1.4f);
            }
            m_prefab.distance=std::clamp(m_prefab.distance*
                std::pow(0.88f,ImGui::GetIO().MouseWheel),.25f,500.0f);
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##prefabInspector",ImVec2(-1,panelH),true);
        ImGui::TextDisabled("Prefab nodes (not parent instances)");
        const Scene& source=m_prefab.world->scene;
        static char filter[128] = {};
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##prefabFilter", "Filter prefab nodes...",
            filter, sizeof(filter));
        if (ImGui::BeginChild("##prefabTree",ImVec2(-1,190),true)) {
            // Match the main Outliner's parent/child structure, expand
            // arrows, selection highlighting and filtered flat results.
            // Nodes keep their own prefab-local selection identity.
            const auto drawNode = [&](auto&& self, uint32_t i) -> void {
                if (i >= source.hierarchy.size() || !IsLiveSlot(source,i))
                    return;
                ImGui::PushID(int(i));
                std::string name = getNodeName(source,int(i));
                if (name.empty()) name = "(node)";
                bool hasChildren = false;
                for (int child = source.hierarchy[i].firstChild;
                    child >= 0; child = source.hierarchy[child].nextSibling) {
                    if (IsLiveSlot(source,uint32_t(child))) {
                        hasChildren = true;
                        break;
                    }
                }
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                    ImGuiTreeNodeFlags_OpenOnDoubleClick;
                if (m_prefab.selected == i)
                    flags |= ImGuiTreeNodeFlags_Selected;
                if (!hasChildren)
                    flags |= ImGuiTreeNodeFlags_Leaf;
                const bool opened = ImGui::TreeNodeEx(
                    (name + "###prefabNode").c_str(),flags);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    m_prefab.selected = i;
                if (opened) {
                    for (int child = source.hierarchy[i].firstChild;
                        child >= 0; child = source.hierarchy[child].nextSibling)
                        self(self,uint32_t(child));
                    ImGui::TreePop();
                }
                ImGui::PopID();
            };
            if (filter[0]) {
                for (uint32_t i=1;i<source.hierarchy.size();++i) {
                    if (!IsLiveSlot(source,i)) continue;
                    std::string name = getNodeName(source,int(i));
                    if (name.find(filter)==std::string::npos) continue;
                    ImGui::PushID(int(i));
                    if (ImGui::Selectable(
                        (name.empty() ? "(node)" : name).c_str(),
                        m_prefab.selected==i))
                        m_prefab.selected=i;
                    ImGui::PopID();
                }
            }
            else {
                for (int child = source.hierarchy[0].firstChild;
                    child >= 0; child = source.hierarchy[child].nextSibling)
                    drawNode(drawNode,uint32_t(child));
            }
        }
        ImGui::EndChild();
        const uint32_t node=m_prefab.selected;
        if (node>0 && node<source.localTransform.size()
            && IsLiveSlot(source,node)) {
            glm::vec3 t,r,sc;
            if (ReadTrs(source.localTransform[node],t,r,sc)) {
                ImGui::Text("Node %u",node);
                auto editField=[&](int field,const char* label,
                    glm::vec3& value,float speed) {
                    float data[3]={value.x,value.y,value.z};
                    const bool changed=ImGui::DragFloat3(label,data,speed);
                    if (changed) {
                        if (m_prefab.dragNode!=node ||
                            m_prefab.dragField!=field) {
                            CapturePrefabUndo();
                            m_prefab.dragNode=node;
                            m_prefab.dragField=field;
                        }
                        const glm::vec3 next(data[0],data[1],data[2]);
                        if (field==0) t=next;
                        if (field==1) r=next;
                        if (field==2) sc=next;
                        PrefabSetTRS(node,t,r,sc);
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit()
                        && m_prefab.dragNode==node
                        && m_prefab.dragField==field) {
                        smvk::tour::Command cmd;
                        cmd.node=node-1; // single-cluster mapping
                        cmd.name=getNodeName(source,int(node));
                        cmd.verb=field==0
                            ? smvk::tour::Verb::SetTranslate
                            : field==1 ? smvk::tour::Verb::SetRotate
                            : smvk::tour::Verb::SetScale;
                        glm::vec3 curT,curR,curS;
                        if (ReadTrs(source.localTransform[node],
                            curT,curR,curS)) {
                            cmd.vec=field==0?curT:field==1?curR:curS;
                            m_prefab.document.commands.push_back(
                                std::move(cmd));
                        }
                        m_prefab.dragField=-1;
                        m_prefab.dragNode=UINT32_MAX;
                    }
                };
                editField(0,"Translate",t,.04f);
                editField(1,"Rotate",r,.6f);
                editField(2,"Scale",sc,.012f);
            }
        } else ImGui::TextDisabled("Select a node to edit.");
        ImGui::Separator();
        ImGui::TextWrapped("RMB drag: orbit | wheel: zoom. "
            "Edits preview live in the parent world. "
            "Parent instance overrides take precedence.");
        ImGui::EndChild();
    } else {
        m_prefabRectFresh=false;
        ImGui::TextDisabled("Choose a .tour scene in the Reference editor,"
            " then click Edit prefab...");
    }
    if (!m_prefab.error.empty())
        ImGui::TextColored(ImVec4(1,.45f,.4f,1),
            "%s",m_prefab.error.c_str());
    ImGui::End();
}
} // namespace editor
