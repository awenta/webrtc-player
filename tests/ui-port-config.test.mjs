import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const html = await readFile(new URL("../web/index.html", import.meta.url), "utf8");
const parserStart = html.indexOf("  function parseSrtUrl(value) {");
const parserEnd = html.indexOf("\n  function describeSrtHost", parserStart);
assert.notEqual(parserStart, -1, "parseSrtUrl is present");
assert.notEqual(parserEnd, -1, "parseSrtUrl boundary is present");
const parserSource = html.slice(parserStart, parserEnd);
const parseSrtUrl = new Function(
  "isPresent",
  `${parserSource}\nreturn parseSrtUrl;`
)((value) => value !== null && value !== undefined && value !== "");

test("SRT parser accepts supported endpoint forms", () => {
  assert.deepEqual(
    { ...parseSrtUrl("srt://source.example:9000") },
    { valid: true, host: "source.example", port: 9000, role: "caller", explicitMode: false, error: "" }
  );
  assert.equal(parseSrtUrl("srt://source.example:9001?mode=caller&latency=120000").role, "caller");
  assert.equal(parseSrtUrl("srt://[2001:db8::1]:9002?mode=rendezvous").role, "rendezvous");
});

test("SRT parser rejects unsafe or unsupported endpoint forms", () => {
  const invalid = [
    "http://127.0.0.1:9000",
    "srt://127.0.0.1:1023",
    "srt://127.0.0.1:09000",
    "srt://127.0.0.1:65536",
    "srt://user@127.0.0.1:9000",
    "srt://127.0.0.1:9000/path",
    "srt://127.0.0.1:9000#fragment",
    "srt://127.0.0.1:9000?",
    "srt://127.0.0.1:9000?mode=listener&&latency=1",
    "srt://127.0.0.1:9000?mode=listener&mode=caller",
    "srt://127.0.0.1:9000?mode=unsupported",
    "srt://127.0.0.1:9000?mode=listener?latency=1"
  ];
  for (const url of invalid) assert.equal(parseSrtUrl(url).valid, false, url);
});

test("channel form exposes all configurable direct ports", () => {
  for (const name of ["VIDEO_PORT", "VIDEO_RTCP_PORT", "AUDIO_PORT", "AUDIO_RTCP_PORT"]) {
    assert.match(html, new RegExp(`name="${name}"[^>]+min="1024"[^>]+max="65535"`));
  }
  assert.match(html, /result\.version !== 3/);
  assert.match(html, /Docker RTP\/RTCP mappings cannot be changed from the dashboard/);
  assert.doesNotMatch(html, /id="fixedRtpPorts"/);
});
