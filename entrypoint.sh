#!/command/with-contenv bash
# shellcheck shell=bash
set -Eeuo pipefail

# shellcheck source=scripts/load-settings.sh
source /usr/local/bin/load-settings.sh

readonly CONFIG_DIR=/run/webrtc-player/janus
readonly CHANNEL_RUNTIME_ROOT=/run/webrtc-player/channels
readonly NETWORK_JSON=/run/webrtc-player/network.json
readonly NETWORK_ENV=/run/webrtc-player/network.env
readonly NGINX_CONFIG=/run/webrtc-player/nginx.conf
declare -A STARTUP_UDP_PORT_OWNERS=()

fail() {
    echo "[configure] error: $*" >&2
    exit 1
}

require_uint() {
    local name="$1" value="$2" min="$3" max="$4"
    [[ "${value}" =~ ^[0-9]{1,5}$ && ( ${value} == 0 || ${value} != 0* ) ]] \
        || fail "${name} must be a canonical integer"
    (( 10#${value} >= min && 10#${value} <= max )) || fail "${name} must be between ${min} and ${max}"
}

require_bool() {
    local name="$1" value="$2"
    [[ "${value}" == "true" || "${value}" == "false" ]] || fail "${name} must be true or false"
}

config_escape() {
    local value="$1"
    value=${value//\/\\}
    value=${value//\"/\\\"}
    printf '%s' "${value}"
}

validate_channel() {
    local port_name port srt_url_port srt_mode
    local -A external_ports=()

    [[ "${CHANNEL_NAME}" != *$'\n'* && "${CHANNEL_NAME}" != *$'\r'* && -n "${CHANNEL_NAME}" ]] \
        || fail "Channel ${CHANNEL_ID} has an invalid name"
    require_bool CHANNEL_ENABLED "${CHANNEL_ENABLED}"
    [[ "${INPUT_MODE}" == "auto" || "${INPUT_MODE}" == "rtp" || "${INPUT_MODE}" == "srt" ]] \
        || fail "Channel ${CHANNEL_ID} INPUT_MODE must be auto, rtp, or srt"
    require_bool AUDIO_ENABLED "${AUDIO_ENABLED}"
    require_bool SRT_AUDIO "${SRT_AUDIO}"
    [[ "${SRT_COLOR_MODE}" == "auto" || "${SRT_COLOR_MODE}" == "fast" || "${SRT_COLOR_MODE}" == "hdr-to-sdr" ]] \
        || fail "Channel ${CHANNEL_ID} SRT_COLOR_MODE is invalid"
    if [[ ${SRT_URL_VALID} == true ]]; then
        _load_settings_parse_srt_url "${SRT_URL}" \
            || fail "Channel ${CHANNEL_ID} SRT_URL has an invalid authority, port, or query"
        srt_url_port=${REPLY}
        srt_mode=${REPLY_MODE}
        [[ ${SRT_PORT} == "${srt_url_port}" && ${SRT_PUBLIC_PORT} == "${srt_url_port}" ]] \
            || fail "Channel ${CHANNEL_ID} SRT port resolution is inconsistent"
        [[ ${srt_mode} == listener || ${srt_mode} == caller || ${srt_mode} == rendezvous ]] \
            || fail "Channel ${CHANNEL_ID} SRT_URL mode is invalid"
    elif [[ ${CHANNEL_ENABLED} != false && ${INPUT_MODE} != rtp ]]; then
        fail "Channel ${CHANNEL_ID} requires a valid SRT_URL"
    fi
    for port_name in VIDEO_PORT AUDIO_PORT VIDEO_RTCP_PORT AUDIO_RTCP_PORT \
            SRT_RELAY_VIDEO_PORT SRT_RELAY_AUDIO_PORT SRT_RELAY_VIDEO_RTCP_PORT SRT_RELAY_AUDIO_RTCP_PORT \
            JANUS_VIDEO_PORT JANUS_AUDIO_PORT JANUS_VIDEO_RTCP_PORT JANUS_AUDIO_RTCP_PORT; do
        require_uint "Channel ${CHANNEL_ID} ${port_name}" "${!port_name}" 1024 65535
    done
    for port_name in VIDEO_PORT AUDIO_PORT VIDEO_RTCP_PORT AUDIO_RTCP_PORT; do
        port=$((10#${!port_name}))
        [[ ! -v external_ports["${port}"] ]] \
            || fail "Channel ${CHANNEL_ID} direct RTP and RTCP ports must all be different"
        external_ports["${port}"]=1
    done
}

reserve_startup_udp_port() {
    local channel_id="$1" field="$2" value="$3" port
    port=$((10#${value}))
    if (( port >= 20000 && port <= 20100 )); then
        fail "Channel ${channel_id} ${field} port ${port} conflicts with fixed ICE range 20000-20100"
    fi
    if [[ -v STARTUP_UDP_PORT_OWNERS["${port}"] ]]; then
        fail "UDP port ${port} conflicts between ${STARTUP_UDP_PORT_OWNERS[${port}]} and Channel ${channel_id} ${field}"
    fi
    STARTUP_UDP_PORT_OWNERS["${port}"]="Channel ${channel_id} ${field}"
}

validate_all_channel_ports() {
    local channel_id port_name srt_port srt_mode bridge_base bridge_srt_port
    STARTUP_UDP_PORT_OWNERS=()
    for channel_id in 1 2 3 4 5; do
        load_channel_settings "${channel_id}" || fail "Channel ${channel_id} settings could not be loaded"
        validate_channel
        if [[ ${NETWORK_MODE} == bridge ]]; then
            bridge_base=$((5004 + 4 * (channel_id - 1)))
            [[ ${VIDEO_PORT} == "${bridge_base}" && ${AUDIO_PORT} == "$((bridge_base + 1))" &&
                    ${VIDEO_RTCP_PORT} == "$((bridge_base + 2))" &&
                    ${AUDIO_RTCP_PORT} == "$((bridge_base + 3))" ]] \
                || fail "Channel ${channel_id} direct ports must use the published default allocation in bridge mode"
        fi
        for port_name in VIDEO_PORT AUDIO_PORT VIDEO_RTCP_PORT AUDIO_RTCP_PORT; do
            reserve_startup_udp_port "${channel_id}" "${port_name}" "${!port_name}"
        done
        if [[ ${SRT_URL_VALID} == true ]]; then
            _load_settings_parse_srt_url "${SRT_URL}" \
                || fail "Channel ${channel_id} SRT_URL could not be parsed for port reservation"
            srt_port=${REPLY}
            srt_mode=${REPLY_MODE}
            if [[ ${srt_mode} != caller ]]; then
                if [[ ${NETWORK_MODE} == bridge ]]; then
                    bridge_srt_port=$((9000 + channel_id - 1))
                    [[ ${srt_port} == "${bridge_srt_port}" ]] \
                        || fail "Channel ${channel_id} local SRT port must be ${bridge_srt_port} in bridge mode"
                fi
                reserve_startup_udp_port "${channel_id}" "SRT_URL local" "${srt_port}"
            fi
        fi
    done
}

load_global_settings
NETWORK_MODE=${NETWORK_MODE:-bridge}
[[ "${NETWORK_MODE}" == host || "${NETWORK_MODE}" == bridge ]] || fail "NETWORK_MODE must be host or bridge"
if [[ -n "${PUBLIC_IP}" ]]; then
    /usr/local/bin/network-info validate-ipv4 "${PUBLIC_IP}" \
        || fail "PUBLIC_IP must be an IPv4 address"
fi

JANUS_ADMIN_KEY="${JANUS_ADMIN_KEY:-$(< /proc/sys/kernel/random/uuid)}"
STREAM_SECRET="${STREAM_SECRET:-$(< /proc/sys/kernel/random/uuid)}"
JANUS_ADMIN_SECRET="${JANUS_ADMIN_SECRET:-$(< /proc/sys/kernel/random/uuid)}"
[[ "${JANUS_ADMIN_KEY}" =~ ^[A-Za-z0-9._-]+$ ]] || fail "JANUS_ADMIN_KEY contains unsupported characters"
[[ "${STREAM_SECRET}" =~ ^[A-Za-z0-9._-]+$ ]] || fail "STREAM_SECRET contains unsupported characters"
[[ "${JANUS_ADMIN_SECRET}" =~ ^[A-Za-z0-9._-]+$ ]] || fail "JANUS_ADMIN_SECRET contains unsupported characters"

install -d -o root -g player -m 1770 /run/webrtc-player
install -d -o player -g player -m 0750 \
    "${CONFIG_DIR}" "${CHANNEL_RUNTIME_ROOT}" /config /config/settings /config/channels
chown -R player:player /config
chmod 0750 /config /config/settings /config/channels
validate_all_channel_ports

management_resolution=$(/usr/local/bin/network-info resolve "${MANAGEMENT_INTERFACE}") \
    || fail "MANAGEMENT_INTERFACE does not select an UP IPv4 interface"
ingest_resolution=$(/usr/local/bin/network-info resolve "${INGEST_INTERFACE}") \
    || fail "INGEST_INTERFACE does not select an UP IPv4 interface"
webrtc_resolution=$(/usr/local/bin/network-info resolve "${WEBRTC_INTERFACE}") \
    || fail "WEBRTC_INTERFACE does not select an UP IPv4 interface"
IFS=$'\t' read -r MANAGEMENT_INTERFACE_NAME MANAGEMENT_IP <<<"${management_resolution}"
IFS=$'\t' read -r INGEST_INTERFACE_NAME INGEST_IP <<<"${ingest_resolution}"
IFS=$'\t' read -r WEBRTC_INTERFACE_NAME WEBRTC_IP <<<"${webrtc_resolution}"
[[ -n ${MANAGEMENT_INTERFACE_NAME} && -n ${MANAGEMENT_IP} &&
        -n ${INGEST_INTERFACE_NAME} && -n ${INGEST_IP} &&
        -n ${WEBRTC_INTERFACE_NAME} && -n ${WEBRTC_IP} ]] || fail "Could not resolve network roles"

umask 0027
network_json_tmp=$(mktemp "${NETWORK_JSON}.tmp.XXXXXX")
network_env_tmp=$(mktemp "${NETWORK_ENV}.tmp.XXXXXX")
janus_secret_tmp=
cleanup_network_temps() {
    rm -f "${network_json_tmp:-}" "${network_env_tmp:-}" "${janus_secret_tmp:-}"
}
trap cleanup_network_temps EXIT
/usr/local/bin/network-info json "${NETWORK_MODE}" "${MANAGEMENT_INTERFACE}" \
    "${INGEST_INTERFACE}" "${WEBRTC_INTERFACE}" >"${network_json_tmp}" \
    || fail "Could not describe resolved network roles"
DEFAULT_INTERFACE=$(/usr/local/bin/network-info resolve auto)
DEFAULT_INTERFACE=${DEFAULT_INTERFACE%%$'\t'*}
printf '%s\n' \
    "NETWORK_MODE=${NETWORK_MODE}" \
    "DEFAULT_INTERFACE=${DEFAULT_INTERFACE}" \
    "MANAGEMENT_SELECTOR=${MANAGEMENT_INTERFACE}" \
    "MANAGEMENT_INTERFACE_NAME=${MANAGEMENT_INTERFACE_NAME}" \
    "MANAGEMENT_IP=${MANAGEMENT_IP}" \
    "INGEST_SELECTOR=${INGEST_INTERFACE}" \
    "INGEST_INTERFACE_NAME=${INGEST_INTERFACE_NAME}" \
    "INGEST_IP=${INGEST_IP}" \
    "WEBRTC_SELECTOR=${WEBRTC_INTERFACE}" \
    "WEBRTC_INTERFACE_NAME=${WEBRTC_INTERFACE_NAME}" \
    "WEBRTC_IP=${WEBRTC_IP}" >"${network_env_tmp}"
chown root:player "${network_json_tmp}" "${network_env_tmp}"
chmod 0640 "${network_json_tmp}" "${network_env_tmp}"
mv -f "${network_json_tmp}" "${NETWORK_JSON}"
mv -f "${network_env_tmp}" "${NETWORK_ENV}"
network_json_tmp=
network_env_tmp=
load_network_settings || fail "Generated network settings are invalid"
echo "[configure] network mode=${NETWORK_MODE} management=${MANAGEMENT_INTERFACE_NAME}/${MANAGEMENT_IP} ingest=${INGEST_INTERFACE_NAME}/${INGEST_IP} webrtc=${WEBRTC_INTERFACE_NAME}/${WEBRTC_IP}"

management_listen="# Management interface is loopback"
if [[ "${MANAGEMENT_IP}" != 127.0.0.1 ]]; then
    management_listen="listen ${MANAGEMENT_IP}:8088;"
fi
sed -e "s|        # __MANAGEMENT_LISTEN__|        ${management_listen}|" \
    /etc/webrtc-player/nginx/nginx.conf >"${NGINX_CONFIG}.tmp"
chown root:player "${NGINX_CONFIG}.tmp"
chmod 0640 "${NGINX_CONFIG}.tmp"
mv -f "${NGINX_CONFIG}.tmp" "${NGINX_CONFIG}"

if [[ -n "${PUBLIC_IP}" ]]; then
    nat_mapping="nat_1_1_mapping = \"${PUBLIC_IP}\""
    ice_lite=false
    echo "[configure] advertising PUBLIC_IP=${PUBLIC_IP} in ICE candidates"
else
    nat_mapping="# No one-to-one NAT mapping configured"
    ice_lite=true
    echo "[configure] PUBLIC_IP is unset; Janus will use direct ICE Lite candidates"
fi

sed \
    -e "s|# __NAT_MAPPING__|${nat_mapping}|" \
    -e "s|__ICE_LITE__|${ice_lite}|" \
    -e "s|__ICE_ENFORCE_LIST__|${WEBRTC_INTERFACE_NAME},${WEBRTC_IP}|" \
    -e "s|__JANUS_ADMIN_SECRET__|${JANUS_ADMIN_SECRET}|" \
    /etc/webrtc-player/janus/janus.jcfg >"${CONFIG_DIR}/janus.jcfg"
cp /etc/webrtc-player/janus/janus.transport.websockets.jcfg "${CONFIG_DIR}/janus.transport.websockets.jcfg"
cp /etc/webrtc-player/janus/janus.transport.http.jcfg "${CONFIG_DIR}/janus.transport.http.jcfg"
janus_secret_tmp=$(mktemp /run/webrtc-player/janus-admin.secret.tmp.XXXXXX)
printf '%s\n' "${JANUS_ADMIN_SECRET}" >"${janus_secret_tmp}"
chown player:player "${janus_secret_tmp}"
chmod 0600 "${janus_secret_tmp}"
mv -f "${janus_secret_tmp}" /run/webrtc-player/janus-admin.secret
janus_secret_tmp=

cat >"${CONFIG_DIR}/janus.plugin.streaming.jcfg" <<EOF
general: {
    admin_key = "${JANUS_ADMIN_KEY}"
}
EOF

for channel_id in 1 2 3 4 5; do
    load_channel_settings "${channel_id}"
    validate_channel
    channel_dir="${CHANNEL_RUNTIME_ROOT}/${channel_id}"
    install -d -o player -g player -m 0750 "${channel_dir}"
    progress_fifo="${channel_dir}/ffmpeg-progress"
    if [[ ! -p "${progress_fifo}" ]]; then
        rm -f "${progress_fifo}"
        mkfifo -m 0600 "${progress_fifo}"
    fi
    chown player:player "${progress_fifo}"

    escaped_name=$(config_escape "${CHANNEL_NAME}")
    cat >>"${CONFIG_DIR}/janus.plugin.streaming.jcfg" <<EOF

stream-${STREAM_ID}: {
    type = "rtp"
    id = ${STREAM_ID}
    description = "${escaped_name}"
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
            iface = "127.0.0.1"
            pt = 96
            codec = "h264"
            fmtp = "profile-level-id=42e02a;packetization-mode=1"
        },
        {
            type = "audio"
            mid = "a"
            label = "Audio"
            port = ${JANUS_AUDIO_PORT}
            rtcpport = ${JANUS_AUDIO_RTCP_PORT}
            iface = "127.0.0.1"
            pt = 111
            codec = "opus"
            fmtp = "stereo=1;sprop-stereo=1;minptime=10;useinbandfec=1"
        }
    )
}
EOF
    echo "[configure] channel=${CHANNEL_ID} enabled=${CHANNEL_ENABLED} stream=${STREAM_ID} srt-port=${SRT_PORT} video-rtp=${VIDEO_PORT} audio-rtp=${AUDIO_PORT} video-rtcp=${VIDEO_RTCP_PORT} audio-rtcp=${AUDIO_RTCP_PORT}"
done

chown -R player:player "${CONFIG_DIR}" "${CHANNEL_RUNTIME_ROOT}"
chown root:player /run/webrtc-player
chmod 1770 /run/webrtc-player
chmod 0640 "${CONFIG_DIR}"/*.jcfg
