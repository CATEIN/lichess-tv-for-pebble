#include <pebble.h>

#define BOARD_TOP         30
#define STATUS_HEIGHT     26
#define STALE_TIMEOUT_MS  20000
#define STREAM_CHECK_TIMEOUT_MS 6000
#define NUM_PIECE_TYPES   12
#define NUM_CHANNELS      15

// --- Lichess Broadcasts -------------------------------------------------
//
// The channel menu gets one extra row ("Live Broadcasts", row index
// NUM_CHANNELS) that leads to a broadcast list, then a live-game list,
// both populated dynamically over AppMessage by index.js (see
// MESSAGE_KEY_BroadcastIndex / MESSAGE_KEY_GameIndex below) since neither
// list is known at compile time the way the TV channel list is.
#define MAX_BROADCASTS        20
#define MAX_BROADCAST_GAMES   12
#define BROADCAST_ID_LEN      12
#define BROADCAST_NAME_LEN    72
#define BROADCAST_PLAYER_LEN  32

#define MAX_STREAMERS          20
#define STREAMER_NAME_LEN      24

#define TITLE_NAME_GAP    3
#define NAME_WING_GAP     3
#define WING_ICON_SIZE    14
#define CLOCK_WIDTH       70
#define ROW_HEIGHT        16

#define TITLE_COLOR       GColorFromRGB(0xC5, 0x8C, 0x18)

// Head-to-head score badge (small green box) shown to the right of each
// player's rating - see SCORE_BADGE_COLOR / layout_score_badge below.
#define SCORE_BADGE_HEIGHT   14
#define SCORE_BADGE_PAD_X    4
#define SCORE_ELO_GAP        4
#define SCORE_BADGE_COLOR    GColorGreen

// Scrolling ("marquee") status text - used when the status line (channel/
// broadcast name plus "Lichess TV"/"live" etc) is too wide to fit, e.g.
// "Racing Kings" or a long tournament name. Runs a single pass per trigger:
// parked showing the text truncated with an ellipsis -> after a dwell,
// scrolls left until the end of the text is on screen -> pauses there
// briefly -> back to parked/truncated, and stops there. A fresh pass only
// starts again when explicitly (re)triggered - see status_scroll_timer_
// callback for the state machine and refresh_status_scroll_state /
// restart_status_scroll_pass for the trigger points (text actually
// changing - which covers a channel switch or pausing/unpausing, since
// both change the status text - or the board sliding up/down).
#define STATUS_SCROLL_STEP_PX          2
#define STATUS_SCROLL_INTERVAL_MS      40
#define STATUS_SCROLL_PAUSE_AT_END_MS  500
#define STATUS_SCROLL_PAUSE_AT_START_MS 1200
#define STATUS_FONT_KEY                FONT_KEY_GOTHIC_18_BOLD

// Broadcast-list row marquee (see restart_broadcast_row_marquee below) -
// character-granular rather than pixel-granular, so a plain estimate of
// the default menu cell's left/right padding is good enough here.
#define ROW_SCROLL_CHAR_INTERVAL_MS    140
#define BROADCAST_ROW_TEXT_PAD_PX      20

// Shared marquee state machine, used by both the status bar above and the
// broadcast-list row marquee (see broadcast_row_scroll_timer_callback).
typedef enum {
    MARQUEE_PARKED = 0,   // static, showing the truncated/ellipsis view
    MARQUEE_SCROLLING,    // actively scrolling toward the end of the text
    MARQUEE_END_PAUSE     // reached the end, paused there before resetting
} MarqueePhase;

// Small "button feedback" nudge of the board while paused in move-review
// mode (UP/DOWN no longer slide the info panel there, so this stands in as
// a tactile acknowledgement that the press registered). A stronger jitter
// is used for the "hit the end of history" and enter/exit-review cases so
// those stand out from ordinary move-to-move navigation.
#define JITTER_PIXELS_WEAK    3
#define JITTER_PIXELS_STRONG  6
#define JITTER_OUT_MS         70
#define JITTER_BACK_MS        90

// How fast UP/DOWN step through history while held down in paused/review
// mode (see s_up_hold_timer / s_down_hold_timer below). A quick tap has to
// clear HOLD_INITIAL_DELAY_MS before auto-repeat kicks in at all, so an
// ordinary single press only ever moves one step - only a genuine hold
// reaches the faster HOLD_REPEAT_MS pace.
#define HOLD_INITIAL_DELAY_MS  400
#define HOLD_REPEAT_MS         130

#define PERSIST_KEY_THEME               1
#define PERSIST_KEY_SIZE                2
#define PERSIST_KEY_WRIST_FLICK         3
#define PERSIST_KEY_HIGHLIGHT           4
#define PERSIST_KEY_INACTIVITY_TIMEOUT  5
#define PERSIST_KEY_ROOT_CHOICE         6

typedef enum {
    HIGHLIGHT_OFF = 0,
    HIGHLIGHT_BLUE = 1,
    HIGHLIGHT_BLUE2 = 2,
    HIGHLIGHT_GREEN = 3,
    HIGHLIGHT_GREEN2 = 4
} HighlightMode;

#define NUM_HIGHLIGHT_MODES 5

typedef enum {
    BOARD_SIZE_BIG = 0,
    BOARD_SIZE_BIGGEST = 1
} BoardSize;

typedef struct {
    const char *name;
    GColor light_color;
    GColor dark_color;
} BoardTheme;

static const BoardTheme s_themes[] = {
    { "Brown",       GColorFromRGB(0xFF, 0xFF, 0xAA), GColorFromRGB(0xAA, 0xAA, 0x55) },
    { "Blue 2",      GColorFromRGB(0xAA, 0xAA, 0xAA), GColorFromRGB(0x55, 0x55, 0xAA) },
    { "Green",       GColorFromRGB(0xFF, 0xFF, 0xAA), GColorFromRGB(0x55, 0xAA, 0x55) },
    { "Olive",       GColorFromRGB(0xAA, 0xAA, 0x55), GColorFromRGB(0x55, 0x55, 0x00) },
    { "Gray",        GColorFromRGB(0xFF, 0xFF, 0xFF), GColorFromRGB(0xAA, 0xAA, 0xAA) },
    { "Purple",      GColorFromRGB(0xAA, 0x55, 0xAA), GColorFromRGB(0x55, 0x00, 0xAA) },
    { "Purple-Diag", GColorFromRGB(0xFF, 0xAA, 0xFF), GColorFromRGB(0xAA, 0x55, 0xAA) },
    { "Leather",     GColorFromRGB(0xFF, 0xAA, 0x55), GColorFromRGB(0xAA, 0x55, 0x00) },
    { "Pink",        GColorFromRGB(0xFF, 0xAA, 0xAA), GColorFromRGB(0xAA, 0x55, 0x55) }
};

#define NUM_THEMES (sizeof(s_themes) / sizeof(s_themes[0]))

static const GColor s_highlight_colors[NUM_HIGHLIGHT_MODES] = {
    GColorClear,
    GColorCeleste,
    GColorBabyBlueEyes,
    GColorMintGreen,
    GColorScreaminGreen,
};

static Window     *s_main_window;
static Layer      *s_board_layer;
static Layer      *s_status_layer;
static AppTimer   *s_stale_timer;
static AppTimer   *s_stream_check_timer;
static bool        s_awaiting_stream_check = false;

// Scrolling status text state (see status_layer_update_proc /
// refresh_status_scroll_state). s_status_display is whichever of s_status
// (live) or s_review_status_text (paused) is currently on screen - kept
// separate from both so re-measuring/scrolling never has to guess which
// source is active.
static char      s_status_display[64] = "";
static int        s_status_text_width = 0;
static int        s_status_scroll_offset = 0;
static bool        s_status_needs_scroll = false;
static MarqueePhase s_status_scroll_phase = MARQUEE_PARKED;
static AppTimer   *s_status_scroll_timer = NULL;

// Inactivity (auto-exit) timeout. 0 minutes means "Off" - no auto-exit.
static AppTimer   *s_inactivity_timer = NULL;
static uint32_t    s_inactivity_timeout_min = 0;

static Layer      *s_info_layer;
static TextLayer  *s_black_title_layer;
static TextLayer  *s_black_name_layer;
static TextLayer  *s_black_elo_layer;
static TextLayer  *s_black_clock_layer;
static Layer      *s_black_wing_layer;
static Layer      *s_black_score_layer;
static TextLayer  *s_white_title_layer;
static TextLayer  *s_white_name_layer;
static TextLayer  *s_white_elo_layer;
static TextLayer  *s_white_clock_layer;
static Layer      *s_white_wing_layer;
static Layer      *s_white_score_layer;

// Root chooser ("Lichess TV" / "Live Broadcasts") - the very first thing
// UP shows until the user has picked one or the other at least once (see
// s_root_choice / perform_up_step). Persisted (PERSIST_KEY_ROOT_CHOICE) so
// it's remembered across app restarts the same way the channel/broadcast
// selection itself is remembered by index.js.
typedef enum {
    ROOT_CHOICE_NONE = 0,
    ROOT_CHOICE_TV = 1,
    ROOT_CHOICE_BROADCAST = 2,
    ROOT_CHOICE_STREAMER = 3
} RootChoice;
static RootChoice s_root_choice = ROOT_CHOICE_NONE;
static Window     *s_root_window;
static MenuLayer  *s_root_menu_layer;

static Window     *s_channel_window;
static MenuLayer  *s_menu_layer;

static Window     *s_broadcast_window;
static MenuLayer  *s_broadcast_menu_layer;
static Window     *s_game_window;
static MenuLayer  *s_game_menu_layer;

static Window     *s_streamer_window;
static MenuLayer  *s_streamer_menu_layer;

static GBitmap *s_piece_bitmaps[NUM_PIECE_TYPES];
static GBitmap *s_patron_wing_bitmap;

static int s_current_theme_index = 0;
static BoardSize s_current_board_size = BOARD_SIZE_BIG;

static bool s_is_info_visible = false;
static PropertyAnimation *s_board_animation = NULL;

// Move-review mode: entered/exited with a single press of the SELECT
// button. While active, UP/DOWN page through the locally-stored move
// history (see s_move_history below) instead of their normal channel-menu
// / info-panel roles - no round trip to the phone needed - and the clocks
// stop ticking locally since we're looking at a frozen historical position
// rather than the live game.
static bool s_review_mode = false;

// Jitter animation state (see JITTER_* defines above / start_board_jitter).
static PropertyAnimation *s_jitter_animation = NULL;
static int s_jitter_base_x = 0;
static int s_jitter_base_y = 0;

// Auto-repeat timers for holding UP/DOWN in review mode (see HOLD_REPEAT_MS
// and perform_up_step/perform_down_step below). Only ever armed while
// s_review_mode is active - live mode's UP/DOWN presses stay strictly
// one-shot, driven straight off the raw button press with no timer at all.
static AppTimer *s_up_hold_timer = NULL;
static AppTimer *s_down_hold_timer = NULL;

static bool s_wrist_flick_enabled = false;
static bool s_board_flipped = false;

static int s_highlight_mode = HIGHLIGHT_OFF;
static char s_last_move[8] = "";

// --- Move history / review mode --------------------------------------
//
// The watch itself keeps the last moves so UP/DOWN can page through them
// instantly with no round trip to the phone. Every field here comes from
// data the phone already sends on each live board update (BoardFEN,
// LastMove, CheckSquare, the clocks, ActiveColor) plus one new short move
// label (LastMoveSAN) computed on the phone, where string work is much
// cheaper than it would be here.
//
// Two sizes are involved:
//  - MAX_MOVE_HISTORY (64) is the steady-state size used while live -
//    once full, each new move evicts the oldest one, same as before.
//  - MAX_MOVE_HISTORY_PAUSE_CAP (200) is a higher ceiling that only
//    applies while paused: nothing gets evicted out from under a paused
//    view just because the live game keeps producing moves in the
//    background, all the way up to this cap. The moment you resume, the
//    buffer is trimmed straight back down to the steady-state 64.
//
// Each entry is 88 bytes (83 bytes of char fields + 1 byte alignment
// padding + 4 bytes of int16_t fields), and the backing array has to be
// sized for the pause cap up front since it's a plain static array, not a
// dynamic allocation - so this reserves 200 * 88 = ~17.2KB, versus ~5.5KB
// for the steady-state 64. Emery's total app budget (code + heap combined)
// is 128KB, so this is about 13% of that - leaves plenty of room for
// everything else the app needs, and doesn't touch the OS's own memory at
// all (that's a separate pool Pebble apps can't reach regardless).
#define MAX_MOVE_HISTORY            64
#define MAX_MOVE_HISTORY_PAUSE_CAP  200

typedef struct {
    char board[65];       // board only, no move counters (matches s_board)
    char san[10];          // e.g. "Nf3", "Bxc6", "O-O", "exd8=Q"
    char last_move[8];     // uci form, for last-move-square highlighting
    int16_t clock_secs;    // the clock of whichever side just moved, -1 if unknown
    int16_t check_square;  // board index of the checked king, -1 if none
} MoveHistoryEntry;

static MoveHistoryEntry s_move_history[MAX_MOVE_HISTORY_PAUSE_CAP];
static int s_move_history_count = 0;

// -1 = no valid entry selected (either not reviewing, or reviewing with an
// empty history); otherwise an index into s_move_history.
static int s_review_index = -1;

// Scratch buffer for whatever's currently shown on the status layer while
// reviewing ("4:37  Kf3  Paused", "Last recorded move!", etc). Kept
// separate from the live s_status buffer (below) so entering/exiting
// review never has to guess at or reconstruct the live text - it's always
// sitting there ready to restore.
static char s_review_status_text[64] = "";

