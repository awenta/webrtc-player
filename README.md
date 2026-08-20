# WebRTC Player

An embeddable, low-latency WebRTC player powered by Janus Gateway. It accepts either direct H.264/Opus RTP or an SRT MPEG-TS contribution feed, then serves the stream to browsers through WebRTC from one container.

```text
SRT (H.264/HEVC + common audio) -> FFmpeg -> H.264/Opus RTP -+
Direct H.264/Opus RTP ----------------------------------------+-> Janus -> WebRTC -> Browser
                                                                  ^
                                            Nginx player UI + WebSocket proxy
```

## Components

The image uses reviewed, immutable upstream revisions rather than floating branches. Ubuntu packages use a dated snapshot after the minimal CA-certificate bootstrap needed to reach that HTTPS snapshot.

| Component | Pinned version |
|---|---|
| Ubuntu | 26.04 LTS image digest and 2026-08-20 package snapshot |
| Janus Gateway | 1.4.1 |
| FFmpeg | 9.0.1 |
| Haivision SRT | 1.5.6 |
| libSRTP | 2.8.0 |
| Nginx mainline | 1.31.4 |
| s6-overlay | 3.2.3.2 |
| x264 | Immutable commit in `Dockerfile` |

The runtime image contains no compiler or source tree. Janus, Nginx, and the optional SRT relay run as the unprivileged `player` user under s6 supervision.

## Quick Start

Build and start with automatic RTP/SRT input selection:

```bash
docker compose up --build
```

Open <http://localhost:8088/?stream=1>, or embed it:

```html
<iframe src="http://localhost:8088/?stream=1" allow="autoplay" style="width:640px;height:360px;border:none"></iframe>
```

The player begins muted to satisfy browser autoplay rules. Its native controls can enable audio.

## Automatic Input Selection

`INPUT_MODE=auto` is the default. The container listens for supported direct RTP and SRT at the same time, validates incoming RTP, and locks onto the first source that delivers a valid H.264 video sequence. The selected input is forwarded to the existing Janus WebRTC output; the WebRTC stream ID and browser workflow do not change.

The selector keeps the active source until its video packets stop for `INPUT_TIMEOUT_MS` (default 5000 ms), then returns to waiting for either input. Simultaneous sources are never mixed. Use `INPUT_MODE=rtp` or `INPUT_MODE=srt` only when an operator needs to restrict accepted inputs.

Automatic mode accepts:

- Direct H.264 RTP PT 96 and Opus RTP PT 111 on the documented RTP/RTCP ports
- SRT MPEG-TS callers on UDP 9000, with H.264/HEVC and supported audio normalized by FFmpeg
- An SRT caller URL configured through `SRT_URL`; the URL supplies the destination that cannot be discovered automatically

When an SRT passphrase or passphrase file is configured, automatic mode disables unauthenticated direct RTP so it cannot bypass SRT encryption.

## Monitoring UI

The root page is a read-only signal monitor designed for both desktop and mobile. It shows only data available from the running application:

- Input mode, transport state, listener/caller address, and RTP/RTCP ports
- SRT transcoder frame rate, encoded bitrate, processing speed, and application CPU/memory use
- Janus, FFmpeg, and Nginx process state plus system load and monitor uptime
- This browser's WebRTC resolution, frame rate, video/audio receive bitrate, codec, packet loss, jitter, and selected ICE path

Runtime status is refreshed from `GET /api/status`; browser receive statistics come directly from `RTCPeerConnection.getStats()`. Direct RTP input bitrate and sender details are not displayed because Janus does not currently expose those values to the monitor. The page intentionally has no configuration or administrative controls.

## SRT Listener

The default automatic mode already runs an SRT listener on UDP port 9000:

```bash
docker compose up --build
```

Send MPEG-TS from a source to:

```text
srt://<container-host>:9000?mode=caller&latency=120000
```

The container's matching default input is:

```text
srt://0.0.0.0:9000?mode=listener&latency=120000
```

## SRT Caller

Make the container connect to a remote SRT listener:

```bash
SRT_URL='srt://source.example.net:9000?mode=caller&latency=120000' \
docker compose up --build
```

On PowerShell:

