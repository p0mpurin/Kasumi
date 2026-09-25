#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The DSP audio driver, started once when Kasumi opens and kept until it
 * closes (the stream and the queue chime both use it). False: no sound. */
bool audio_system_init(void);
bool audio_system_ready(void);
void audio_system_exit(void);

/* 48 kHz stereo Opus from the GFN WebRTC audio track. */
bool audio_output_init(void);
int audio_output_submit(const uint8_t *packet, size_t packet_size, uint8_t payload_type,
                        uint16_t sequence);
/* Lost packets filled in with Opus concealment since the stream started. */
unsigned audio_output_concealed(void);
void audio_output_close(void);
/* Stream volume 0..1, and a mute that overrides it (menus). */
void audio_output_set_volume(float volume);
void audio_output_set_muted(bool muted);
const char *audio_output_status(void);
