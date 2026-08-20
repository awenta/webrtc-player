#!/command/with-contenv bash
# shellcheck shell=bash
set -Eeuo pipefail

fail() {
    echo "[srt-relay] error: $*" >&2
    exit 1
}

require_port() {
    local name="$1" value="$2"
    if [[ ! "${value}" =~ ^[0-9]+$ ]] || (( value < 1024 || value > 65535 )); then
        fail "${name} must be an integer between 1024 and 65535"
    fi
}

SRT_URL="${SRT_URL:-srt://0.0.0.0:9000?mode=listener&latency=120000}"
SRT_AUDIO="${SRT_AUDIO:-true}"
SRT_COLOR_MODE="${SRT_COLOR_MODE:-auto}"
VIDEO_PORT="${SRT_RELAY_VIDEO_PORT:-15004}"
AUDIO_PORT="${SRT_RELAY_AUDIO_PORT:-15005}"
VIDEO_RTCP_PORT="${SRT_RELAY_VIDEO_RTCP_PORT:-15006}"
AUDIO_RTCP_PORT="${SRT_RELAY_AUDIO_RTCP_PORT:-15007}"
VIDEO_BITRATE="${VIDEO_BITRATE:-6M}"
VIDEO_BUFFER_SIZE="${VIDEO_BUFFER_SIZE:-2M}"
VIDEO_PRESET="${VIDEO_PRESET:-veryfast}"
AUDIO_BITRATE="${AUDIO_BITRATE:-128k}"
VIDEO_MAP="${VIDEO_MAP:-0:v:0}"
AUDIO_MAP="${AUDIO_MAP:-0:a:0}"
MAX_WIDTH="${MAX_WIDTH:-1920}"
MAX_HEIGHT="${MAX_HEIGHT:-1080}"
MAX_FPS="${MAX_FPS:-60}"
FFMPEG_LOG_LEVEL="${FFMPEG_LOG_LEVEL:-warning}"
PROGRESS_FIFO="${CHANNEL_RUNTIME_DIR:-/run/webrtc-player}/ffmpeg-progress"

