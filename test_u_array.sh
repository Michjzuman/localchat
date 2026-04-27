#!/bin/bash
set -u
echo "Testing array..."
unset MY_ARRAY
echo "Value: ${MY_ARRAY[0]:-default}"
