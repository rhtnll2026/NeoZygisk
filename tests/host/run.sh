#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
test_build_dir="$(mktemp -d)"
trap 'rm -rf -- "$test_build_dir"' EXIT

flags=(-std=c++20 -Wall -Wextra -Werror -g -pthread)
if [[ "${SANITIZE:-1}" == 1 ]]; then
    flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

"${CXX:-c++}" "${flags[@]}" \
    -I"$repo_dir/tests/host/include" -I"$repo_dir/loader/src/include" \
    "$repo_dir/tests/host/socket_utils_test.cpp" \
    "${SOCKET_UTILS_SOURCE:-$repo_dir/loader/src/common/socket_utils.cpp}" \
    -o "$test_build_dir/socket_utils_test"

timeout 30s "$test_build_dir/socket_utils_test" "$@"
