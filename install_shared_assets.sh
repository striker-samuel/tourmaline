#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")"
python3 tools/install_shared_assets.py "$@"
