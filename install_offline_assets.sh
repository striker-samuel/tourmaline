#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")"
python3 tools/install_offline_assets.py "$@"
