import assert from "node:assert/strict";

const baseUrl = process.env.WEBRTC_PLAYER_TEST_URL || "http://127.0.0.1:18088";
const expectedMode = process.env.WEBRTC_PLAYER_TEST_MODE || "host";

async function request(path, options) {
  const response = await fetch(baseUrl + path, options);
  let body;
  try {
    body = await response.json();
  } catch {
    body = null;
  }
  return { response, body };
}

async function waitForConfig() {
  for (let attempt = 0; attempt < 60; attempt++) {
    try {
      const result = await request("/api/config");
      if (result.response.ok) return result.body;
    } catch {
      // Container startup is still in progress.
    }
    await new Promise((resolve) => setTimeout(resolve, 1000));
  }
  throw new Error("configuration API did not become ready");
}

function channelRequest(channel, overrides = {}) {
  const values = { ...channel.values, ...overrides };
  const body = new URLSearchParams();
  body.set("scope", "channel");
  body.set("CHANNEL_ID", String(channel.id));
  body.set("CHANNEL_NAME", String(overrides.CHANNEL_NAME ?? channel.name));
  body.set("CHANNEL_ENABLED", String(overrides.CHANNEL_ENABLED ?? channel.enabled));
  for (const name of [
    "INPUT_MODE", "SRT_URL", "SRT_AUDIO", "AUDIO_ENABLED", "VIDEO_PORT", "AUDIO_PORT",
    "VIDEO_RTCP_PORT", "AUDIO_RTCP_PORT", "SRT_COLOR_MODE", "VIDEO_PRESET", "VIDEO_BITRATE",
    "VIDEO_BUFFER_SIZE", "AUDIO_BITRATE", "MAX_WIDTH", "MAX_HEIGHT", "MAX_FPS",
    "INPUT_TIMEOUT_MS", "SRT_PBKEYLEN"
  ]) body.set(name, String(values[name]));
  body.set("passphraseAction", "keep");
  body.set("SRT_PASSPHRASE", "");
  return body;
}

async function postChannel(channel, overrides, expectedStatus = 200) {
  const result = await request("/api/config", {
    method: "POST",
    headers: {
      "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8",
      "X-Requested-With": "WebRTC-Player"
    },
    body: channelRequest(channel, overrides)
  });
  assert.equal(result.response.status, expectedStatus, JSON.stringify(result.body));
  return result.body;
}

const initial = await waitForConfig();
assert.equal(initial.version, 3);
assert.equal(initial.network.mode, expectedMode);

const channel1 = initial.channels.find((channel) => channel.id === 1);
if (expectedMode === "bridge") {
  const directRejection = await postChannel(channel1, { VIDEO_PORT: "6100" }, 400);
  assert.match(directRejection.error, /NETWORK_MODE=bridge/);
  await postChannel(channel1, { SRT_URL: "srt://source.example:9100?mode=caller" });
} else {
  await postChannel(channel1, {
    VIDEO_PORT: "6100",
    AUDIO_PORT: "6101",
    VIDEO_RTCP_PORT: "6102",
    AUDIO_RTCP_PORT: "6103",
    SRT_URL: "srt://0.0.0.0:9100?mode=listener&latency=120000"
  });

  const updated = await waitForConfig();
  const updatedChannel1 = updated.channels.find((channel) => channel.id === 1);
  assert.equal(updatedChannel1.values.VIDEO_PORT, "6100");
  assert.equal(updatedChannel1.values.AUDIO_RTCP_PORT, "6103");
  assert.equal(updatedChannel1.values.SRT_URL, "srt://0.0.0.0:9100?mode=listener&latency=120000");

  for (let attempt = 0; attempt < 10; attempt++) {
    const status = await request("/api/status");
    if (status.response.ok) {
      const statusChannel = status.body.channels.find((channel) => channel.id === 1);
      if (statusChannel.input.videoRtpPort === 6100 && statusChannel.input.srtPort === 9100) break;
    }
    if (attempt === 9) throw new Error("runtime status did not report the configured ports");
    await new Promise((resolve) => setTimeout(resolve, 500));
  }

  const channel2 = updated.channels.find((channel) => channel.id === 2);
  const conflict = await postChannel(channel2, { VIDEO_PORT: "6100" }, 400);
  assert.match(conflict.error, /UDP port 6100 conflicts/);

  await postChannel(channel2, { CHANNEL_ENABLED: "false", SRT_URL: "srt://legacy-invalid" });
  const legacyConfig = await waitForConfig();
  const legacyChannel = legacyConfig.channels.find((channel) => channel.id === 2);
  await postChannel(legacyChannel, { CHANNEL_NAME: "Disabled legacy channel" });
  const legacyStatus = await request("/api/status");
  assert.equal(legacyStatus.response.status, 200, JSON.stringify(legacyStatus.body));

  if (process.env.WEBRTC_PLAYER_TEST_OCCUPIED_PORT === "true") {
    const rollback = await postChannel(updatedChannel1, {
      VIDEO_PORT: "6200",
      AUDIO_PORT: "6201",
      VIDEO_RTCP_PORT: "6202",
      AUDIO_RTCP_PORT: "6203"
    }, 500);
    assert.match(rollback.error, /previous settings were restored and reapplied/);
    const restored = await waitForConfig();
    const restoredChannel1 = restored.channels.find((channel) => channel.id === 1);
    assert.equal(restoredChannel1.values.VIDEO_PORT, "6100");
    assert.equal(restoredChannel1.values.AUDIO_RTCP_PORT, "6103");
  }
}

console.log("container port configuration tests passed");
