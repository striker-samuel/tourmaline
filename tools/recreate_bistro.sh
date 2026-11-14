#!/usr/bin/env bash
# Recreate the Bistro demo from its ORCA source.
#
# The Bistro demo's model + textures are ~800 MB and come from NVIDIA's
# Open Research Content Archive (CC BY 4.0); they are NOT in the git
# tree. This script verifies the source is present and drives the recipe
# cook, or prints the recreate steps when the source is missing.
#
#   tools/recreate_bistro.sh [project-dir] [project-exe]
#
# See releases/bistro/README.md and BISTRO-CREDITS.md.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

PROJECT="${1:-}"
PROJECT_EXE="${2:-}"
if [ -z "$PROJECT" ]; then PROJECT="$repo_root/releases/bistro"; fi
if [ -z "$PROJECT_EXE" ]; then PROJECT_EXE="$repo_root/out/build/linux-Release/Project"; fi

RECIPE="$PROJECT/bistro_static.tourrecipe"
GLTF="$PROJECT/content/models/bistro_static/bistro_static.gltf"
MODEL_DIR="$PROJECT/content/models/bistro_static"

texture_count=0
if [ -d "$MODEL_DIR" ]; then
  texture_count="$(find "$MODEL_DIR" -name '*.ktx2' | wc -l | tr -d ' ')"
fi

missing=0
if [ ! -f "$RECIPE" ]; then
  echo "  recipe MISSING:  $RECIPE"
  missing=1
fi
if [ ! -f "$GLTF" ]; then
  echo "  glTF MISSING:    $GLTF"
  missing=1
fi
if [ "$texture_count" -eq 0 ]; then
  echo "  textures MISSING: 0 .ktx2 files under $MODEL_DIR (needs ~343)"
  missing=1
fi

if [ "$missing" -ne 0 ]; then
  echo ""
  echo "Bistro source is not fully present under $PROJECT."
  echo "Obtain the ORCA Amazon Lumberyard Bistro:"
  echo "  https://developer.nvidia.com/orca/amazon-lumberyard-bistro"
  echo "convert the FBX to glTF and its textures to KTX2, then place"
  echo "  - bistro.bin          (the glTF geometry buffer) and"
  echo "  - the .ktx2 textures  (under objects/ and textures/)"
  echo "beside the committed glTF under content/models/bistro_static/."
  echo "See releases/bistro/README.md for the full flow."
  exit 1
fi

if [ ! -x "$PROJECT_EXE" ]; then
  echo "Project not found at $PROJECT_EXE - build the Project target first."
  exit 1
fi

echo "Cooking the Bistro demo (this is the multi-GB bake; be patient):"
"$PROJECT_EXE" build \
  --project "$PROJECT" \
  --recipe "$RECIPE" \
  --publish "$PROJECT/build" \
  --manifest-name bistro_static
