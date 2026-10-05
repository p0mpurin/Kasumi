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
    MODAL_ERROR,
    /* A game found still running at start-up: resume it or end it. */
    MODAL_RESUME,
    /* Opt-in diagnostic report: what is sent, then the code to share. */
    MODAL_SEND_REPORT,
    MODAL_REPORT_SENT,
    /* Asked once: send reports automatically when something goes wrong? */
    MODAL_SHARE_ASK,
    /* Another GeForce NOW session holds the slot: resume it or end it. */
    MODAL_CONFLICT,
    /* Waiting for NVIDIA to free the slot; the launch retries on a timer. */
    MODAL_LIMIT_WAIT,
    /* Take this game's shortcut off the HOME Menu? */
    MODAL_SHORTCUT_REMOVE,
    /* Signing in where a partner runs GeForce NOW: which account? */
    MODAL_PROVIDER_PICK,
    /* Delete the screenshot on screen? */
    MODAL_DELETE_SHOT
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
    /* The C-STICK button by PS, or HIDE on the pad: show or hide the pad. */
    ACTION_LOOK_TOGGLE,
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
    /* Game page: add (or remove) its HOME Menu shortcut. */
    ACTION_SHORTCUT,
    /* Settings grid: a section tile (screens_touched_section). */
    ACTION_SETTINGS_SECTION,
    /* Screenshot viewer. */
    ACTION_GALLERY_PREV,
    ACTION_GALLERY_NEXT,
    ACTION_GALLERY_DELETE,
    ACTION_GALLERY_CLOSE,
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
    ACTION_DISCORD_CLOSE,
    ACTION_MAP_PREV,
    ACTION_MAP_NEXT,
    ACTION_MAP_RESET,
    ACTION_MAP_CANCEL,
    ACTION_MAP_DONE,
    /* A row of the mapping card (screens_touched_map_field). */
    ACTION_MAP_FIELD,
    /* The MIC button in a game: mute or unmute voice chat. */
    ACTION_MIC_TOGGLE,
    ACTION_CONTINUE
} AppAction;

enum { SETTING_LAYOUT, SETTING_PAD_NAMES, SETTING_MAPPING, SETTING_TRIGGERS, SETTING_DEADZONE, SETTING_POINTER,
       SETTING_STATS, SETTING_FAST_INPUT, SETTING_RESOLUTION, SETTING_BITRATE,
       SETTING_FILTER, SETTING_GYRO, SETTING_GYRO_SPEED,
       SETTING_THEME, SETTING_VOLUME, SETTING_MENU_AUDIO, SETTING_LID,
       SETTING_CONNECTION, SETTING_NETWORK, SETTING_SERVER, SETTING_GUIDE, SETTING_REPORT, SETTING_SHARE, SETTING_SHARE_STATS,
       SETTING_UPDATES, SETTING_AUTO_UPDATE, SETTING_UPDATE_CHANNEL,
       SETTING_PROVIDER, SETTING_ACCOUNT, SETTING_COMMUNITY, SETTING_MUSIC, SETTING_VOICE, SETTING_SFX,
       SETTING_CAMERA_SPEED, SETTING_CAMERA_INVERT, SETTING_SCREENSHOTS,
       SETTING_VIDEO_SHARPEN, SETTING_VIDEO_COLOR, SETTING_TOUCH_CAMERA, SETTING_TOUCH_STICK_SIZE,
       SETTING_FRAME_RATE, SETTING_MIC, SETTING_COUNT };

/* Library tabs (L / R). */
enum { LIBRARY_TAB_ALL, LIBRARY_TAB_FAVOURITES, LIBRARY_TAB_RECENT, LIBRARY_TAB_COUNT };

