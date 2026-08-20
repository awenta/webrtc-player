#!/command/with-contenv bash
# shellcheck shell=bash
set -Eeuo pipefail

readonly STATUS_FILE=/run/webrtc-player/status.json
readonly STATUS_TMP=/run/webrtc-player/status.json.tmp
readonly PROGRESS_FIFO=/run/webrtc-player/ffmpeg-progress
readonly SELECTOR_STATUS=/run/webrtc-player/input-selector.status

INPUT_MODE="${INPUT_MODE:-auto}"
STREAM_ID="${STREAM_ID:-1}"
VIDEO_PORT="${VIDEO_PORT:-5004}"
AUDIO_PORT="${AUDIO_PORT:-5005}"
VIDEO_RTCP_PORT="${VIDEO_RTCP_PORT:-5006}"
AUDIO_RTCP_PORT="${AUDIO_RTCP_PORT:-5007}"
AUDIO_ENABLED="${AUDIO_ENABLED:-true}"
SRT_AUDIO="${SRT_AUDIO:-true}"
PUBLIC_IP="${PUBLIC_IP:-}"

started_at="$(date +%s)"
cpu_count="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')"
page_size="$(getconf PAGESIZE 2>/dev/null || printf '4096')"
page_kib=$((page_size / 1024))
container_ip=""
read -r container_ip _ < <(getent ahostsv4 "${HOSTNAME:-localhost}" 2>/dev/null || true)

srt_host="0.0.0.0"
srt_port=""
srt_role="listener"
if [[ "${INPUT_MODE}" != "rtp" ]]; then
    srt_target="${SRT_URL#srt://}"
    srt_target="${srt_target%%\?*}"
    srt_target="${srt_target%%/*}"
    if [[ "${srt_target}" == \[* ]]; then
        srt_host="${srt_target#\[}"
        srt_host="${srt_host%%\]*}"
        srt_port="${srt_target##*]:}"
    elif [[ "${srt_target}" == *:* ]]; then
        srt_host="${srt_target%:*}"
        srt_port="${srt_target##*:}"
    else
        srt_host="${srt_target}"
    fi
    if [[ "${SRT_URL}" == *"mode=caller"* ]]; then
        srt_role="caller"
    elif [[ "${SRT_URL}" == *"mode=rendezvous"* ]]; then
        srt_role="rendezvous"
    fi
fi

json_quote() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value//\"/\\\"}"
    value="${value//$'\n'/\\n}"
    value="${value//$'\r'/\\r}"
    value="${value//$'\t'/\\t}"
    printf '"%s"' "${value}"
}