[[ "${SRT_URL}" == srt://* ]] || fail "SRT_URL must start with srt://"
[[ "${SRT_URL}" != *$'\n'* && "${SRT_URL}" != *$'\r'* ]] || fail "SRT_URL must not contain newlines"
[[ "${SRT_AUDIO}" == "true" || "${SRT_AUDIO}" == "false" ]] || fail "SRT_AUDIO must be true or false"
[[ "${SRT_COLOR_MODE}" == "auto" || "${SRT_COLOR_MODE}" == "fast" || "${SRT_COLOR_MODE}" == "hdr-to-sdr" ]] \
    || fail "SRT_COLOR_MODE must be auto, fast, or hdr-to-sdr"
[[ "${VIDEO_BITRATE}" =~ ^[1-9][0-9]*([kK]|M)?$ ]] || fail "VIDEO_BITRATE must be positive with an optional k, K, or M suffix"
[[ "${VIDEO_BUFFER_SIZE}" =~ ^[1-9][0-9]*([kK]|M)?$ ]] || fail "VIDEO_BUFFER_SIZE must be positive with an optional k, K, or M suffix"
[[ "${AUDIO_BITRATE}" =~ ^[1-9][0-9]*([kK]|M)?$ ]] || fail "AUDIO_BITRATE must be positive with an optional k, K, or M suffix"
[[ "${MAX_WIDTH}" =~ ^[0-9]+$ && "${MAX_HEIGHT}" =~ ^[0-9]+$ && "${MAX_FPS}" =~ ^[0-9]+$ ]] \
    || fail "MAX_WIDTH, MAX_HEIGHT, and MAX_FPS must be integers"
(( MAX_WIDTH >= 16 && MAX_HEIGHT >= 16 )) || fail "MAX_WIDTH and MAX_HEIGHT must be at least 16"
(( MAX_FPS >= 1 && MAX_FPS <= 60 )) || fail "MAX_FPS must be between 1 and 60"
require_port VIDEO_PORT "${VIDEO_PORT}"
require_port AUDIO_PORT "${AUDIO_PORT}"
require_port VIDEO_RTCP_PORT "${VIDEO_RTCP_PORT}"
require_port AUDIO_RTCP_PORT "${AUDIO_RTCP_PORT}"
port_count="$(printf '%s\n' "${VIDEO_PORT}" "${AUDIO_PORT}" "${VIDEO_RTCP_PORT}" "${AUDIO_RTCP_PORT}" | sort -u | wc -l)"
[[ "${port_count}" == "4" ]] || fail "RTP and RTCP ports must all be different"
[[ -p "${PROGRESS_FIFO}" ]] || fail "FFmpeg progress channel is unavailable"

EFFECTIVE_SRT_URL=${SRT_URL}
srt_query=
if [[ "${SRT_URL}" == *\?* ]]; then
    srt_query=${SRT_URL#*\?}
fi
if [[ "${SRT_URL}" == srt://0.0.0.0:* && "&${srt_query}&" == *'&mode=listener&'* ]]; then
    [[ "${INGEST_IP:-}" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]] \
        || fail "INGEST_IP is unavailable for the wildcard SRT listener"
    EFFECTIVE_SRT_URL="srt://${INGEST_IP}${SRT_URL#srt://0.0.0.0}"
fi

case "${VIDEO_PRESET}" in
    ultrafast|superfast|veryfast|faster|fast|medium|slow|slower|veryslow) ;;
    *) fail "VIDEO_PRESET is not a supported x264 preset" ;;
esac

scale_filter="scale=w='min(${MAX_WIDTH},iw)':h='min(${MAX_HEIGHT},ih)':force_original_aspect_ratio=decrease:force_divisible_by=2:flags=lanczos"
base_filter="bwdif=mode=send_frame:parity=auto:deint=interlaced,${scale_filter},setsar=1"

case "${SRT_COLOR_MODE}" in
    fast|auto)
        # This accepts 8/10-bit and 4:2:0/4:2:2/4:4:4 sources. Select the
        # explicit HDR path when conversion to BT.709 is required.
        video_filter="${base_filter},format=yuv420p"
        ;;
    hdr-to-sdr)
        video_filter="${base_filter},zscale=transfer=linear:npl=100,format=gbrpf32le,tonemap=mobius:desat=0,zscale=primaries=bt709:transfer=bt709:matrix=bt709:range=tv,format=yuv420p"
        ;;
esac

input_options=()
if [[ -n "${SRT_PASSPHRASE_FILE:-}" ]]; then
    [[ -r "${SRT_PASSPHRASE_FILE}" ]] || fail "SRT_PASSPHRASE_FILE is not readable"
    SRT_PASSPHRASE="$(<"${SRT_PASSPHRASE_FILE}")"
fi
if [[ -n "${SRT_PASSPHRASE:-}" ]]; then
    (( ${#SRT_PASSPHRASE} >= 10 && ${#SRT_PASSPHRASE} <= 79 )) \
        || fail "SRT passphrase must contain 10 to 79 characters"
    SRT_PBKEYLEN="${SRT_PBKEYLEN:-16}"
    [[ "${SRT_PBKEYLEN}" == "16" || "${SRT_PBKEYLEN}" == "24" || "${SRT_PBKEYLEN}" == "32" ]] \
        || fail "SRT_PBKEYLEN must be 16, 24, or 32"
    input_options+=( -passphrase "${SRT_PASSPHRASE}" -pbkeylen "${SRT_PBKEYLEN}" )
fi

ffmpeg_args=(
    -hide_banner
    -nostdin
    -loglevel "${FFMPEG_LOG_LEVEL}"
    -progress "${PROGRESS_FIFO}"
    -stats_period 1
    -fflags +genpts+discardcorrupt
    -flags low_delay
    -probesize "${PROBE_SIZE:-10M}"
    -analyzeduration "${ANALYZE_DURATION_US:-5000000}"
    -thread_queue_size 4096
    "${input_options[@]}"
    -i "${EFFECTIVE_SRT_URL}"
    -map "${VIDEO_MAP}"
    -vf "${video_filter}"
    -c:v libx264
    -preset "${VIDEO_PRESET}"
    -tune zerolatency
    -profile:v baseline
    -level:v 4.2
    -b:v "${VIDEO_BITRATE}"
    -maxrate "${VIDEO_BITRATE}"
    -bufsize "${VIDEO_BUFFER_SIZE}"
    -g 120
    -keyint_min 1
    -sc_threshold 0
    -force_key_frames "expr:gte(t,n_forced*2)"
    -x264-params repeat-headers=1:aud=1
    -fpsmax "${MAX_FPS}"
    -an
    -f rtp
    -payload_type 96
    "rtp://127.0.0.1:${VIDEO_PORT}?rtcpport=${VIDEO_RTCP_PORT}&pkt_size=1200"
)

if [[ "${SRT_AUDIO}" == "true" ]]; then
    ffmpeg_args+=(
        -map "${AUDIO_MAP}"
        -c:a libopus
        -application lowdelay
        -b:a "${AUDIO_BITRATE}"
        -ar 48000
        -ac 2
        -vn
        -f rtp
        -payload_type 111
        "rtp://127.0.0.1:${AUDIO_PORT}?rtcpport=${AUDIO_RTCP_PORT}&pkt_size=1200"
    )
fi

echo "[srt-relay] channel=${CHANNEL_ID:-1} starting codec-normalizing SRT relay (URL and secrets hidden)"
if [[ "${EFFECTIVE_SRT_URL}" != "${SRT_URL}" ]]; then
    echo "[srt-relay] wildcard listener resolved to ingest address ${INGEST_IP}"
fi
echo "[srt-relay] normalized output: H.264 PT=96; audio=${SRT_AUDIO}; color=${SRT_COLOR_MODE}"
exec /opt/media/bin/ffmpeg "${ffmpeg_args[@]}"
