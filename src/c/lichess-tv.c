#include <pebble.h>

#define BOARD_TOP         30
#define STATUS_HEIGHT     26
#define STALE_TIMEOUT_MS  20000
#define STREAM_CHECK_TIMEOUT_MS 6000
#define NUM_PIECE_TYPES   12
#define NUM_CHANNELS      15

#define TITLE_NAME_GAP    3
#define NAME_WING_GAP     3
#define WING_ICON_SIZE    14
#define CLOCK_WIDTH       54
#define ROW_HEIGHT        16

#define TITLE_COLOR       GColorFromRGB(0xC5, 0x8C, 0x18)

#define PERSIST_KEY_THEME               1
#define PERSIST_KEY_SIZE                2
#define PERSIST_KEY_WRIST_FLICK         3
#define PERSIST_KEY_HIGHLIGHT           4
#define PERSIST_KEY_INACTIVITY_TIMEOUT  5

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
static TextLayer  *s_status_layer;
static AppTimer   *s_stale_timer;
static AppTimer   *s_stream_check_timer;
static bool        s_awaiting_stream_check = false;

// Inactivity (auto-exit) timeout. 0 minutes means "Off" - no auto-exit.
static AppTimer   *s_inactivity_timer = NULL;
static uint32_t    s_inactivity_timeout_min = 0;

static Layer      *s_info_layer;
static TextLayer  *s_black_title_layer;
static TextLayer  *s_black_name_layer;
static TextLayer  *s_black_elo_layer;
static TextLayer  *s_black_clock_layer;
static Layer      *s_black_wing_layer;
static TextLayer  *s_white_title_layer;
static TextLayer  *s_white_name_layer;
static TextLayer  *s_white_elo_layer;
static TextLayer  *s_white_clock_layer;
static Layer      *s_white_wing_layer;

static Window     *s_channel_window;
static MenuLayer  *s_menu_layer;

static GBitmap *s_piece_bitmaps[NUM_PIECE_TYPES];
static GBitmap *s_patron_wing_bitmap;

static int s_current_theme_index = 0;
static BoardSize s_current_board_size = BOARD_SIZE_BIG;

static bool s_is_info_visible = false;
static PropertyAnimation *s_board_animation = NULL;

static bool s_wrist_flick_enabled = false;
static bool s_board_flipped = false;

static int s_highlight_mode = HIGHLIGHT_OFF;
static char s_last_move[8] = "";

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

static char s_black_title[8] = "";
static char s_black_name[32] = "Black";
static char s_black_rating[16] = "";
static char s_black_clock[16] = "";
static bool s_black_patron = false;

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

static const uint32_t s_piece_resource_ids[NUM_PIECE_TYPES] = {
    RESOURCE_ID_IMAGE_PIECE_WK,
    RESOURCE_ID_IMAGE_PIECE_WQ,
    RESOURCE_ID_IMAGE_PIECE_WR,
    RESOURCE_ID_IMAGE_PIECE_WB,
    RESOURCE_ID_IMAGE_PIECE_WN,
    RESOURCE_ID_IMAGE_PIECE_WP,
    RESOURCE_ID_IMAGE_PIECE_BK,
    RESOURCE_ID_IMAGE_PIECE_BQ,
    RESOURCE_ID_IMAGE_PIECE_BR,
    RESOURCE_ID_IMAGE_PIECE_BB,
    RESOURCE_ID_IMAGE_PIECE_BN,
    RESOURCE_ID_IMAGE_PIECE_BP,
};

static void reset_stale_timer(void);
static void reset_inactivity_timer(void);
static int channel_key_to_index(const char *key);
static void apply_player_row_positions(void);
static void set_wrist_flick_enabled(bool enabled);

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

static bool get_last_move_squares(int *from_idx, int *to_idx) {
    if (strlen(s_last_move) < 4) return false;
    if (!parse_square(s_last_move, from_idx)) return false;
    if (!parse_square(s_last_move + 2, to_idx)) return false;
    return true;
}