```powershell
$env:SRT_URL = 'srt://source.example.net:9000?mode=caller&latency=120000'
docker compose up --build
```

FFmpeg exits when an SRT connection ends; s6 restarts it with a short delay so listener and caller modes both reconnect.

## Supported SRT Media

SRT is expected to carry MPEG-TS with one selected video track and, by default, one selected audio track.

Video input includes:

- H.264/AVC and HEVC/H.265 Main/Main10
- 8-bit and 10-bit input
- Common 4:2:0, 4:2:2, and 4:4:4 pixel formats supported by FFmpeg
- Progressive and correctly flagged interlaced content
- SDR content by default, with an explicit HDR/HLG/PQ-to-SDR mode

Common audio decoders include AAC, Opus, MP2/MP3, AC-3, E-AC-3, and PCM. Actual acceptance depends on the demuxer and decoders included in FFmpeg 9.0.1.

All SRT video is decoded and normalized to constrained-baseline H.264 Level 4.2, 8-bit 4:2:0 at no more than 60 fps. Audio is normalized to stereo Opus at 48 kHz. This is required because HEVC is not broadly interoperable in browser WebRTC and Janus does not transcode.

The default output ceiling is 1920x1080. CPU-only HEVC-to-H.264 at 1080p60 requires a suitably provisioned modern CPU; measure the `speed=` value in FFmpeg logs on the deployment hardware. Values below `1.0x` indicate the host cannot sustain real time.

### Color Modes

| `SRT_COLOR_MODE` | Behavior |
|---|---|
| `auto` | Default. Accepts common bit depths/chroma and converts to 8-bit 4:2:0 without expensive tone mapping. HDR color appearance may not be preserved. |
| `fast` | Same low-cost conversion path, intended for known SDR sources. |
| `hdr-to-sdr` | CPU-intensive zscale/Mobius conversion from HDR, HLG, or PQ to BT.709 SDR. |

Use `hdr-to-sdr` only when required and benchmark it at the target frame rate.

### Video-Only SRT

Set `SRT_AUDIO=false` if the transport stream has no audio track:

```bash
SRT_AUDIO=false docker compose up
```

When audio is enabled, a missing selected audio stream is treated as an input error and the relay retries. Select alternate tracks with `VIDEO_MAP` and `AUDIO_MAP`, for example `AUDIO_MAP=0:a:1`.

## Direct RTP Mode

Automatic mode listens for direct RTP by default. Send:

| Media | Destination | RTP payload |
|---|---|---|
| H.264 video | `rtp://<host>:5004` | PT 96, 90 kHz |
| Opus audio | `rtp://<host>:5005` | PT 111, 48 kHz stereo |

Optional RTCP sender reports use UDP 5006 for video and 5007 for audio. These ports are separate so video RTCP cannot collide with audio RTP.

Set `AUDIO_ENABLED=false` for a video-only direct RTP mountpoint.

If direct RTP and SRT arrive together, the first validated video source is selected and the other is ignored until the active input times out.

## Configuration

| Variable | Default | Description |
|---|---|---|
| `INPUT_MODE` | `auto` | Automatic RTP/SRT selection, or the `rtp`/`srt` restriction overrides |
| `INPUT_TIMEOUT_MS` | `5000` | Video silence before automatic mode releases the selected input |
| `STREAM_ID` | `1` | Positive Janus mountpoint ID |
| `VIDEO_PORT` | `5004` | Internal/direct H.264 RTP port |
| `AUDIO_PORT` | `5005` | Internal/direct Opus RTP port |
| `VIDEO_RTCP_PORT` | `5006` | Internal/direct H.264 RTCP port |
| `AUDIO_RTCP_PORT` | `5007` | Internal/direct Opus RTCP port |
| `AUDIO_ENABLED` | `true` | Enable audio in direct RTP mode |
| `PUBLIC_IP` | unset | IP advertised by Janus for one-to-one NAT |
| `SRT_URL` | listener on `0.0.0.0:9000` | Complete listener or caller URL |
| `SRT_AUDIO` | `true` | Expect and transcode an SRT audio stream |
| `SRT_COLOR_MODE` | `auto` | `auto`, `fast`, or `hdr-to-sdr` |
| `VIDEO_BITRATE` | `6M` | H.264 target and maximum bitrate |
| `VIDEO_PRESET` | `veryfast` | x264 CPU/quality preset |
| `AUDIO_BITRATE` | `128k` | Opus bitrate |
| `MAX_WIDTH` | `1920` | Maximum output width |
| `MAX_HEIGHT` | `1080` | Maximum output height |
| `MAX_FPS` | `60` | Maximum output frame rate, from 1 through 60 |
| `VIDEO_MAP` | `0:v:0` | FFmpeg video stream selector |
| `AUDIO_MAP` | `0:a:0` | FFmpeg audio stream selector |
| `FFMPEG_LOG_LEVEL` | `warning` | FFmpeg logging level |
| `JANUS_ADMIN_KEY` | generated | Optional persistent Streaming plugin management key |
| `STREAM_SECRET` | generated | Optional persistent mountpoint edit/destroy secret |

