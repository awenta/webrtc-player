#!/bin/bash
set -e

# Create a VideoRoom dynamically if ROOM_ID is set
if [ -n "$ROOM_ID" ]; then
  ROOM_DESC="${ROOM_DESC:-Live Stream $ROOM_ID}"
  ROOM_PUBLISHERS="${ROOM_PUBLISHERS:-1}"
  ROOM_BITRATE="${ROOM_BITRATE:-4000000}"

  echo "[entrypoint] Creating VideoRoom id=$ROOM_ID desc=\"$ROOM_DESC\""

  cat > /etc/janus/janus.plugin.videoroom.jcfg <<EOF
general: {
    admin_key = "janusadmin"
}

room-${ROOM_ID}: {
    room = ${ROOM_ID}
    description = "${ROOM_DESC}"
    publishers = ${ROOM_PUBLISHERS}
    bitrate = ${ROOM_BITRATE}
    fir_freq = 10
    videocodec = "h264"
    audiocodec = "opus"
    record = false
    notify_joining = true
}
EOF
fi

exec /usr/bin/supervisord -c /etc/supervisor/conf.d/supervisord.conf
