# Missing SDP offer: report E42JF8

Investigated October 27, 2026, using the supplied Kasumi 1.1.1 build 160 report,
the local sources, and a fresh launch in the user's Zen browser with Network
Monitor open. No credentials, real session identifiers, IP addresses, full SDP,
or raw traffic capture are retained in this document or the test fixtures.

## Evidence

Four fresh Fortnite launches reach a ready CloudMatch session. Each completes
TLS and the WebSocket upgrade, receives its own peer assignment, acknowledgements
and heartbeats, but recognizes no SDP offer before the 30-second deadline. Both
automatic retries reuse the peer name and receive `peerRemoved`. Subsequent
manual retries with fresh names also receive no offer. The initial region-ping
TLS failure is separate from these successful signaling connections.

The fresh browser launch of Genshin Impact completes and reaches the game's
login screen. Its original WebSocket request uses:

```
/nvst/sign_in?peer_id=<peer>&version=3.0&peer_role=1&pairing_id=<session>
```

A subsequent browser reconnect retains the peer name and adds `&reconnect=1`.
The original connection's first messages are:

1. Received: own `peer_info`, ID 2, `peer_role: 1`, `version: 3`,
   `signalingVersion: "3.0"`.
2. Received: server `peer_info`, ID 1, `peer_role: 0`, `version: 3`,
   `signalingVersion: "3.0"`, envelope `ackid: 1`.
3. Sent: `{"ack":1}`.
4. Received: envelope `ackid: 2`, `peer_msg.from: 1`, containing the SDP offer.
5. Sent: `{"ack":2}`, followed by the answer and ICE messages.

No outgoing `peer_info` is present in that initial captured sequence. The browser
URL has no browser/resolution parameters beyond the parameters shown above.

Kasumi instead requested `version=2`, sent a version-2 `peer_info` immediately
and every two seconds until an offer, and omitted the reconnect flag even when
reusing its peer name. These are observed compatibility differences; this
capture alone does not prove that NVIDIA has disabled v2 for every session.

## Changes

- Match the captured v3.0 sign-in and automatic server peer announcements.
- Remove unsolicited/repeated legacy v2 `peer_info` messages.
- Add `reconnect=1` when reusing the previous peer name; omit it for fresh peers.
- Track the server peer ID from its announcement and acknowledge its messages.
- Resolve network endpoints afresh on every CloudMatch response. Previously an
  existing derived signaling URL hid later usage-14 endpoints, and an existing
  media endpoint could defeat the current response's media priority order.
  Preserve endpoints when a partial response supplies no usable replacement.
- Record protocol/reconnect mode and endpoint changes without logging URLs or
  session IDs. Record the signaling error in the final END summary rather than
  the stale CloudMatch "waiting for SDP offer" status.

## Validation and limits

`python tests/run_gfn_network_test.py` builds the production network parser and
incoming signaling handler with Jansson on the host. It tests queue-to-ready
endpoint migration, media priority replacement, explicit URL precedence, sparse
responses, string usage/array IP forms, URL conversion, v3 URL construction,
reconnect flags, truncation detection, and a sanitized browser message sequence
through peer assignment, acknowledgements and offer acceptance.

The changed C sources also pass the devkitARM compiler's syntax checks with
`-Wall -Wextra -Werror` for the 3DS target.

The fix still requires a real-console launch with an affected account. A browser
capture and fixture tests do not establish that the console can complete the
updated handshake or that Fortnite follows the identical game-specific path.
The supplied report did not retain endpoint history or ignored message shapes,
so it cannot establish which compatibility difference triggered this incident.

## Follow-up: report G5EP53 and a second browser capture (same day)

Report G5EP53 ("this run") came from the uncommitted v3 build above and still
failed, as did stock 1.1.1 ("previous run"). In every attempt Kasumi receives its
own `peer_info`, but NVIDIA's streamer peer (ID 1) never appears, so the v3
handshake change cannot be the fix. OpenNOW-vita issue #27, opened the same
morning, reports the identical stall ("Waiting for offer SDP"); Kasumi, Vita and
Switch all send the same Windows-flavoured request.

A fresh Genshin launch in the Claude browser pane (web client 2.0.89.154, free
tier, queue ~80) streamed normally. Timeline after the WebSocket opened:

- +2.9 s: own `peer_info` (ID 2) merged with a `headers` echo of the upgrade.
- Same instant: server `peer_info` ID 1, name `OneSdkServer-<n>`, `peer_role 0`,
  `signalingVersion "3.0"`, envelope `ackid 1` -> client `{"ack":1}`.
- +70 ms: offer (`ackid 2`), then answer + trickle ICE. Signaling closes 4001
  about a second later once media is up (normal for the browser).