`SRT_PORT` only controls the Docker Compose host-to-container port publication. Keep it consistent with the listener port in `SRT_URL`.

## SRT Encryption

Prefer a Docker secret over embedding a passphrase in `SRT_URL`, which can expose it in logs and deployment metadata:

```yaml
services:
  webrtc-player:
    environment:
      SRT_PASSPHRASE_FILE: /run/secrets/srt_passphrase
      SRT_PBKEYLEN: 32
    secrets:
      - srt_passphrase

secrets:
  srt_passphrase:
    file: ./srt-passphrase.txt
```

Valid passphrases contain 10 to 79 characters. Valid `SRT_PBKEYLEN` values are 16, 24, and 32.

FFmpeg requires the passphrase as a protocol option, so a privileged host operator or a process inspector inside the container can still read it from FFmpeg's command line. The secret mount keeps it out of Compose metadata and normal logs; restrict Docker and container-exec access accordingly. Passphrase files matching `*passphrase*.txt` are excluded from Git and the Docker build context by default.

## Janus Management Protection

Viewing a stream requires no browser-visible secret. At each startup, the container generates a random Streaming plugin admin key and mountpoint secret to prevent public WebSocket clients from creating, editing, or destroying mountpoints. Set `JANUS_ADMIN_KEY` and `STREAM_SECRET` explicitly only when an external trusted management client needs stable credentials; supported characters are letters, digits, `.`, `_`, and `-`.

Janus always receives media from the loopback input selector. With encrypted SRT, automatic mode disables the selector's external RTP listeners so published RTP ports cannot bypass authentication.

## NAT and Public Deployment

`PUBLIC_IP` must be the IP browsers use to reach this host. Leave it unset only when Janus interface candidates are directly reachable. For remote or cloud deployments:

```bash
PUBLIC_IP=203.0.113.10 docker compose up
```

Allow these ports through the host firewall:

- `8088/tcp`: player and Janus WebSocket proxy
- `9000/udp`: default SRT listener
- `5004-5007/udp`: direct RTP/RTCP mode
- `20000-20100/udp`: Janus WebRTC ICE/DTLS/SRTP

Host networking can simplify ICE on Linux. Remove Compose port publishing when enabling `network_mode: host`.

Terminate TLS in a trusted reverse proxy for production so the player is served over HTTPS/WSS.

## Operations

The image health check verifies that s6 reports the input selector, Janus, Nginx, and status collector as running, verifies the SRT relay when enabled, and requests Nginx `/healthz`. A disconnected SRT listener remains healthy because automatic mode is ready to accept either supported input; the selected source is shown in the monitoring UI.

Useful commands:

```bash
docker compose ps
docker compose logs -f webrtc-player
docker compose exec webrtc-player /opt/media/bin/ffmpeg -version
docker compose exec webrtc-player /opt/janus/bin/janus --version
```

## Updating Dependencies

Versions, source commits, image digests, checksums, and the post-bootstrap Ubuntu package snapshot are deliberately pinned in `Dockerfile`. Update them manually as one reviewed change, rebuild without cache, and repeat the H.264/HEVC listener/caller playback tests before release.

The FFmpeg build enables GPL-licensed x264. Distributors are responsible for satisfying applicable open-source and codec patent/licensing obligations.
