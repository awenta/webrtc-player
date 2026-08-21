#!/usr/bin/env bash
set -Eeuo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
temporary=$(mktemp -d)
trap 'rm -rf "${temporary}"' EXIT
cd "${root}"

bash -n entrypoint.sh scripts/*.sh tests/*.sh
cc -O2 -pipe -Wall -Wextra -Werror -std=c11 \
    tests/config-api-port.test.c -o "${temporary}/config-api-port.test"
cc -O2 -pipe -Wall -Wextra -Werror -std=c11 -pthread \
    native/input-selector.c -o "${temporary}/input-selector"
"${temporary}/config-api-port.test"
bash tests/settings-port.test.sh
bash tests/config-applier-readiness.test.sh
node --test tests/ui-port-config.test.mjs
