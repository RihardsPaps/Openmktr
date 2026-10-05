#!/usr/bin/env bash
set -euo pipefail

workspace='/mnt/c/Users/Harry/Documents/ChatGPT/Universal Tool'
campaign=/root/universal-tool-campaign-20260927
for file in "$@"; do
    cp "$workspace/$file" "$campaign/tool-next2/$file"
done
cd "$campaign/tool-next2"
export CPATH="$campaign/host-deps/extracted/usr/lib/llvm-18/include/c++/v1"
export LIBRARY_PATH="$campaign/host-deps/extracted/usr/lib/llvm-18/lib"
ninja -j1 omktr
python3 tests/correctness.py -q
