#!/bin/bash
set -e

# If STREAM_URL is set, create a dynamic stream using FFmpeg
# This converts an RTSP/SRT/other source into RTP for Janus
if [ -n "$STREAM_URL" ]; then
  STREAM_ID="${STREAM_ID:-1}"
  VIDEO_PORT="${VIDEO_PORT:-5004}"
  AUDIO_PORT="${AUDIO_PORT:-5005}"

  echo "[entrypoint] Creating Janus stream config for id=$STREAM_ID"

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

  # Add FFmpeg as a supervised process to relay the source
  cat >> /etc/supervisor/conf.d/supervisord.conf <<EOF

[program:ffmpeg]
command=ffmpeg -re -i ${STREAM_URL} -c:v libx264 -preset ultrafast -tune zerolatency -b:v 2000k -f rtp rtp://127.0.0.1:${VIDEO_PORT} -c:a libopus -b:a 64k -f rtp rtp://127.0.0.1:${AUDIO_PORT}
autostart=true
autorestart=true
startretries=9999
startsecs=3
stdout_logfile=/dev/fd/1
stdout_logfile_maxbytes=0
stderr_logfile=/dev/fd/2
stderr_logfile_maxbytes=0
EOF

  echo "[entrypoint] Will relay $STREAM_URL → RTP ports $VIDEO_PORT/$AUDIO_PORT"
fi

exec /usr/bin/supervisord -c /etc/supervisor/conf.d/supervisord.conf