static char s_board[65] =
    "rnbqkbnr"
    "pppppppp"
    "........"
    "........"
    "........"
    "........"
    "PPPPPPPP"
    "RNBQKBNR";

static char s_status[64] = "Connecting to Lichess TV...";

static char s_white_title[8] = "";
static char s_white_name[32] = "White";
static char s_white_rating[16] = "";
static char s_white_clock[16] = "";
static bool s_white_patron = false;
// Head-to-head score vs. the current opponent (e.g. "1", "0.5", "1.5"),
// from Lichess's crosstable API - see index.js fetchCrosstable. Empty
// string means "no score to show" (e.g. first-ever meeting still in
// progress, or the fetch hasn't come back yet).
static char s_white_score[8] = "";

static char s_black_title[8] = "";
static char s_black_name[32] = "Black";
static char s_black_rating[16] = "";
static char s_black_clock[16] = "";
static bool s_black_patron = false;
static char s_black_score[8] = "";

// True whenever the phone is currently streaming a broadcast (tournament)
// game rather than a plain TV channel - set from MESSAGE_KEY_BroadcastActive
// (see in_recv_handler / index.js sendChannelSwitching). Drives the UP
// button's "show tournament players" behavior in perform_up_step.
static bool s_watching_broadcast = false;

// True whenever the phone is currently streaming a single streamer's
// game rather than a TV channel or broadcast - set from
// MESSAGE_KEY_StreamerActive alongside s_watching_broadcast above (same
// sendChannelSwitching message). Drives perform_up_step's UP-button
// "reopen the streamer list" behavior.
static bool s_watching_streamer = false;

static int s_check_square = -1;

static int s_white_clock_secs = -1;
static int s_black_clock_secs = -1;
static char s_active_color = 0;
static AppTimer *s_clock_tick_timer = NULL;

static int s_current_channel_index = 0;

typedef struct {
    const char *key;
    const char *label;
} ChannelDef;

static const ChannelDef s_channels[NUM_CHANNELS] = {
    { "",            "Top Rated" },
    { "bullet",      "Bullet" },
    { "blitz",       "Blitz" },
    { "rapid",       "Rapid" },
    { "classical",   "Classical" },
    { "chess960",    "Chess960" },
    { "crazyhouse",  "Crazyhouse" },
    { "antichess",   "Antichess" },
    { "atomic",      "Atomic" },
    { "horde",       "Horde" },
    { "racingKings", "Racing Kings" },
    { "threeCheck",  "Three-check" },
    { "ultraBullet", "UltraBullet" },
    { "bot",         "Bot" },
    { "computer",    "Computer" },
};

// Broadcast list (populated by MESSAGE_KEY_BroadcastIndex/Id/Name messages,
// terminated by MESSAGE_KEY_BroadcastListDone - see in_recv_handler).
typedef struct {
    char id[BROADCAST_ID_LEN];     // Lichess broadcast round ID
    char name[BROADCAST_NAME_LEN]; // "<tournament> - <round>" display label
} BroadcastListEntry;

static BroadcastListEntry s_broadcast_list[MAX_BROADCASTS];
static int  s_broadcast_list_count = 0;
static bool s_broadcast_list_loading = false;

// Marquee for the currently-*selected* row's title on the broadcast list
// screen - tournament names routinely run longer than the screen, and
// unlike the status bar this is a MenuLayer, whose rows are normally
// drawn with the stock menu_cell_basic_draw(). Rather than hand-replicate
// its internal layout/font to pixel-scroll, this reuses the exact same
// draw call and just varies *which characters of the name* it's given
// each tick - advancing a character offset shifts the visible window of
// the string toward the end, and once the tail fits without truncating
// (measured once, up front, when a pass starts - see
// restart_broadcast_row_marquee) that's the same "stop once the end of
// the text is on screen" behavior as the status bar, just character- 
// rather than pixel-granular. Only the selected row scrolls; every other
// row keeps the default single ellipsis-truncated look for free.
static AppTimer     *s_broadcast_row_scroll_timer = NULL;
static MarqueePhase  s_broadcast_row_scroll_phase = MARQUEE_PARKED;
static int           s_broadcast_row_scroll_char_offset = 0;
static int           s_broadcast_row_scroll_max_offset = 0;
static bool          s_broadcast_row_needs_scroll = false;
static int           s_broadcast_row_scroll_row = -1;

// Live-streamer list (populated by MESSAGE_KEY_StreamerIndex/Name
// messages, terminated by MESSAGE_KEY_StreamerListDone). The username
// doubles as both the display label and, sent back via
// MESSAGE_KEY_SelectedStreamer, the lookup key index.js uses to find
// their current game.
typedef struct {
    char name[STREAMER_NAME_LEN];
} StreamerListEntry;

static StreamerListEntry s_streamer_list[MAX_STREAMERS];
static int  s_streamer_list_count = 0;
static bool s_streamer_list_loading = false;

// Live-game list for whichever broadcast round was selected (populated by
// MESSAGE_KEY_GameIndex/White/Black messages, terminated by
// MESSAGE_KEY_GameListDone). pgn_index is the game's position among ALL
// (not just live) games in the round's PGN - index.js uses that same
// position to keep identifying this game once the live board stream
// starts, so it has to travel back out as MESSAGE_KEY_SelectedGame as-is.
typedef struct {
    int  pgn_index;
    char white[BROADCAST_PLAYER_LEN];
    char black[BROADCAST_PLAYER_LEN];
} BroadcastGameEntry;

static BroadcastGameEntry s_broadcast_games[MAX_BROADCAST_GAMES];
static int  s_broadcast_games_count = 0;
static bool s_broadcast_games_loading = false;

static const uint32_t s_piece_resource_ids_big[NUM_PIECE_TYPES] = {
    RESOURCE_ID_IMAGE_PIECE_WK_BIG,
    RESOURCE_ID_IMAGE_PIECE_WQ_BIG,
    RESOURCE_ID_IMAGE_PIECE_WR_BIG,
    RESOURCE_ID_IMAGE_PIECE_WB_BIG,
    RESOURCE_ID_IMAGE_PIECE_WN_BIG,
    RESOURCE_ID_IMAGE_PIECE_WP_BIG,
    RESOURCE_ID_IMAGE_PIECE_BK_BIG,
    RESOURCE_ID_IMAGE_PIECE_BQ_BIG,
    RESOURCE_ID_IMAGE_PIECE_BR_BIG,
    RESOURCE_ID_IMAGE_PIECE_BB_BIG,
    RESOURCE_ID_IMAGE_PIECE_BN_BIG,
    RESOURCE_ID_IMAGE_PIECE_BP_BIG,
};

static const uint32_t s_piece_resource_ids_biggest[NUM_PIECE_TYPES] = {
    RESOURCE_ID_IMAGE_PIECE_WK_BIGGEST,
    RESOURCE_ID_IMAGE_PIECE_WQ_BIGGEST,
    RESOURCE_ID_IMAGE_PIECE_WR_BIGGEST,
    RESOURCE_ID_IMAGE_PIECE_WB_BIGGEST,
    RESOURCE_ID_IMAGE_PIECE_WN_BIGGEST,
    RESOURCE_ID_IMAGE_PIECE_WP_BIGGEST,
    RESOURCE_ID_IMAGE_PIECE_BK_BIGGEST,
    RESOURCE_ID_IMAGE_PIECE_BQ_BIGGEST,
    RESOURCE_ID_IMAGE_PIECE_BR_BIGGEST,
    RESOURCE_ID_IMAGE_PIECE_BB_BIGGEST,
    RESOURCE_ID_IMAGE_PIECE_BN_BIGGEST,
    RESOURCE_ID_IMAGE_PIECE_BP_BIGGEST,
};

// Reloads s_piece_bitmaps from whichever resource set matches the current
// board size (see BOARD_SIZE_BIG / BOARD_SIZE_BIGGEST). Safe to call again
// later if the board size changes at runtime - destroys the old bitmaps
// first so nothing leaks.
static void load_piece_bitmaps_for_current_size(void) {
    const uint32_t *resource_ids = (s_current_board_size == BOARD_SIZE_BIGGEST)
        ? s_piece_resource_ids_biggest
        : s_piece_resource_ids_big;

    for (int i = 0; i < NUM_PIECE_TYPES; i++) {
        if (s_piece_bitmaps[i]) {
            gbitmap_destroy(s_piece_bitmaps[i]);
        }
        s_piece_bitmaps[i] = gbitmap_create_with_resource(resource_ids[i]);
    }
}

static void reset_stale_timer(void);
static void reset_inactivity_timer(void);
static int channel_key_to_index(const char *key);
static void apply_player_row_positions(void);
static void set_wrist_flick_enabled(bool enabled);
static void open_broadcast_list(void);
static void open_streamer_list(void);
static void open_current_broadcast_game_list(void);
static void refresh_status_scroll_state(void);
static void restart_status_scroll_pass(void);
static void restart_broadcast_row_marquee(void);
// Defined later alongside enter_review_mode/exit_review_mode; shared by the
// SELECT long-press handler and the accelerometer tap handler below.
static void toggle_pause_mode_dir(int jitter_dx);
static void toggle_pause_mode(void);

static int piece_char_to_index(char c) {
    switch (c) {
        case 'K': return 0;
        case 'Q': return 1;
        case 'R': return 2;
        case 'B': return 3;
        case 'N': return 4;
        case 'P': return 5;
        case 'k': return 6;
        case 'q': return 7;
        case 'r': return 8;
        case 'b': return 9;
        case 'n': return 10;
        case 'p': return 11;
        default:  return -1;
    }
}

static int32_t overshoot_curve(int32_t progress) {
    int64_t t = progress - ANIMATION_NORMALIZED_MAX;
    int64_t max = ANIMATION_NORMALIZED_MAX;
    int64_t s = (int64_t)(1.2 * max);
    int64_t t2 = (t * t) / max;
    int64_t t3 = (t2 * t) / max;
    int64_t result = max + ((s + max) * t3 / max) + (s * t2 / max);
    return (int32_t)result;
}

// The animation framework destroys an Animation automatically once it
// finishes playing to completion - manual animation_destroy() is only
// correct for cancelling one early. Without this handler, s_board_animation
// keeps pointing at an already-freed animation after it finishes on its
// own, and the next slide_board() call tries to destroy that stale handle,
// producing "Animation <id> does not exist" in the logs.
static void board_animation_stopped(Animation *animation, bool finished, void *context) {
    if ((Animation *)s_board_animation == animation) {
        s_board_animation = NULL;
    }
}

static void slide_board(bool slide_up) {
    if (!s_main_window || !s_board_layer) return;

    bool was_info_visible = s_is_info_visible;

    Layer *window_layer = window_get_root_layer(s_main_window);
    GRect bounds = layer_get_bounds(window_layer);

    int board_pixels;
    int board_left;

    if (s_current_board_size == BOARD_SIZE_BIGGEST) {
        int sq_size = bounds.size.w / 8;
        board_pixels = sq_size * 8;
        board_left = (bounds.size.w - board_pixels) / 2;
    } else {
        int sq_size = 24;
        board_pixels = sq_size * 8;
        board_left = (bounds.size.w - board_pixels) / 2;
    }

    int target_y;
    if (slide_up) {
        int info_height = (bounds.size.h * 32) / 100;
        target_y = bounds.size.h - info_height - board_pixels;
    } else {
        target_y = BOARD_TOP;
    }

    GRect start_frame = layer_get_frame(s_board_layer);
    GRect target_frame = GRect(board_left, target_y, board_pixels, board_pixels);

    if (s_board_animation) {
        // Still genuinely running (the .stopped handler would have nulled
        // this out otherwise) - cancel it before starting the new one.
        // animation_destroy() implicitly unschedules first if needed.
        animation_destroy((Animation*)s_board_animation);
        s_board_animation = NULL;
    }

    s_board_animation = property_animation_create_layer_frame(s_board_layer, &start_frame, &target_frame);
    Animation *anim = (Animation*)s_board_animation;
    animation_set_duration(anim, 240);
    animation_set_custom_curve(anim, overshoot_curve);
    animation_set_handlers(anim, (AnimationHandlers) {
        .started = NULL,
        .stopped = board_animation_stopped,
    }, NULL);
    animation_schedule(anim);

    s_is_info_visible = slide_up;

    if (slide_up != was_info_visible) {
        // The board was just slid up (to show player info) or back down
        // (to the board) - either way, give the status text a single
        // fresh pass rather than leaving it wherever it happened to be.
        restart_status_scroll_pass();
    }
}

static void update_board_layout(void) {
    if (!s_main_window || !s_board_layer) return;
    slide_board(s_is_info_visible);
    layer_mark_dirty(s_board_layer);
}

static bool parse_square(const char *s, int *out_idx) {
    if (!s || s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8') {
        return false;
    }
    int col = s[0] - 'a';
    int row = 8 - (s[1] - '0');
    *out_idx = row * 8 + col;
    return true;
}

static bool get_last_move_squares(const char *last_move, int *from_idx, int *to_idx) {
    if (!last_move || strlen(last_move) < 4) return false;
    if (!parse_square(last_move, from_idx)) return false;
    if (!parse_square(last_move + 2, to_idx)) return false;
    return true;
}

