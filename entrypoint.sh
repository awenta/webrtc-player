#!/bin/bash
set -e

# Allow overriding stream config via environment variables
STREAM_ID="${STREAM_ID:-1}"
VIDEO_PORT="${VIDEO_PORT:-5004}"
AUDIO_PORT="${AUDIO_PORT:-5005}"

cat > /etc/janus/janus.plugin.streaming.jcfg <<EOF
stream-${STREAM_ID}: {
    type = "rtp"
    id = ${STREAM_ID}
    description = "Live Stream ${STREAM_ID}"
    audio = true
    video = true
    videoport = ${VIDEO_PORT}
    videopt = 96
    videortpmap = "H264/90000"
    videofmtp = "profile-level-id=42e01f;packetization-mode=1"
    audioport = ${AUDIO_PORT}
    audiopt = 111
    audiortpmap = "opus/48000/2"
}
EOF

echo "[entrypoint] Janus Streaming: id=$STREAM_ID video=rtp://0.0.0.0:$VIDEO_PORT audio=rtp://0.0.0.0:$AUDIO_PORT"

exec /usr/bin/supervisord -c /etc/supervisor/conf.d/supervisord.conf
