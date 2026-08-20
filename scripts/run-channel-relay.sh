#!/command/with-contenv bash
# shellcheck shell=bash
set -Eeuo pipefail

channel_id=${PWD##*-}
[[ "${channel_id}" =~ ^[1-5]$ ]] || { echo "[srt-relay] invalid channel service: ${PWD}" >&2; exit 1; }

# shellcheck source=scripts/load-settings.sh
source /usr/local/bin/load-settings.sh
load_channel_settings "${channel_id}"

if [[ "${CHANNEL_ENABLED}" != "true" || "${INPUT_MODE}" == "rtp" ]]; then
    exec s6-pause
fi

export CHANNEL_RUNTIME_DIR="/run/webrtc-player/channels/${channel_id}"
printf '%s\n' "$$" >"${CHANNEL_RUNTIME_DIR}/ffmpeg.pid"
exec s6-setuidgid player /bin/bash /usr/local/bin/srt-relay.sh