static void board_layer_update_proc(Layer *layer, GContext *ctx) {
    graphics_context_set_compositing_mode(ctx, GCompOpSet);

    GRect bounds = layer_get_bounds(layer);
    int sq_size = bounds.size.w / 8;

    BoardTheme current_theme = s_themes[s_current_theme_index];

    // While reviewing a historical position, paint from that move's own
    // snapshot (board/check-square/last-move) instead of the live game
    // state, which keeps advancing in the background.
    bool reviewing = s_review_mode && s_review_index >= 0 && s_review_index < s_move_history_count;
    MoveHistoryEntry *entry = reviewing ? &s_move_history[s_review_index] : NULL;

    const char *active_board = entry ? entry->board : s_board;
    int active_check_square = entry ? entry->check_square : s_check_square;
    const char *active_last_move = entry ? entry->last_move : s_last_move;

    int hl_from = -1, hl_to = -1;
    bool show_highlight = (s_highlight_mode != HIGHLIGHT_OFF) &&
        get_last_move_squares(active_last_move, &hl_from, &hl_to);
    GColor highlight_color = s_highlight_colors[s_highlight_mode];

    for (int rank = 0; rank < 8; rank++) {
        for (int file = 0; file < 8; file++) {
            int idx = s_board_flipped ? (63 - (rank * 8 + file)) : (rank * 8 + file);
            char c = active_board[idx];

            GRect square = GRect(file * sq_size, rank * sq_size, sq_size, sq_size);

            bool light_square = ((rank + file) % 2 == 0);
            bool is_highlighted = show_highlight && (idx == hl_from || idx == hl_to);
            bool is_check = (idx == active_check_square);

            GColor fill_color = is_check ? GColorRed : 
                (is_highlighted ? highlight_color : (light_square ? current_theme.light_color : current_theme.dark_color));

            graphics_context_set_fill_color(ctx, fill_color);
            graphics_fill_rect(ctx, square, 0, GCornerNone);

            int pidx = piece_char_to_index(c);
            if (pidx < 0 || !s_piece_bitmaps[pidx]) {
                continue;
            }

            GRect bmp_bounds = gbitmap_get_bounds(s_piece_bitmaps[pidx]);
            GRect piece_rect = GRect(
                square.origin.x + (sq_size - bmp_bounds.size.w) / 2,
                square.origin.y + (sq_size - bmp_bounds.size.h) / 2,
                bmp_bounds.size.w, bmp_bounds.size.h);
            graphics_draw_bitmap_in_rect(ctx, s_piece_bitmaps[pidx], piece_rect);
        }
    }
}

// Draws s_status_display centered/clipped-with-ellipsis while parked
// (whether it fits outright, or is too wide and waiting for its next
// pass), or scrolling/paused left across the screen while a pass is in
// progress - see refresh_status_scroll_state (starts the loop whenever
// the text changes) and restart_status_scroll_pass (re-starts it on
// returning to the board after navigating away).
static void status_layer_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);

    // The layer only repaints its own bounds each frame - explicitly clear
    // them first so a previous, wider scroll frame never leaves stray
    // pixels behind.
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    GFont font = fonts_get_system_font(STATUS_FONT_KEY);
    graphics_context_set_text_color(ctx, GColorBlack);

    if (s_status_needs_scroll &&
        (s_status_scroll_phase == MARQUEE_SCROLLING || s_status_scroll_phase == MARQUEE_END_PAUSE)) {
        graphics_draw_text(ctx, s_status_display, font,
            GRect(-s_status_scroll_offset, 0, s_status_text_width + 4, bounds.size.h),
            GTextOverflowModeFill, GTextAlignmentLeft, NULL);
        return;
    }

    // Parked - either it fits outright, or it's too wide and waiting out
    // its dwell before the next pass; GTextOverflowModeTrailingEllipsis
    // handles both identically (a plain clipped draw when it fits, an
    // automatic "..." truncation when it doesn't), e.g. "super long piece
    // of text information" parks as "super long piece of tex...".
    graphics_draw_text(ctx, s_status_display, font, bounds,
        GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

// Drives the parked(truncated) -> scrolling -> paused-at-end -> parked...
// loop, one step/phase-transition at a time.
static void status_scroll_timer_callback(void *data) {
    s_status_scroll_timer = NULL;
    if (!s_status_needs_scroll || !s_status_layer) return;

    if (s_status_scroll_phase == MARQUEE_PARKED) {
        // Dwell over - start scrolling from the beginning.
        s_status_scroll_phase = MARQUEE_SCROLLING;
        s_status_scroll_offset = 0;
        layer_mark_dirty(s_status_layer);
        s_status_scroll_timer = app_timer_register(STATUS_SCROLL_INTERVAL_MS, status_scroll_timer_callback, NULL);
        return;
    }

    if (s_status_scroll_phase == MARQUEE_END_PAUSE) {
        // Pause at the end is over. Land on the truncated/ellipsis resting
        // view and stop - no more scrolling until refresh_status_scroll_state
        // (the text actually changed) or restart_status_scroll_pass (board
        // slid up/down, or paused/unpaused - both change the status text,
        // which triggers the former) explicitly kicks off another pass.
        s_status_scroll_phase = MARQUEE_PARKED;
        s_status_scroll_offset = 0;
        layer_mark_dirty(s_status_layer);
        return;
    }

    // MARQUEE_SCROLLING: advance one step toward the end of the text.
    GRect bounds = layer_get_bounds(s_status_layer);
    int max_offset = s_status_text_width - bounds.size.w;
    if (max_offset < 0) max_offset = 0;

    s_status_scroll_offset += STATUS_SCROLL_STEP_PX;
    if (s_status_scroll_offset >= max_offset) {
        s_status_scroll_offset = max_offset;
        s_status_scroll_phase = MARQUEE_END_PAUSE;
        layer_mark_dirty(s_status_layer);
        s_status_scroll_timer = app_timer_register(STATUS_SCROLL_PAUSE_AT_END_MS, status_scroll_timer_callback, NULL);
        return;
    }

    layer_mark_dirty(s_status_layer);
    s_status_scroll_timer = app_timer_register(STATUS_SCROLL_INTERVAL_MS, status_scroll_timer_callback, NULL);
}

// Re-measures s_status_display against the status layer's width and kicks
// off a fresh parked-truncated -> scroll -> pause loop if it doesn't fit.
// Called any time the displayed text changes (apply_status_display) -
// cheap enough to do unconditionally.
static void refresh_status_scroll_state(void) {
    if (!s_status_layer) return;

    GRect bounds = layer_get_bounds(s_status_layer);
    GFont font = fonts_get_system_font(STATUS_FONT_KEY);

    GSize sz = graphics_text_layout_get_content_size(s_status_display, font,
        GRect(0, 0, 2000, bounds.size.h), GTextOverflowModeFill, GTextAlignmentLeft);
    s_status_text_width = sz.w;
    s_status_needs_scroll = (s_status_text_width > bounds.size.w);
    s_status_scroll_offset = 0;
    s_status_scroll_phase = MARQUEE_PARKED;

    if (s_status_scroll_timer) {
        app_timer_cancel(s_status_scroll_timer);
        s_status_scroll_timer = NULL;
    }
    if (s_status_needs_scroll) {
        s_status_scroll_timer = app_timer_register(STATUS_SCROLL_PAUSE_AT_START_MS, status_scroll_timer_callback, NULL);
    }

    layer_mark_dirty(s_status_layer);
}

// Re-arms the loop from the beginning without re-measuring anything - used
// when the board reappears after the user slid it away to look at player
// names/info (slide_board), so they get a fresh look at the full status
// text rather than it staying parked mid-loop.
static void restart_status_scroll_pass(void) {
    if (!s_status_needs_scroll || !s_status_layer) return;

    s_status_scroll_offset = 0;
    s_status_scroll_phase = MARQUEE_PARKED;
    if (s_status_scroll_timer) {
        app_timer_cancel(s_status_scroll_timer);
        s_status_scroll_timer = NULL;
    }
    s_status_scroll_timer = app_timer_register(STATUS_SCROLL_PAUSE_AT_START_MS, status_scroll_timer_callback, NULL);
    layer_mark_dirty(s_status_layer);
}

static void apply_status_display(const char *text) {
    // The status text gets re-set on every board update (a move, a clock
    // tick's worth of keepalive, etc.) even though the string itself is
    // usually unchanged (e.g. "Blitz \u2022 Lichess TV \u2022 live"). Only
    // touch the scroll state when the text actually changed, so a scroll
    // pass in progress isn't constantly restarted from the beginning by
    // board updates that have nothing to do with the status line.
    if (strncmp(s_status_display, text, sizeof(s_status_display) - 1) == 0) {
        return;
    }
    strncpy(s_status_display, text, sizeof(s_status_display) - 1);
    s_status_display[sizeof(s_status_display) - 1] = '\0';
    refresh_status_scroll_state();
}

static void update_status_text(void) {
    apply_status_display(s_status);
}

static void stream_check_timeout_callback(void *data) {
    s_stream_check_timer = NULL;
    if (!s_awaiting_stream_check) return;
    s_awaiting_stream_check = false;
    snprintf(s_status, sizeof(s_status), "TV feed lost, retrying...");
    if (!s_review_mode) update_status_text();
    reset_stale_timer();
}

static void stale_timer_callback(void *data) {
    s_stale_timer = NULL;

    s_awaiting_stream_check = true;
    snprintf(s_status, sizeof(s_status), "Checking stream...");
    if (!s_review_mode) update_status_text();

    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
        dict_write_uint8(iter, MESSAGE_KEY_CheckStream, 1);
        app_message_outbox_send();
    }

    if (s_stream_check_timer) {
        app_timer_cancel(s_stream_check_timer);
    }
    s_stream_check_timer = app_timer_register(STREAM_CHECK_TIMEOUT_MS, stream_check_timeout_callback, NULL);
}

static void reset_stale_timer(void) {
    if (s_stale_timer) {
        app_timer_cancel(s_stale_timer);
    }
    s_stale_timer = app_timer_register(STALE_TIMEOUT_MS, stale_timer_callback, NULL);
}

// Fires when the user hasn't pressed a button for s_inactivity_timeout_min
// minutes. Popping every window off the stack is the standard way to fully
// exit a Pebble app back to the watchface (saves battery on the always-on
// display / keeps the watch from being stuck showing a stale board).
static void inactivity_timeout_callback(void *data) {
    s_inactivity_timer = NULL;
    window_stack_pop_all(true);
}

// Called on every button press so the countdown restarts from zero, and
// whenever the timeout setting itself changes. A timeout of 0 minutes means
// "Off" - no timer is scheduled and any pending one is cancelled.
static void reset_inactivity_timer(void) {
    if (s_inactivity_timer) {
        app_timer_cancel(s_inactivity_timer);
        s_inactivity_timer = NULL;
    }
    if (s_inactivity_timeout_min > 0) {
        uint32_t timeout_ms = s_inactivity_timeout_min * 60u * 1000u;
        s_inactivity_timer = app_timer_register(timeout_ms, inactivity_timeout_callback, NULL);
    }
}

static void format_clock(int seconds, char *buf, size_t len) {
    if (seconds < 0) {
        buf[0] = '\0';
        return;
    }
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    if (h > 0) {
        snprintf(buf, len, "%d:%02d:%02d", h, m, s);
    } else {
        snprintf(buf, len, "%d:%02d", m, s);
    }
}

static void update_clock_layers(void) {
    format_clock(s_white_clock_secs, s_white_clock, sizeof(s_white_clock));
    if (s_white_clock_layer) text_layer_set_text(s_white_clock_layer, s_white_clock);

    format_clock(s_black_clock_secs, s_black_clock, sizeof(s_black_clock));
    if (s_black_clock_layer) text_layer_set_text(s_black_clock_layer, s_black_clock);
}

static void clock_tick_callback(void *data) {
    s_clock_tick_timer = NULL;

    if (s_review_mode) return;

    if (s_active_color == 'w' && s_white_clock_secs > 0) {
        s_white_clock_secs--;
    } else if (s_active_color == 'b' && s_black_clock_secs > 0) {
        s_black_clock_secs--;
    }
    update_clock_layers();

    if (s_active_color == 'w' || s_active_color == 'b') {
        s_clock_tick_timer = app_timer_register(1000, clock_tick_callback, NULL);
    }
}

static void restart_clock_tick(void) {
    if (s_clock_tick_timer) {
        app_timer_cancel(s_clock_tick_timer);
        s_clock_tick_timer = NULL;
    }
    if (s_review_mode) return;
    if (s_active_color == 'w' || s_active_color == 'b') {
        s_clock_tick_timer = app_timer_register(1000, clock_tick_callback, NULL);
    }
}

// Draws a small rounded, green-filled box with the player's head-to-head
// score centered inside (e.g. "1", "0.5", "1.5"). Which player's score to
// draw is determined by pointer identity against the two score layers -
// same trick used by wing_layer_update_proc, just per-side instead of
// shared, since the text itself differs between the two.
static void score_badge_update_proc(Layer *layer, GContext *ctx) {
    const char *text = (layer == s_white_score_layer) ? s_white_score : s_black_score;
    if (text[0] == '\0') return;

    GRect bounds = layer_get_bounds(layer);
    graphics_context_set_fill_color(ctx, SCORE_BADGE_COLOR);
    graphics_fill_rect(ctx, bounds, 2, GCornersAll);

    graphics_context_set_text_color(ctx, GColorBlack);
    // GOTHIC_14_BOLD carries a few pixels of built-in leading above the
    // glyphs' actual ink, so a text box whose center matches the badge's
    // center still renders with visibly more green above the digits than
    // below. Shifting the box up compensates for that leading so the
    // digits themselves - not just the box - end up centered in the badge.
    graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
        GRect(0, -3, bounds.size.w, bounds.size.h + 2), GTextOverflowModeFill,
        GTextAlignmentCenter, NULL);
}

