#!/bin/bash
set -e

# Allow overriding stream config via environment variables
STREAM_ID="${STREAM_ID:-1}"
VIDEO_PORT="${VIDEO_PORT:-5004}"
AUDIO_PORT="${AUDIO_PORT:-5005}"

# Configure NAT mapping so ICE candidates use the correct external IP
if [ -n "$PUBLIC_IP" ]; then
  echo "[entrypoint] Setting nat_1_1_mapping = $PUBLIC_IP"
  sed -i "s|# nat_1_1_mapping = \"1.2.3.4\"|nat_1_1_mapping = \"$PUBLIC_IP\"|" /etc/janus/janus.jcfg
else
  # Auto-detect: use the container's default route IP (works for local Docker)
  DETECTED_IP=$(hostname -I | awk '{print $1}')
  echo "[entrypoint] No PUBLIC_IP set, using detected IP: $DETECTED_IP"
  sed -i "s|# nat_1_1_mapping = \"1.2.3.4\"|nat_1_1_mapping = \"$DETECTED_IP\"|" /etc/janus/janus.jcfg
fi

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
