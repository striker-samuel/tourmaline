// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Authoring source change service. Unlike the runtime ContentRegistry
// (cooked content providers), this owns the editor's native OS watcher,
// project ignore policy, event classification and overflow recovery.
// All actual world/texture mutations remain on EditorApp's update boundary.
#include "editor_watch_routes.h"
#include <Engine/Utils/file_watcher.h>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace editor {

class EditorContentServer {
  public:
    struct Change {
        std::string path;
        smvk::FileWatcher::Event event = smvk::FileWatcher::Event::Modified;
        WatchRoute route = WatchRoute::Other;
    };
    struct Batch {
        std::vector<Change> changes;
        bool recoveredOverflow = false;
        std::string warning;
    };

    [[nodiscard]] bool Start(const std::string& projectRoot, std::string& err) {
        Stop(); // Project switches must never leak old-root events.
        err.clear();
        if (projectRoot.empty()) {
            err = "no project root";
            return false;
        }
        auto watcher = std::make_unique<smvk::FileWatcher>();
        auto policy = smvk::FileWatcher::Policy::ProjectDefault();
        for (const char* dir : {"artifacts", "gi"})
            policy.ignoredDirNames.emplace_back(dir);
        // Editor-only authoring link metadata cannot trigger a source
        // reimport or a self-induced watcher feedback loop.
        policy.ignoredExtensions.emplace_back(".dccsource");
        if (!watcher->Start(policy, err) || !watcher->AddDirectory(projectRoot, err))
            return false;
        // Establish the baseline once; later reconciliation is reserved
        // for lost OS events, never a per-frame filesystem scan.
        (void)watcher->Reconcile(err);
        if (!err.empty())
            return false;
        m_watcher = std::move(watcher);
        return true;
    }

    void Stop() {
        m_watcher.reset();
    }
    [[nodiscard]] bool Running() const {
        return m_watcher && m_watcher->Running();
    }

    // Last event for a path wins if OS events and reconciliation overlap.
    // A removal must not probe for a now-vanished sidecar.
    [[nodiscard]] static std::vector<Change> RouteChanges(
        const std::vector<smvk::FileWatcher::Change>& raw) {
        std::map<std::string, smvk::FileWatcher::Change> latest;
        for (const auto& change : raw)
            latest[change.path] = change;
        std::vector<Change> result;
        result.reserve(latest.size());
        for (const auto& [path, change] : latest) {
            result.push_back({path,
                              change.event,
                              change.event == smvk::FileWatcher::Event::Removed
                                  ? WatchRoute::Removed
                                  : ClassifyWatchedChange(std::filesystem::path(path))});
        }
        return result;
    }

    // Main thread only, before frame recording. The native watcher
    // handles settle/debounce; only overflow incurs a stat reconcile.
    [[nodiscard]] Batch Poll(int settleMs = 150) {
        Batch batch;
        if (!Running())
            return batch;
        auto changes = m_watcher->Poll(settleMs);
        if (m_watcher->Overflowed()) {
            std::string err;
            auto recovered = m_watcher->Reconcile(err);
            if (!err.empty())
                batch.warning = "content watcher reconcile failed: " + err;
            else {
                batch.recoveredOverflow = true;
                changes.insert(changes.end(), recovered.begin(), recovered.end());
            }
        }
        batch.changes = RouteChanges(changes);
        return batch;
    }

  private:
    std::unique_ptr<smvk::FileWatcher> m_watcher;
};

} // namespace editor
