#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Export only this experiment's llama.cpp delta relative to the original dirty tree.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
original="$here/../../kbuild/llama.cpp"
worktree="$here/llama.cpp"
output="$here/llama-dynamic.patch"
temporary=$(mktemp "$here/.llama-dynamic.patch.XXXXXX")
trap 'rm -f "$temporary"' EXIT INT TERM

for file in \
    ggml/src/ggml-cpu/CMakeLists.txt \
    ggml/src/ggml-cpu/ggml-cpu.c \
    ggml/src/ggml-cpu/ggml-cpu.cpp \
    include/llama.h \
    src/llama-context.cpp \
    src/llama-context.h \
    tools/server/server-context.cpp
do
    diff -u --label "a/$file" --label "b/$file" \
        "$original/$file" "$worktree/$file" >> "$temporary" || [ $? -eq 1 ]
done

for file in \
    ggml/src/ggml-cpu/dynamic-client.c \
    ggml/src/ggml-cpu/dynamic-client.h \
    ggml/src/ggml-cpu/dynamic-control.h \
    ggml/src/ggml-cpu/dynamic-worker-team.c \
    ggml/src/ggml-cpu/dynamic-worker-team.h \
    ggml/src/ggml-cpu/dynamic-grant-client.c \
    ggml/src/ggml-cpu/dynamic-grant-client.h
do
    diff -u --label /dev/null --label "b/$file" \
        /dev/null "$worktree/$file" >> "$temporary" || [ $? -eq 1 ]
done

mv "$temporary" "$output"
trap - EXIT INT TERM
echo "$output"
