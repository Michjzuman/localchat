#!/bin/bash
set -u
echo " Testing robust SCRIPT_DIR "
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-}")" && pwd)"
echo "SCRIPT_DIR: $SCRIPT_DIR"