// Positions/sizes a score badge at the start of the row (before the
// rating, "score elo" left to right), hiding it entirely when there's no
// score to show. Returns the x where the rating text should start - 4
// (the row's usual left margin) when there's no badge, or just past the
// badge plus a gap when there is. Called before laying out the rating
// text itself so the two never overlap.
static int layout_score_badge(Layer *badge_layer, const char *score_text, int y) {
    if (!s_main_window || !badge_layer) return 4;

    if (score_text[0] == '\0') {
        layer_set_hidden(badge_layer, true);
        return 4;
    }

    GFont badge_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
    GSize score_sz = graphics_text_layout_get_content_size(score_text, badge_font,
        GRect(0, 0, 60, 14), GTextOverflowModeFill, GTextAlignmentCenter);
    int badge_w = score_sz.w + SCORE_BADGE_PAD_X * 2;

    layer_set_frame(badge_layer, GRect(4, y, badge_w, SCORE_BADGE_HEIGHT));
    layer_set_hidden(badge_layer, false);
    layer_mark_dirty(badge_layer);

    return 4 + badge_w + SCORE_ELO_GAP;
}

static void layout_player_row(TextLayer *title_layer, TextLayer *name_layer, Layer *wing_layer,
                               const char *title_text, const char *name_text, bool patron, int y) {
    if (!s_main_window) return;
    Layer *window_layer = window_get_root_layer(s_main_window);
    GRect bounds = layer_get_bounds(window_layer);
    GFont bold_font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);

    int current_x = 4;

    if (wing_layer) {
        if (patron) {
            layer_set_frame(wing_layer, GRect(current_x, y + 1, WING_ICON_SIZE, WING_ICON_SIZE));
            layer_set_hidden(wing_layer, false);
            current_x += WING_ICON_SIZE + NAME_WING_GAP;
        } else {
            layer_set_hidden(wing_layer, true);
        }
    }

    int title_w = 0;
    if (title_text[0] != '\0') {
        GSize sz = graphics_text_layout_get_content_size(title_text, bold_font,
            GRect(0, 0, 60, ROW_HEIGHT), GTextOverflowModeFill, GTextAlignmentLeft);
        title_w = sz.w;
    }
    layer_set_frame(text_layer_get_layer(title_layer), GRect(current_x, y, title_w, ROW_HEIGHT));
    if (title_w > 0) {
        current_x += title_w + TITLE_NAME_GAP;
    }

    int max_name_w = bounds.size.w - CLOCK_WIDTH - current_x - 2;
    if (max_name_w < 0) max_name_w = 0;
    layer_set_frame(text_layer_get_layer(name_layer), GRect(current_x, y, max_name_w, ROW_HEIGHT));
}

static void apply_player_row_positions(void) {
    if (!s_main_window) return;
    Layer *window_layer = window_get_root_layer(s_main_window);
    GRect bounds = layer_get_bounds(window_layer);

    int black_y = s_board_flipped ? 36 : 2;
    int white_y = s_board_flipped ? 2 : 36;

    layout_player_row(s_black_title_layer, s_black_name_layer, s_black_wing_layer,
        s_black_title, s_black_name, s_black_patron, black_y);
    layout_player_row(s_white_title_layer, s_white_name_layer, s_white_wing_layer,
        s_white_title, s_white_name, s_white_patron, white_y);

    int black_elo_x = layout_score_badge(s_black_score_layer, s_black_score, black_y + 16);
    layer_set_frame(text_layer_get_layer(s_black_elo_layer),
        GRect(black_elo_x, black_y + 16, bounds.size.w - CLOCK_WIDTH - 4 - black_elo_x, 14));
    layer_set_frame(text_layer_get_layer(s_black_clock_layer),
        GRect(bounds.size.w - CLOCK_WIDTH - 4, black_y, CLOCK_WIDTH, 20));

    int white_elo_x = layout_score_badge(s_white_score_layer, s_white_score, white_y + 16);
    layer_set_frame(text_layer_get_layer(s_white_elo_layer),
        GRect(white_elo_x, white_y + 16, bounds.size.w - CLOCK_WIDTH - 4 - white_elo_x, 14));
    layer_set_frame(text_layer_get_layer(s_white_clock_layer),
        GRect(bounds.size.w - CLOCK_WIDTH - 4, white_y, CLOCK_WIDTH, 20));
}

// Flips the board (same effect as SELECT double-click) - fires on a
// detected wrist-flick tap gesture while set_wrist_flick_enabled(true) has
// this subscribed via accel_tap_service_subscribe.
static void accel_tap_handler(AccelAxisType axis, int32_t direction) {
    reset_inactivity_timer();
    s_board_flipped = !s_board_flipped;
    apply_player_row_positions();
    if (s_board_layer) {
        layer_mark_dirty(s_board_layer);
    }
}

static void set_wrist_flick_enabled(bool enabled) {
    if (enabled == s_wrist_flick_enabled) return;
    s_wrist_flick_enabled = enabled;
    if (enabled) {
        accel_tap_service_subscribe(accel_tap_handler);
    } else {
        accel_tap_service_unsubscribe();
        if (s_board_flipped) {
            s_board_flipped = false;
            apply_player_row_positions();
            layer_mark_dirty(s_board_layer);
        }
    }
}

static void wing_layer_update_proc(Layer *layer, GContext *ctx) {
    if (!s_patron_wing_bitmap) return;
    graphics_context_set_compositing_mode(ctx, GCompOpSet);
    graphics_draw_bitmap_in_rect(ctx, s_patron_wing_bitmap, layer_get_bounds(layer));
}

// Adjusts an in-progress review position after the oldest history entry
// gets dropped (see push_move_history): every remaining index just moved
// down by one. push_move_history guarantees this is never called with
// s_review_index == 0 (that case is handled by refusing to evict instead),
// so a plain decrement is always correct here.
static void shift_review_index_for_trim(void) {
    if (s_review_index > 0) {
        s_review_index--;
    }
}

static void push_move_history(const char *board64, const char *san, const char *last_move,
                               int clock_secs, int check_square) {
    // Live: steady-state cap, evict the oldest as usual. Paused: allow
    // growth all the way up to the higher pause cap instead, so nothing
    // gets evicted out from under a paused view just because the live
    // game keeps going in the background.
    int effective_cap = s_review_mode ? MAX_MOVE_HISTORY_PAUSE_CAP : MAX_MOVE_HISTORY;

    if (s_move_history_count == effective_cap) {
        if (s_review_mode && s_review_index == 0) {
            // We'd have to evict the exact entry currently on screen. Never
            // silently swap what the user is looking at - just stop
            // recording further live moves until they resume (an
            // extreme-length pause well past the cap, in practice).
            return;
        }
        memmove(&s_move_history[0], &s_move_history[1], sizeof(MoveHistoryEntry) * (effective_cap - 1));
        s_move_history_count--;
        if (s_review_mode) shift_review_index_for_trim();
    }

    MoveHistoryEntry *e = &s_move_history[s_move_history_count++];
    strncpy(e->board, board64, 64);
    e->board[64] = '\0';
    strncpy(e->san, san, sizeof(e->san) - 1);
    e->san[sizeof(e->san) - 1] = '\0';
    strncpy(e->last_move, last_move ? last_move : "", sizeof(e->last_move) - 1);
    e->last_move[sizeof(e->last_move) - 1] = '\0';
    e->clock_secs = (int16_t)clock_secs;
    e->check_square = (int16_t)check_square;
}

// Note: the history buffer is intentionally NEVER cleared just because a
// new game starts (channel switch, or the next featured game on the same
// channel). It simply keeps accumulating moves and, once full, rolls the
// oldest ones off exactly like any other overflow - so pausing right as a
// game ends never leaves you looking at an empty "No moves yet" screen.

