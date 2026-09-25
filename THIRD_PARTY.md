# Third-party software

Kasumi is licensed under the GNU General Public License v3.0 (see
[LICENSE](LICENSE)). It includes or links the following software, each under
its own licence. All of these licences are compatible with GPL-3.0.

## Included in this repository (`vendor/`)

| Project | Licence | Use |
|---|---|---|
| [libpeer](https://github.com/sepfy/libpeer) | MIT (`vendor/libpeer/LICENSE`) | WebRTC peer connection; modified for Kasumi |
| [libsrtp](https://github.com/cisco/libsrtp) | BSD-3-Clause (`vendor/libsrtp/LICENSE`) | SRTP media encryption |
| [usrsctp](https://github.com/sctplab/usrsctp) | BSD-3-Clause (`vendor/usrsctp/LICENSE.md`) | SCTP data channels (input) |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) 2.28.8 | Apache-2.0 OR GPL-2.0-or-later (`vendor/mbedtls/LICENSE`) | DTLS for WebRTC; `config.h` and `timing.c` adapted |
| [stb_image, stb_image_write](https://github.com/nothings/stb) | MIT or public domain (in each header) | Box art decoding, PNG screenshots |

## Linked from devkitPro portlibs

| Project | Licence |
|---|---|
| [libctru](https://github.com/devkitPro/libctru), [citro3d](https://github.com/devkitPro/citro3d), [citro2d](https://github.com/devkitPro/citro2d) | zlib |
| [libcurl](https://curl.se/) | curl licence (MIT-style) |
| [Jansson](https://github.com/akheron/jansson) | MIT |
| [Opus](https://opus-codec.org/) | BSD-3-Clause |
| [zlib](https://zlib.net/) | zlib |
| Mbed TLS 2.28.8 (for libcurl) | Apache-2.0 |

`romfs/cacert.pem` is the Mozilla CA certificate list as distributed by curl
(MPL-2.0).

## Projects that informed Kasumi

Kasumi's GeForce NOW protocol work follows the open-source OpenNOW clients,
and its MVD video decoding was informed by Moonlight-N3DS:

- OpenNOW, MIT, Copyright (c) 2025 Zortos
- [OpenNOW-Switch](https://github.com/OpenCloudGaming/OpenNOW-Switch), MIT, Copyright (c) 2026 OpenCloudGaming
- OpenNOW-Vita, MPL-2.0
- [Moonlight-N3DS](https://github.com/zoeyjodon/moonlight-N3DS), GPL-3.0

GeForce NOW and NVIDIA are trademarks of NVIDIA Corporation. Nintendo 3DS is a
trademark of Nintendo. Kasumi is not affiliated with or endorsed by either.
