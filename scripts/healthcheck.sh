#!/bin/bash
set -Eeuo pipefail

# shellcheck source=scripts/load-settings.sh
source /usr/local/bin/load-settings.sh

for service in config-api config-applier janus janus-monitor nginx stats; do
    /package/admin/s6/command/s6-svstat -u "/run/service/${service}" >/dev/null
done

for channel_id in 1 2 3 4 5; do
    /package/admin/s6/command/s6-svstat -u "/run/service/input-selector-${channel_id}" >/dev/null
    /package/admin/s6/command/s6-svstat -u "/run/service/srt-relay-${channel_id}" >/dev/null
done

exec 3<>/dev/tcp/127.0.0.1/8088
printf 'GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' >&3
IFS= read -r status <&3
exec 3>&-

[[ "${status}" == *" 200 "* ]]
