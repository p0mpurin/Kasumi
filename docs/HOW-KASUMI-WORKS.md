# How Kasumi works, from power-on to playing

A walk through Kasumi, the unofficial GeForce NOW client for the New Nintendo 3DS.
It is written to be read aloud: each part starts with the plain idea, then goes as
deep as you like. Numbers come from Kasumi's source and from real player
diagnostics (beta.31–32, October 2026).

---

## Part 1: The machine we're working with

First of all, the New 3DS is a small computer from 2014, and every design choice in
Kasumi comes from its limits.

**The CPU.** It's an ARM11 MPCore with **four cores at 804 MHz**. The original
3DS has two cores at 268 MHz, which is one reason Kasumi is New-3DS-only. Kasumi
switches on the New 3DS "speedup" mode and the L2 cache at startup, so it gets the
full 804 MHz.

The four cores are not all ours:

- **Core 0** is the application's main core. Most of Kasumi lives here.
- **Core 1** belongs to the system. Apps can borrow a slice of it, but Kasumi
  doesn't rely on that.
- **Core 2** is the extra core the New 3DS gives applications. Kasumi puts its
  whole network and media pipeline here, so receiving video never fights the
  user interface for time.
- **Core 3** is reserved for the system (HOME Menu and friends).

**Memory.** Kasumi asks for the New 3DS's extended memory mode: **124 MB** for
the app. The important part is "linear" memory: physically contiguous RAM
that the GPU and the hardware video decoder can read directly. About **27 MB** of
it is free when Kasumi starts. The video decoder's own working memory takes about
15.5 MB of that on its own, so linear memory is the tightest resource in the app.
During a stream only about 1.5 MB is left over.

**The graphics chip.** A **PICA200** GPU with a fixed-function pixel pipeline.
There are no pixel shaders like a modern GPU; you combine textures and colours
through six configurable "combiner" stages. Kasumi uses that for its sharpening
and colour filter (Part 8).

**The secret weapon: a hardware H.264 decoder.** The New 3DS has a video
decoder block that most people never knew about. The system service that drives
it is called **MVD**. Without it, none of this would work: the CPU alone can't
decode 30 frames of HD video a second.

**The screens.** The top screen is normally 400×240. To show 3D, it has a mode
where it's addressed as **800×240**, two images side by side. Kasumi switches the
screen into that 800-column mode *without* the 3D, which gives twice the
horizontal detail of a normal 3DS app. That's why small text in games is
readable.

**Wi-Fi.** 802.11 b/g/n on 2.4 GHz only. In practice a few Mbit/s of steady
throughput, with occasional bursts and stalls. Much of Kasumi's design is about
surviving that.

---

## Part 2: The threads, who does what

Kasumi is a multi-threaded program. On the 3DS a thread has a **priority**
(lower number = more important) and a **core**. Here is everyone, and why
they're set up the way they are.

| Thread | Core | Priority vs. the UI | What it does |
|---|---|---|---|
| **Main / UI thread** | 0 | — (reference) | Reads buttons and touch, runs the app's state machine, draws both screens, paces video frames, sends controller state |
| **Network worker** | 0 | one step *lower* | Every HTTPS request: sign-in, catalog, sessions, the queue, cover art, updates, reports. Runs whenever the UI waits for the screen refresh, and never interrupts it |
| **Media thread** | **2** | one step *higher* | The whole live stream: the UDP socket, connection checks (ICE), encryption (DTLS/SRTP), video packets (RTP), controller channel (SCTP), and audio decoding |
| **Video decoder thread** | 0 | one step higher | Feeds encoded frames to the MVD hardware decoder. It spends almost all its time waiting on the chip, so high priority just means it reacts instantly when a frame is done |
| **Menu audio thread** | 0 | one step higher | Menu music, Tsumugi's voice lines, sound effects. Wakes every 10 ms and decodes a little |
| **Log writer** | 0 | four steps lower | Writes the diagnostic log to the SD card only when nothing else wants the CPU |
| **Screenshot writer** | 0 | three steps lower | Encodes a PNG in the background |
| **Gallery decoder** | any | low | Decodes saved screenshots for the viewer |

