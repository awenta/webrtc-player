# WebRTC Player

Embeddable WebRTC video player powered by Janus Gateway. Accepts RTSP, SRT, or any FFmpeg-compatible live source and delivers it to browsers via WebRTC. Runs in a single Docker container.

## Architecture

```
RTSP/SRT source → FFmpeg → RTP → Janus Streaming Plugin → WebRTC → Browser
                                                            ↑
                                              Nginx (serves UI + proxies WS)
```

## Quick Start

### With automatic stream relay (easiest)

```bash
STREAM_URL=rtsp://camera.example.com/stream docker compose up --build
```

Open `http://localhost:8080/?stream=1` in your browser, or embed it:

```html
<iframe src="http://localhost:8080/?stream=1" allow="autoplay" style="width:640px;height:360px;border:none"></iframe>
```

### With manual FFmpeg / external RTP source

Start the container without `STREAM_URL`:

```bash
docker compose up --build
```

Then push RTP to the container from outside:

```bash
ffmpeg -re -i input.mp4 \
  -c:v libx264 -preset ultrafast -tune zerolatency \
  -f rtp rtp://localhost:5004 \
  -c:a libopus \
  -f rtp rtp://localhost:5005
```

## URL Parameters

| Param    | Default | Description                          |
|----------|---------|--------------------------------------|
| `stream` | `1`     | Janus stream ID to watch             |
| `ws`     | auto    | WebSocket URL override for Janus API |

## Environment Variables

| Variable     | Default | Description                              |
|--------------|---------|------------------------------------------|
| `STREAM_URL` | (none)  | Source URL for FFmpeg relay (RTSP, SRT…) |
| `STREAM_ID`  | `1`     | Janus stream ID to create                |
| `VIDEO_PORT` | `5004`  | RTP port for video                       |
| `AUDIO_PORT` | `5005`  | RTP port for audio                       |

## Multiple Streams

Add more streams by editing `janus/janus.plugin.streaming.jcfg` with different IDs and ports, then push RTP to each port pair. Select a stream in the player with `?stream=<id>`.

## NAT / Public Deployment

If the container runs behind NAT, uncomment and set `nat_1_1_mapping` in `janus/janus.jcfg` to your public IP. You may also need `network_mode: host` in the compose file.

## Ports

- `8080` — HTTP (web player + WebSocket proxy)
- `20000-20100/udp` — RTP range for incoming media
