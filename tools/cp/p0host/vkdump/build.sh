#!/usr/bin/env bash
# tools/cp/p0host/vkdump/build.sh — build the vkdump layer next to this script (on strix).
set -eu
cd "$(dirname "$0")"
python3 gen.py /usr/include/vulkan/vulkan_core.h > features.inc
g++ -O1 -std=c++17 -shared -fPIC -fvisibility=hidden -o libVkLayer_P0HOST_vkdump.so vkdump.cpp
echo "built $(pwd)/libVkLayer_P0HOST_vkdump.so ($(grep -c '^BEGIN' features.inc) feature structs)"
