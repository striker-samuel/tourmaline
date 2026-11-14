#!/usr/bin/env bash
# Fetch the Tourmaline demo content - the rolling 'demo-content' GitHub
# release carrying the cooked sponza_10 and bistro_static projects (each
# .tapp runtime manifest + its content-addressed .pak) - into a build's
# content/paks directory. Linux twin of fetch_demo_content.ps1 (same
# contract, same explicit file set).
#
#   1. queries the release metadata by tag (public API, no auth),
#   2. downloads the demo assets + the SHA256SUMS.txt manifest,
#   3. verifies every downloaded file against the published SHA-256,
#   4. installs them into $1 (default: the conventional Linux build dir
#      under the repo root, resolved from this script's location),
#   5. removes STALE sponza_10-*.pak / bistro_static-*.pak files left by
#      earlier fetches (recooks change the content-addressed name).
#
# Invoked by the CMake `demo_content` target (which passes the build's
# content/paks as $1), or directly:
#   tools/fetch_demo_content.sh [dest-dir]
#
# The demo file set is deliberately explicit (sponza_10.tapp +
# sponza_10-*.pak + bistro_static.tapp + bistro_static-*.pak): the
# destination also hosts engine-shaders.pak and the user's own cooked
# paks, so the install/cleanup NEVER touches files it does not own. If
# the demo projects ever change identity, update the patterns here and
# in fetch_demo_content.ps1 alongside the release.

set -euo pipefail

REPO="${REPO:-striker-samuel/tourmaline}"
TAG="${TAG:-demo-content}"
DEST="${1:-}"

# Default destination: <repo-root>/out/build/linux-Release/content/paks
# (resolved from the script's own location so CWD does not matter).
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -z "$DEST" ]; then
  DEST="$script_dir/../out/build/linux-Release/content/paks"
fi

fail() { echo "error: $1" >&2; exit 1; }

api="https://api.github.com/repos/$REPO/releases/tags/$TAG"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if ! curl -fsSL --max-time 30 -H 'User-Agent: tourmaline-demo-fetch' "$api" -o "$tmp/release.json"; then
  fail "could not query the '$TAG' release ($api). Check the network, or browse https://github.com/$REPO/releases/tag/$TAG and download the assets manually into '$DEST'."
fi

# The release JSON's browser_download_url rows (one per asset). Parsed
# with grep/sed on purpose: no jq dependency, and the shape is stable.
urls="$(grep -o '"browser_download_url": *"[^"]*"' "$tmp/release.json" \
  | sed -E 's/.*: *"([^"]*)"/\1/')" || true
[ -n "$urls" ] || fail "the '$TAG' release carries no downloadable assets."

# Download the demo assets (manifests + content-addressed paks) and the
# checksum manifest; anything else on the release is ignored.
for url in $urls; do
  name="$(basename "$url")"
  case "$name" in
    sponza_10.tapp|sponza_10-*.pak|bistro_static.tapp|bistro_static-*.pak|SHA256SUMS.txt)
      echo "  downloading $name"
      curl -fsSL --retry 3 -o "$tmp/$name" "$url" \
        || fail "download failed: $name"
      ;;
  esac
done

[ -f "$tmp/SHA256SUMS.txt" ] || fail "the '$TAG' release carries no SHA256SUMS.txt manifest - cannot verify downloads."
# Require at least one complete demo set (manifest + its pak); the rolling
# release carries both sponza_10 and bistro_static.
have_demo=0
for demo in sponza_10 bistro_static; do
  if [ -f "$tmp/$demo.tapp" ] && ls "$tmp"/"$demo"-*.pak >/dev/null 2>&1; then
    have_demo=1
  fi
done
[ "$have_demo" -eq 1 ] || fail "the '$TAG' release carries no demo assets (sponza_10.tapp / bistro_static.tapp + pak)."

# Verify: sha256sum -c covers every checksum line, so a half-updated
# release (a checksum without its file, or vice versa) fails loudly
# here and never installs. CR characters are stripped first: a manifest
# regenerated on Windows could carry CRLF, which sha256sum would treat
# as part of the filename.
( cd "$tmp" && tr -d '\r' < SHA256SUMS.txt > SHA256SUMS.lf \
    && sha256sum -c SHA256SUMS.lf --quiet ) \
  || fail "SHA-256 verification failed - the downloaded assets do not match SHA256SUMS.txt."
echo "  checksums verified"

# Install (overwrite in place): every downloaded manifest + pak.
mkdir -p "$DEST"
for f in "$tmp"/*.tapp "$tmp"/*.pak; do
  [ -e "$f" ] || continue
  cp "$f" "$DEST"/
done

# The GitHub release has data + checksums; stage the checked-in source
# attribution notices next to the fetched content as well.
for credits in SPONZA-CREDITS.md BISTRO-CREDITS.md; do
  src="$script_dir/../$credits"
  [ -f "$src" ] || fail "$credits is missing from this checkout; cannot stage demo attribution."
  cp "$src" "$DEST/$credits"
done

# Drop stale demo paks from earlier fetches whose content-addressed
# names are no longer current (the .tapp only ever names the newest).
for old in "$DEST"/sponza_10-*.pak "$DEST"/bistro_static-*.pak; do
  [ -e "$old" ] || continue
  base="$(basename "$old")"
  if [ ! -f "$tmp/$base" ]; then
    echo "  removing stale demo pak: $base"
    rm -f "$old"
  fi
done

echo ""
echo "demo content installed into $DEST (credits: SPONZA-CREDITS.md, BISTRO-CREDITS.md):"
for f in "$tmp"/*.tapp "$tmp"/*.pak; do
  [ -e "$f" ] || continue
  du -h "$f" | cut -f1 | xargs -I{} echo "  $(basename "$f")  ({})"
done
echo ""
echo "run them from the build directory:"
for tapp in "$tmp"/*.tapp; do
  [ -e "$tapp" ] || continue
  echo "  App --project content/paks/$(basename "$tapp")"
done