The endpoint list was a single entry at every stage: usage 14,
appLevelProtocol 5, `ip` = `<dashed-ip>.cloudmatchbeta.nvidiagrid.net`, port 443,
`resourcePath` `/nvst/`. The queue-time POST response carries a placeholder
host `.cloudmatchbeta.nvidiagrid.net` (empty prefix) that must not be used.

So the endpoint and handshake match what Kasumi does; the difference is the
CloudMatch request, which decides whether a WebRTC streamer ever joins.

### Request fields: Kasumi 1.1.1 vs browser (2026-10-27)

| Field | Kasumi | Browser |
|---|---|---|
| clientPlatformName | `windows` | `browser` |
| appLaunchMode | 2 | 1 |
| remoteControllersBitmap | 1 | 0 |
| clientTimezoneOffset | 0 | real offset (ms) |
| accountLinked | true | false (this account) |
| metaData | - | adds `clientTimezone`, `surroundAudioInfo` "2" |
| requestedStreamingFeatures.profile | 0 | 1 |
| requestedStreamingFeatures extras | codec, maxBitrateKbps, dynamicStreamingMode, vsync, sdr/hdrColorSpace, audioChannelCount, mouseMovementFlags, trueHdr, supportedHidDevices, hidDevices, prefilterSharpness/NoiseReduction | none of these; adds `dlssOverrides` {srMode 0, srModel 0, srScale 100, rrModel 0, fgMode 0, fgModel 0} |
| monitor | monitorId, position, displayData null, hdr10PlusGamingData null, dpi 100 | no id/position, displayData {desiredContent*Luminance 0}, dpi 0 |

Unchanged in both: clientIdentification `GFN-PC`, clientVersion `30.0`,
sdkVersion `1.0`, streamerVersion 1, useOps true, enhancedStreamMode 1,
`wssignaling` 1, `GSStreamerType` WebRTC. Prime suspect: `clientPlatformName`.
The server's ready response echoed `clientPlatformName browser`, gpuType
`3050b / L40S-6`.

## Root cause (confirmed in the browser, same evening)

Report HKJNDC (test build with `clientPlatformName browser`) still failed, so the
body alone was not it. Two controlled launches from the signed-in web client,
rewriting only what the page sent:

1. **Kasumi's exact body** (960x544, codec/bitrate fields, browser platform) with
   the web client's headers: streamer `OneSdkServer` joined and sent the offer.
   The request body is not the cause.
2. **Same body plus Kasumi's CloudMatch headers** (`nv-client-type: NATIVE`,
   `nv-client-streamer: NVIDIA-CLASSIC`, `nv-client-version: 2.0.87.131`): the
   ready session listed only `rtsps://<host>:322` and `:48322` (usage 14,
   appLevelProtocol 6), i.e. a classic native streamer. The web client refused it
   (error 0xC0F22213).

Kasumi converts that rtsps entry to `wss://<host>/nvst/`; the host accepts the
WebSocket and assigns a peer, but no WebRTC streamer exists there, hence no
offer. Before 2026-10-27 the body's `GSStreamerType WebRTC` evidently won; now
the headers decide.

Web client headers (2.0.89.154): `nv-client-type BROWSER`, `nv-client-streamer
WEBRTC`, `nv-client-version 2.0.89.154`, `nv-browser-type CHROMIUM` (Chrome
proper sends CHROME), `nv-device-os WINDOWS`, `nv-device-type DESKTOP`,
`nv-device-make/model UNKNOWN`, plus `nv-client-id`, `x-device-id`.

Fix: `cloudmatch_headers()` sends BROWSER / WEBRTC / 2.0.89.154, CloudMatch calls
use a plain Chrome user agent instead of the NVIDIACEFClient one, and the
"signaling endpoint" log line records `kind=rtsp|wss|https|nvst` so a classic
streamer assignment is visible in reports.

## Side effect: queue ads for free accounts (report J3CB23)

As a web client, a free account gets `sessionAdsRequired=1` and a 120 s queue
ad, and NVIDIA holds the queue near the front (position 6-8) until the client
reports on the ad. Kasumi's answer went to `rig_base(server_ip)`, and while
queued `server_ip` was the placeholder `.cloudmatchbeta.nvidiagrid.net`, which
does not resolve ("ad answer: no connection"). The ad was counted as answered
anyway, so it was never retried.

Fix: `connection_ip()` ignores placeholder hosts that start with `.`. The answer
goes to the session control server and uses the web client's own body
(`adAction 5`, `cancelReason FAILED_TO_REQUEST_ADS`, zero watched/paused/duration
times; the browser's queue then advanced to ready). An ad counts as answered
only after a 2xx response, otherwise the next poll retries.
