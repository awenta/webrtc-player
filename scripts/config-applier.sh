#!/command/with-contenv bash
# shellcheck shell=bash
set -Eeuo pipefail

# shellcheck source=scripts/load-settings.sh
load_settings_path=/usr/local/bin/load-settings.sh
[[ -r ${load_settings_path} ]] || load_settings_path="$(dirname "${BASH_SOURCE[0]}")/load-settings.sh"
source "${load_settings_path}"
unset load_settings_path

readonly REQUEST_FILE=/run/webrtc-player/config-apply.request
readonly PROCESSING_FILE=/run/webrtc-player/config-apply.processing
readonly RESULT_FILE=/run/webrtc-player/config-apply.result
readonly S6_SVC=/package/admin/s6/command/s6-svc
readonly S6_SVSTAT=/package/admin/s6/command/s6-svstat
readonly S6_SVWAIT=/package/admin/s6/command/s6-svwait
readonly READINESS_ATTEMPTS=50
declare -A EXPECTED_IPV4_LISTENERS=()
declare -A EXPECT_SELECTOR_PROCESS=()
declare -A EXPECT_FFMPEG_PROCESS=()
declare -A REQUIRE_FULL_READINESS=()
declare -A OBSERVED_PROCESS_PIDS=()
READY_REASON=

publish_result() {
    local generation="$1" status="$2"
    local temporary="${RESULT_FILE}.tmp"
    printf 'generation=%s\nstatus=%s\n' "${generation}" "${status}" >"${temporary}"
    chown player:player "${temporary}"
    mv -f "${temporary}" "${RESULT_FILE}"
}

