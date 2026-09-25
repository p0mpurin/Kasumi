#pragma once

#include <stdbool.h>

/* "Your rig is ready": the notification LED pulses in the theme colour and a
 * soft chime plays, so the console can wait in the queue with the lid shut.
 * Also switches the screens off while the lid is closed during a queue. */

void queue_alert_start(void);
void queue_alert_stop(void);
bool queue_alert_active(void);
/* Screens off while the lid is shut and a queue is running; on otherwise. */
void queue_alert_screens(bool lid_closed_waiting);
/* True when the lid is closed. */
bool queue_alert_lid_closed(void);
void queue_alert_exit(void);
