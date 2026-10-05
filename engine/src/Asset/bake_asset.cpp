// Bake-asset leftovers (see Engine/Asset/bake_asset.h): the 32-hex test
// that recognizes legacy guid columns in volume-manifest rows. The
// sidecar identity machinery is gone — bakes ride stable VFS paths.

#include <Engine/Asset/bake_asset.h>

namespace smvk::bake {

namespace {

bool IsHexLower(char c) {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

} // namespace

bool IsBakeAssetGuid(std::string_view s) {
	if (s.size() != 32)
		return false;
	for (char c : s)
		if (!IsHexLower(c))
			return false;
	return true;
}

} // namespace smvk::bake