static void in_recv_handler(DictionaryIterator *iter, void *context) {
    Tuple *board_tuple = dict_find(iter, MESSAGE_KEY_BoardFEN);
    Tuple *status_tuple = dict_find(iter, MESSAGE_KEY_StatusText);
    Tuple *theme_tuple = dict_find(iter, MESSAGE_KEY_BoardTheme);
    Tuple *size_tuple = dict_find(iter, MESSAGE_KEY_BoardSize);

    Tuple *wtitle_tuple = dict_find(iter, MESSAGE_KEY_WhiteTitle);
    Tuple *wname_tuple = dict_find(iter, MESSAGE_KEY_WhiteName);
    Tuple *welo_tuple = dict_find(iter, MESSAGE_KEY_WhiteRating);
    Tuple *wscore_tuple = dict_find(iter, MESSAGE_KEY_WhiteScore);
    Tuple *wclocksecs_tuple = dict_find(iter, MESSAGE_KEY_WhiteClockSecs);
    Tuple *wpatron_tuple = dict_find(iter, MESSAGE_KEY_WhitePatron);
    Tuple *btitle_tuple = dict_find(iter, MESSAGE_KEY_BlackTitle);
    Tuple *bname_tuple = dict_find(iter, MESSAGE_KEY_BlackName);
    Tuple *belo_tuple = dict_find(iter, MESSAGE_KEY_BlackRating);
    Tuple *bscore_tuple = dict_find(iter, MESSAGE_KEY_BlackScore);
    Tuple *bclocksecs_tuple = dict_find(iter, MESSAGE_KEY_BlackClockSecs);
    Tuple *bpatron_tuple = dict_find(iter, MESSAGE_KEY_BlackPatron);
    Tuple *active_tuple = dict_find(iter, MESSAGE_KEY_ActiveColor);
    Tuple *broadcast_active_tuple = dict_find(iter, MESSAGE_KEY_BroadcastActive);
    Tuple *streamer_active_tuple = dict_find(iter, MESSAGE_KEY_StreamerActive);

    Tuple *stream_alive_tuple = dict_find(iter, MESSAGE_KEY_StreamAlive);
    Tuple *last_move_tuple = dict_find(iter, MESSAGE_KEY_LastMove);
    Tuple *highlight_tuple = dict_find(iter, MESSAGE_KEY_BoardHighlight);
    Tuple *wristflick_tuple = dict_find(iter, MESSAGE_KEY_WristFlickEnabled);
    Tuple *inactivity_tuple = dict_find(iter, MESSAGE_KEY_InactivityTimeoutMin);

    if (board_tuple) {
        const char *incoming = board_tuple->value->cstring;
        if (strlen(incoming) == 64) {
            memcpy(s_board, incoming, 64);
            s_board[64] = '\0';
            if (!s_review_mode) layer_mark_dirty(s_board_layer);
        }
    }

    if (status_tuple) {
        strncpy(s_status, status_tuple->value->cstring, sizeof(s_status) - 1);
        s_status[sizeof(s_status) - 1] = '\0';
        if (!s_review_mode) update_status_text();
    }

    if (theme_tuple) {
        s_current_theme_index = theme_tuple->value->int32;
        if (s_current_theme_index < 0 || s_current_theme_index >= (int)NUM_THEMES) {
            s_current_theme_index = 0;
        }
        layer_mark_dirty(s_board_layer);
    }

    if (size_tuple) {
        BoardSize new_size = (BoardSize)size_tuple->value->int32;
        if (new_size != s_current_board_size) {
            s_current_board_size = new_size;
            load_piece_bitmaps_for_current_size();
        }
        update_board_layout();
    }

    bool white_row_changed = false;
    bool black_row_changed = false;

    if (wtitle_tuple) { strncpy(s_white_title, wtitle_tuple->value->cstring, sizeof(s_white_title) - 1); s_white_title[sizeof(s_white_title) - 1] = '\0'; text_layer_set_text(s_white_title_layer, s_white_title); white_row_changed = true; }
    if (wname_tuple) { strncpy(s_white_name, wname_tuple->value->cstring, sizeof(s_white_name) - 1); s_white_name[sizeof(s_white_name) - 1] = '\0'; text_layer_set_text(s_white_name_layer, s_white_name); white_row_changed = true; }
    if (welo_tuple) { strncpy(s_white_rating, welo_tuple->value->cstring, sizeof(s_white_rating) - 1); s_white_rating[sizeof(s_white_rating) - 1] = '\0'; text_layer_set_text(s_white_elo_layer, s_white_rating); white_row_changed = true; }
    if (wscore_tuple) { strncpy(s_white_score, wscore_tuple->value->cstring, sizeof(s_white_score) - 1); s_white_score[sizeof(s_white_score) - 1] = '\0'; white_row_changed = true; }
    if (wpatron_tuple) { s_white_patron = (wpatron_tuple->value->int32 != 0); white_row_changed = true; }

    if (btitle_tuple) { strncpy(s_black_title, btitle_tuple->value->cstring, sizeof(s_black_title) - 1); s_black_title[sizeof(s_black_title) - 1] = '\0'; text_layer_set_text(s_black_title_layer, s_black_title); black_row_changed = true; }
    if (bname_tuple) { strncpy(s_black_name, bname_tuple->value->cstring, sizeof(s_black_name) - 1); s_black_name[sizeof(s_black_name) - 1] = '\0'; text_layer_set_text(s_black_name_layer, s_black_name); black_row_changed = true; }
    if (belo_tuple) { strncpy(s_black_rating, belo_tuple->value->cstring, sizeof(s_black_rating) - 1); s_black_rating[sizeof(s_black_rating) - 1] = '\0'; text_layer_set_text(s_black_elo_layer, s_black_rating); black_row_changed = true; }
    if (bscore_tuple) { strncpy(s_black_score, bscore_tuple->value->cstring, sizeof(s_black_score) - 1); s_black_score[sizeof(s_black_score) - 1] = '\0'; black_row_changed = true; }
    if (bpatron_tuple) { s_black_patron = (bpatron_tuple->value->int32 != 0); black_row_changed = true; }

    // apply_player_row_positions repositions title/name/wing/elo/clock/score
    // together (and respects the current board-flip state), so route ANY
    // row change - name, rating, or score - through it rather than
    // repositioning pieces individually.
    if (white_row_changed || black_row_changed) {
        apply_player_row_positions();
    }

    if (broadcast_active_tuple) {
        s_watching_broadcast = (broadcast_active_tuple->value->int32 != 0);
    }
    if (streamer_active_tuple) {
        s_watching_streamer = (streamer_active_tuple->value->int32 != 0);
    }

    bool clock_state_changed = false;
    if (wclocksecs_tuple) { s_white_clock_secs = wclocksecs_tuple->value->int32; clock_state_changed = true; }
    if (bclocksecs_tuple) { s_black_clock_secs = bclocksecs_tuple->value->int32; clock_state_changed = true; }
    if (active_tuple) {
        const char *ac = active_tuple->value->cstring;
        s_active_color = (ac && ac[0]) ? ac[0] : 0;
        clock_state_changed = true;
    }
    if (clock_state_changed && !s_review_mode) {
        update_clock_layers();
        restart_clock_tick();
    }

    if (last_move_tuple) {
        strncpy(s_last_move, last_move_tuple->value->cstring, sizeof(s_last_move) - 1);
        s_last_move[sizeof(s_last_move) - 1] = '\0';
        if (!s_review_mode) layer_mark_dirty(s_board_layer);
    }

    if (highlight_tuple) {
        int mode = highlight_tuple->value->int32;
        if (mode < 0 || mode >= NUM_HIGHLIGHT_MODES) mode = HIGHLIGHT_OFF;
        s_highlight_mode = mode;
        layer_mark_dirty(s_board_layer);
    }

    if (wristflick_tuple) {
        set_wrist_flick_enabled(wristflick_tuple->value->int32 != 0);
    }

    if (inactivity_tuple) {
        int mins = inactivity_tuple->value->int32;
        if (mins < 0) mins = 0;
        s_inactivity_timeout_min = (uint32_t)mins;
        reset_inactivity_timer();
    }

    if (stream_alive_tuple) {
        if (s_stream_check_timer) {
            app_timer_cancel(s_stream_check_timer);
            s_stream_check_timer = NULL;
        }
        s_awaiting_stream_check = false;
        if (stream_alive_tuple->value->int32 == 0) {
            snprintf(s_status, sizeof(s_status), "TV feed lost, retrying...");
            if (!s_review_mode) update_status_text();
        }
    }

    Tuple *selected_tuple = dict_find(iter, MESSAGE_KEY_SelectedChannel);
    if (selected_tuple) {
        int idx = channel_key_to_index(selected_tuple->value->cstring);
        if (idx >= 0) {
            s_current_channel_index = idx;
        }
    }

    Tuple *check_tuple = dict_find(iter, MESSAGE_KEY_CheckSquare);
    if (check_tuple) {
        s_check_square = check_tuple->value->int32;
        if (!s_review_mode) layer_mark_dirty(s_board_layer);
    }

    Tuple *wflag_tuple = dict_find(iter, MESSAGE_KEY_WhiteFlagged);
    Tuple *bflag_tuple = dict_find(iter, MESSAGE_KEY_BlackFlagged);

    if (wflag_tuple && !s_review_mode) {
        bool white_flagged = (wflag_tuple->value->int32 != 0);
        text_layer_set_text_color(s_white_clock_layer, white_flagged ? GColorRed : GColorBlack);
    }
    if (bflag_tuple && !s_review_mode) {
        bool black_flagged = (bflag_tuple->value->int32 != 0);
        text_layer_set_text_color(s_black_clock_layer, black_flagged ? GColorRed : GColorBlack);
    }

    // Broadcast list entries arrive one MenuIndex at a time (see
    // fetchBroadcastList in index.js) so index.js can stream results in as
    // they're parsed rather than building one giant message; the final
    // MESSAGE_KEY_BroadcastListDone message carries the authoritative
    // count so a dropped item doesn't leave a stale trailing entry.
    Tuple *bidx_tuple = dict_find(iter, MESSAGE_KEY_BroadcastIndex);
    if (bidx_tuple) {
        int idx = bidx_tuple->value->int32;
        Tuple *bid_tuple = dict_find(iter, MESSAGE_KEY_BroadcastId);
        Tuple *bname_tuple = dict_find(iter, MESSAGE_KEY_BroadcastName);
        if (idx >= 0 && idx < MAX_BROADCASTS && bid_tuple && bname_tuple) {
            BroadcastListEntry *e = &s_broadcast_list[idx];
            strncpy(e->id, bid_tuple->value->cstring, sizeof(e->id) - 1);
            e->id[sizeof(e->id) - 1] = '\0';
            strncpy(e->name, bname_tuple->value->cstring, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
            if (idx + 1 > s_broadcast_list_count) s_broadcast_list_count = idx + 1;
            if (s_broadcast_menu_layer) {
                menu_layer_reload_data(s_broadcast_menu_layer);
                restart_broadcast_row_marquee();
            }
        }
    }

    Tuple *bdone_tuple = dict_find(iter, MESSAGE_KEY_BroadcastListDone);
    if (bdone_tuple) {
        Tuple *bcount_tuple = dict_find(iter, MESSAGE_KEY_BroadcastCount);
        if (bcount_tuple) {
            int count = bcount_tuple->value->int32;
            if (count < 0) count = 0;
            if (count > MAX_BROADCASTS) count = MAX_BROADCASTS;
            s_broadcast_list_count = count;
        }
        s_broadcast_list_loading = false;
        if (s_broadcast_menu_layer) {
            menu_layer_reload_data(s_broadcast_menu_layer);
            restart_broadcast_row_marquee();
        }
    }

    // Streamer list entries - same streamed-in-one-at-a-time pattern as
    // the broadcast list above (see fetchStreamerList in index.js).
    Tuple *sidx_tuple = dict_find(iter, MESSAGE_KEY_StreamerIndex);
    if (sidx_tuple) {
        int idx = sidx_tuple->value->int32;
        Tuple *sname_tuple = dict_find(iter, MESSAGE_KEY_StreamerName);
        if (idx >= 0 && idx < MAX_STREAMERS && sname_tuple) {
            StreamerListEntry *e = &s_streamer_list[idx];
            strncpy(e->name, sname_tuple->value->cstring, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
            if (idx + 1 > s_streamer_list_count) s_streamer_list_count = idx + 1;
            if (s_streamer_menu_layer) menu_layer_reload_data(s_streamer_menu_layer);
        }
    }

    Tuple *sdone_tuple = dict_find(iter, MESSAGE_KEY_StreamerListDone);
    if (sdone_tuple) {
        Tuple *scount_tuple = dict_find(iter, MESSAGE_KEY_StreamerCount);
        if (scount_tuple) {
            int count = scount_tuple->value->int32;
            if (count < 0) count = 0;
            if (count > MAX_STREAMERS) count = MAX_STREAMERS;
            s_streamer_list_count = count;
        }
        s_streamer_list_loading = false;
        if (s_streamer_menu_layer) menu_layer_reload_data(s_streamer_menu_layer);
    }

    // Live-game entries for the selected round, same streamed-in pattern.
    Tuple *gidx_tuple = dict_find(iter, MESSAGE_KEY_GameIndex);
    if (gidx_tuple) {
        Tuple *gwhite_tuple = dict_find(iter, MESSAGE_KEY_GameWhite);
        Tuple *gblack_tuple = dict_find(iter, MESSAGE_KEY_GameBlack);
        if (s_broadcast_games_count < MAX_BROADCAST_GAMES && gwhite_tuple && gblack_tuple) {
            BroadcastGameEntry *e = &s_broadcast_games[s_broadcast_games_count];
            e->pgn_index = gidx_tuple->value->int32;
            strncpy(e->white, gwhite_tuple->value->cstring, sizeof(e->white) - 1);
            e->white[sizeof(e->white) - 1] = '\0';
            strncpy(e->black, gblack_tuple->value->cstring, sizeof(e->black) - 1);
            e->black[sizeof(e->black) - 1] = '\0';
            s_broadcast_games_count++;
            if (s_game_menu_layer) menu_layer_reload_data(s_game_menu_layer);
        }
    }

    Tuple *gdone_tuple = dict_find(iter, MESSAGE_KEY_GameListDone);
    if (gdone_tuple) {
        s_broadcast_games_loading = false;
        if (s_game_menu_layer) menu_layer_reload_data(s_game_menu_layer);
    }

    Tuple *san_tuple = dict_find(iter, MESSAGE_KEY_LastMoveSAN);
    if (san_tuple && board_tuple) {
        // The mover is whichever side ISN'T "to move next" - s_active_color
        // was just updated above (from active_tuple, if present this
        // message) to reflect that.
        int mover_clock_secs = -1;
        if (s_active_color == 'w') mover_clock_secs = s_black_clock_secs;
        else if (s_active_color == 'b') mover_clock_secs = s_white_clock_secs;

        push_move_history(s_board, san_tuple->value->cstring, s_last_move,
            mover_clock_secs, s_check_square);
    }

    reset_stale_timer();
}

static void in_dropped_handler(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage dropped, reason: %d", (int)reason);
}

// Plain TV channel list - reached only via the root chooser now (see
// root_menu_select_callback / perform_up_step), not via an inline row of
// its own.
static uint16_t menu_get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
    return NUM_CHANNELS;
}

static void menu_draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
    menu_cell_basic_draw(ctx, cell_layer, s_channels[cell_index->row].label, NULL, NULL);
}

static int channel_key_to_index(const char *key) {
    for (int i = 0; i < NUM_CHANNELS; i++) {
        if (strcmp(s_channels[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

// Picking a channel should drop all the way back to the board, skipping
// both this window AND the root chooser beneath it - so remove the
// chooser from the stack first (a no-op if it isn't there, e.g. when this
// window was reached via the up-arrow skip-path in perform_up_step, which
// pushes the chooser hidden underneath rather than never pushing it).
static void menu_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
    reset_inactivity_timer();

    const char *key = s_channels[cell_index->row].key;
    const char *label = s_channels[cell_index->row].label;

    s_current_channel_index = cell_index->row;

    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
        dict_write_cstring(iter, MESSAGE_KEY_SelectedChannel, key);
        app_message_outbox_send();
    }

    snprintf(s_status, sizeof(s_status), "%s \u2022 switching...", label);
    update_status_text();

    window_stack_remove(s_root_window, false);
    window_stack_pop(true);
}

// --- Root chooser window ("Lichess TV" / "Live Broadcasts" / "Streamers") -

static uint16_t root_menu_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
    return 3;
}

static void root_menu_draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
    const char *label = "Streamers";
    if (cell_index->row == 0) label = "Lichess TV";
    else if (cell_index->row == 1) label = "Live Broadcasts";
    menu_cell_basic_draw(ctx, cell_layer, label, NULL, NULL);
}

static void root_menu_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
    reset_inactivity_timer();
    if (cell_index->row == 0) {
        s_root_choice = ROOT_CHOICE_TV;
        window_stack_push(s_channel_window, true);
    } else if (cell_index->row == 1) {
        s_root_choice = ROOT_CHOICE_BROADCAST;
        open_broadcast_list();
    } else {
        s_root_choice = ROOT_CHOICE_STREAMER;
        open_streamer_list();
    }
}

static void root_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    s_root_menu_layer = menu_layer_create(bounds);
    menu_layer_set_callbacks(s_root_menu_layer, NULL, (MenuLayerCallbacks) {
        .get_num_rows = root_menu_num_rows_callback,
        .draw_row = root_menu_draw_row_callback,
        .select_click = root_menu_select_callback,
    });
    menu_layer_set_click_config_onto_window(s_root_menu_layer, window);
    layer_add_child(window_layer, menu_layer_get_layer(s_root_menu_layer));
}

static void root_window_unload(Window *window) {
    menu_layer_destroy(s_root_menu_layer);
    s_root_menu_layer = NULL;
}

static void channel_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    s_menu_layer = menu_layer_create(bounds);
    menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks) {
        .get_num_rows = menu_get_num_rows_callback,
        .draw_row = menu_draw_row_callback,
        .select_click = menu_select_callback,
    });
    menu_layer_set_click_config_onto_window(s_menu_layer, window);
    menu_layer_set_selected_index(s_menu_layer,
        (MenuIndex) { .row = s_current_channel_index, .section = 0 },
        MenuRowAlignCenter, false);
    layer_add_child(window_layer, menu_layer_get_layer(s_menu_layer));
}

static void channel_window_unload(Window *window) {
    menu_layer_destroy(s_menu_layer);
}

// --- Broadcast list window ----------------------------------------------

// Sends MESSAGE_KEY_EnterBroadcastMode so index.js fetches
// https://lichess.org/api/broadcast, then pushes the (initially empty/
// "Loading...") broadcast list window - entries stream in afterwards via
// in_recv_handler as MESSAGE_KEY_BroadcastIndex/Id/Name messages.
static void open_broadcast_list(void) {
    s_broadcast_list_count = 0;
    s_broadcast_list_loading = true;
    if (s_broadcast_menu_layer) menu_layer_reload_data(s_broadcast_menu_layer);

    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
        dict_write_uint8(iter, MESSAGE_KEY_EnterBroadcastMode, 1);
        app_message_outbox_send();
    }

    window_stack_push(s_broadcast_window, true);
}

