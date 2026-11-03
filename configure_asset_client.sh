#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")"
python3 tools/configure_asset_client.py "$@"