/* Per-game options sheet rows. */
enum { OPTION_BITRATE, OPTION_CAMERA_SPEED, OPTION_CAMERA_INVERT, OPTION_TOUCH_CAMERA, OPTION_GYRO,
       OPTION_GYRO_SPEED, OPTION_LAYOUT, OPTION_MAPPING, OPTION_CONNECTION, OPTION_COUNT };

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
    /* Button mapping editor: the input being edited, which row of it
     * (0 sends, 1 also sends, 2 mode), the working map and what RESET
     * goes back to. mapping_global: Settings' map for every game, else the
     * selected game's own. */
    bool mapping_open;
    bool mapping_global;
    int mapping_input;
    int mapping_field;
    GfnButtonMap mapping;
    GfnButtonMap mapping_default;
    /* Library tab and the visible list: positions -> client->games. */
    int library_tab;
    unsigned short list_map[GFN_MAX_GAMES];
    size_t list_count;
    /* First-run guide page, -1 when closed. */
    int guide_page;
    /* Software update page and the one-time "what's new" page. */
    bool update_open;
    bool whats_new_open;
    /* The one-time Discord invite card. */
    bool discord_open;
    int notes_scroll;
    char whats_new_version[32];
    const char *whats_new_notes;
    AppModal modal;
    char modal_title[48];
    char modal_jp[24];
    char modal_text[192];
    /* The code of the diagnostic report just sent ("K7F-2QX"). */
    char report_code[16];

    size_t selected;
    size_t list_top;
    /* Position in the grouped settings list (see screens_setting_at). */
    int setting_index;
    /* Settings opens on a grid of sections (-1); then one section's list.
     * settings_grid is the grid's highlighted tile. */
    int settings_section;
    int settings_grid;
    /* The screenshot viewer, over Settings (gallery.h). */
    bool gallery_open;
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
    /* The signalling said the session is gone: one check with CloudMatch
     * (it may only be paused) before giving up. */
    bool recover_tried;
    /* This session's [END] line is written (see log_session_end). */
    bool end_logged;
    /* Automatic retries of a connection that failed before the first frame. */
    unsigned setup_retries;
    /* Launch waiting for NVIDIA to free the slot: give up at `until`, next
     * try at `retry_at` (0 = not waiting). */
    u64 limit_wait_until, limit_retry_at;
    /* The conflicting session is this same game (Resume) or another (End). */
    bool conflict_same_game;
    /* The session in the way can't be closed from here (see gfn_client.h). */
    bool limit_unclosable;
    /* The wait is NVIDIA's rate limit (429), not a busy slot. */
    bool limit_rate;
    /* The wait is for NVIDIA's limited mode (busy), not a session slot. */
    bool limit_busy;
    /* Weak / hotspot switched on for this session because the last one on
     * this network was choppy. */
    bool auto_weak;
    /* Estimated seconds left in NVIDIA's queue (-1 unknown, 0 any moment). */
    int queue_eta;
    /* The most recently played library game (-1 none), for "Continue". */
    int continue_index;
    /* Lid closed mid-game with "Pause": screens off, sound and input held. */
    bool lid_paused;
    /* The connection dropped while Wi-Fi was off (the system turns it off
     * with the lid shut): waiting for it before reconnecting. */
    bool waiting_wifi;
    /* The "Welcome back" card after a lid pause: when it started (0 none)
     * and how long the lid was shut. */
    u64 welcome_at;
    unsigned welcome_away_s;
    /* Transient message (notice) with its expiry, shown as a toast. */
    const char *toast;
    unsigned stream_frame_base;
    unsigned fps;
    /* Video packets re-sent in the last second: the stutter indicator. */
    unsigned resent_per_second;

    bool touching;
    int touch_x, touch_y;
    /* Touch camera, driven by main.c and drawn by screens.c. look_mode is
     * 0 while the layout is not showing, else 1 trackpad or 2 stick. */
    unsigned look_mode;
    /* The touch camera is on for this game, pad showing or not (C-STICK). */
    bool look_available;
    bool look_active;
    int look_anchor_x, look_anchor_y;
    /* The finger as the pad last saw it (kept through a dropped touch). */
    int look_x, look_y;
    /* The touch C-stick's rim: full push this far from its centre (px). */
    float look_radius;
    float look_amount;
    u64 look_released_at;
    int look_release_x, look_release_y;
    int look_trail_x[10], look_trail_y[10];
    u64 look_trail_at[10];
    unsigned look_trail_head;
    /* R3 held by a double tap on the pad (drawn lit). */
    bool look_r3;
    /* L3 / R3 / PS held from the lower screen this frame (main.c, which
     * also spots two fingers on L3 and R3). */
    uint16_t touch_buttons;
    /* Voice chat is running this game (the MIC button shows). */
    bool mic_available;

    /* A network job is running on the worker; shown as a non-blocking overlay. */
    const char *busy;
    /* One-line status for the lower screen's strip, chosen by main each frame. */
    const char *status;
    bool video_stalled;

    unsigned wifi_bars;
    unsigned battery_level;
    bool charging;

    /* The HOME Menu shortcut sheet over the game page (SHORTCUT_SHEET_*):
     * progress while it is made, then the result. */
    int shortcut_sheet;
    char shortcut_message[128];
} App;

enum {
    SHORTCUT_SHEET_NONE,
    SHORTCUT_SHEET_WORKING,
    SHORTCUT_SHEET_ADDED,
    SHORTCUT_SHEET_REMOVED,
    SHORTCUT_SHEET_FAILED
};

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
/* The touch camera's pad and its R3 corner, while look_mode is on. */
UiRect screens_look_pad(void);
UiRect screens_look_r3(void);
UiRect screens_look_hide(void);
UiRect screens_look_both(void);
UiRect screens_look_mic(void);
/* The mapping card row a tap landed on (with ACTION_MAP_*), or -1. */
int screens_touched_map_field(void);
/* Settings helpers shared by input handling and drawing. The settings list
 * is grouped into sections; positions map to SETTING_* ids. */
int screens_setting_at(int position);
int screens_section_count(void);
/* The section a position is in, and a section's first position and size. */
int screens_section_of(int position);
int screens_section_first(int section);
int screens_section_size(int section);
/* The settings tile a tap landed on (with ACTION_SETTINGS_SECTION). */
int screens_touched_section(void);
void screens_setting_change(App *app, int setting, int direction);
/* The options row a tap landed on (with ACTION_OPTION_PREV/NEXT). */
int screens_touched_option_row(void);
/* The lower screen is mid-animation and wants redrawing every frame. */
bool screens_bottom_animating(void);
