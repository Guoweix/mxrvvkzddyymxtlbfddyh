#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$SCRIPT_DIR/../run.sh" \
    --scenario symmetric \
    --arrival-gap-sec 0 \
    "$@"