// Reopens the live-game list (see s_game_window below) for whichever
// broadcast round is currently being watched, without making the user
// browse there by hand - this is what the UP button uses while
// s_watching_broadcast is true (see perform_up_step). It still rebuilds
// the same navigation chain the normal browse flow would have (root
// chooser -> Live Broadcasts list -> this round's games) underneath the
// game list, hidden, so BACK retraces it properly instead of dropping
// straight to the board.
//
// index.js already remembers the current round ID, so this just asks it
// to re-run fetchBroadcastList()/fetchRoundGames() via
// MESSAGE_KEY_EnterBroadcastMode/ShowTournamentPlayers rather than the
// watch needing to track/send the round ID itself. Both go out in a
// single message (rather than two separate outbox transactions) since the
// outbox buffer can only hold one transaction at a time.
static void open_current_broadcast_game_list(void) {
    s_root_choice = ROOT_CHOICE_BROADCAST;
    window_stack_push(s_root_window, false);

    s_broadcast_list_count = 0;
    s_broadcast_list_loading = true;
    if (s_broadcast_menu_layer) menu_layer_reload_data(s_broadcast_menu_layer);
    window_stack_push(s_broadcast_window, false);

    s_broadcast_games_count = 0;
    s_broadcast_games_loading = true;
    if (s_game_menu_layer) menu_layer_reload_data(s_game_menu_layer);

    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
        dict_write_uint8(iter, MESSAGE_KEY_EnterBroadcastMode, 1);
        dict_write_uint8(iter, MESSAGE_KEY_ShowTournamentPlayers, 1);
        app_message_outbox_send();
    }

    window_stack_push(s_game_window, true);
}

static uint16_t broadcast_menu_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
    // Always at least one row so there's somewhere to show "Loading..."
    // or "No live broadcasts" while/if the real list is empty.
    return (s_broadcast_list_count > 0) ? (uint16_t)s_broadcast_list_count : 1;
}

static void broadcast_menu_draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
    if (s_broadcast_list_count == 0) {
        menu_cell_basic_draw(ctx, cell_layer,
            s_broadcast_list_loading ? "Loading Broadcasts..." : "No live broadcasts", NULL, NULL);
        return;
    }

    const char *name = s_broadcast_list[cell_index->row].name;

    // Only the currently-selected row (tracked by restart_broadcast_row_
    // marquee/broadcast_row_scroll_timer_callback) ever has a nonzero
    // offset, so every other row falls straight through to the normal,
    // full-name draw - which self-truncates with "..." via the stock
    // menu_cell_basic_draw() if it's too long to fit, same as before.
    if (cell_index->row == s_broadcast_row_scroll_row &&
        s_broadcast_row_needs_scroll && s_broadcast_row_scroll_char_offset > 0 &&
        s_broadcast_row_scroll_char_offset < (int)strlen(name)) {
        menu_cell_basic_draw(ctx, cell_layer, name + s_broadcast_row_scroll_char_offset, NULL, NULL);
        return;
    }

    menu_cell_basic_draw(ctx, cell_layer, name, NULL, NULL);
}

// Drives the same parked(truncated) -> scrolling -> paused-at-end ->
// parked... loop as the status bar (status_scroll_timer_callback), but in
// terms of a character offset into the selected row's name rather than a
// pixel offset - see restart_broadcast_row_marquee for how a pass starts
// and s_broadcast_row_scroll_max_offset for where it stops.
static void broadcast_row_scroll_timer_callback(void *data) {
    s_broadcast_row_scroll_timer = NULL;
    if (!s_broadcast_row_needs_scroll || !s_broadcast_menu_layer) return;

    // The selection may have moved on since this timer was scheduled;
    // selection_changed already restarts the marquee for the new row, so
    // just bail rather than animating the wrong one.
    MenuIndex sel = menu_layer_get_selected_index(s_broadcast_menu_layer);
    if ((int)sel.row != s_broadcast_row_scroll_row) return;

    if (s_broadcast_row_scroll_phase == MARQUEE_PARKED) {
        s_broadcast_row_scroll_phase = MARQUEE_SCROLLING;
        s_broadcast_row_scroll_char_offset = 0;
        layer_mark_dirty(menu_layer_get_layer(s_broadcast_menu_layer));
        s_broadcast_row_scroll_timer = app_timer_register(ROW_SCROLL_CHAR_INTERVAL_MS, broadcast_row_scroll_timer_callback, NULL);
        return;
    }

    if (s_broadcast_row_scroll_phase == MARQUEE_END_PAUSE) {
        // Land on the truncated/ellipsis resting view and stop - a fresh
        // pass only starts again via restart_broadcast_row_marquee (new
        // list data, or the selection actually moving to a different
        // row).
        s_broadcast_row_scroll_phase = MARQUEE_PARKED;
        s_broadcast_row_scroll_char_offset = 0;
        layer_mark_dirty(menu_layer_get_layer(s_broadcast_menu_layer));
        return;
    }

    // MARQUEE_SCROLLING
    s_broadcast_row_scroll_char_offset++;
    if (s_broadcast_row_scroll_char_offset >= s_broadcast_row_scroll_max_offset) {
        s_broadcast_row_scroll_char_offset = s_broadcast_row_scroll_max_offset;
        s_broadcast_row_scroll_phase = MARQUEE_END_PAUSE;
        layer_mark_dirty(menu_layer_get_layer(s_broadcast_menu_layer));
        s_broadcast_row_scroll_timer = app_timer_register(STATUS_SCROLL_PAUSE_AT_END_MS, broadcast_row_scroll_timer_callback, NULL);
        return;
    }

    layer_mark_dirty(menu_layer_get_layer(s_broadcast_menu_layer));
    s_broadcast_row_scroll_timer = app_timer_register(ROW_SCROLL_CHAR_INTERVAL_MS, broadcast_row_scroll_timer_callback, NULL);
}

// (Re)starts the marquee loop for whichever row is currently selected -
// called on window load, whenever new list data arrives (a row's name may
// have just been filled in), and on selection_changed. Measures the
// selected row's name against the menu's width using the same font the
// status bar marquee uses (a reasonable stand-in for the stock cell
// font - this is a character-granular approximation, not pixel-perfect,
// which is fine for deciding "does this need to scroll at all" and
// "how many leading characters until the tail fits").
static void restart_broadcast_row_marquee(void) {
    if (s_broadcast_row_scroll_timer) {
        app_timer_cancel(s_broadcast_row_scroll_timer);
        s_broadcast_row_scroll_timer = NULL;
    }
    s_broadcast_row_scroll_char_offset = 0;
    s_broadcast_row_scroll_phase = MARQUEE_PARKED;
    s_broadcast_row_needs_scroll = false;
    s_broadcast_row_scroll_row = -1;

    if (!s_broadcast_menu_layer || s_broadcast_list_count == 0) return;

    MenuIndex sel = menu_layer_get_selected_index(s_broadcast_menu_layer);
    if ((int)sel.row >= s_broadcast_list_count) return;
    s_broadcast_row_scroll_row = sel.row;

    const char *name = s_broadcast_list[sel.row].name;
    int avail_w = layer_get_bounds(menu_layer_get_layer(s_broadcast_menu_layer)).size.w - BROADCAST_ROW_TEXT_PAD_PX;
    if (avail_w < 10) avail_w = 10;
    GFont font = fonts_get_system_font(STATUS_FONT_KEY);

    GSize full_size = graphics_text_layout_get_content_size(name, font,
        GRect(0, 0, 2000, 40), GTextOverflowModeFill, GTextAlignmentLeft);
    if (full_size.w <= avail_w) return; // fits outright - default draw is enough

    s_broadcast_row_needs_scroll = true;

    int len = strlen(name);
    int cut;
    for (cut = 0; cut < len; cut++) {
        GSize sz = graphics_text_layout_get_content_size(name + cut, font,
            GRect(0, 0, 2000, 40), GTextOverflowModeFill, GTextAlignmentLeft);
        if (sz.w <= avail_w) break;
    }
    s_broadcast_row_scroll_max_offset = cut;

    layer_mark_dirty(menu_layer_get_layer(s_broadcast_menu_layer));
    s_broadcast_row_scroll_timer = app_timer_register(STATUS_SCROLL_PAUSE_AT_START_MS, broadcast_row_scroll_timer_callback, NULL);
}

static void broadcast_menu_selection_changed_callback(MenuLayer *menu_layer, MenuIndex new_index, MenuIndex old_index, void *data) {
    restart_broadcast_row_marquee();
}

// Sends MESSAGE_KEY_SelectedBroadcast (the chosen round's ID) so index.js
// fetches that round's PGN and reports back its still-live games, then
// pushes the (initially empty/"Loading...") game list window.
static void broadcast_menu_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
    reset_inactivity_timer();
    if (s_broadcast_list_count == 0) return;

    s_broadcast_games_count = 0;
    s_broadcast_games_loading = true;
    if (s_game_menu_layer) menu_layer_reload_data(s_game_menu_layer);

    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
        dict_write_cstring(iter, MESSAGE_KEY_SelectedBroadcast, s_broadcast_list[cell_index->row].id);
        app_message_outbox_send();
    }

    window_stack_push(s_game_window, true);
}

static void broadcast_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    s_broadcast_menu_layer = menu_layer_create(bounds);
    menu_layer_set_callbacks(s_broadcast_menu_layer, NULL, (MenuLayerCallbacks) {
        .get_num_rows = broadcast_menu_num_rows_callback,
        .draw_row = broadcast_menu_draw_row_callback,
        .select_click = broadcast_menu_select_callback,
        .selection_changed = broadcast_menu_selection_changed_callback,
    });
    menu_layer_set_click_config_onto_window(s_broadcast_menu_layer, window);
    layer_add_child(window_layer, menu_layer_get_layer(s_broadcast_menu_layer));

    restart_broadcast_row_marquee();
}

static void broadcast_window_unload(Window *window) {
    if (s_broadcast_row_scroll_timer) {
        app_timer_cancel(s_broadcast_row_scroll_timer);
        s_broadcast_row_scroll_timer = NULL;
    }
    s_broadcast_row_scroll_row = -1;
    menu_layer_destroy(s_broadcast_menu_layer);
    s_broadcast_menu_layer = NULL;
}


// --- Streamer list window -------------------------------------------------

// Sends MESSAGE_KEY_EnterStreamerMode so index.js fetches
// https://lichess.org/api/streamer/live, then pushes the (initially
// empty/"Loading...") streamer list window - entries stream in
// afterwards via in_recv_handler as MESSAGE_KEY_StreamerIndex/Name
// messages, mirroring open_broadcast_list above.
static void open_streamer_list(void) {
    s_streamer_list_count = 0;
    s_streamer_list_loading = true;
    if (s_streamer_menu_layer) menu_layer_reload_data(s_streamer_menu_layer);

    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
        dict_write_uint8(iter, MESSAGE_KEY_EnterStreamerMode, 1);
        app_message_outbox_send();
    }

    window_stack_push(s_streamer_window, true);
}

// Reopens the streamer list without making the user browse there by
// hand - this is what the UP button uses while s_watching_streamer is
// true (see perform_up_step), the streamer-mode equivalent of
// open_current_broadcast_game_list above. Unlike that function there's
// no third "games" level to rebuild underneath - a streamer only has one
// current game - so this just retraces root chooser -> streamer list.
static void open_current_streamer_list(void) {
    s_root_choice = ROOT_CHOICE_STREAMER;
    window_stack_push(s_root_window, false);
    open_streamer_list();
}

static uint16_t streamer_menu_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
    // Always at least one row so there's somewhere to show "Loading..."
    // or "No streamers live" while/if the real list is empty.
    return (s_streamer_list_count > 0) ? (uint16_t)s_streamer_list_count : 1;
}

static void streamer_menu_draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
    if (s_streamer_list_count == 0) {
        menu_cell_basic_draw(ctx, cell_layer,
            s_streamer_list_loading ? "Loading Streamers..." : "No streamers live", NULL, NULL);
        return;
    }
    menu_cell_basic_draw(ctx, cell_layer, s_streamer_list[cell_index->row].name, NULL, NULL);
}

// Sends MESSAGE_KEY_SelectedStreamer (the chosen streamer's username) so
// index.js looks up their current game and, if they're actually playing
// one right now, connects the board to it - then drops straight back to
// the board, same as picking a broadcast game (there's no games sub-list
// to show first, since a streamer only has one current game).
static void streamer_menu_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
    reset_inactivity_timer();
    if (s_streamer_list_count == 0) return;
    // Defensive: cell_index->row could in principle be stale relative to
    // s_streamer_list_count if the list shrank between render and this
    // click (e.g. a StreamerListDone with a smaller StreamerCount landed
    // mid-scroll), and MAX_STREAMERS bounds the array itself either way.
    if (cell_index->row >= s_streamer_list_count ||
        cell_index->row >= MAX_STREAMERS) {
        return;
    }

    StreamerListEntry *e = &s_streamer_list[cell_index->row];

    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
        dict_write_cstring(iter, MESSAGE_KEY_SelectedStreamer, e->name);
        app_message_outbox_send();
    }

    snprintf(s_status, sizeof(s_status), "%s \u2022 connecting...", e->name);
    update_status_text();

    // s_streamer_window is the currently-active top window (this click
    // handler is running inside it) - unlike s_channel_window/s_root_window
    // below, which are buried underneath it on the stack, it must NOT be
    // removed by name here. Explicitly removing the window whose own
    // click callback is still on the call stack crashes the app; the
    // window_stack_pop(true) below already pops whatever's currently on
    // top (i.e. this window) once the buried ones are gone, exactly like
    // game_menu_select_callback pops game_window implicitly rather than
    // removing it by name.
    window_stack_remove(s_channel_window, false);
    window_stack_remove(s_root_window, false);
    window_stack_pop(true);
}

static void streamer_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    s_streamer_menu_layer = menu_layer_create(bounds);
    menu_layer_set_callbacks(s_streamer_menu_layer, NULL, (MenuLayerCallbacks) {
        .get_num_rows = streamer_menu_num_rows_callback,
        .draw_row = streamer_menu_draw_row_callback,
        .select_click = streamer_menu_select_callback,
    });
    menu_layer_set_click_config_onto_window(s_streamer_menu_layer, window);
    layer_add_child(window_layer, menu_layer_get_layer(s_streamer_menu_layer));
}

