#include <Engine/Flintbase/flintbase_client.h>

#include <windows.h>
#include <winhttp.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <string_view>

namespace smvk {
namespace {

std::wstring ToWide(std::string_view s) {
	if (s.empty())
		return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

struct ParsedUrl {
	std::wstring host;
	INTERNET_PORT port = INTERNET_DEFAULT_PORT;
	std::wstring pathPrefix; // starts with '/' or empty
	bool tls = false;
};

bool ParseBaseUrl(const std::string& url, ParsedUrl& out) {
	size_t hostStart = 0;
	if (url.rfind("https://", 0) == 0) {
		out.tls = true;
		hostStart = 8;
	}
	else if (url.rfind("http://", 0) == 0) {
		hostStart = 7;
	}
	else {
		return false;
	}
	const size_t slash = url.find('/', hostStart);
	std::string_view hostPort(url.data() + hostStart,
		slash == std::string::npos ? url.size() - hostStart : slash - hostStart);
	if (slash != std::string::npos)
		out.pathPrefix = ToWide(std::string_view(url).substr(slash));

	out.port = out.tls ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
	const size_t colon = hostPort.rfind(':');
	if (colon != std::string_view::npos) {
		out.host = ToWide(hostPort.substr(0, colon));
		out.port = (INTERNET_PORT)std::atoi(std::string(hostPort.substr(colon + 1)).c_str());
	}
	else {
		out.host = ToWide(hostPort);
	}
	return !out.host.empty();
}

std::string UrlEncode(std::string_view v) {
	static const char* kHex = "0123456789ABCDEF";
	std::string out;
	out.reserve(v.size() + 16);
	for (const unsigned char c : v) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
			|| c == '-' || c == '_' || c == '.' || c == '~') {
			out.push_back((char)c);
		}
		else {
			out.push_back('%');
			out.push_back(kHex[c >> 4]);
			out.push_back(kHex[c & 0x0F]);
		}
	}
	return out;
}

} // namespace

std::string FlintbaseClient::DefaultApiUrl() {
	const char* env = std::getenv("FLINTBASE_API_URL");
	return (env && *env) ? std::string(env) : "http://127.0.0.1:8000";
}

FlintbaseClient::FlintbaseClient(std::string baseUrl)
	: baseUrl_(std::move(baseUrl)) {
	while (!baseUrl_.empty() && baseUrl_.back() == '/')
		baseUrl_.pop_back();
}

bool FlintbaseClient::Get(const std::string& requestPath, std::string& body,
	FlintbaseError& error) {
	ParsedUrl url;
	if (!ParseBaseUrl(baseUrl_, url)) {
		error = {0, "Flintbase: bad API url '" + baseUrl_ + "'"};
		return false;
	}

	HINTERNET session = WinHttpOpen(L"smvk-flintbase/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0);
	if (!session) {
		error = {0, "Flintbase: WinHttpOpen failed"};
		return false;
	}
	WinHttpSetTimeouts(session, 2000, 2000, 5000, 60000);

	struct HandleGuard {
		~HandleGuard() {
			for (size_t i = handles.size(); i-- > 0;)
				WinHttpCloseHandle(handles[i]);
		}
		std::vector<HINTERNET> handles;
	} guard;

	HINTERNET connect = WinHttpConnect(session, url.host.c_str(), url.port, 0);
	if (!connect) {
		WinHttpCloseHandle(session);
		error = {0, "Flintbase: WinHttpConnect failed (host '" + baseUrl_ + "')"};
		return false;
	}
	guard.handles = {session, connect};

	const std::wstring fullRequest = url.pathPrefix + ToWide(requestPath);
	HINTERNET request = WinHttpOpenRequest(connect, L"GET", fullRequest.c_str(),
		nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
		url.tls ? WINHTTP_FLAG_SECURE : 0);
	if (!request) {
		error = {0, "Flintbase: WinHttpOpenRequest failed"};
		return false;
	}
	guard.handles.push_back(request);

	if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
		WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
		|| !WinHttpReceiveResponse(request, nullptr)) {
		error = {0, "Flintbase: request failed (API unreachable at '" + baseUrl_ + "')"};
		return false;
	}

	DWORD status = 0;
	DWORD size = sizeof(status);
	WinHttpQueryHeaders(request,
		WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr);

	body.clear();
	for (;;) {
		DWORD avail = 0;
		if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0)
			break;
		std::string chunk((size_t)avail, '\0');
		DWORD read = 0;
		if (!WinHttpReadData(request, chunk.data(), avail, &read))
			break;
		body.append(chunk.data(), (size_t)read);
	}

	if (status != 200) {
		// FastAPI errors are {"detail": "..."}.
		std::string detail = body;
		const auto j = nlohmann::json::parse(body, nullptr, false);
		if (!j.is_discarded() && j.contains("detail"))
			detail = j["detail"].get<std::string>();
		error = {(int)status, detail};
		return false;
	}
	return true;
}