**How threads share data:**

- **UI and network worker:** they never touch each other's data directly. The
  worker keeps its own copy of the "client state" and publishes a snapshot about
  5 times a second. The UI copies it under a short lock (a `LightLock`) and uses
  its own copy.
- **Live connection:** the WebRTC connection object is guarded by a recursive
  lock. The media thread takes it for one UDP packet at a time, so the UI (which
  needs it to send controller input) never waits more than a moment.
- **Video frames:** they move between threads through queues. A ring buffer
  holds encoded frames waiting to be decoded, and a set of six decoded surfaces
  hold frames waiting to be shown.

**Why the network work is split in two:**

- **The worker** handles slow, request-response web calls (a sign-in can take
  seconds).
- **The media thread** handles the real-time stream (a packet every few
  milliseconds).

Mixing them would make the stream stutter whenever the app did a web request.

---

## Part 3: Starting up

When you open Kasumi:

1. **Hardware services come up.** These are the speedup mode, graphics (citro3d
   and citro2d), the sound DSP (NDSP), and the network stack (SOC, with a 1 MB
   buffer). They also include the gyroscope and the HOME Menu event hooks, so
   Kasumi knows about sleep, the lid and HOME.
2. **Saved state loads from the SD card:** settings, your login, the cached
   library, favourites and per-game options, the queue-time model, and play
   history. The library shows *instantly* from cache.
3. **The network worker starts.** In the background it:
   - renews your login if needed;
   - downloads the list of GeForce NOW providers and works out which country
     you're in;
   - fetches the live queue estimates;
   - checks GitHub for updates;
   - starts filling the cover-art cache.

---

## Part 4: Signing in, with no password on the 3DS

Kasumi uses the **OAuth 2.0 Device Authorization Grant** (RFC 8628), the same
"enter this code on your phone" flow smart TVs use.

1. The worker calls `POST https://login.nvidia.com/device/authorize`. NVIDIA
   returns a `device_code` (secret, kept on the console), a short `user_code`, a
   URL, how often to check back, and when the code expires.
2. The 3DS shows the URL and the code.
3. You sign in to NVIDIA on your phone or PC and type the code.
4. Meanwhile the worker checks `POST https://login.nvidia.com/token` with the
   device code every few seconds. As soon as you approve, NVIDIA returns tokens.
5. Kasumi trades them for a **client token** that lasts about 90 days. The
   short-lived access token (1 hour) is renewed from it automatically, even in
   the middle of a game, so a long session never logs you out.

**Your password never touches the 3DS.** Only tokens are stored, on the SD card.

**Providers.** GeForce NOW isn't always run by NVIDIA: Cloud.GG in Australia,
KDDI au in Japan, and others. Kasumi downloads
`pcs.geforcenow.com/v1/serviceUrls`, which lists every provider, their login and
session servers, and the country NVIDIA thinks you're in. Where a partner runs the
service, Kasumi asks which one your account belongs to.

**A wrong clock breaks this.** Every one of these connections is HTTPS, and
checking a certificate needs the right date. If the 3DS clock still says 2011,
Kasumi says so instead of failing silently.

---

## Part 5: Your library

- **Getting the games:** the worker asks NVIDIA's catalog for the games in your
  account's library, page by page. Each game has a title, an app ID, and
  **variants**: the same game on Steam, Epic, Ubisoft and so on. Search uses the
  same catalog, one page at a time.
- **Cover art** comes from NVIDIA's image server, which resizes on request (Kasumi
  asks for three times the display width). The worker downloads it while no game
  is running, decodes it, shrinks it with a high-quality filter, and saves it on
  the SD card. The UI uploads it to the GPU as a texture, keeping only a few in
  memory and freeing old ones a frame late so the GPU never reads freed memory.
- **Library checks:** Kasumi keeps a set of every store ID in your library. That's
  how search can show "NOT IN LIBRARY", and how a refusal can say *why* you
  can't play a game.

---

## Part 6: Starting a game and the queue (CloudMatch)

Pressing Play asks NVIDIA's session service, **CloudMatch**, for a game server.

