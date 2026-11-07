// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#pragma once

// Per-job line tee. The tool sink still streams every line into the editor
// feed; this independently persists and flushes each line so the full record
// survives the task window's bounded tail, failures and editor restarts.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace editor {

class BakeLogFile {
  public:
    explicit BakeLogFile(const std::string& projectRoot, std::string_view jobName) {
        std::error_code ec;
        std::filesystem::path base;
        if (!projectRoot.empty())
            base = std::filesystem::path(projectRoot) / ".tourmaline" / "logs";
        else {
            base = std::filesystem::temp_directory_path(ec);
            if (ec)
                return;
            base /= "tourmaline";
            base /= "logs";
        }
        std::filesystem::create_directories(base, ec);
        if (ec)
            return;
        const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        path_ = (base / (std::string(jobName) + "-" + std::to_string(stamp) + ".log")).string();
        file_.open(path_, std::ios::out | std::ios::trunc);
        if (!file_)
            path_.clear();
    }

    const std::string& Path() const {
        return path_;
    }

    // Called on the owning worker thread, alongside CookFeed::push.
    // stderr is marked without changing the text shown in the editor.
    void Write(const std::string& line, bool isErr = false) {
        if (!file_)
            return;
        if (isErr)
            file_ << "[stderr] ";
        file_ << line << '\n';
        file_.flush(); // deliberately no block buffering
    }

  private:
    std::string path_;
    std::ofstream file_;
};

} // namespace editor
