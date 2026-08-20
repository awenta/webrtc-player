#!/command/with-contenv bash
# shellcheck shell=bash
set -Eeuo pipefail

# shellcheck source=scripts/load-settings.sh
source /usr/local/bin/load-settings.sh

readonly CONFIG_DIR=/run/webrtc-player/janus

fail() {
    echo "[configure] error: $*" >&2
    exit 1
}

require_uint() {
    local name="$1" value="$2" min="$3" max="$4"
    [[ "${value}" =~ ^[0-9]+$ ]] || fail "${name} must be an integer"
    (( value >= min && value <= max )) || fail "${name} must be between ${min} and ${max}"
}

require_bool() {
    local name="$1" value="$2"
    [[ "${value}" == "true" || "${value}" == "false" ]] || fail "${name} must be true or false"
}

INPUT_MODE="${INPUT_MODE:-auto}"
STREAM_ID="${STREAM_ID:-1}"
VIDEO_PORT="${VIDEO_PORT:-5004}"
AUDIO_PORT="${AUDIO_PORT:-5005}"
VIDEO_RTCP_PORT="${VIDEO_RTCP_PORT:-5006}"
AUDIO_RTCP_PORT="${AUDIO_RTCP_PORT:-5007}"
SRT_RELAY_VIDEO_PORT="${SRT_RELAY_VIDEO_PORT:-15004}"
SRT_RELAY_AUDIO_PORT="${SRT_RELAY_AUDIO_PORT:-15005}"
SRT_RELAY_VIDEO_RTCP_PORT="${SRT_RELAY_VIDEO_RTCP_PORT:-15006}"
SRT_RELAY_AUDIO_RTCP_PORT="${SRT_RELAY_AUDIO_RTCP_PORT:-15007}"
JANUS_VIDEO_PORT="${JANUS_VIDEO_PORT:-25004}"
JANUS_AUDIO_PORT="${JANUS_AUDIO_PORT:-25005}"
JANUS_VIDEO_RTCP_PORT="${JANUS_VIDEO_RTCP_PORT:-25006}"
JANUS_AUDIO_RTCP_PORT="${JANUS_AUDIO_RTCP_PORT:-25007}"
AUDIO_ENABLED="${AUDIO_ENABLED:-true}"
SRT_AUDIO="${SRT_AUDIO:-true}"
SRT_COLOR_MODE="${SRT_COLOR_MODE:-auto}"
PUBLIC_IP="${PUBLIC_IP:-}"

[[ "${INPUT_MODE}" == "auto" || "${INPUT_MODE}" == "rtp" || "${INPUT_MODE}" == "srt" ]] \
    || fail "INPUT_MODE must be auto, rtp, or srt"
require_uint STREAM_ID "${STREAM_ID}" 1 2147483647
require_uint VIDEO_PORT "${VIDEO_PORT}" 1024 65535
require_uint AUDIO_PORT "${AUDIO_PORT}" 1024 65535
require_uint VIDEO_RTCP_PORT "${VIDEO_RTCP_PORT}" 1024 65535
require_uint AUDIO_RTCP_PORT "${AUDIO_RTCP_PORT}" 1024 65535
require_uint SRT_RELAY_VIDEO_PORT "${SRT_RELAY_VIDEO_PORT}" 1024 65535
require_uint SRT_RELAY_AUDIO_PORT "${SRT_RELAY_AUDIO_PORT}" 1024 65535
require_uint SRT_RELAY_VIDEO_RTCP_PORT "${SRT_RELAY_VIDEO_RTCP_PORT}" 1024 65535
require_uint SRT_RELAY_AUDIO_RTCP_PORT "${SRT_RELAY_AUDIO_RTCP_PORT}" 1024 65535
require_uint JANUS_VIDEO_PORT "${JANUS_VIDEO_PORT}" 1024 65535
require_uint JANUS_AUDIO_PORT "${JANUS_AUDIO_PORT}" 1024 65535
require_uint JANUS_VIDEO_RTCP_PORT "${JANUS_VIDEO_RTCP_PORT}" 1024 65535
require_uint JANUS_AUDIO_RTCP_PORT "${JANUS_AUDIO_RTCP_PORT}" 1024 65535
port_count="$(printf '%s\n' \
    "${VIDEO_PORT}" "${AUDIO_PORT}" "${VIDEO_RTCP_PORT}" "${AUDIO_RTCP_PORT}" \
    "${SRT_RELAY_VIDEO_PORT}" "${SRT_RELAY_AUDIO_PORT}" "${SRT_RELAY_VIDEO_RTCP_PORT}" "${SRT_RELAY_AUDIO_RTCP_PORT}" \
    "${JANUS_VIDEO_PORT}" "${JANUS_AUDIO_PORT}" "${JANUS_VIDEO_RTCP_PORT}" "${JANUS_AUDIO_RTCP_PORT}" \
    | sort -u | wc -l)"
[[ "${port_count}" == "12" ]] || fail "External, relay, and Janus media ports must all be different"
require_bool AUDIO_ENABLED "${AUDIO_ENABLED}"
require_bool SRT_AUDIO "${SRT_AUDIO}"
[[ "${SRT_COLOR_MODE}" == "auto" || "${SRT_COLOR_MODE}" == "fast" || "${SRT_COLOR_MODE}" == "hdr-to-sdr" ]] \
    || fail "SRT_COLOR_MODE must be auto, fast, or hdr-to-sdr"