bool FlintbaseClient::Resolve(const std::string& remainder,
	FlintbaseResolved& out, FlintbaseError& error) {
	if (remainder.empty()) {
		error = {400, "Flintbase: empty path remainder"};
		return false;
	}
	std::string body;
	const std::string request = "/resolve?path=" + UrlEncode("//flintbase/" + remainder);
	if (!Get(request, body, error))
		return false;

	const auto j = nlohmann::json::parse(body, nullptr, false);
	if (j.is_discarded()) {
		error = {0, "Flintbase: invalid JSON from /resolve"};
		return false;
	}

	out = FlintbaseResolved{};
	out.resolvedPath = j.value("resolved_path", std::string());
	out.productVersionId = j.value("product_version_id", 0);
	out.versionNumber = j.value("version_number", 0);
	out.status = j.value("status", std::string());
	out.rootFolder = j.value("root_folder", std::string());
	out.contentsHash = j.value("contents_hash", std::string());

	out.files.clear();
	if (j.contains("files") && j["files"].is_array()) {
		for (const auto& f : j["files"]) {
			FlintbaseFile file;
			file.path = f.value("path", std::string());
			file.size = f.value("size", (uint64_t)0);
			file.sha256 = f.value("sha256", std::string());
			file.absPath = f.value("abs_path", std::string());
			out.files.push_back(std::move(file));
		}
	}

	out.fileIndex = -1;
	if (j.contains("file") && j["file"].is_object()) {
		const auto& f = j["file"];
		FlintbaseFile file;
		file.path = f.value("path", std::string());
		file.size = f.value("size", (uint64_t)0);
		file.sha256 = f.value("sha256", std::string());
		file.absPath = f.value("abs_path", std::string());
		out.files.push_back(std::move(file));
		out.fileIndex = (int)out.files.size() - 1;
	}
	return true;
}

std::string FlintbaseProductInfo::Path() const {
	std::string p = "//flintbase/" + project + "/" + itemName + "/" + task;
	if (!step.empty())
		p += "/" + step;
	p += "/" + name;
	return p;
}

bool FlintbaseClient::ListItems(const std::string& project,
	std::vector<FlintbaseItemInfo>& out, FlintbaseError& error) {
	out.clear();
	std::string body;
	const std::string request = "/items?project=" + UrlEncode(project);
	if (!Get(request, body, error))
		return false;
	const auto j = nlohmann::json::parse(body, nullptr, false);
	if (j.is_discarded() || !j.is_array()) {
		error = {0, "Flintbase: invalid JSON from /items"};
		return false;
	}
	for (const auto& it : j) {
		FlintbaseItemInfo info;
		info.id = it.value("id", 0);
		info.name = it.value("name", std::string());
		info.type = it.value("type", std::string());
		info.project = it.value("project", std::string());
		info.folder = it.value("folder", std::string());
		out.push_back(std::move(info));
	}
	return true;
}

bool FlintbaseClient::ListProducts(const std::string& project,
	const std::string& itemName,
	std::vector<FlintbaseProductInfo>& out, FlintbaseError& error) {
	out.clear();
	std::string body;
	const std::string request = "/products?project=" + UrlEncode(project)
		+ "&item_name=" + UrlEncode(itemName);
	if (!Get(request, body, error))
		return false;
	const auto j = nlohmann::json::parse(body, nullptr, false);
	if (j.is_discarded() || !j.is_array()) {
		error = {0, "Flintbase: invalid JSON from /products"};
		return false;
	}
	for (const auto& p : j) {
		FlintbaseProductInfo info;
		info.id = p.value("id", 0);
		info.itemName = p.value("item_name", std::string());
		info.project = p.value("project", std::string());
		info.task = p.value("task", std::string());
		info.step = p.value("step", std::string());
		info.name = p.value("name", std::string());
		info.description = p.value("description", std::string());
		info.latestVersion = p.value("latest_version", 0);
		info.latestStatus = p.value("latest_status", std::string());
		out.push_back(std::move(info));
	}
	return true;
}

} // namespace smvk
