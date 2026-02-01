# WebRTC Player

Embeddable WebRTC video player powered by Janus Gateway. Your source app publishes WebRTC to the Janus VideoRoom, and browsers subscribe to watch. Runs in a single Docker container.

## Architecture

```
Source App (WebRTC Tx) → Janus VideoRoom Plugin → WebRTC → Browser(s)
                              ↑
                Nginx (serves player UI + proxies Janus API/WS)
```

## Quick Start

```bash
docker compose up --build
```

### Configure your source app

In your source application, set:

| Field                  | Value                                    |
|------------------------|------------------------------------------|
| **Publish Point URL**  | `http://<host>:8088`                     |
| **Stream URL Identifier** | `1` (the VideoRoom room ID)           |

The source app connects to the Janus HTTP API at `http://<host>:8088/janus` and publishes WebRTC into room `1`.

### Watch the stream

Open `http://localhost:8088/?room=1` in a browser, or embed it:

```html
<iframe src="http://localhost:8088/?room=1" allow="autoplay" style="width:640px;height:360px;border:none"></iframe>
```

## URL Parameters (player)

| Param  | Default | Description                          |
|--------|---------|--------------------------------------|
| `room` | `1`     | Janus VideoRoom ID to subscribe to   |
| `ws`   | auto    | WebSocket URL override for Janus API |

## Environment Variables

| Variable          | Default          | Description                        |
|-------------------|------------------|------------------------------------|
| `ROOM_ID`         | (uses config)    | VideoRoom ID to create at startup  |
| `ROOM_DESC`       | `Live Stream N`  | Room description                   |
| `ROOM_PUBLISHERS` | `1`              | Max publishers in the room         |
| `ROOM_BITRATE`    | `4000000`        | Max video bitrate (bps)            |

## NAT / Public Deployment

If the container runs behind NAT, uncomment and set `nat_1_1_mapping` in `janus/janus.jcfg` to your public IP. You may also need `network_mode: host` in the compose file.

## Ports

- `8088` — HTTP (web player + Janus REST API + WebSocket proxy)
- `20000-20100/udp` — WebRTC media (ICE/DTLS)