static void streamer_window_unload(Window *window) {
    menu_layer_destroy(s_streamer_menu_layer);
    s_streamer_menu_layer = NULL;
}

// --- Broadcast game (live-game) list window ------------------------------

static uint16_t game_menu_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
    return (s_broadcast_games_count > 0) ? (uint16_t)s_broadcast_games_count : 1;
}

static void game_menu_draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
    if (s_broadcast_games_count == 0) {
        menu_cell_basic_draw(ctx, cell_layer,
            s_broadcast_games_loading ? "Loading Games..." : "No live games", NULL, NULL);
        return;
    }
    BroadcastGameEntry *e = &s_broadcast_games[cell_index->row];
    menu_cell_basic_draw(ctx, cell_layer, e->white, e->black, NULL);
}

// Sends MESSAGE_KEY_SelectedGame (that game's position in the round PGN)
// so index.js switches the live stream over to broadcast mode and starts
// following that game, then drops straight back to the board - removing
// s_broadcast_window, s_channel_window, AND s_root_window from underneath
// (not just the immediate parent) so the stack ends up as just
// [main_window], identical to the end state after a TV channel pick.
// s_channel_window is included defensively - it's not normally in the
// stack along this path anymore, but removal is a harmless no-op if it
// isn't present.
static void game_menu_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
    reset_inactivity_timer();
    if (s_broadcast_games_count == 0) return;

    BroadcastGameEntry *e = &s_broadcast_games[cell_index->row];

    DictionaryIterator *iter;
    if (app_message_outbox_begin(&iter) == APP_MSG_OK) {
        dict_write_int32(iter, MESSAGE_KEY_SelectedGame, e->pgn_index);
        app_message_outbox_send();
    }

    snprintf(s_status, sizeof(s_status), "%s \u2013 %s \u2022 connecting...", e->white, e->black);
    update_status_text();

    window_stack_remove(s_broadcast_window, false);
    window_stack_remove(s_channel_window, false);
    window_stack_remove(s_root_window, false);
    window_stack_pop(true);
}

static void game_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    s_game_menu_layer = menu_layer_create(bounds);
    menu_layer_set_callbacks(s_game_menu_layer, NULL, (MenuLayerCallbacks) {
        .get_num_rows = game_menu_num_rows_callback,
        .draw_row = game_menu_draw_row_callback,
        .select_click = game_menu_select_callback,
    });
    menu_layer_set_click_config_onto_window(s_game_menu_layer, window);
    layer_add_child(window_layer, menu_layer_get_layer(s_game_menu_layer));
}

static void game_window_unload(Window *window) {
    menu_layer_destroy(s_game_menu_layer);
    s_game_menu_layer = NULL;
}

static void jitter_back_stopped(Animation *animation, bool finished, void *context) {
    if ((Animation *)s_jitter_animation == animation) {
        s_jitter_animation = NULL;
    }
}

// Second leg of the jitter: slide back to the original resting position.
static void jitter_out_stopped(Animation *animation, bool finished, void *context) {
    if (!s_board_layer) return;

    GRect out_frame = layer_get_frame(s_board_layer);
    GRect back_frame = GRect(s_jitter_base_x, s_jitter_base_y, out_frame.size.w, out_frame.size.h);

    s_jitter_animation = property_animation_create_layer_frame(s_board_layer, &out_frame, &back_frame);
    Animation *anim = (Animation *)s_jitter_animation;
    animation_set_duration(anim, JITTER_BACK_MS);
    animation_set_handlers(anim, (AnimationHandlers) {
        .started = NULL,
        .stopped = jitter_back_stopped,
    }, NULL);
    animation_schedule(anim);
}

// A tiny nudge of the board (delta_x horizontally, delta_y vertically) and
// back, used as button-press feedback while paused in review mode. Ignores
// the press if a jitter is already mid-flight rather than stacking offsets.
static void start_board_jitter(int delta_x, int delta_y) {
    if (!s_main_window || !s_board_layer) return;
    if (s_jitter_animation) return;

    GRect start_frame = layer_get_frame(s_board_layer);
    s_jitter_base_x = start_frame.origin.x;
    s_jitter_base_y = start_frame.origin.y;
    GRect out_frame = GRect(start_frame.origin.x + delta_x, start_frame.origin.y + delta_y,
        start_frame.size.w, start_frame.size.h);

    s_jitter_animation = property_animation_create_layer_frame(s_board_layer, &start_frame, &out_frame);
    Animation *anim = (Animation *)s_jitter_animation;
    animation_set_duration(anim, JITTER_OUT_MS);
    animation_set_handlers(anim, (AnimationHandlers) {
        .started = NULL,
        .stopped = jitter_out_stopped,
    }, NULL);
    animation_schedule(anim);
}

// Puts arbitrary text on the status layer without touching the live
// s_status buffer (so the live text is always intact and ready to restore
// the moment review mode exits). text_layer_set_text keeps a pointer, not
// a copy, so the backing buffer has to be static.
static void set_review_status(const char *text) {
    strncpy(s_review_status_text, text, sizeof(s_review_status_text) - 1);
    s_review_status_text[sizeof(s_review_status_text) - 1] = '\0';
    apply_status_display(s_review_status_text);
}

// Renders s_move_history[s_review_index]: "<mover's clock>  <move>  Paused"
// up top, plus that move's board (with its own check-square/last-move
// highlighting, independent of whatever the live game looks like right
// now).
static void render_review_entry(void) {
    if (s_move_history_count == 0 || s_review_index < 0) {
        set_review_status("No moves yet");
        return;
    }

    MoveHistoryEntry *entry = &s_move_history[s_review_index];

    char clock_buf[16];
    format_clock(entry->clock_secs, clock_buf, sizeof(clock_buf));

    char buf[64];
    snprintf(buf, sizeof(buf), "%s  %s  Paused", clock_buf, entry->san);
    set_review_status(buf);

    layer_mark_dirty(s_board_layer);
}

static void enter_review_mode(void) {
    s_review_mode = true;
    if (s_clock_tick_timer) {
        app_timer_cancel(s_clock_tick_timer);
        s_clock_tick_timer = NULL;
    }
    s_review_index = (s_move_history_count > 0) ? (s_move_history_count - 1) : -1;
    render_review_entry();
}

// Snaps straight back to the live position using the state that's been
// kept up to date in the background the whole time (see in_recv_handler) -
// no phone round trip needed to "catch up".
// Snaps straight back to the live position using the state that's been
// kept up to date in the background the whole time (see in_recv_handler) -
// no phone round trip needed to "catch up". If a new game started while we
// were paused, this is also the moment its stale move history (from the
// game we were actually reviewing) finally gets cleared out - not before,
// since only the user gets to decide when to stop looking at it.
static void exit_review_mode(void) {
    s_review_mode = false;
    s_review_index = -1;
    if (s_up_hold_timer) {
        app_timer_cancel(s_up_hold_timer);
        s_up_hold_timer = NULL;
    }
    if (s_down_hold_timer) {
        app_timer_cancel(s_down_hold_timer);
        s_down_hold_timer = NULL;
    }
    // Drop back down to the steady-state cap now that we're tracking live
    // again, discarding whatever extra history piled up while paused.
    if (s_move_history_count > MAX_MOVE_HISTORY) {
        int excess = s_move_history_count - MAX_MOVE_HISTORY;
        memmove(&s_move_history[0], &s_move_history[excess], sizeof(MoveHistoryEntry) * MAX_MOVE_HISTORY);
        s_move_history_count = MAX_MOVE_HISTORY;
    }
    update_status_text();
    update_clock_layers();
    restart_clock_tick();
    layer_mark_dirty(s_board_layer);
}

// One step of UP's action, shared by the initial press and every
// auto-repeat tick while held (see main_up_raw_down_handler below).
static void perform_up_step(void) {
    reset_inactivity_timer();
    if (s_review_mode) {
        if (s_review_index >= 0 && s_review_index < s_move_history_count - 1) {
            start_board_jitter(0, -JITTER_PIXELS_WEAK);
            s_review_index++;
            render_review_entry();
        } else if (s_review_index >= 0) {
            start_board_jitter(0, -JITTER_PIXELS_STRONG);
            set_review_status("Current move!");
        }
    } else if (s_is_info_visible) {
        slide_board(false);
    } else if (s_watching_broadcast) {
        open_current_broadcast_game_list();
    } else if (s_watching_streamer) {
        open_current_streamer_list();
    } else if (s_root_choice == ROOT_CHOICE_NONE) {
        window_stack_push(s_root_window, true);
    } else if (s_root_choice == ROOT_CHOICE_TV) {
        // Push the chooser hidden underneath (no animation) so BACK from
        // the channel list still lands on it, per the user having already
        // picked "Lichess TV" once before.
        window_stack_push(s_root_window, false);
        window_stack_push(s_channel_window, true);
    } else if (s_root_choice == ROOT_CHOICE_STREAMER) {
        window_stack_push(s_root_window, false);
        open_streamer_list();
    } else {
        window_stack_push(s_root_window, false);
        open_broadcast_list();
    }
}

static void perform_down_step(void) {
    reset_inactivity_timer();
    if (s_review_mode) {
        if (s_review_index > 0) {
            start_board_jitter(0, JITTER_PIXELS_WEAK);
            s_review_index--;
            render_review_entry();
        } else if (s_review_index == 0) {
            start_board_jitter(0, JITTER_PIXELS_STRONG);
            set_review_status("Last recorded move!");
        }
    } else if (!s_is_info_visible) {
        slide_board(true);
    } else {
        slide_board(false);
    }
}

static void up_hold_timer_callback(void *data) {
    s_up_hold_timer = NULL;
    // Mode may have changed (or the game may have reset) mid-hold; bail
    // rather than keep firing.
    if (!s_review_mode) return;

    perform_up_step();

    // Stop once we've reached the newest recorded move - no point
    // repeating "Current move!" over and over while still held.
    if (s_review_index < 0 || s_review_index >= s_move_history_count - 1) return;
    s_up_hold_timer = app_timer_register(HOLD_REPEAT_MS, up_hold_timer_callback, NULL);
}

static void down_hold_timer_callback(void *data) {
    s_down_hold_timer = NULL;
    if (!s_review_mode) return;

    perform_down_step();

    if (s_review_index <= 0) return;
    s_down_hold_timer = app_timer_register(HOLD_REPEAT_MS, down_hold_timer_callback, NULL);
}

// Raw click handlers (rather than window_single_click_subscribe) so we get
// a real, separate "released" callback to cancel the repeat timer with -
// no click-type detection or built-in delay involved either way.
static void main_up_raw_down_handler(ClickRecognizerRef recognizer, void *context) {
    perform_up_step();
    if (s_review_mode && !s_up_hold_timer &&
        s_review_index >= 0 && s_review_index < s_move_history_count - 1) {
        s_up_hold_timer = app_timer_register(HOLD_INITIAL_DELAY_MS, up_hold_timer_callback, NULL);
    }
}

static void main_up_raw_up_handler(ClickRecognizerRef recognizer, void *context) {
    if (s_up_hold_timer) {
        app_timer_cancel(s_up_hold_timer);
        s_up_hold_timer = NULL;
    }
}

static void main_down_raw_down_handler(ClickRecognizerRef recognizer, void *context) {
    perform_down_step();
    if (s_review_mode && !s_down_hold_timer && s_review_index > 0) {
        s_down_hold_timer = app_timer_register(HOLD_INITIAL_DELAY_MS, down_hold_timer_callback, NULL);
    }
}

static void main_down_raw_up_handler(ClickRecognizerRef recognizer, void *context) {
    if (s_down_hold_timer) {
        app_timer_cancel(s_down_hold_timer);
        s_down_hold_timer = NULL;
    }
}

static void main_back_click_handler(ClickRecognizerRef recognizer, void *context) {
    if (s_review_mode) {
        // Exiting pause via BACK jitters toward the opposite side from
        // SELECT's enter/exit jitter, so the feedback direction matches
        // which button was actually pressed.
        toggle_pause_mode_dir(JITTER_PIXELS_STRONG);
    } else if (s_is_info_visible) {
        reset_inactivity_timer();
        slide_board(false);
    } else {
        reset_inactivity_timer();
        window_stack_pop(true);
    }
}

// Fires when SELECT has been held for the long-click threshold - toggles
// move-review ("pause") mode. Since this is driven by hold *duration*
// rather than a second tap, it doesn't need to wait around after release
// the way double-click detection does, so committing to the hold still
// feels immediate at the moment it triggers.
// Toggles move-review ("pause") mode, jittering the board horizontally by
// jitter_dx as button-press feedback. Shared by the SELECT long-press
// handler, the accelerometer tap handler (accel_tap_handler above), and the
// BACK button's "exit pause" path - jitter_dx lets each caller pick which
// side the board nudges toward, e.g. so BACK's exit jitters the opposite
// way from SELECT's enter/exit, keeping the feedback visually distinct per
// button.
static void toggle_pause_mode_dir(int jitter_dx) {
    // Only guard the "entering" transition - exiting pause (BACK, or
    // SELECT long-press while already paused) must always be allowed
    // regardless of what's on screen. The info panel and pause/review
    // mode both use UP/DOWN for their own, different purposes, so the two
    // can't be active at once.
    if (!s_review_mode && s_is_info_visible) {
        return;
    }
    reset_inactivity_timer();
    // Pebble's vibration motor is on/off only (no intensity control in the
    // SDK) - this custom pattern is a shorter pulse than the built-in
    // vibes_short_pulse(), about as light a tap as the hardware allows.
    static const uint32_t s_pause_vibe_segments[] = { 25 };
    VibePattern pause_vibe_pattern = {
        .durations = s_pause_vibe_segments,
        .num_segments = ARRAY_LENGTH(s_pause_vibe_segments)
    };
    vibes_enqueue_custom_pattern(pause_vibe_pattern);
    start_board_jitter(jitter_dx, 0);
    if (s_review_mode) {
        exit_review_mode();
    } else {
        enter_review_mode();
    }
}

