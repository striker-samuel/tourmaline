#pragma once
// Flintbase engine client: resolves //flintbase/ asset paths through
// the Flintbase FastAPI service (/resolve endpoint).
//
// IFlintbaseApi is the seam used by tests (fake implementations);
// FlintbaseClient is the production implementation (WinHTTP + JSON).
// Resolutions take the scheme-free remainder
// ("project/item/task[/step]/vNNN|latest/product[/file]") so fakes do
// not need scheme handling.

#include <cstdint>
#include <string>
#include <vector>
#include <vector>

namespace smvk {

struct FlintbaseFile {
	std::string path;     // product-relative posix path
	uint64_t size = 0;
	std::string sha256;   // hex
	std::string absPath;  // storage host path
};

struct FlintbaseResolved {
	std::string resolvedPath; // canonical, version-pinned path
	int productVersionId = 0;
	int versionNumber = 0;
	std::string status;
	std::string rootFolder;
	std::string contentsHash;
	std::vector<FlintbaseFile> files;
	// Index into files when the path addressed exactly one file, else -1.
	int fileIndex = -1;

	[[nodiscard]] const FlintbaseFile* File() const {
		return fileIndex >= 0 && fileIndex < (int)files.size()
			? &files[(size_t)fileIndex] : nullptr;
	}
};

struct FlintbaseError {
	int status = 0;        // HTTP status (400/404/409/...), 0 = transport failure
	std::string message;
};

class IFlintbaseApi {
public:
	virtual ~IFlintbaseApi() = default;
	[[nodiscard]] virtual bool Resolve(const std::string& remainder,
		FlintbaseResolved& out, FlintbaseError& error) = 0;
};

class FlintbaseClient final : public IFlintbaseApi {
public:
	// baseUrl: scheme + host[:port] (+ optional path prefix); trailing
	// slash tolerated. Default: FLINTBASE_API_URL env or
	// http://127.0.0.1:8000.
	explicit FlintbaseClient(std::string baseUrl = DefaultApiUrl());

	[[nodiscard]] bool Resolve(const std::string& remainder,
		FlintbaseResolved& out, FlintbaseError& error) override;

	[[nodiscard]] static std::string DefaultApiUrl();

private:
	[[nodiscard]] bool Get(const std::string& requestPath, std::string& body,
		FlintbaseError& error);

	std::string baseUrl_;
};

} // namespace smvk
