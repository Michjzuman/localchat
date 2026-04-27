#!/bin/bash
set -eu
echo "Testing BASH_SOURCE"
if [ -n "${BASH_SOURCE[0]:-}" ]; then
    echo "BASH_SOURCE is set: ${BASH_SOURCE[0]}"
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    echo "SCRIPT_DIR: $SCRIPT_DIR"
else
    echo "BASH_SOURCE is not set"
fi