// Default-direction toggle (jitters toward the SELECT side) - used by the
// SELECT long-press handler and the accelerometer tap handler.
static void toggle_pause_mode(void) {
    toggle_pause_mode_dir(-JITTER_PIXELS_STRONG);
}

static void main_select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
    toggle_pause_mode();
}

static void main_select_double_click_handler(ClickRecognizerRef recognizer, void *context) {
    reset_inactivity_timer();
    s_board_flipped = !s_board_flipped;
    apply_player_row_positions();
    if (s_board_layer) {
        layer_mark_dirty(s_board_layer);
    }
}

static void main_click_config_provider(void *context) {
    // Raw (not window_single_click_subscribe) so we get real press/release
    // callbacks: needed to drive the hold-to-repeat behavior in review
    // mode. Outside review mode these still fire exactly once per press,
    // same as a plain single click did.
    window_raw_click_subscribe(BUTTON_ID_UP, main_up_raw_down_handler, main_up_raw_up_handler, NULL);
    window_raw_click_subscribe(BUTTON_ID_DOWN, main_down_raw_down_handler, main_down_raw_up_handler, NULL);
    window_single_click_subscribe(BUTTON_ID_BACK, main_back_click_handler);
    // SELECT has no plain single-click handler: hold to pause/resume
    // review mode, double-tap to flip the board. Neither one competes with
    // a single-click's release-based firing, so there's no artificial
    // "wait and see" delay on either gesture.
    window_long_click_subscribe(BUTTON_ID_SELECT, 500, main_select_long_click_handler, NULL);
    window_multi_click_subscribe(BUTTON_ID_SELECT, 2, 0, 0, true, main_select_double_click_handler);
}

static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    s_status_layer = layer_create(GRect(0, 2, bounds.size.w, STATUS_HEIGHT));
    layer_set_update_proc(s_status_layer, status_layer_update_proc);
    layer_add_child(window_layer, s_status_layer);
    apply_status_display(s_status);

    int info_height = (bounds.size.h * 32) / 100;
    int info_top = bounds.size.h - info_height;

    s_info_layer = layer_create(GRect(0, info_top, bounds.size.w, info_height));
    layer_add_child(window_layer, s_info_layer);

    s_black_title_layer = text_layer_create(GRect(4, 2, 0, ROW_HEIGHT));
    text_layer_set_font(s_black_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
    text_layer_set_text_color(s_black_title_layer, TITLE_COLOR);
    text_layer_set_text_alignment(s_black_title_layer, GTextAlignmentLeft);
    text_layer_set_text(s_black_title_layer, s_black_title);
    layer_add_child(s_info_layer, text_layer_get_layer(s_black_title_layer));

    s_black_name_layer = text_layer_create(GRect(4, 2, 0, ROW_HEIGHT));
    text_layer_set_font(s_black_name_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
    text_layer_set_text_alignment(s_black_name_layer, GTextAlignmentLeft);
    text_layer_set_text(s_black_name_layer, s_black_name);
    layer_add_child(s_info_layer, text_layer_get_layer(s_black_name_layer));

    s_black_wing_layer = layer_create(GRect(4, 2, WING_ICON_SIZE, WING_ICON_SIZE));
    layer_set_update_proc(s_black_wing_layer, wing_layer_update_proc);
    layer_set_hidden(s_black_wing_layer, true);
    layer_add_child(s_info_layer, s_black_wing_layer);

    s_black_score_layer = layer_create(GRect(4, 18, 0, SCORE_BADGE_HEIGHT));
    layer_set_update_proc(s_black_score_layer, score_badge_update_proc);
    layer_set_hidden(s_black_score_layer, true);
    layer_add_child(s_info_layer, s_black_score_layer);

    s_black_clock_layer = text_layer_create(GRect(bounds.size.w - CLOCK_WIDTH - 4, 2, CLOCK_WIDTH, 20));
    text_layer_set_font(s_black_clock_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    text_layer_set_text_alignment(s_black_clock_layer, GTextAlignmentRight);
    text_layer_set_text(s_black_clock_layer, s_black_clock);
    layer_add_child(s_info_layer, text_layer_get_layer(s_black_clock_layer));

    s_black_elo_layer = text_layer_create(GRect(4, 18, bounds.size.w - CLOCK_WIDTH - 8, 14));
    text_layer_set_font(s_black_elo_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
    text_layer_set_text_alignment(s_black_elo_layer, GTextAlignmentLeft);
    text_layer_set_text(s_black_elo_layer, s_black_rating);
    layer_add_child(s_info_layer, text_layer_get_layer(s_black_elo_layer));

    s_white_title_layer = text_layer_create(GRect(4, 36, 0, ROW_HEIGHT));
    text_layer_set_font(s_white_title_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
    text_layer_set_text_color(s_white_title_layer, TITLE_COLOR);
    text_layer_set_text_alignment(s_white_title_layer, GTextAlignmentLeft);
    text_layer_set_text(s_white_title_layer, s_white_title);
    layer_add_child(s_info_layer, text_layer_get_layer(s_white_title_layer));

    s_white_name_layer = text_layer_create(GRect(4, 36, 0, ROW_HEIGHT));
    text_layer_set_font(s_white_name_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
    text_layer_set_text_alignment(s_white_name_layer, GTextAlignmentLeft);
    text_layer_set_text(s_white_name_layer, s_white_name);
    layer_add_child(s_info_layer, text_layer_get_layer(s_white_name_layer));

    s_white_wing_layer = layer_create(GRect(4, 36, WING_ICON_SIZE, WING_ICON_SIZE));
    layer_set_update_proc(s_white_wing_layer, wing_layer_update_proc);
    layer_set_hidden(s_white_wing_layer, true);
    layer_add_child(s_info_layer, s_white_wing_layer);

    s_white_score_layer = layer_create(GRect(4, 52, 0, SCORE_BADGE_HEIGHT));
    layer_set_update_proc(s_white_score_layer, score_badge_update_proc);
    layer_set_hidden(s_white_score_layer, true);
    layer_add_child(s_info_layer, s_white_score_layer);

    s_white_clock_layer = text_layer_create(GRect(bounds.size.w - CLOCK_WIDTH - 4, 36, CLOCK_WIDTH, 20));
    text_layer_set_font(s_white_clock_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    text_layer_set_text_alignment(s_white_clock_layer, GTextAlignmentRight);
    text_layer_set_text(s_white_clock_layer, s_white_clock);
    layer_add_child(s_info_layer, text_layer_get_layer(s_white_clock_layer));

    s_white_elo_layer = text_layer_create(GRect(4, 52, bounds.size.w - CLOCK_WIDTH - 8, 14));
    text_layer_set_font(s_white_elo_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
    text_layer_set_text_alignment(s_white_elo_layer, GTextAlignmentLeft);
    text_layer_set_text(s_white_elo_layer, s_white_rating);
    layer_add_child(s_info_layer, text_layer_get_layer(s_white_elo_layer));

    layout_player_row(s_black_title_layer, s_black_name_layer, s_black_wing_layer,
        s_black_title, s_black_name, s_black_patron, 2);
    layout_player_row(s_white_title_layer, s_white_name_layer, s_white_wing_layer,
        s_white_title, s_white_name, s_white_patron, 36);

    s_board_layer = layer_create(GRect(0, BOARD_TOP, bounds.size.w, bounds.size.w));
    layer_set_update_proc(s_board_layer, board_layer_update_proc);
    layer_add_child(window_layer, s_board_layer);

    update_board_layout();
}

static void main_window_unload(Window *window) {
    if (s_status_scroll_timer) {
        app_timer_cancel(s_status_scroll_timer);
        s_status_scroll_timer = NULL;
    }

    layer_destroy(s_board_layer);
    layer_destroy(s_status_layer);

    text_layer_destroy(s_black_title_layer);
    text_layer_destroy(s_black_name_layer);
    text_layer_destroy(s_black_elo_layer);
    text_layer_destroy(s_black_clock_layer);
    layer_destroy(s_black_wing_layer);
    layer_destroy(s_black_score_layer);
    text_layer_destroy(s_white_title_layer);
    text_layer_destroy(s_white_name_layer);
    text_layer_destroy(s_white_elo_layer);
    text_layer_destroy(s_white_clock_layer);
    layer_destroy(s_white_wing_layer);
    layer_destroy(s_white_score_layer);
    layer_destroy(s_info_layer);
}

static void init(void) {
    if (persist_exists(PERSIST_KEY_THEME)) {
        s_current_theme_index = persist_read_int(PERSIST_KEY_THEME);
        if (s_current_theme_index < 0 || s_current_theme_index >= (int)NUM_THEMES) {
            s_current_theme_index = 0;
        }
    }

    if (persist_exists(PERSIST_KEY_SIZE)) {
        s_current_board_size = (BoardSize)persist_read_int(PERSIST_KEY_SIZE);
    }

    if (persist_exists(PERSIST_KEY_HIGHLIGHT)) {
        s_highlight_mode = persist_read_int(PERSIST_KEY_HIGHLIGHT);
        if (s_highlight_mode < 0 || s_highlight_mode >= NUM_HIGHLIGHT_MODES) {
            s_highlight_mode = HIGHLIGHT_OFF;
        }
    }

    bool wrist_flick_enabled = false;
    if (persist_exists(PERSIST_KEY_WRIST_FLICK)) {
        wrist_flick_enabled = persist_read_bool(PERSIST_KEY_WRIST_FLICK);
    }

    if (persist_exists(PERSIST_KEY_INACTIVITY_TIMEOUT)) {
        int mins = persist_read_int(PERSIST_KEY_INACTIVITY_TIMEOUT);
        s_inactivity_timeout_min = (mins > 0) ? (uint32_t)mins : 0;
    }

    if (persist_exists(PERSIST_KEY_ROOT_CHOICE)) {
        int choice = persist_read_int(PERSIST_KEY_ROOT_CHOICE);
        if (choice == ROOT_CHOICE_TV || choice == ROOT_CHOICE_BROADCAST) {
            s_root_choice = (RootChoice)choice;
        }
    }

    load_piece_bitmaps_for_current_size();
    s_patron_wing_bitmap = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PATRON_WING);

    s_main_window = window_create();
    window_set_click_config_provider(s_main_window, main_click_config_provider);
    window_set_window_handlers(s_main_window, (WindowHandlers) {
        .load = main_window_load,
        .unload = main_window_unload,
    });

    s_root_window = window_create();
    window_set_window_handlers(s_root_window, (WindowHandlers) {
        .load = root_window_load,
        .unload = root_window_unload,
    });

    s_channel_window = window_create();
    window_set_window_handlers(s_channel_window, (WindowHandlers) {
        .load = channel_window_load,
        .unload = channel_window_unload,
    });

    s_broadcast_window = window_create();
    window_set_window_handlers(s_broadcast_window, (WindowHandlers) {
        .load = broadcast_window_load,
        .unload = broadcast_window_unload,
    });

    s_streamer_window = window_create();
    window_set_window_handlers(s_streamer_window, (WindowHandlers) {
        .load = streamer_window_load,
        .unload = streamer_window_unload,
    });

    s_game_window = window_create();
    window_set_window_handlers(s_game_window, (WindowHandlers) {
        .load = game_window_load,
        .unload = game_window_unload,
    });

    app_message_register_inbox_received(in_recv_handler);
    app_message_register_inbox_dropped(in_dropped_handler);
    app_message_open(app_message_inbox_size_maximum(), app_message_outbox_size_maximum());

    reset_stale_timer();
    set_wrist_flick_enabled(wrist_flick_enabled);
    reset_inactivity_timer();

    window_stack_push(s_main_window, true);
}

static void deinit(void) {
    persist_write_int(PERSIST_KEY_THEME, s_current_theme_index);
    persist_write_int(PERSIST_KEY_SIZE, s_current_board_size);
    persist_write_int(PERSIST_KEY_HIGHLIGHT, s_highlight_mode);
    persist_write_bool(PERSIST_KEY_WRIST_FLICK, s_wrist_flick_enabled);
    persist_write_int(PERSIST_KEY_INACTIVITY_TIMEOUT, (int)s_inactivity_timeout_min);
    persist_write_int(PERSIST_KEY_ROOT_CHOICE, (int)s_root_choice);

    if (s_wrist_flick_enabled) {
        accel_tap_service_unsubscribe();
    }
    if (s_stale_timer) {
        app_timer_cancel(s_stale_timer);
    }
    if (s_stream_check_timer) {
        app_timer_cancel(s_stream_check_timer);
    }
    if (s_clock_tick_timer) {
        app_timer_cancel(s_clock_tick_timer);
    }
    if (s_inactivity_timer) {
        app_timer_cancel(s_inactivity_timer);
    }
    if (s_up_hold_timer) {
        app_timer_cancel(s_up_hold_timer);
    }
    if (s_down_hold_timer) {
        app_timer_cancel(s_down_hold_timer);
    }
    if (s_board_animation) {
        animation_destroy((Animation*)s_board_animation);
    }
    for (int i = 0; i < NUM_PIECE_TYPES; i++) {
        if (s_piece_bitmaps[i]) {
            gbitmap_destroy(s_piece_bitmaps[i]);
        }
    }
    if (s_patron_wing_bitmap) {
        gbitmap_destroy(s_patron_wing_bitmap);
    }
    window_destroy(s_game_window);
    window_destroy(s_broadcast_window);
    window_destroy(s_streamer_window);
    window_destroy(s_channel_window);
    window_destroy(s_root_window);
    window_destroy(s_main_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}