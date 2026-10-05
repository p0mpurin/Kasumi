#pragma once

/* SD card writes on a background thread. The main loop used to write small
 * JSON files itself (play history, the launch record, the session summary,
 * queue stats, game options); on a slow card one write took 3-12 s, and the
 * game froze right as it started or ended (beta.34 export: most ui-stall
 * flags sat next to one). Jobs run in order; a save of a file that is
 * still waiting replaces the older one. */

#include <jansson.h>
#include <stdbool.h>
#include <stddef.h>

/* Start the thread (once, at launch, before other threads use it). */
void file_worker_init(void);
/* Write root to path (json_dump_file flags). Takes the reference. */
void file_worker_save_json(const char *path, json_t *root, size_t flags);
/* Append record to the JSON array in path, keeping the newest keep
 * entries. Takes the reference. */
void file_worker_append_json(const char *path, json_t *record, size_t keep, size_t flags);
void file_worker_remove(const char *path);
/* Wait for everything queued so far (before reading such a file back, or
 * before exiting). */
void file_worker_flush(void);
/* Stop the thread after finishing the queue (exit). */
void file_worker_exit(void);