1. **Create the session:** `POST {server}/v2/session` with a JSON body that says:
   - which game (`appId`) and from which store;
   - the stream Kasumi wants: **960×544** (NVIDIA sends 960×540) at **30 frames a
     second** (or 60 in the beta mode);
   - a **device ID**, also sent as the `x-device-id` header. Early on, every
     console sent the *same* ID because a random number generator was never
     seeded, and NVIDIA's "too many sessions per device" limit hit everybody at
     once.
2. **The answer** contains a session ID, your place in the queue, which control
   server now owns your session, and where to connect when it's ready.
3. **Waiting:** Kasumi checks `GET /v2/session/{id}` about every 3 seconds until
   the status says a server (a "rig") is ready.
4. **The wait estimate:**
   - Every finished queue is recorded anonymously (where you started in line, how
     long it took).
   - A small server Kasumi runs on Cloudflare fits a straight line per provider:
     `wait ≈ base + seconds-per-place × place`, refreshed hourly.
   - Today NVIDIA's free queue fits as about 35 s plus 2.6 s per place.
5. **When it can't start:** Kasumi turns NVIDIA's error codes into plain words. A
   few cases are handled for you:
   - **Limited mode** (NVIDIA briefly refusing new sessions): retried
     automatically with growing gaps.
   - **Another session on your account:** offers to resume or end it.
   - **Abroad:** points you to a server in your home country.
6. **Leaving:** `DELETE /v2/session/{id}`, so the rig is freed straight away.

---

## Part 7: Connecting to the game server (WebRTC)

Now the 3DS has to open a live, encrypted, real-time connection to a game server
somewhere in a data centre. GeForce NOW uses **WebRTC**, the same technology as
video calls in a browser. Kasumi implements it with a heavily modified small C
library, **libpeer**.

### 7.1 Signalling: agreeing on the settings
- **The connection:** Kasumi opens a secure **WebSocket** to NVIDIA's signalling
  server ("NVST"). It uses libcurl only to open the TLS connection; the WebSocket
  upgrade and message framing are done by Kasumi itself.
- **The messages** are JSON:
  - Kasumi introduces itself (`peer_info`).
  - Every message has an ID that must be acknowledged.
  - Both sides send heartbeats.
- **The server's offer** is an **SDP**, a text description of the media. It
  contains a second, NVIDIA-specific block (**nvstSdp**) with hundreds of tuning
  parameters for the encoder.