static void board_layer_update_proc(Layer *layer, GContext *ctx) {
    graphics_context_set_compositing_mode(ctx, GCompOpSet);

    GRect bounds = layer_get_bounds(layer);
    int sq_size = bounds.size.w / 8;

    BoardTheme current_theme = s_themes[s_current_theme_index];

    int hl_from = -1, hl_to = -1;
    bool show_highlight = (s_highlight_mode != HIGHLIGHT_OFF) &&
        get_last_move_squares(&hl_from, &hl_to);
    GColor highlight_color = s_highlight_colors[s_highlight_mode];

    for (int rank = 0; rank < 8; rank++) {
        for (int file = 0; file < 8; file++) {
            int idx = s_board_flipped ? (63 - (rank * 8 + file)) : (rank * 8 + file);
            char c = s_board[idx];

            GRect square = GRect(file * sq_size, rank * sq_size, sq_size, sq_size);

            bool light_square = ((rank + file) % 2 == 0);
            bool is_highlighted = show_highlight && (idx == hl_from || idx == hl_to);
            bool is_check = (idx == s_check_square);

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

static void update_status_text(void) {
    text_layer_set_text(s_status_layer, s_status);
}

static void stream_check_timeout_callback(void *data) {
    s_stream_check_timer = NULL;
    if (!s_awaiting_stream_check) return;
    s_awaiting_stream_check = false;
    snprintf(s_status, sizeof(s_status), "TV feed lost, retrying...");
    update_status_text();
    reset_stale_timer();
}

static void stale_timer_callback(void *data) {
    s_stale_timer = NULL;

    s_awaiting_stream_check = true;
    snprintf(s_status, sizeof(s_status), "Checking stream...");
    update_status_text();

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
    int m = seconds / 60;
    int s = seconds % 60;
    snprintf(buf, len, "%d:%02d", m, s);
}

static void update_clock_layers(void) {
    format_clock(s_white_clock_secs, s_white_clock, sizeof(s_white_clock));
    if (s_white_clock_layer) text_layer_set_text(s_white_clock_layer, s_white_clock);

    format_clock(s_black_clock_secs, s_black_clock, sizeof(s_black_clock));
    if (s_black_clock_layer) text_layer_set_text(s_black_clock_layer, s_black_clock);
}

static void clock_tick_callback(void *data) {
    s_clock_tick_timer = NULL;

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
    if (s_active_color == 'w' || s_active_color == 'b') {
        s_clock_tick_timer = app_timer_register(1000, clock_tick_callback, NULL);
    }
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

    layer_set_frame(text_layer_get_layer(s_black_elo_layer),
        GRect(4, black_y + 16, bounds.size.w - CLOCK_WIDTH - 8, 14));
    layer_set_frame(text_layer_get_layer(s_black_clock_layer),
        GRect(bounds.size.w - CLOCK_WIDTH - 4, black_y, CLOCK_WIDTH, 20));

    layer_set_frame(text_layer_get_layer(s_white_elo_layer),
        GRect(4, white_y + 16, bounds.size.w - CLOCK_WIDTH - 8, 14));
    layer_set_frame(text_layer_get_layer(s_white_clock_layer),
        GRect(bounds.size.w - CLOCK_WIDTH - 4, white_y, CLOCK_WIDTH, 20));
}

static void accel_tap_handler(AccelAxisType axis, int32_t direction) {
    if (!s_wrist_flick_enabled) return;
    s_board_flipped = !s_board_flipped;
    apply_player_row_positions();
    layer_mark_dirty(s_board_layer);
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

static void in_recv_handler(DictionaryIterator *iter, void *context) {
    Tuple *board_tuple = dict_find(iter, MESSAGE_KEY_BoardFEN);
    Tuple *status_tuple = dict_find(iter, MESSAGE_KEY_StatusText);
    Tuple *theme_tuple = dict_find(iter, MESSAGE_KEY_BoardTheme);
    Tuple *size_tuple = dict_find(iter, MESSAGE_KEY_BoardSize);

    Tuple *wtitle_tuple = dict_find(iter, MESSAGE_KEY_WhiteTitle);
    Tuple *wname_tuple = dict_find(iter, MESSAGE_KEY_WhiteName);
    Tuple *welo_tuple = dict_find(iter, MESSAGE_KEY_WhiteRating);
    Tuple *wclocksecs_tuple = dict_find(iter, MESSAGE_KEY_WhiteClockSecs);
    Tuple *wpatron_tuple = dict_find(iter, MESSAGE_KEY_WhitePatron);
    Tuple *btitle_tuple = dict_find(iter, MESSAGE_KEY_BlackTitle);
    Tuple *bname_tuple = dict_find(iter, MESSAGE_KEY_BlackName);
    Tuple *belo_tuple = dict_find(iter, MESSAGE_KEY_BlackRating);
    Tuple *bclocksecs_tuple = dict_find(iter, MESSAGE_KEY_BlackClockSecs);
    Tuple *bpatron_tuple = dict_find(iter, MESSAGE_KEY_BlackPatron);
    Tuple *active_tuple = dict_find(iter, MESSAGE_KEY_ActiveColor);

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
            layer_mark_dirty(s_board_layer);
        }
    }

    if (status_tuple) {
        strncpy(s_status, status_tuple->value->cstring, sizeof(s_status) - 1);
        s_status[sizeof(s_status) - 1] = '\0';
        update_status_text();
    }

    if (theme_tuple) {
        s_current_theme_index = theme_tuple->value->int32;
        if (s_current_theme_index < 0 || s_current_theme_index >= (int)NUM_THEMES) {
            s_current_theme_index = 0;
        }
        layer_mark_dirty(s_board_layer);
    }

    if (size_tuple) {
        s_current_board_size = (BoardSize)size_tuple->value->int32;
        update_board_layout();
    }

    bool white_row_changed = false;
    bool black_row_changed = false;

    if (wtitle_tuple) { strncpy(s_white_title, wtitle_tuple->value->cstring, sizeof(s_white_title) - 1); s_white_title[sizeof(s_white_title) - 1] = '\0'; text_layer_set_text(s_white_title_layer, s_white_title); white_row_changed = true; }
    if (wname_tuple) { strncpy(s_white_name, wname_tuple->value->cstring, sizeof(s_white_name) - 1); s_white_name[sizeof(s_white_name) - 1] = '\0'; text_layer_set_text(s_white_name_layer, s_white_name); white_row_changed = true; }
    if (welo_tuple) { strncpy(s_white_rating, welo_tuple->value->cstring, sizeof(s_white_rating) - 1); s_white_rating[sizeof(s_white_rating) - 1] = '\0'; text_layer_set_text(s_white_elo_layer, s_white_rating); }
    if (wpatron_tuple) { s_white_patron = (wpatron_tuple->value->int32 != 0); white_row_changed = true; }

    if (btitle_tuple) { strncpy(s_black_title, btitle_tuple->value->cstring, sizeof(s_black_title) - 1); s_black_title[sizeof(s_black_title) - 1] = '\0'; text_layer_set_text(s_black_title_layer, s_black_title); black_row_changed = true; }
    if (bname_tuple) { strncpy(s_black_name, bname_tuple->value->cstring, sizeof(s_black_name) - 1); s_black_name[sizeof(s_black_name) - 1] = '\0'; text_layer_set_text(s_black_name_layer, s_black_name); black_row_changed = true; }
    if (belo_tuple) { strncpy(s_black_rating, belo_tuple->value->cstring, sizeof(s_black_rating) - 1); s_black_rating[sizeof(s_black_rating) - 1] = '\0'; text_layer_set_text(s_black_elo_layer, s_black_rating); }
    if (bpatron_tuple) { s_black_patron = (bpatron_tuple->value->int32 != 0); black_row_changed = true; }

    if (white_row_changed) {
        layout_player_row(s_white_title_layer, s_white_name_layer, s_white_wing_layer,
            s_white_title, s_white_name, s_white_patron, 36);
    }
    if (black_row_changed) {
        layout_player_row(s_black_title_layer, s_black_name_layer, s_black_wing_layer,
            s_black_title, s_black_name, s_black_patron, 2);
    }

    bool clock_state_changed = false;
    if (wclocksecs_tuple) { s_white_clock_secs = wclocksecs_tuple->value->int32; clock_state_changed = true; }
    if (bclocksecs_tuple) { s_black_clock_secs = bclocksecs_tuple->value->int32; clock_state_changed = true; }
    if (active_tuple) {
        const char *ac = active_tuple->value->cstring;
        s_active_color = (ac && ac[0]) ? ac[0] : 0;
        clock_state_changed = true;
    }
    if (clock_state_changed) {
        update_clock_layers();
        restart_clock_tick();
    }

    if (last_move_tuple) {
        strncpy(s_last_move, last_move_tuple->value->cstring, sizeof(s_last_move) - 1);
        s_last_move[sizeof(s_last_move) - 1] = '\0';
        layer_mark_dirty(s_board_layer);
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
            update_status_text();
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
        layer_mark_dirty(s_board_layer);
    }

    Tuple *wflag_tuple = dict_find(iter, MESSAGE_KEY_WhiteFlagged);
    Tuple *bflag_tuple = dict_find(iter, MESSAGE_KEY_BlackFlagged);

    if (wflag_tuple) {
        bool white_flagged = (wflag_tuple->value->int32 != 0);
        text_layer_set_text_color(s_white_clock_layer, white_flagged ? GColorRed : GColorBlack);
    }
    if (bflag_tuple) {
        bool black_flagged = (bflag_tuple->value->int32 != 0);
        text_layer_set_text_color(s_black_clock_layer, black_flagged ? GColorRed : GColorBlack);
    }

    reset_stale_timer();
}

static void in_dropped_handler(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage dropped, reason: %d", (int)reason);
}

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

    window_stack_pop(true);
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

static void main_up_click_handler(ClickRecognizerRef recognizer, void *context) {
    reset_inactivity_timer();
    if (s_is_info_visible) {
        slide_board(false);
    } else {
        window_stack_push(s_channel_window, true);
    }
}

static void main_down_click_handler(ClickRecognizerRef recognizer, void *context) {
    reset_inactivity_timer();
    if (!s_is_info_visible) {
        slide_board(true);
    } else {
        slide_board(false);
    }
}

static void main_back_click_handler(ClickRecognizerRef recognizer, void *context) {
    reset_inactivity_timer();
    if (s_is_info_visible) {
        slide_board(false);
    } else {
        window_stack_pop(true);
    }
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
    window_single_click_subscribe(BUTTON_ID_UP, main_up_click_handler);
    window_single_click_subscribe(BUTTON_ID_DOWN, main_down_click_handler);
    window_single_click_subscribe(BUTTON_ID_BACK, main_back_click_handler);
    window_multi_click_subscribe(BUTTON_ID_SELECT, 2, 0, 0, true, main_select_double_click_handler);
}

static void main_window_load(Window *window) {
    Layer *window_layer = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(window_layer);

    s_status_layer = text_layer_create(GRect(0, 2, bounds.size.w, STATUS_HEIGHT));
    text_layer_set_background_color(s_status_layer, GColorClear);
    text_layer_set_text_color(s_status_layer, GColorBlack);
    text_layer_set_font(s_status_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    text_layer_set_text_alignment(s_status_layer, GTextAlignmentCenter);
    text_layer_set_text(s_status_layer, s_status);
    layer_add_child(window_layer, text_layer_get_layer(s_status_layer));

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
    layer_destroy(s_board_layer);
    text_layer_destroy(s_status_layer);

    text_layer_destroy(s_black_title_layer);
    text_layer_destroy(s_black_name_layer);
    text_layer_destroy(s_black_elo_layer);
    text_layer_destroy(s_black_clock_layer);
    layer_destroy(s_black_wing_layer);
    text_layer_destroy(s_white_title_layer);
    text_layer_destroy(s_white_name_layer);
    text_layer_destroy(s_white_elo_layer);
    text_layer_destroy(s_white_clock_layer);
    layer_destroy(s_white_wing_layer);
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

    for (int i = 0; i < NUM_PIECE_TYPES; i++) {
        s_piece_bitmaps[i] = gbitmap_create_with_resource(s_piece_resource_ids[i]);
    }
    s_patron_wing_bitmap = gbitmap_create_with_resource(RESOURCE_ID_IMAGE_PATRON_WING);

    s_main_window = window_create();
    window_set_click_config_provider(s_main_window, main_click_config_provider);
    window_set_window_handlers(s_main_window, (WindowHandlers) {
        .load = main_window_load,
        .unload = main_window_unload,
    });

    s_channel_window = window_create();
    window_set_window_handlers(s_channel_window, (WindowHandlers) {
        .load = channel_window_load,
        .unload = channel_window_unload,
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
    window_destroy(s_channel_window);
    window_destroy(s_main_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}