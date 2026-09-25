# Kasumi technical notes

How Kasumi works on the New 3DS, and why it is built the way it is. For
using the app, see the [README](../README.md).

## Pipeline

Kasumi signs in with NVIDIA's device-code flow, loads the owned library (or
searches the catalog), creates and polls a CloudMatch session, connects NVST
signalling and WebRTC (libpeer, libsrtp, usrsctp, a renamed mbedTLS), decodes
H.264 on the New 3DS MVD hardware decoder, plays Opus audio through NDSP and
sends gamepad, mouse and keyboard input over WebRTC data channels.

Threads: the UI thread runs the media loop (sleeping in `poll()` on the UDP
socket, 4 ms cap), a decoder thread owns MVD, a lower-priority network
worker owns every blocking HTTP request, and a low-priority writer thread
saves the diagnostic log to the SD card about once a second.

## Picture

The stream is 960x540 at 30 FPS. NVIDIA snaps unsupported sizes: a probe
showed 1280x720 for 854x480, 864x480, 768x432 and 640x360, and 1280x800 for
800x480 (which MVD cannot decode at 30 FPS), so 960x540 is the smallest
stream it provides.

*Wide 800* uses the top screen's 800-column mode (all New models, including
the New 2DS XL): MVD scales the frame to 800x480 inside a 1024x512 surface,
one display transfer tiles it into a texture, and the GPU draws it at half
height so bilinear filtering averages each pair of rows. Text keeps twice the
horizontal detail of the 400-column path. MVD outputs full-range colour, so
no colour correction is applied.

*Encoder filter* Clean sends no NVIDIA pre-encode sharpening, as in OpenNOW;
at 1 Mbps sharpening spends bits on edges and noise. Asking for intra
refresh instead of a full keyframe every ~10 s was tried and ignored by
NVIDIA (IDRs stayed exactly 10.1 s apart).

## Bitrate and loss

3DS Wi-Fi (2.4 GHz 802.11g) carried ~0.9 Mbps with almost no loss, while
~1.3-1.5 Mbps lost about one packet per second and sometimes bursts. Packet
pacing is gentle (10 groups, up to 3 ms), NACK/RTX recovers losses, the RTP
reorder buffer waits up to 150 ms for a retransmission (OpenNOW's window),
and a frame that still cannot be completed freezes the last clean picture
until the next keyframe instead of smearing. Keyframe requests back off from
300 ms to 3 s while loss continues, which prevents a keyframe storm.

## Frame pacing

A frame is shown every second vblank (strictly, so no frame is ever shown
for a single refresh), with one spare frame against Wi-Fi jitter, two after
repeated late frames. NVIDIA sends exactly 30.00 FPS while the LCD shows
29.92, so one surplus frame builds up every ~12 s; the pacer waits for a
frame whose encoded size says little moved and drops that one, anywhere in
the ready queue. The top screen is double-buffered in wide mode, so frames
change only at a vblank.

## Input

The 3DS appears as an XInput gamepad (protocol v3). By default the face
buttons follow PlayStation positions. Changes are sent within 4 ms (buttons)
or 8 ms (sticks) and the state repeats every 16 ms. Gyro aim divides the raw
gyroscope reading by the HID coefficient (14.375 units per degree/second),
calibrates the resting drift at start, tracks it while the console is still
and maps the rate to right-stick deflection.

## Storage

Everything lives in `sdmc:/3ds/kasumi/`:

| File | Contents |
|---|---|
| `gfn-session.json` | Login tokens. **Private: never share.** |
| `settings.json` | Settings |
| `library.json` | Saved library (titles, IDs, stores, art URLs) |
| `games.json` | Favourites and per-game options |
| `history.json` | Play time, sessions and last played per game |
| `zones.json` | Saved zoom zones per game |
| `art/` | Cached box art (96x128 RGB) |
| `screenshots/` | PNG screenshots |
| `kasumi-diagnostic.txt` | Log of the last run (tokens excluded) |

## Diagnostics

The log is written by a background thread, so the stream never waits for
the SD card. Each 4-second window records presents, repeats, drops, queue
depth, the active reserve and the slowest main-loop pass; each second
records bitrate, frames the server skipped and late arrivals. Earlier builds
showed that SD flushes from the media loop caused audio drops and video
stalls, and a crash dump that names the `socket` system process points at
socket usage, not at an app source line.

## Vendored libraries

`vendor/` holds the WebRTC transport sources, built by `tools/build-transport.sh`:
libpeer (modified: accessors, larger receive buffer, 150 ms reorder hold),
libsrtp, usrsctp, mbedTLS 2.28.8 (with `include/mbedtls/config.h` and
`library/timing.c` adapted for the 3DS; its symbols are renamed so it can
coexist with the portlibs mbedTLS used by curl) and stb_image /
stb_image_write. Each keeps its own licence file.