- **Kasumi's answer** has two parts:
  - **The standard part:** "here's my address info and encryption fingerprint; I
    accept H.264 video and Opus audio".
  - **Kasumi's own nvstSdp:**
    - the viewport size, the maximum frame rate, and starting, minimum and maximum
      bitrate;
    - how much error correction to add, packet size, and packet pacing (spreading
      a frame's packets out instead of bursting them, which weak 3DS Wi-Fi needs);
    - server-side sharpening on or off.
  - **In 60 fps mode** it also switches *off* NVIDIA's automatic frame-rate and
    resolution changes. Otherwise the server kept flipping between 30 and 60, with
    a full picture refresh each time.

### 7.2 Finding a path through your router (ICE)
- Both sides list their possible addresses ("candidates"). Kasumi's are its local
  address and its public address as seen through your router (found with STUN).
- NVIDIA's server is **ICE-lite**: it only answers. Kasumi does the actual checks,
  sending small test packets and seeing which come back. It also adds a fallback
  candidate on NVIDIA's port 47999, which rescues some home networks.
- Once connected, Kasumi sends a "consent" check every 2 seconds. That keeps the
  path open and measures the ping you see on screen.

### 7.3 Encryption (DTLS and SRTP)
- **DTLS 1.2** (TLS for UDP, done with mbedTLS) runs over the chosen path. It
  produces keys for **SRTP**, which encrypts every audio and video packet (done
  with libsrtp).
- Everything (video, audio, connection checks, encryption handshakes, the
  controller channel) shares **one UDP socket**. The media thread looks at the
  first byte of each packet to tell them apart.

### 7.4 The three streams
After about 2 seconds of setup, three things flow:

1. **Video** (server → 3DS)
2. **Audio** (server → 3DS)
3. **A data channel for controls** (3DS → server)

---

## Part 8: Video, from Wi-Fi packets to pixels

### 8.1 Packets → frames (RTP)
- **The stream:** H.264 (High profile, 4 reference frames) at about **1 Mbit/s**,
  split into **RTP** packets of about 1,140 bytes. A frame may be one packet,
  several small pieces bundled together (**STAP-A**), or one big piece split
  across many packets (**FU-A**).
- **Ordering:** a reorder buffer puts packets back in order and drops duplicates.
- **When a packet is lost:**
  1. Kasumi immediately sends a **NACK** ("please resend packet 12345").
  2. The server resends it on a separate retransmission stream.
  3. If a frame still can't be completed, it is **never decoded half-done**. Kasumi
     sends a **PLI** (picture loss indication, "send me a fresh full frame") and
     holds the last good picture until the new full frame (an **IDR**) arrives.
     That hold is the brief freeze you sometimes see on bad Wi-Fi.
- **Queueing:** finished frames go into a **2 MB ring buffer** (48 slots). It used
  to be ten fixed slots, and a burst of frames after a Wi-Fi stall overflowed it.
- **Reading the stream's settings:** Kasumi reads each frame's **SPS**, the
  stream's "settings header": size, profile, level, reference frames. If NVIDIA
  changes the size mid-stream, the decoder is rebuilt at the next full frame.

### 8.2 Frames → pictures (the MVD hardware decoder)
- **Setup:**
  - Kasumi asks MVD how much working memory it needs for this stream: about
    **15.5 MB** for 960×544.
  - It starts the decoder in "video processing" mode, with H.264 in and RGB565
    pixels out.
  - The decoder is told to output **800×480** (its built-in scaler shrinks 960 →
    800) into a surface padded to **1024×512**. Powers of two are what the GPU
    needs for a texture, so a decoded frame can be drawn without copying it.
- **Per frame:** the decoder thread feeds the frame's pieces to the chip, then asks
  it to render into one of **six output surfaces**.
  - **Decode time:** about **12.7 ms** per frame on average at 30 fps (each frame
    has 33 ms), and about **15.8 ms** at 60 fps (each frame has 16.7 ms).
- **Zoom** is free: Kasumi just tells the decoder to read a smaller rectangle of
  the source frame and scale *that* up.
- **Odd sizes:** if NVIDIA shrinks the stream on a weak connection (726×544 has
  been seen), the decoder can't output wider than its input. Kasumi now outputs
  the largest size that fits, keeping the shape, and shows black bars.

### 8.3 Pictures → screen (the GPU)
- **Copying into a texture:** a decoded frame sits in normal memory as a flat
  bitmap. One hardware transfer copies it into the GPU's *tiled* texture format
  (the GPU stores textures in small square blocks).
- **Fitting the screen:** the 800×480 picture is drawn into the 800-column top
  screen at **half height**. With bilinear filtering, each output pixel samples
  exactly between two source rows, so the GPU averages the pair for free: a
  perfect 2:1 downscale.
- **Sharpness and colour, on the GPU, for free:**
  - A tiny custom vertex shader gives each pixel three texture lookups: the pixel
    itself and two half-pixel diagonal neighbours. Bilinear filtering turns those
    into a blur of the six nearest pixels.
  - The six combiner stages then compute

    `result = pixel + k·(pixel − blur) + s·(pixel − green) + q·(pixel − ½)`

    That's sharpening (an "unsharp mask"), saturation, and contrast, in one pass.
    It costs no bitrate and no frame time.

### 8.4 Frame pacing: making it *smooth*
Frames arrive unevenly over Wi-Fi: two at once, then a gap. Showing them as they
arrive would look jittery. So:

- **A steady rhythm:** Kasumi shows a new frame exactly every **2 screen
  refreshes** at 30 fps (every 1 at 60). A frame is never shown for an odd
  duration.
- **Spare frames:** it keeps **1–2 frames in reserve** to cover gaps. After
  repeated late frames it keeps one more; after a calm minute it goes back.
- **Clock drift:** NVIDIA sends exactly 30.00 frames a second, but the 3DS screen
  refreshes about 59.83 times a second (29.92 frame slots). One extra frame builds
  up every few seconds. Kasumi waits for a frame whose data size says *little was
  moving* and drops that one, so the drop is practically invisible.
- **The rate that's really arriving:** Kasumi measures the actual frame rate every
  2 seconds, so pacing follows what NVIDIA really sends.

---

## Part 9: Audio
- **Format:** **Opus** at 48 kHz stereo, in 10 ms packets.
- **Decoding:** on the CPU (libopus), right on the media thread as each packet
  arrives.
- **Playback:** the 3DS sound DSP (**NDSP**) plays it from a queue of 24 small
  buffers, capped at 12 queued to keep delay low.
- **Gaps:** missing packets are covered over rather than heard as clicks, and
  counted in diagnostics.
- **Menu audio** (music, voice, effects) is a completely separate path with its
  own thread and sound channels.

---

## Part 10: Your controls, from button to game

### 10.1 Reading the 3DS
Every pass of the main loop (several hundred times a second while streaming),
Kasumi reads:

- **Buttons:** mapped to a standard gamepad. The default is "by position" like a
  PlayStation pad (bottom face button = Cross), and players can remap every
  button per game.
- **Circle Pad:** the left stick, scaled for the Circle Pad's real range.
- **C-Stick:** the right stick, with deadzone, response curve, speed and inversion.
- **Gyro:** the console's gyroscope, calibrated for drift and smoothed, added to
  the right stick for motion aiming.
- **Touch C-stick:** the lower screen as a camera stick. Its push is added to the
  raw C-Stick reading *before* the deadzone and curve, so it behaves exactly like
  the real stick. The touchscreen sometimes loses a light touch for a frame, so a
  160 ms grace period and outlier rejection keep it from "letting go".
- **Mouse and keyboard mode:** the touchscreen becomes a trackpad, and an
  on-screen keyboard sends real key presses, for PC launchers and menus.

### 10.2 The input protocol
- **The channel:** controls travel on a WebRTC **data channel** named
  `input_channel_v1`. When the channel opens, the server says which protocol
  version it speaks.
- **A controller report** is 38 bytes: buttons, two triggers, four stick axes and
  a microsecond timestamp. It's wrapped in a small header: `0x23`, the timestamp,
  `0x21`, then a length. 50 bytes in all.
- **How often:** sent the moment anything changes (button changes within 4 ms),
  and repeated every 16 ms anyway, about 60 times a second. A small heartbeat
  goes every 2 seconds.
- **Mouse and keys:** mouse moves, clicks and key presses have their own small
  record types.

### 10.3 SCTP, and the bug that killed the controls
- **What SCTP is:** WebRTC data channels run over **SCTP**, a protocol with
  numbered chunks (TSNs) and acknowledgements (SACKs).
- **Why Kasumi has its own:** to save memory and threads, Kasumi uses a minimal
  built-in SCTP instead of the usual large library, so it had to make it reliable
  itself.
- **The bug:**
  - The built-in version sent each chunk **once** and ignored acknowledgements.
  - When one controller packet was lost on Wi-Fi, the server waited for that
    missing number forever and kept everything after it on hold.
  - After roughly 4,000 more packets (about a minute), its buffer filled and it
    discarded *all* new input.
  - Players lost control 20–30 minutes in while the game kept running.
- **Fix 1 (beta.31): a resend queue.** Kasumi keeps the last 256 chunks, reads
  the server's acknowledgements (including "I'm missing these" gap reports), and
  resends anything not confirmed in time. The timeout is about 2× the round trip
  plus 100 ms, doubling on each retry.
- **Fix 2 (beta.32): skip, don't resend.** On very weak Wi-Fi, resending old
  controller states made a backlog. So Kasumi now:
  - offers **PR-SCTP** (partial reliability, RFC 3758) in its connection
    handshake;
  - marks controller states as *skippable*;
  - when one is overdue, sends a **FORWARD-TSN** ("treat everything up to here as
    received; move on") instead of resending it. The next controller state, 16 ms
    later, already contains the latest buttons anyway.

  **Key presses and mouse clicks stay fully reliable**, so nothing gets stuck down.

---

## Part 11: Everything around the game

- **Diagnostics.** Kasumi writes a detailed log to the SD card (the log writer
  thread).
  - It raises **flags** when something goes wrong: the UI froze, the picture
    froze, the decoder fell behind, a secure connection failed, and others.
  - With the player's permission, a flag sends an **automatic report**. You can
    also send one by hand and get a short code to share on Discord.
- **Reports and stats** go to a small service on **Cloudflare Workers**, which
  stores them and returns the code. Anonymous per-session summaries (frame rate,
  decode time, ping, queue times) feed the data exports used to find bugs. Most
  big fixes started there:
  - the shared device ID;
  - controls dying after 20–30 minutes;
  - the 4:3 decoder failure;
  - the 60 fps tests.
- **Updates.**
  1. Kasumi checks GitHub's release list and picks the newest version (betas
     included).
  2. It downloads the installer (CIA) and checks it against the published
     **SHA-256** checksums.
  3. It streams it into the 3DS application manager to install, then relaunches.
- **HOME Menu shortcuts.** Kasumi carries a tiny "forwarder" app inside itself. To
  make a shortcut, it builds a new copy of that app *on the console*: its own ID,
  an icon made from the cover art, and a banner made from NVIDIA's wide game art.
  It then installs it. Opening the shortcut launches Kasumi with the game's ID, and
  Kasumi goes straight into the launch.
- **Themes.** Ten themes, each with a hand-made wallpaper on the lower screen and
  frosted-glass buttons. The "glass" is a pre-blurred half-size copy of the
  wallpaper, drawn back at 2× under each button.

---

## Part 12: The whole thing in one picture

```
 You press a button
        │
        ▼
 UI thread (core 0) ── reads buttons/touch/gyro ─▶ 50-byte controller report
        │                                              │
        │                         SCTP data channel ◀──┘ (resend or skip)
        │                                              │
        ▼                                              ▼
  draws menus,                Media thread (core 2): DTLS ─▶ UDP ─▶ Wi-Fi ─▶ NVIDIA rig
  paces video                         ▲                                          │
        ▲                             │                                  game runs, frame
        │                     UDP ◀── Wi-Fi ◀── H.264 + Opus over SRTP ◀── encoded
        │                             │
        │                  RTP: reorder, NACK, rebuild frames
        │                     │                      │
        │              2 MB frame ring          Opus → NDSP sound
        │                     │
        │          Decoder thread → MVD chip (960×540 → 800×480)
        │                     │
        │             6 ready surfaces (1024×512)
        │                     │
        └──── GPU: tile copy → sharpen/colour → 800×240 wide screen
```

---

## Numbers to quote

- **CPU:** New 3DS, 4 cores at 804 MHz. Kasumi runs the stream on its own core
  (core 2).
- **Stream:** 960×540 H.264 at 30 fps (60 fps beta), about **1 Mbit/s**.
- **Decode:** **≈12.7 ms** per frame at 30 fps, **≈15.8 ms** at 60 fps, on the
  New 3DS hardware decoder.
- **Ping:** about **46–50 ms** in a typical session.
- **Smoothness:** a typical session repeats ~2 and skips ~2 frames a minute out of
  1,800.
- **Queue:** about **2 minutes** on the free tier.
- **Memory:** ~15.5 MB of the decoder's working memory out of ~27 MB of
  GPU-reachable RAM, with ~1.5 MB spare during a stream.
- **Controls:** a controller report goes out about 60 times a second, 50 bytes
  each.

## Short answers to likely questions

- **Is it official?** No. It's an independent, open-source (GPLv3) project, not
  made by or affiliated with NVIDIA. It signs in through NVIDIA's normal login,
  and your password never touches the 3DS.
- **Why only the New 3DS?** The original 3DS has no hardware video decoder for
  this and a much slower CPU.
- **Where's the lag from?** Mostly the 3DS's 2.4 GHz Wi-Fi and the distance to the
  data centre. Kasumi adds about one or two frames of buffering for smoothness.
- **What's next?**
  - 60 fps (beta).
  - Faster recovery when Wi-Fi drops a frame.
  - Telling NVIDIA how good the connection is, so it can send a sharper picture.
  - Maybe streaming from your own PC.
