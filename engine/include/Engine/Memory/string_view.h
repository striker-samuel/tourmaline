#pragma once
// StringViewT: non-owning string reference for the asset/URI layer.
//
// The engine's URI hot paths (registry lookups, LOD chain probing, sibling
// URI construction) previously round-tripped std::string through
// NormalizeAssetUri/Cluster maps on every call. StringViewT keeps those
// paths allocation-free: views slice caller memory or arena blocks, and
// the only owned copies live in per-Cluster arenas (see Memory/arena.h).
//
// Boundary interop: implicit view construction from const char* and
// std::string is allowed (importer/cook code builds URIs as std::string);
// copying back out is explicit via Str(). NEVER store a view constructed
// from a temporary std::string.

#include <cstdint>
#include <cstring>
#include <string>

namespace smvk {

struct StringViewT {
	static constexpr size_t npos = static_cast<size_t>(-1);

	constexpr StringViewT() = default;
	StringViewT(const char* s)
		: ptr_(s), len_(s ? std::strlen(s) : 0) {}
	StringViewT(const char* s, size_t n)
		: ptr_(s), len_(n) {}
	StringViewT(const std::string& s) // non-owning: s must outlive the view
		: ptr_(s.data()), len_(s.size()) {}

	// ---- observers ----
	[[nodiscard]] constexpr const char* data() const { return ptr_; }
	[[nodiscard]] constexpr size_t size() const { return len_; }
	[[nodiscard]] constexpr size_t length() const { return len_; }
	[[nodiscard]] constexpr bool empty() const { return len_ == 0; }
	[[nodiscard]] constexpr bool valid() const { return ptr_ != nullptr; }
	[[nodiscard]] constexpr const char* begin() const { return ptr_; }
	[[nodiscard]] constexpr const char* end() const { return ptr_ + len_; }
	[[nodiscard]] constexpr char operator[](size_t i) const { return ptr_[i]; }
	[[nodiscard]] constexpr char front() const { return ptr_[0]; }
	[[nodiscard]] constexpr char back() const { return ptr_[len_ - 1]; }

	// Explicit owning copy (the only allocation on this type).
	[[nodiscard]] std::string Str() const { return std::string(ptr_, len_); }

	// ---- slicing ----
	[[nodiscard]] constexpr StringViewT substr(size_t off,
		size_t n = npos) const {
		if (off > len_)
			return {};
		const size_t count = (n == npos || off + n > len_) ? len_ - off : n;
		return StringViewT(ptr_ + off, count);
	}
	// Trim leading/trailing whitespace.
	[[nodiscard]] StringViewT trim() const {
		size_t b = 0, e = len_;
		while (b < e && (ptr_[b] == ' ' || ptr_[b] == '\t'
			|| ptr_[b] == '\r' || ptr_[b] == '\n'))
			++b;
		while (e > b && (ptr_[e - 1] == ' ' || ptr_[e - 1] == '\t'
			|| ptr_[e - 1] == '\r' || ptr_[e - 1] == '\n'))
			--e;
		return substr(b, e - b);
	}

	// ---- search ----
	[[nodiscard]] size_t find(char c, size_t from = 0) const {
		for (size_t i = from; i < len_; ++i)
			if (ptr_[i] == c)
				return i;
		return npos;
	}
	[[nodiscard]] size_t rfind(char c) const {
		for (size_t i = len_; i-- > 0;)
			if (ptr_[i] == c)
				return i;
		return npos;
	}
	[[nodiscard]] size_t find(StringViewT needle, size_t from = 0) const {
		if (needle.len_ == 0)
			return from <= len_ ? from : npos;
		if (needle.len_ > len_)
			return npos;
		for (size_t i = from; i + needle.len_ <= len_; ++i)
			if (std::memcmp(ptr_ + i, needle.ptr_, needle.len_) == 0)
				return i;
		return npos;
	}
	[[nodiscard]] constexpr bool contains(char c) const {
		for (size_t i = 0; i < len_; ++i)
			if (ptr_[i] == c)
				return true;
		return false;
	}
	[[nodiscard]] bool contains(StringViewT needle) const {
		return find(needle) != npos;
	}
	[[nodiscard]] constexpr bool starts_with(StringViewT p) const {
		return len_ >= p.len_ && (p.len_ == 0
			|| std::memcmp(ptr_, p.ptr_, p.len_) == 0);
	}
	[[nodiscard]] constexpr bool ends_with(StringViewT p) const {
		return len_ >= p.len_ && (p.len_ == 0
			|| std::memcmp(ptr_ + len_ - p.len_, p.ptr_, p.len_) == 0);
	}

	// ---- comparison ----
	[[nodiscard]] int compare(StringViewT o) const {
		const size_t n = len_ < o.len_ ? len_ : o.len_;
		const int c = n ? std::memcmp(ptr_, o.ptr_, n) : 0;
		if (c != 0)
			return c;
		if (len_ < o.len_)
			return -1;
		return len_ > o.len_ ? 1 : 0;
	}
	[[nodiscard]] constexpr bool operator==(StringViewT o) const {
		return len_ == o.len_ && (len_ == 0
			|| std::memcmp(ptr_, o.ptr_, len_) == 0);
	}
	[[nodiscard]] constexpr bool operator!=(StringViewT o) const {
		return !(*this == o);
	}
	[[nodiscard]] bool operator<(StringViewT o) const { return compare(o) < 0; }
	[[nodiscard]] bool operator<=(StringViewT o) const { return compare(o) <= 0; }
	[[nodiscard]] bool operator>(StringViewT o) const { return compare(o) > 0; }
	[[nodiscard]] bool operator>=(StringViewT o) const { return compare(o) >= 0; }
	[[nodiscard]] bool operator==(const char* s) const {
		return *this == StringViewT(s);
	}
	[[nodiscard]] bool operator!=(const char* s) const {
		return !(*this == s);
	}

	const char* ptr_ = nullptr;
	size_t len_ = 0;
};

// FNV-1a 64 over the view bytes (UriMap / lookup hashing).
struct StringViewHash {
	[[nodiscard]] static uint64_t Of(const char* p, size_t n) {
		uint64_t h = 1469598103934665603ull;
		for (size_t i = 0; i < n; ++i) {
			h ^= static_cast<uint8_t>(p[i]);
			h *= 1099511628211ull;
		}
		return h;
	}
	[[nodiscard]] uint64_t operator()(StringViewT s) const {
		return s.valid() ? Of(s.data(), s.size()) : 0;
	}
};

// Composes "a" + "b" into `arena` (no std::string on the hot path).
// Defined in Memory/arena.cpp (needs the complete Arena type).
[[nodiscard]] StringViewT ConcatInto(class Arena& arena,
	StringViewT a, StringViewT b);

} // namespace smvk
