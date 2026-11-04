#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")"
exec python3 tools/setup_wizard.py --terminal "$@"
