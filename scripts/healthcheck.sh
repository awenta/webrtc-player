#!/bin/bash
set -Eeuo pipefail

# shellcheck source=scripts/load-settings.sh
source /usr/local/bin/load-settings.sh

for service in config-api config-applier input-selector janus nginx stats; do
    /package/admin/s6/command/s6-svstat -u "/run/service/${service}" >/dev/null
done

if [[ "${INPUT_MODE:-auto}" != "rtp" ]]; then
    /package/admin/s6/command/s6-svstat -u /run/service/srt-relay >/dev/null
fi

exec 3<>/dev/tcp/127.0.0.1/8088
printf 'GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' >&3
IFS= read -r status <&3
exec 3>&-

[[ "${status}" == *" 200 "* ]]
