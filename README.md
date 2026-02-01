# WebRTC Player

Embeddable WebRTC video player powered by Janus Gateway. Your source app sends RTP to the Janus Streaming plugin, and browsers watch via WebRTC. Runs in a single Docker container.

## Architecture

```
Source App → RTP → Janus Streaming Plugin → WebRTC → Browser(s)
                        ↑
          Nginx (serves player UI + proxies Janus WS)
```

## Quick Start

```bash
docker compose up --build
```

### Configure your source app

In your source application's Senders tab, set the Tx Publish Point URL:

| Field                  | Value                                     |
|------------------------|-------------------------------------------|
| **Video RTP**          | `rtp://<host>:5004`                       |
| **Audio RTP**          | `rtp://<host>:5005`                       |

The source sends H.264 video RTP to port `5004` and Opus audio RTP to port `5005`.

### Watch the stream

Open `http://localhost:8088/?stream=1` in a browser, or embed it:

```html
<iframe src="http://localhost:8088/?stream=1" allow="autoplay" style="width:640px;height:360px;border:none"></iframe>
```

## URL Parameters (player)

| Param    | Default | Description                          |
|----------|---------|--------------------------------------|
| `stream` | `1`     | Janus stream ID to watch             |
| `ws`     | auto    | WebSocket URL override for Janus API |

## Environment Variables

| Variable     | Default      | Description                                          |
|--------------|--------------|------------------------------------------------------|
| `STREAM_ID`  | `1`          | Janus stream ID                                      |
| `VIDEO_PORT` | `5004`       | RTP port for video (H.264)                           |
| `AUDIO_PORT` | `5005`       | RTP port for audio (Opus)                            |
| `PUBLIC_IP`  | auto-detect  | IP the browser uses to reach this host (for ICE/NAT) |

## NAT / Public Deployment

The `PUBLIC_IP` environment variable tells Janus what IP to advertise in ICE candidates. If not set, it auto-detects the container's IP (works for local Docker). For remote/cloud deployments, set it explicitly:

```bash
PUBLIC_IP=203.0.113.10 docker compose up --build
```

Alternatively, use `network_mode: host` in the compose file to avoid Docker NAT entirely.

## Ports

- `8088` — HTTP (web player + WebSocket proxy)
- `5004/udp` — RTP video ingest
- `5005/udp` — RTP audio ingest
- `20000-20100/udp` — WebRTC media (ICE/DTLS)