if [[ "${INPUT_MODE}" != "rtp" ]]; then
    [[ "${SRT_URL:-}" == srt://* ]] || fail "SRT_URL must start with srt:// when SRT input is enabled"
    if [[ "${INPUT_MODE}" == "srt" ]]; then
        AUDIO_ENABLED="${SRT_AUDIO}"
    elif [[ "${SRT_AUDIO}" == "true" ]]; then
        AUDIO_ENABLED=true
    fi
fi

if [[ -n "${PUBLIC_IP}" ]]; then
    [[ "${PUBLIC_IP}" =~ ^[0-9A-Fa-f:.]+$ ]] || fail "PUBLIC_IP must be an IPv4 or IPv6 address"
    getent ahosts "${PUBLIC_IP}" >/dev/null 2>&1 || fail "PUBLIC_IP is not a valid IP address"
fi

JANUS_ADMIN_KEY="${JANUS_ADMIN_KEY:-$(< /proc/sys/kernel/random/uuid)}"
STREAM_SECRET="${STREAM_SECRET:-$(< /proc/sys/kernel/random/uuid)}"
[[ "${JANUS_ADMIN_KEY}" =~ ^[A-Za-z0-9._-]+$ ]] || fail "JANUS_ADMIN_KEY contains unsupported characters"
[[ "${STREAM_SECRET}" =~ ^[A-Za-z0-9._-]+$ ]] || fail "STREAM_SECRET contains unsupported characters"

media_iface='            iface = "127.0.0.1"'

install -d -o player -g player -m 0750 /run/webrtc-player "${CONFIG_DIR}" /config /config/settings
chown -R player:player /config
chmod 0750 /config /config/settings
if [[ "${INPUT_MODE}" != "rtp" ]]; then
    if [[ ! -p /run/webrtc-player/ffmpeg-progress ]]; then
        rm -f /run/webrtc-player/ffmpeg-progress
        mkfifo -m 0600 /run/webrtc-player/ffmpeg-progress
    fi
else
    rm -f /run/webrtc-player/ffmpeg-progress
fi

if [[ -n "${PUBLIC_IP}" ]]; then
    nat_mapping="nat_1_1_mapping = \"${PUBLIC_IP}\""
    echo "[configure] advertising PUBLIC_IP=${PUBLIC_IP} in ICE candidates"
else
    nat_mapping="# No one-to-one NAT mapping configured"
    echo "[configure] PUBLIC_IP is unset; Janus will advertise its interface addresses"
fi

sed "s|# __NAT_MAPPING__|${nat_mapping}|" /etc/webrtc-player/janus/janus.jcfg >"${CONFIG_DIR}/janus.jcfg"
cp /etc/webrtc-player/janus/janus.transport.websockets.jcfg "${CONFIG_DIR}/janus.transport.websockets.jcfg"

cat >"${CONFIG_DIR}/janus.plugin.streaming.jcfg" <<EOF
general: {
    admin_key = "${JANUS_ADMIN_KEY}"
}

stream-${STREAM_ID}: {
    type = "rtp"
    id = ${STREAM_ID}
    description = "Live Stream ${STREAM_ID}"
    secret = "${STREAM_SECRET}"
    collision = 1000
    threads = 2
    media = (
        {
            type = "video"
            mid = "v"
            label = "Video"
            port = ${JANUS_VIDEO_PORT}
            rtcpport = ${JANUS_VIDEO_RTCP_PORT}
${media_iface}
            pt = 96
            codec = "h264"
            fmtp = "profile-level-id=42e02a;packetization-mode=1"
        }
EOF

if [[ "${AUDIO_ENABLED}" == "true" ]]; then
    cat >>"${CONFIG_DIR}/janus.plugin.streaming.jcfg" <<EOF
        ,
        {
            type = "audio"
            mid = "a"
            label = "Audio"
            port = ${JANUS_AUDIO_PORT}
            rtcpport = ${JANUS_AUDIO_RTCP_PORT}
${media_iface}
            pt = 111
            codec = "opus"
            fmtp = "stereo=1;sprop-stereo=1;minptime=10;useinbandfec=1"
        }
EOF
fi

cat >>"${CONFIG_DIR}/janus.plugin.streaming.jcfg" <<'EOF'
    )
}
EOF

chown -R player:player "${CONFIG_DIR}"
chown player:player /run/webrtc-player
if [[ -p /run/webrtc-player/ffmpeg-progress ]]; then
    chown player:player /run/webrtc-player/ffmpeg-progress
fi
chmod 0640 "${CONFIG_DIR}"/*.jcfg

echo "[configure] mode=${INPUT_MODE} stream=${STREAM_ID} video-port=${VIDEO_PORT} audio=${AUDIO_ENABLED}"
if [[ "${INPUT_MODE}" != "rtp" ]]; then
    echo "[configure] SRT relay enabled (URL hidden; color mode=${SRT_COLOR_MODE})"
fi