valid_port() {
    local value="$1"
    [[ ${value} =~ ^[0-9]{1,5}$ && ( ${value} == 0 || ${value} != 0* ) ]] || return 1
    (( 10#${value} >= 1024 && 10#${value} <= 65535 ))
}

ipv4_to_proc_hex() {
    local address="$1" first second third fourth
    _load_settings_valid_ipv4 "${address}" || return 1
    IFS='.' read -r first second third fourth <<<"${address}"
    printf -v REPLY '%02X%02X%02X%02X' \
        "$((10#${fourth}))" "$((10#${third}))" "$((10#${second}))" "$((10#${first}))"
}

ipv4_listener_is_bound() {
    local address="$1" port="$2" address_hex port_hex local_address ignored count=0
    ipv4_to_proc_hex "${address}" || return 1
    address_hex=${REPLY}
    printf -v port_hex '%04X' "$((10#${port}))"
    if [[ -r /proc/net/udp ]]; then
        while read -r ignored local_address ignored; do
            [[ ${local_address} == "${address_hex}:${port_hex}" ]] && count=$((count + 1))
        done </proc/net/udp
    fi
    (( count == 1 ))
}

service_is_up() {
    local service="$1"
    "${S6_SVSTAT}" -u "/run/service/${service}" >/dev/null 2>&1
}

process_is_stably_running() {
    local key="$1" pid_file="$2" expected="$3" label="$4" pid= process_name= process_state= previous_pid
    local -a process_stat=()
    if [[ -r ${pid_file} ]]; then
        IFS= read -r pid <"${pid_file}" || pid=
    fi
    if [[ ! ${pid} =~ ^[0-9]+$ || ! -r /proc/${pid}/comm || ! -r /proc/${pid}/stat ]]; then
        READY_REASON="${label} process is not running"
        return 1
    fi
    IFS= read -r process_name <"/proc/${pid}/comm" || process_name=
    read -r -a process_stat <"/proc/${pid}/stat" || process_stat=()
    process_state=${process_stat[2]:-}
    if [[ ${process_name} != "${expected}" || -z ${process_state} ||
            ${process_state} == Z || ${process_state} == X ]]; then
        READY_REASON="${label} process is not running"
        return 1
    fi
    previous_pid=${OBSERVED_PROCESS_PIDS[${key}]:-}
    OBSERVED_PROCESS_PIDS["${key}"]=${pid}
    if [[ ${previous_pid} != "${pid}" ]]; then
        READY_REASON="${label} process is starting"
        return 1
    fi
}

prepare_channel_readiness() {
    local channel_id="$1" port_name port srt_mode direct_expected=false
    local -a expected=()
    local -A seen_ports=()

    if ! load_network_settings; then
        READY_REASON="effective network settings could not be loaded"
        return 1
    fi
    if ! _load_settings_valid_ipv4 "${INGEST_IP}" || [[ ${INGEST_IP} == 0.0.0.0 || ${INGEST_IP} == 127.* ]]; then
        READY_REASON="effective ingest address is not an external IPv4 address"
        return 1
    fi
    if ! load_channel_settings "${channel_id}"; then
        READY_REASON="channel ${channel_id} settings could not be loaded"
        return 1
    fi
    if [[ ${CHANNEL_ENABLED} != true && ${CHANNEL_ENABLED} != false ]]; then
        READY_REASON="channel ${channel_id} CHANNEL_ENABLED is invalid"
        return 1
    fi
    if [[ ${AUDIO_ENABLED} != true && ${AUDIO_ENABLED} != false ]]; then
        READY_REASON="channel ${channel_id} AUDIO_ENABLED is invalid"
        return 1
    fi
    if [[ ${INPUT_MODE} != auto && ${INPUT_MODE} != rtp && ${INPUT_MODE} != srt ]]; then
        READY_REASON="channel ${channel_id} INPUT_MODE is invalid"
        return 1
    fi
    for port_name in VIDEO_PORT AUDIO_PORT VIDEO_RTCP_PORT AUDIO_RTCP_PORT; do
        port=${!port_name}
        if ! valid_port "${port}"; then
            READY_REASON="channel ${channel_id} ${port_name} is not a valid UDP port"
            return 1
        fi
        port=$((10#${port}))
        if [[ -v seen_ports["${port}"] ]]; then
            READY_REASON="channel ${channel_id} direct RTP and RTCP ports are not unique"
            return 1
        fi
        seen_ports["${port}"]=1
    done
    srt_mode=caller
    if [[ ${SRT_URL_VALID} == true ]]; then
        if ! _load_settings_parse_srt_url "${SRT_URL}"; then
            READY_REASON="channel ${channel_id} SRT_URL is invalid"
            return 1
        fi
        srt_mode=${REPLY_MODE}
    fi
    EXPECTED_IPV4_LISTENERS["${channel_id}"]=
    EXPECT_SELECTOR_PROCESS["${channel_id}"]=false
    EXPECT_FFMPEG_PROCESS["${channel_id}"]=false
    REQUIRE_FULL_READINESS["${channel_id}"]=false

    if [[ ${CHANNEL_ENABLED} == false ]]; then
        return 0
    fi
    EXPECT_SELECTOR_PROCESS["${channel_id}"]=true
    if [[ ( ${INPUT_MODE} == auto || ${INPUT_MODE} == srt ) && ${srt_mode} != caller ]]; then
        EXPECT_FFMPEG_PROCESS["${channel_id}"]=true
    fi

    if [[ ${INPUT_MODE} == rtp || ( ${INPUT_MODE} == auto && -z ${SRT_PASSPHRASE} && -z ${SRT_PASSPHRASE_FILE:-} ) ]]; then
        direct_expected=true
    fi
    if [[ ${direct_expected} == true ]]; then
        expected+=("${INGEST_IP},${VIDEO_PORT}" "${INGEST_IP},${VIDEO_RTCP_PORT}")
        if [[ ${AUDIO_ENABLED} == true ]]; then
            expected+=("${INGEST_IP},${AUDIO_PORT}" "${INGEST_IP},${AUDIO_RTCP_PORT}")
        fi
    fi
    # libsrt sockets are not represented consistently in /proc/net/udp across
    # kernels. A stable FFmpeg PID proves that listener setup did not fail.
    if [[ ( ${INPUT_MODE} == auto || ${INPUT_MODE} == srt ) && ${srt_mode} != caller ]]; then
        REQUIRE_FULL_READINESS["${channel_id}"]=true
    fi
    EXPECTED_IPV4_LISTENERS["${channel_id}"]="${expected[*]}"
}

channels_are_ready() {
    local require_janus="$1" channel_id listener address port channel_root
    shift

    if [[ ${require_janus} == true ]] && ! service_is_up janus; then
        READY_REASON="janus service is not supervised/up"
        return 1
    fi
    for channel_id in "$@"; do
        if ! service_is_up "input-selector-${channel_id}"; then
            READY_REASON="channel ${channel_id} selector service is not supervised/up"
            return 1
        fi
        if ! service_is_up "srt-relay-${channel_id}"; then
            READY_REASON="channel ${channel_id} SRT relay service is not supervised/up"
            return 1
        fi
        channel_root="/run/webrtc-player/channels/${channel_id}"
        if [[ ${EXPECT_SELECTOR_PROCESS[${channel_id}]} == true ]] && \
                ! process_is_stably_running "selector-${channel_id}" "${channel_root}/selector.pid" \
                    input-selector "channel ${channel_id} selector"; then
            return 1
        fi
        if [[ ${EXPECT_FFMPEG_PROCESS[${channel_id}]} == true ]] && \
                ! process_is_stably_running "ffmpeg-${channel_id}" "${channel_root}/ffmpeg.pid" \
                    ffmpeg "channel ${channel_id} FFmpeg"; then
            return 1
        fi
        for listener in ${EXPECTED_IPV4_LISTENERS[${channel_id}]}; do
            address=${listener%,*}
            port=${listener##*,}
            if ! ipv4_listener_is_bound "${address}" "${port}"; then
                READY_REASON="channel ${channel_id} expected UDP listener ${address}:${port} is not bound"
                return 1
            fi
        done
    done
    READY_REASON=
}

wait_for_channels() {
    local require_janus="$1" attempt ready_streak=0 channel_id full_readiness=false maximum_attempts
    shift
    for channel_id in "$@"; do
        [[ ${REQUIRE_FULL_READINESS[${channel_id}]} == true ]] && full_readiness=true
    done
    maximum_attempts=${READINESS_ATTEMPTS}
    [[ ${full_readiness} == true ]] && maximum_attempts=$((READINESS_ATTEMPTS * 2))
    for ((attempt = 0; attempt < maximum_attempts; attempt++)); do
        if channels_are_ready "${require_janus}" "$@"; then
            ready_streak=$((ready_streak + 1))
            if [[ ${full_readiness} == false ]] && (( ready_streak >= 3 )); then
                return 0
            fi
            if [[ ${full_readiness} == true ]] && (( ready_streak >= READINESS_ATTEMPTS )); then return 0; fi
        else
            ready_streak=0
        fi
        sleep 0.1
    done
    return 1
}

if [[ ${BASH_SOURCE[0]} != "$0" ]]; then
    return 0
fi

while true; do
    if [[ ! -f "${REQUEST_FILE}" ]]; then
        sleep 0.25
        continue
    fi

    if ! mv "${REQUEST_FILE}" "${PROCESSING_FILE}" 2>/dev/null; then
        sleep 0.1
        continue
    fi
    generation=unknown
    channel=
    global_changed=false
    while IFS='=' read -r key value; do
        case "${key}" in
            generation) generation="${value}" ;;
            channel) channel="${value}" ;;
            global) global_changed="${value}" ;;
        esac
    done <"${PROCESSING_FILE}"
    if [[ ! "${channel}" =~ ^[0-5]$ || ( "${channel}" == 0 && "${global_changed}" != true ) ]]; then
        publish_result "${generation}" error
        echo "[config] invalid channel in generation ${generation}" >&2
        rm -f "${PROCESSING_FILE}"
        continue
    fi
    apply_status=ok
    stop_services=()
    if [[ ${global_changed} == true ]]; then
        echo "[config] applying global generation ${generation}"
        stop_services+=(/run/service/janus)
        for service_channel in 1 2 3 4 5; do
            stop_services+=("/run/service/input-selector-${service_channel}")
            stop_services+=("/run/service/srt-relay-${service_channel}")
        done
    else
        echo "[config] applying generation ${generation} to channel ${channel}"
        selector_service="input-selector-${channel}"
        relay_service="srt-relay-${channel}"
        stop_services+=("/run/service/${selector_service}" "/run/service/${relay_service}")
    fi
    for service_path in "${stop_services[@]}"; do
        "${S6_SVC}" -d "${service_path}"
    done
    if ! "${S6_SVWAIT}" -d -a -t 5000 "${stop_services[@]}"; then
        apply_status=error
        READY_REASON="affected services did not stop within five seconds"
    fi

    if [[ "${global_changed}" == "true" ]]; then
        if [[ ${apply_status} == ok ]] && ! /etc/cont-init.d/10-configure; then
            apply_status=error
            READY_REASON="global configuration failed"
        fi
        "${S6_SVC}" -u /run/service/janus
        for service_channel in 1 2 3 4 5; do
            "${S6_SVC}" -u "/run/service/input-selector-${service_channel}"
            "${S6_SVC}" -u "/run/service/srt-relay-${service_channel}"
        done
    fi

    if [[ ${global_changed} != true ]]; then
        "${S6_SVC}" -u "/run/service/${selector_service}"
        "${S6_SVC}" -u "/run/service/${relay_service}"
    fi

    if [[ ${apply_status} == ok ]]; then
        EXPECTED_IPV4_LISTENERS=()
        EXPECT_SELECTOR_PROCESS=()
        EXPECT_FFMPEG_PROCESS=()
        REQUIRE_FULL_READINESS=()
        OBSERVED_PROCESS_PIDS=()
        if [[ ${global_changed} == true ]]; then
            verify_channels=(1 2 3 4 5)
            require_janus=true
        else
            verify_channels=("${channel}")
            require_janus=false
        fi
        for service_channel in "${verify_channels[@]}"; do
            if ! prepare_channel_readiness "${service_channel}"; then
                apply_status=error
                break
            fi
        done
        if [[ ${apply_status} == ok ]] && ! wait_for_channels "${require_janus}" "${verify_channels[@]}"; then
            apply_status=error
        fi
    fi
    if [[ ${apply_status} == error ]]; then
        echo "[config] generation ${generation} readiness failed: ${READY_REASON}" >&2
    fi
    publish_result "${generation}" "${apply_status}"
    echo "[config] generation ${generation} status=${apply_status}"
    rm -f "${PROCESSING_FILE}"
    if [[ "${global_changed}" == true && "${apply_status}" == ok ]]; then
        sleep 2
        "${S6_SVC}" -d /run/service/nginx
        "${S6_SVC}" -u /run/service/nginx
    fi
done
