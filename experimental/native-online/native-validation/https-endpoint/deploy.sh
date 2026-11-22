#!/bin/sh
# Local preparation only. This script does not deploy or change a running server.
set -eu
exec python3 "$(dirname "$0")/prepare_endpoint.py" "$@"
