#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

#include "gfn_client.h"
#include "nvst_signal.h"
#include "settings.h"
#include "ui.h"
#include "webrtc_transport.h"

typedef enum {
    VIEW_WELCOME,
    VIEW_LOGIN,
    VIEW_LIBRARY,
    VIEW_SETTINGS,
    VIEW_SESSION,
    VIEW_STREAM,
    /* One game's page: cover, stores, play time. */
    VIEW_DETAILS
} AppView;

typedef enum {
    MODAL_NONE,
    MODAL_EXIT,
    MODAL_SIGN_OUT,
    MODAL_ERROR
} AppModal;

typedef enum {
    ACTION_NONE,
    ACTION_PLAY,
    ACTION_LIBRARY,
    ACTION_SEARCH,
    ACTION_SETTINGS,
    ACTION_SIGN_IN,
    ACTION_NEW_CODE,
    ACTION_CANCEL,
    ACTION_BACK,
    ACTION_PREV,
    ACTION_NEXT,
    ACTION_EXIT,
    ACTION_CONFIRM,
    ACTION_DISMISS,
    ACTION_RETRY,
    ACTION_VALUE_PREV,
    ACTION_VALUE_NEXT,
    ACTION_STREAM_KEYBOARD,
    ACTION_STREAM_POINTER,
    ACTION_STREAM_ZOOM,
    ACTION_STREAM_MENU,
    /* Same order as the stream menu items (index -> action). */
    ACTION_MENU_RESUME,
    ACTION_MENU_SCREENSHOT,
    ACTION_MENU_CONTROLS,
    ACTION_MENU_ZONE,
    ACTION_MENU_GYRO,
    ACTION_MENU_SOUND,
    ACTION_MENU_LAYOUT,
    ACTION_MENU_DISCONNECT,
    ACTION_CONTROLS_CLOSE,
    ACTION_DETAILS_PLAY,
    ACTION_VARIANT_PREV,
    ACTION_VARIANT_NEXT,
    ACTION_FAVOURITE,
    ACTION_OPTIONS,
    ACTION_OPTIONS_CLOSE,
    ACTION_OPTION_PREV,
    ACTION_OPTION_NEXT,
    ACTION_GUIDE_NEXT,
    ACTION_GUIDE_BACK,
    ACTION_GUIDE_SKIP,
    ACTION_UPDATE_PRIMARY,
    ACTION_UPDATE_CLOSE,
    ACTION_UPDATE_LATER,
    ACTION_WHATS_NEW_CLOSE,
    ACTION_MAP_PREV,
    ACTION_MAP_NEXT,
    ACTION_MAP_RESET,
    ACTION_MAP_CANCEL,
    ACTION_MAP_DONE
} AppAction;

enum { SETTING_LAYOUT, SETTING_TRIGGERS, SETTING_DEADZONE, SETTING_POINTER,
       SETTING_STATS, SETTING_FAST_INPUT, SETTING_RESOLUTION, SETTING_BITRATE,
       SETTING_FILTER, SETTING_GYRO, SETTING_GYRO_SPEED,
       SETTING_THEME, SETTING_VOLUME, SETTING_MENU_AUDIO, SETTING_LID,
       SETTING_CONNECTION, SETTING_GUIDE,
       SETTING_UPDATES, SETTING_AUTO_UPDATE, SETTING_UPDATE_CHANNEL,
       SETTING_ACCOUNT, SETTING_COUNT };

/* Library tabs (L / R). */
enum { LIBRARY_TAB_ALL, LIBRARY_TAB_FAVOURITES, LIBRARY_TAB_RECENT, LIBRARY_TAB_COUNT };

/* Per-game options sheet rows. */
enum { OPTION_BITRATE, OPTION_GYRO, OPTION_LAYOUT, OPTION_MAPPING, OPTION_CONNECTION, OPTION_COUNT };

#define GUIDE_PAGES 5

/* The stream menu is a 2 x 4 grid, read row by row. */
enum { STREAM_MENU_RESUME, STREAM_MENU_SCREENSHOT, STREAM_MENU_CONTROLS, STREAM_MENU_ZONE,
       STREAM_MENU_GYRO, STREAM_MENU_SOUND, STREAM_MENU_LAYOUT, STREAM_MENU_DISCONNECT,
       STREAM_MENU_COUNT };

