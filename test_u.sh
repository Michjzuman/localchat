#!/bin/bash
set -u
echo "Testing..."
# Use a variable that is definitely not set
echo "Value: ${UNDEFINED_VAR:-default}"