decimal_or_null() {
    local value="$1"
    if [[ "${value}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        printf '%s' "${value}"
    else
        printf 'null'
    fi
}

tenths() {
    local value="$1"
    printf '%d.%d' "$((value / 10))" "$((value % 10))"
}

declare -A previous_jiffies=( [selector]=0 [janus]=0 [ffmpeg]=0 [nginx]=0 )
declare -A progress=()
previous_total=0

if [[ "${INPUT_MODE}" != "rtp" ]]; then
    exec 3<>"${PROGRESS_FIFO}"
fi

while true; do
    now="$(date +%s)"

    if [[ "${INPUT_MODE}" != "rtp" ]]; then
        while IFS= read -r -t 0.01 progress_line <&3; do
            [[ "${progress_line}" == *=* ]] || continue
            progress_key="${progress_line%%=*}"
            progress_value="${progress_line#*=}"
            case "${progress_key}" in
                frame|fps|bitrate|out_time|speed|dup_frames|drop_frames|progress)
                    progress["${progress_key}"]="${progress_value}"
                    ;;
            esac
        done
    fi

    read -r _ cpu_user cpu_nice cpu_system cpu_idle cpu_iowait cpu_irq cpu_softirq cpu_steal _ < /proc/stat
    total_jiffies=$((cpu_user + cpu_nice + cpu_system + cpu_idle + cpu_iowait + cpu_irq + cpu_softirq + cpu_steal))

    declare -A current_jiffies=( [selector]=0 [janus]=0 [ffmpeg]=0 [nginx]=0 )
    declare -A rss_kib=( [selector]=0 [janus]=0 [ffmpeg]=0 [nginx]=0 )
    declare -A process_count=( [selector]=0 [janus]=0 [ffmpeg]=0 [nginx]=0 )

    for proc_dir in /proc/[0-9]*; do
        [[ -r "${proc_dir}/comm" && -r "${proc_dir}/stat" ]] || continue
        IFS= read -r process_name <"${proc_dir}/comm" || continue
        case "${process_name}" in
            input-selector) process_group=selector ;;
            janus) process_group=janus ;;
            ffmpeg) process_group=ffmpeg ;;
            nginx) process_group=nginx ;;
            *) continue ;;
        esac

        read -r -a process_stat <"${proc_dir}/stat" || continue
        (( current_jiffies[${process_group}] += process_stat[13] + process_stat[14] )) || true
        (( rss_kib[${process_group}] += process_stat[23] * page_kib )) || true
        (( process_count[${process_group}] += 1 )) || true
    done

    declare -A cpu_tenths=( [selector]=0 [janus]=0 [ffmpeg]=0 [nginx]=0 )
    if (( previous_total > 0 && total_jiffies > previous_total )); then
        total_delta=$((total_jiffies - previous_total))
        for process_group in selector janus ffmpeg nginx; do
            process_delta=$((current_jiffies[${process_group}] - previous_jiffies[${process_group}]))
            (( process_delta < 0 )) && process_delta=0
            cpu_tenths["${process_group}"]=$((process_delta * cpu_count * 1000 / total_delta))
        done
    fi
    previous_total="${total_jiffies}"
    for process_group in selector janus ffmpeg nginx; do
        previous_jiffies["${process_group}"]="${current_jiffies[${process_group}]}"
    done

    selector_running=false
    janus_running=false
    nginx_running=false
    ffmpeg_running=false
    (( process_count[selector] > 0 )) && selector_running=true
    (( process_count[janus] > 0 )) && janus_running=true
    (( process_count[nginx] > 0 )) && nginx_running=true
    (( process_count[ffmpeg] > 0 )) && ffmpeg_running=true

    selected_source=none
    direct_enabled=false
    srt_enabled=false
    remote_address=""
    remote_port=""
    if [[ -r "${SELECTOR_STATUS}" ]]; then
        while IFS='=' read -r selector_key selector_value; do
            case "${selector_key}" in
                source) selected_source="${selector_value}" ;;
                direct_enabled) direct_enabled="${selector_value}" ;;
                srt_enabled) srt_enabled="${selector_value}" ;;
                remote_address) remote_address="${selector_value}" ;;
                remote_port) remote_port="${selector_value}" ;;
            esac
        done <"${SELECTOR_STATUS}"
    fi

    input_state=waiting
    [[ "${selected_source}" != "none" ]] && input_state=active
    input_transport="RTP + SRT"
    input_role="automatic"
    input_host="0.0.0.0"
    input_port=""
    input_audio_enabled="${AUDIO_ENABLED}"
    processing_mode=standby
    if [[ "${selected_source}" == "rtp" || "${INPUT_MODE}" == "rtp" ]]; then
        input_transport=RTP
        input_role=receiver
        input_port="${VIDEO_PORT}"
        processing_mode=passthrough
    fi
    if [[ "${selected_source}" == "srt" || "${INPUT_MODE}" == "srt" ]]; then
        input_transport="SRT / MPEG-TS"
        input_role="${srt_role}"
        input_host="${srt_host}"
        input_port="${srt_port}"
        input_audio_enabled="${SRT_AUDIO}"
        processing_mode=transcode
    fi

    progress_active=false
    [[ "${selected_source}" == "srt" && "${input_state}" == "active" ]] && progress_active=true
    progress_frame=null
    progress_fps=null
    progress_bitrate=null
    progress_speed=null
    progress_out_time=null
    progress_dropped=null
    if [[ "${progress_active}" == "true" ]]; then
        progress_frame="$(decimal_or_null "${progress[frame]:-}")"
        progress_fps="$(decimal_or_null "${progress[fps]:-}")"
        progress_bitrate="$(json_quote "${progress[bitrate]:-}")"
        progress_speed="$(json_quote "${progress[speed]:-}")"
        progress_out_time="$(json_quote "${progress[out_time]:-}")"
        progress_dropped="$(decimal_or_null "${progress[drop_frames]:-}")"
    fi

    total_cpu_tenths=$((cpu_tenths[selector] + cpu_tenths[janus] + cpu_tenths[ffmpeg] + cpu_tenths[nginx]))
    total_rss_kib=$((rss_kib[selector] + rss_kib[janus] + rss_kib[ffmpeg] + rss_kib[nginx]))
    read -r load_average _ < /proc/loadavg

    public_ip_json=null
    [[ -n "${PUBLIC_IP}" ]] && public_ip_json="$(json_quote "${PUBLIC_IP}")"
    container_ip_json=null
    [[ -n "${container_ip}" ]] && container_ip_json="$(json_quote "${container_ip}")"
    remote_address_json=null
    [[ -n "${remote_address}" ]] && remote_address_json="$(json_quote "${remote_address}")"
    remote_port_json=null
    [[ "${remote_port}" =~ ^[0-9]+$ && "${remote_port}" != "0" ]] && remote_port_json="${remote_port}"

    cat >"${STATUS_TMP}" <<EOF
{
  "version": 1,
  "timestamp": ${now},
  "uptimeSeconds": $((now - started_at)),
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
    "srtAddress": $(json_quote "${srt_host}"),
    "srtPort": $(decimal_or_null "${srt_port}"),
    "videoRtpPort": ${VIDEO_PORT},
    "audioRtpPort": ${AUDIO_PORT},
    "videoRtcpPort": ${VIDEO_RTCP_PORT},
    "audioRtcpPort": ${AUDIO_RTCP_PORT},
    "audioEnabled": ${input_audio_enabled}
  },
  "processing": {
    "mode": $(json_quote "${processing_mode}"),
    "active": ${progress_active},
    "frame": ${progress_frame},
    "fps": ${progress_fps},
    "bitrate": ${progress_bitrate},
    "speed": ${progress_speed},
    "elapsed": ${progress_out_time},
    "droppedFrames": ${progress_dropped},
    "cpuPercent": $(tenths "${total_cpu_tenths}"),
    "memoryMiB": $(tenths "$((total_rss_kib * 10 / 1024))")
  },
  "output": {
    "transport": "WebRTC",
    "streamId": ${STREAM_ID},
    "httpPort": 8088,
    "icePortStart": 20000,
    "icePortEnd": 20100,
    "publicIp": ${public_ip_json},
    "containerIp": ${container_ip_json}
  },
  "system": {
    "cpuCores": ${cpu_count},
    "loadAverage1m": $(decimal_or_null "${load_average}"),
    "services": {
      "selector": { "running": ${selector_running}, "cpuPercent": $(tenths "${cpu_tenths[selector]}"), "memoryMiB": $(tenths "$((rss_kib[selector] * 10 / 1024))") },
      "janus": { "running": ${janus_running}, "cpuPercent": $(tenths "${cpu_tenths[janus]}"), "memoryMiB": $(tenths "$((rss_kib[janus] * 10 / 1024))") },
      "ffmpeg": { "running": ${ffmpeg_running}, "cpuPercent": $(tenths "${cpu_tenths[ffmpeg]}"), "memoryMiB": $(tenths "$((rss_kib[ffmpeg] * 10 / 1024))") },
      "nginx": { "running": ${nginx_running}, "cpuPercent": $(tenths "${cpu_tenths[nginx]}"), "memoryMiB": $(tenths "$((rss_kib[nginx] * 10 / 1024))") }
    }
  }
}
EOF
    mv -f "${STATUS_TMP}" "${STATUS_FILE}"
    sleep 1
done
