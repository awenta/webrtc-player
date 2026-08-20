#!/command/with-contenv bash
# shellcheck shell=bash
set -Eeuo pipefail

# shellcheck source=scripts/load-settings.sh
source /usr/local/bin/load-settings.sh

readonly STATUS_FILE=/run/webrtc-player/status.json
readonly STATUS_TMP=/run/webrtc-player/status.json.tmp
readonly CHANNEL_ROOT=/run/webrtc-player/channels

started_at=$(date +%s)
cpu_count=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
page_size=$(getconf PAGESIZE 2>/dev/null || printf '4096')
page_kib=$((page_size / 1024))
container_ip=""
read -r container_ip _ < <(getent ahostsv4 "${HOSTNAME:-localhost}" 2>/dev/null || true)

json_quote() {
    local value="$1"
    value=${value//\/\\}
    value=${value//\"/\\\"}
    value=${value//$'\n'/\\n}
    value=${value//$'\r'/\\r}
    value=${value//$'\t'/\\t}
    printf '"%s"' "${value}"
}

decimal_or_null() {
    local value="$1"
    if [[ "${value}" =~ ^-?[0-9]+([.][0-9]+)?$ ]]; then
        printf '%s' "${value}"
    else
        printf 'null'
    fi
}

tenths() {
    local value="$1"
    printf '%d.%d' "$((value / 10))" "$((value % 10))"
}

parse_srt_url() {
    local url="$1" target
    parsed_srt_host=0.0.0.0
    parsed_srt_port=
    parsed_srt_role=listener
    target=${url#srt://}
    target=${target%%\?*}
    target=${target%%/*}
    if [[ "${target}" == \[* ]]; then
        parsed_srt_host=${target#\[}
        parsed_srt_host=${parsed_srt_host%%\]*}
        parsed_srt_port=${target##*]:}
    elif [[ "${target}" == *:* ]]; then
        parsed_srt_host=${target%:*}
        parsed_srt_port=${target##*:}
    else
        parsed_srt_host=${target}
    fi
    if [[ "${url}" == *mode=caller* ]]; then
        parsed_srt_role=caller
    elif [[ "${url}" == *mode=rendezvous* ]]; then
        parsed_srt_role=rendezvous
    fi
    return 0
}

read_service_process() {
    local key="$1" pid_file="$2" expected="$3" pid= process_name= stat_line
    current_jiffies["${key}"]=0
    rss_kib["${key}"]=0
    process_running["${key}"]=false
    if [[ -r "${pid_file}" ]]; then
        IFS= read -r pid <"${pid_file}" || pid=
    fi
    if [[ "${pid}" =~ ^[0-9]+$ && -r "/proc/${pid}/comm" && -r "/proc/${pid}/stat" ]]; then
        IFS= read -r process_name <"/proc/${pid}/comm" || process_name=
        if [[ "${process_name}" == "${expected}" ]]; then
            read -r -a stat_line <"/proc/${pid}/stat"
            current_jiffies["${key}"]=$((stat_line[13] + stat_line[14]))
            rss_kib["${key}"]=$((stat_line[23] * page_kib))
            process_running["${key}"]=true
        fi
    fi
}

read_shared_processes() {
    local proc_dir process_name key
    for key in janus nginx; do
        current_jiffies["${key}"]=0
        rss_kib["${key}"]=0
        process_running["${key}"]=false
    done
    for proc_dir in /proc/[0-9]*; do
        [[ -r "${proc_dir}/comm" && -r "${proc_dir}/stat" ]] || continue
        IFS= read -r process_name <"${proc_dir}/comm" || continue
        case "${process_name}" in
            janus) key=janus ;;
            nginx) key=nginx ;;
            *) continue ;;
        esac
        read -r -a stat_line <"${proc_dir}/stat" || continue
        current_jiffies["${key}"]=$((current_jiffies[${key}] + stat_line[13] + stat_line[14]))
        rss_kib["${key}"]=$((rss_kib[${key}] + stat_line[23] * page_kib))
        process_running["${key}"]=true
    done
}

declare -A progress=()
declare -A progress_fds=()
declare -A previous_jiffies=()
declare -A current_jiffies=()
declare -A rss_kib=()
declare -A process_running=()
declare -A cpu_tenths=()
previous_total=0

for channel_id in 1 2 3 4 5; do
    progress_fifo="${CHANNEL_ROOT}/${channel_id}/ffmpeg-progress"
    exec {progress_fd}<>"${progress_fifo}"
    progress_fds["${channel_id}"]=${progress_fd}
done

while true; do
    now=$(date +%s)
    load_global_settings

    for channel_id in 1 2 3 4 5; do
        progress_fd=${progress_fds[${channel_id}]}
        while IFS= read -r -t 0.001 progress_line <&"${progress_fd}"; do
            [[ "${progress_line}" == *=* ]] || continue
            progress_key=${progress_line%%=*}
            progress_value=${progress_line#*=}
            case "${progress_key}" in
                frame|fps|bitrate|out_time|speed|dup_frames|drop_frames|progress)
                    progress["${channel_id}_${progress_key}"]=${progress_value}
                    ;;
            esac
        done
    done

    read -r _ cpu_user cpu_nice cpu_system cpu_idle cpu_iowait cpu_irq cpu_softirq cpu_steal _ < /proc/stat
    total_jiffies=$((cpu_user + cpu_nice + cpu_system + cpu_idle + cpu_iowait + cpu_irq + cpu_softirq + cpu_steal))

    for channel_id in 1 2 3 4 5; do
        read_service_process "selector_${channel_id}" "${CHANNEL_ROOT}/${channel_id}/selector.pid" input-selector
        read_service_process "ffmpeg_${channel_id}" "${CHANNEL_ROOT}/${channel_id}/ffmpeg.pid" ffmpeg
    done
    read_shared_processes

    if (( previous_total > 0 && total_jiffies > previous_total )); then
        total_delta=$((total_jiffies - previous_total))
        for key in "${!current_jiffies[@]}"; do
            process_delta=$((current_jiffies[${key}] - ${previous_jiffies[${key}]:-0}))
            (( process_delta < 0 )) && process_delta=0
            cpu_tenths["${key}"]=$((process_delta * cpu_count * 1000 / total_delta))
        done
    fi
    previous_total=${total_jiffies}
    for key in "${!current_jiffies[@]}"; do
        previous_jiffies["${key}"]=${current_jiffies[${key}]}
    done

    read -r load_average _ < /proc/loadavg
    public_ip_json=null
    [[ -n "${PUBLIC_IP}" ]] && public_ip_json=$(json_quote "${PUBLIC_IP}")
    container_ip_json=null
    [[ -n "${container_ip}" ]] && container_ip_json=$(json_quote "${container_ip}")

    cat >"${STATUS_TMP}" <<EOF
{
  "version": 2,
  "timestamp": ${now},
  "uptimeSeconds": $((now - started_at)),
  "channels": [
EOF

    for channel_id in 1 2 3 4 5; do
        load_channel_settings "${channel_id}"
        parse_srt_url "${SRT_URL}"
        selector_status="${CHANNEL_ROOT}/${channel_id}/input-selector.status"
        selected_source=none
        direct_enabled=false
        srt_enabled=false
        remote_address=
        remote_port=
        forward_drops=0
        direct_packets=0
        srt_packets=0
        switches=0
        if [[ -r "${selector_status}" ]]; then
            while IFS='=' read -r selector_key selector_value; do
                case "${selector_key}" in
                    source) selected_source=${selector_value} ;;
                    direct_enabled) direct_enabled=${selector_value} ;;
                    srt_enabled) srt_enabled=${selector_value} ;;
                    remote_address) remote_address=${selector_value} ;;
                    remote_port) remote_port=${selector_value} ;;
                    forward_drops) forward_drops=${selector_value} ;;
                    direct_packets) direct_packets=${selector_value} ;;
                    srt_packets) srt_packets=${selector_value} ;;
                    switches) switches=${selector_value} ;;
                esac
            done <"${selector_status}"
        fi

        input_state=disabled
        [[ "${CHANNEL_ENABLED}" == true ]] && input_state=waiting
        [[ "${CHANNEL_ENABLED}" == true && "${selected_source}" != none ]] && input_state=active
        input_transport="RTP + SRT"
        input_role=automatic
        input_host=0.0.0.0
        input_port=
        input_audio_enabled=${AUDIO_ENABLED}
        processing_mode=standby
        if [[ "${selected_source}" == rtp || "${INPUT_MODE}" == rtp ]]; then
            input_transport=RTP
            input_role=receiver
            input_port=${VIDEO_PORT}
            processing_mode=passthrough
        elif [[ "${selected_source}" == srt || "${INPUT_MODE}" == srt ]]; then
            input_transport="SRT / MPEG-TS"
            input_role=${parsed_srt_role}
            input_host=${parsed_srt_host}
            input_port=${parsed_srt_port}
            input_audio_enabled=${SRT_AUDIO}
            processing_mode=transcode
        fi

        processing_active=false
        [[ "${selected_source}" == srt && "${input_state}" == active ]] && processing_active=true
        progress_frame=null
        progress_fps=null
        progress_bitrate=null
        progress_speed=null
        progress_out_time=null
        progress_dropped=null
        progress_duplicated=null
        if [[ "${processing_active}" == true ]]; then
            progress_frame=$(decimal_or_null "${progress[${channel_id}_frame]:-}")
            progress_fps=$(decimal_or_null "${progress[${channel_id}_fps]:-}")
            progress_bitrate=$(json_quote "${progress[${channel_id}_bitrate]:-}")
            progress_speed=$(json_quote "${progress[${channel_id}_speed]:-}")
            progress_out_time=$(json_quote "${progress[${channel_id}_out_time]:-}")
            progress_dropped=$(decimal_or_null "${progress[${channel_id}_drop_frames]:-}")
            progress_duplicated=$(decimal_or_null "${progress[${channel_id}_dup_frames]:-}")
        fi

        selector_key="selector_${channel_id}"
        ffmpeg_key="ffmpeg_${channel_id}"
        selector_cpu=${cpu_tenths[${selector_key}]:-0}
        ffmpeg_cpu=${cpu_tenths[${ffmpeg_key}]:-0}
        selector_rss=${rss_kib[${selector_key}]:-0}
        ffmpeg_rss=${rss_kib[${ffmpeg_key}]:-0}
        channel_cpu=$((selector_cpu + ffmpeg_cpu))
        channel_rss=$((selector_rss + ffmpeg_rss))
        selector_running=${process_running[${selector_key}]:-false}
        ffmpeg_running=${process_running[${ffmpeg_key}]:-false}
        remote_address_json=null
        [[ -n "${remote_address}" ]] && remote_address_json=$(json_quote "${remote_address}")
        remote_port_json=null
        [[ "${remote_port}" =~ ^[0-9]+$ && "${remote_port}" != 0 ]] && remote_port_json=${remote_port}
        janus_available=false
        viewers=0
        packets_sent=0
        bytes_sent=0
        bitrate_bps=
        nacks_received=0
        remote_lost=
        rtt_ms=
        jitter_ms=
        link_quality=
        media_link_quality=
        janus_output_status="${CHANNEL_ROOT}/${channel_id}/janus-output.status"
        if [[ -r "${janus_output_status}" ]]; then
            while IFS='=' read -r output_key output_value; do
                case "${output_key}" in
                    available) janus_available=${output_value} ;;
                    viewers) viewers=${output_value} ;;
                    packets_sent) packets_sent=${output_value} ;;
                    bytes_sent) bytes_sent=${output_value} ;;
                    bitrate_bps) bitrate_bps=${output_value} ;;
                    nacks_received) nacks_received=${output_value} ;;
                    remote_lost) remote_lost=${output_value} ;;
                    rtt_ms) rtt_ms=${output_value} ;;
                    jitter_ms) jitter_ms=${output_value} ;;
                    link_quality) link_quality=${output_value} ;;
                    media_link_quality) media_link_quality=${output_value} ;;
                esac
            done <"${janus_output_status}"
        fi
        comma=,
        [[ "${channel_id}" == 5 ]] && comma=

        cat >>"${STATUS_TMP}" <<EOF
    {
      "id": ${channel_id},
      "name": $(json_quote "${CHANNEL_NAME}"),
      "enabled": ${CHANNEL_ENABLED},
      "input": {
        "mode": $(json_quote "${INPUT_MODE}"),
        "selectedSource": $(json_quote "${selected_source}"),
        "state": $(json_quote "${input_state}"),
        "transport": $(json_quote "${input_transport}"),
        "role": $(json_quote "${input_role}"),
        "address": $(json_quote "${input_host}"),
        "port": $(decimal_or_null "${input_port}"),
        "remoteAddress": ${remote_address_json},
        "remotePort": ${remote_port_json},
        "acceptsRtp": ${direct_enabled},
        "acceptsSrt": ${srt_enabled},
        "srtAddress": $(json_quote "${parsed_srt_host}"),
        "srtPort": $(decimal_or_null "${parsed_srt_port}"),
        "srtPublicPort": ${SRT_PUBLIC_PORT},
        "videoRtpPort": ${VIDEO_PORT},
        "audioRtpPort": ${AUDIO_PORT},
        "videoRtcpPort": ${VIDEO_RTCP_PORT},
        "audioRtcpPort": ${AUDIO_RTCP_PORT},
        "audioEnabled": ${input_audio_enabled},
        "packets": { "direct": ${direct_packets}, "srt": ${srt_packets} },
        "sourceSwitches": ${switches},
        "forwardDrops": ${forward_drops}
      },
      "processing": {
        "mode": $(json_quote "${processing_mode}"),
        "active": ${processing_active},
        "frame": ${progress_frame},
        "fps": ${progress_fps},
        "bitrate": ${progress_bitrate},
        "speed": ${progress_speed},
        "elapsed": ${progress_out_time},
        "droppedFrames": ${progress_dropped},
        "duplicatedFrames": ${progress_duplicated},
        "cpuPercent": $(tenths "${channel_cpu}"),
        "memoryMiB": $(tenths "$((channel_rss * 10 / 1024))")
      },
      "output": {
        "transport": "WebRTC",
        "streamId": ${STREAM_ID},
        "active": $([[ "${input_state}" == active ]] && printf true || printf false),
        "telemetryAvailable": ${janus_available},
        "viewerCount": ${viewers},
        "aggregateBitrate": $(decimal_or_null "${bitrate_bps}"),
        "packetsSent": ${packets_sent},
        "bytesSent": ${bytes_sent},
        "nacksReceived": ${nacks_received},
        "remoteLoss": $(decimal_or_null "${remote_lost}"),
        "rttMs": $(decimal_or_null "${rtt_ms}"),
        "jitterMs": $(decimal_or_null "${jitter_ms}"),
        "linkQuality": $(decimal_or_null "${link_quality}"),
        "mediaLinkQuality": $(decimal_or_null "${media_link_quality}")
      },
      "services": {
        "selector": { "running": ${selector_running}, "cpuPercent": $(tenths "${selector_cpu}"), "memoryMiB": $(tenths "$((selector_rss * 10 / 1024))") },
        "ffmpeg": { "running": ${ffmpeg_running}, "cpuPercent": $(tenths "${ffmpeg_cpu}"), "memoryMiB": $(tenths "$((ffmpeg_rss * 10 / 1024))") }
      }
    }${comma}
EOF
    done

    cat >>"${STATUS_TMP}" <<EOF
  ],
  "system": {
    "cpuCores": ${cpu_count},
    "loadAverage1m": $(decimal_or_null "${load_average}"),
    "httpPort": 8088,
    "icePortStart": 20000,
    "icePortEnd": 20100,
    "publicIp": ${public_ip_json},
    "containerIp": ${container_ip_json},
    "services": {
      "janus": { "running": ${process_running[janus]:-false}, "cpuPercent": $(tenths "${cpu_tenths[janus]:-0}"), "memoryMiB": $(tenths "$(( ${rss_kib[janus]:-0} * 10 / 1024 ))") },
      "nginx": { "running": ${process_running[nginx]:-false}, "cpuPercent": $(tenths "${cpu_tenths[nginx]:-0}"), "memoryMiB": $(tenths "$(( ${rss_kib[nginx]:-0} * 10 / 1024 ))") }
    }
  }
}
EOF
    mv -f "${STATUS_TMP}" "${STATUS_FILE}"
    sleep 1
done
