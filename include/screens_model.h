#pragma once

/* What the screens say, shared by each console's screens.c (the 3DS's two
 * screens, the Vita's one): the settings and their texts, each service's
 * name, colour and art, the guide pages, the per-game options. */

#include <stdbool.h>
#include <stddef.h>

#include "app.h"
#include "game_prefs.h"
#include "updater.h"

/* Each service: names, what it is, what it needs, its sign-in button. */
typedef struct {
    const char *name, *about, *needs, *sign_in, *sign_in_jp;
} ServiceInfo;
extern const ServiceInfo SERVICE_INFO[SERVICE_COUNT];
extern const char *const SERVICE_TABS[SERVICE_COUNT];
/* Each service's mark on its seal, banner art and white emblem. */
extern const char *const SERVICE_KANJI[SERVICE_COUNT];
extern const UiImage SERVICE_ART[SERVICE_COUNT];
extern const UiImage SERVICE_ICON[SERVICE_COUNT];
/* Each service's own light: card rims, glows, motes. */
u32 service_light(int service);

/* The settings list: section headers (setting -1) and settings, in order. */
typedef struct {
    int setting;
    const char *jp;
    const char *en;
} SettingEntry;
extern const SettingEntry SETTING_ENTRIES[];
extern const int SETTING_ENTRY_COUNT;
/* Each section's tile: one kanji and what is inside. */
typedef struct {
    const char *kanji, *summary;
} SectionInfo;
extern const SectionInfo SECTION_INFO[];
/* A section's header entry (NULL past the end). */
const SettingEntry *section_header(int section);
extern const char *const SETTING_LABELS[SETTING_COUNT];
extern const char *const SETTING_JP[SETTING_COUNT];
/* The setting's current choice and how many there are (dots). */
unsigned setting_option(const App *app, int setting, unsigned *count);
const char *setting_value(const App *app, int setting);
const char *setting_description(const App *app, int setting);

/* NVIDIA's store codes as people know the stores. */
const char *store_label(const char *code);
const char *gyro_mode_name(GfnGyroMode mode);
/* What the last connection check means for play. */
const char *connection_advice(const GfnClient *c);

/* Steam Link's own entries have covers of their own (UI_IMAGE_COUNT: none). */
UiImage special_cover(const GfnGame *game);
/* The service in use's last game, for the hub's Continue (NULL: none). */
const GfnGame *hub_last_game(const App *app);

/* A sign-in in progress: through Xbox, a Steam pairing, and its service. */
bool login_is_xbox(const GfnClient *client);
bool login_is_steam(const GfnClient *client);
int login_service(const GfnClient *client);

/* "synced 2 h ago" for the saved library ("" when unknown). */
void library_age(const GfnClient *client, char *out, size_t size);
/* How a game will be streamed, for the library and the game page. */
const char *stream_line(const App *app);

/* The session screen: 0 queue, 1 rig, 2 signal, 3 stream; and failure. */
int session_stage(const App *app);
bool screens_session_failed(const App *app);

/* The game page: the chosen store, play time as text. */
const char *details_store(const App *app, const GfnGame *game);
void format_played(char *out, size_t size, uint32_t seconds);
/* A per-game options row's value, and whether this game changes it. */
const char *option_value(const App *app, const GamePrefs *prefs, int row, char *buffer, size_t size);
bool option_custom(const GamePrefs *prefs, int row);
/* What a mapped input sends ("LB + RB"), and whether it differs from the
 * default. */
void binding_text(char *out, size_t size, const GfnButtonMap *map, int input);
bool binding_changed(const App *app, int input);

const char *update_phase_title(const UpdateInfo *info);

/* The first-run guide. */
typedef struct {
    const char *kanji, *jp, *en, *body, *tip;
} GuidePage;
extern const GuidePage GUIDE[GUIDE_PAGES];

/* Was this status line written from this message (in the chosen language)? */
bool status_from(const char *status, const char *english_format);
/* 1:02:03 or 2:03. */
void format_elapsed(char *out, size_t size, u64 ms);