#define LIBRARY_ROWS 6

typedef struct {
    GfnClient *client;
    NvstSignal *signal;
    WebRtcTransport *transport;
    AppSettings settings;

    AppView view;
    bool settings_open;
    /* The details page of the selected library game, and its store. */
    bool details_open;
    unsigned details_variant;
    /* Per-game options sheet on the details page. */
    bool options_open;
    int options_index;
    /* Button mapping editor: the input being edited and the working map. */
    bool mapping_open;
    int mapping_input;
    unsigned char mapping[GFN_INPUT_COUNT];
    unsigned char mapping_default[GFN_INPUT_COUNT];
    /* Library tab and the visible list: positions -> client->games. */
    int library_tab;
    unsigned short list_map[GFN_MAX_GAMES];
    size_t list_count;
    /* First-run guide page, -1 when closed. */
    int guide_page;
    /* Software update page and the one-time "what's new" page. */
    bool update_open;
    bool whats_new_open;
    int notes_scroll;
    char whats_new_version[32];
    const char *whats_new_notes;
    AppModal modal;
    char modal_title[48];
    char modal_jp[24];
    char modal_text[192];

    size_t selected;
    size_t list_top;
    /* Position in the grouped settings list (see screens_setting_at). */
    int setting_index;
    char search_text[80];

    char game_title[96];
    /* The game being launched or played (for its art). */
    const GfnGame *current_game;
    char game_store[24];
    bool genshin_session;

    bool keyboard_open;
    bool stream_menu;
    int stream_menu_index;
    /* Controls reference sheet on the lower screen. */
    bool controls_open;
    /* Zoom zone being shown (-1 = none) and a temporary sound mute. */
    int zone_index;
    bool sound_muted;
    /* When video first appeared this session (0 before), for the timer. */
    u64 stream_started_at;
    /* NVIDIA queued this session: almost certainly a free-tier rig, which
     * ends after one hour. */
    bool free_tier_guess;
    /* Automatic reconnects after the connection dropped mid-game. */
    unsigned reconnect_attempt;
    /* Estimated seconds left in NVIDIA's queue (-1 unknown, 0 any moment). */
    int queue_eta;
    /* Transient message (notice) with its expiry, shown as a toast. */
    const char *toast;
    unsigned stream_frame_base;
    unsigned fps;
    /* Video packets re-sent in the last second: the stutter indicator. */
    unsigned resent_per_second;

    bool touching;
    int touch_x, touch_y;

    /* A network job is running on the worker; shown as a non-blocking overlay. */
    const char *busy;
    /* One-line status for the lower screen's strip, chosen by main each frame. */
    const char *status;
    bool video_stalled;

    unsigned wifi_bars;
    unsigned battery_level;
    bool charging;
} App;

/* The game at a position of the visible library list, or NULL. */
static inline const GfnGame *app_game(const App *app, size_t position)
{
    return position < app->list_count ? &app->client->games[app->list_map[position]] : NULL;
}

/* Drawing: top is skipped while video owns the top framebuffer. */
void screens_draw_top(const App *app);
void screens_draw_bottom(const App *app);
/* Resolve a tap on the lower screen to an action for the current view. */
AppAction screens_touch(const App *app, int x, int y);
/* Stream view: L3/R3/Guide held by the current touch. */
uint16_t screens_stream_held_buttons(const App *app, int x, int y);
/* Stream view: the centre panel (touchpad or zoom map). */
UiRect screens_stream_panel(void);
/* Settings helpers shared by input handling and drawing. The settings list
 * is grouped into sections; positions map to SETTING_* ids. */
int screens_setting_at(int position);
void screens_setting_change(App *app, int setting, int direction);
/* The options row a tap landed on (with ACTION_OPTION_PREV/NEXT). */
int screens_touched_option_row(void);
/* The lower screen is mid-animation and wants redrawing every frame. */
bool screens_bottom_animating(void);
