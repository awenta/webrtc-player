#!/command/with-contenv bash
# shellcheck shell=bash
set -Eeuo pipefail

channel_id=${PWD##*-}
[[ "${channel_id}" =~ ^[1-5]$ ]] || { echo "[selector] invalid channel service: ${PWD}" >&2; exit 1; }

# shellcheck source=scripts/load-settings.sh
source /usr/local/bin/load-settings.sh
load_network_settings || { echo "[selector] effective network settings are unavailable" >&2; exit 1; }
load_channel_settings "${channel_id}"

if [[ "${CHANNEL_ENABLED}" != "true" ]]; then
    exec s6-pause
fi

export CHANNEL_RUNTIME_DIR="/run/webrtc-player/channels/${channel_id}"
export SELECTOR_STATUS_PATH="${CHANNEL_RUNTIME_DIR}/input-selector.status"
printf '%s\n' "$$" >"${CHANNEL_RUNTIME_DIR}/selector.pid"
exec s6-setuidgid player /usr/local/bin/input-selector
