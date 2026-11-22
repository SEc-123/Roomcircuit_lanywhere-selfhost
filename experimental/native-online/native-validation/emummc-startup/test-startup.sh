#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
set -eu
cd "$(dirname "$0")"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Itest -I. startup.c test/startup_test.c -o "$test_dir/startup-test"
for scenario in sm-error spl-init-error spl-config-error sysmmc other-emu-type \
    unsupported-cached-hos unsupported-reported-hos version-read-error \
    accepted-reported-hos accepted-cached-hos sd-mount-error; do
    "$test_dir/startup-test" "$scenario"
done
