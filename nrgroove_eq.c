#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>

/* =========================================================================
 * 1. STITCH COLOR PALETTE (Extracted from NRGroove v0.1.0 HTML)
 * ========================================================================= */
#define COL_BG_DARK        0x131315  // surface / background
#define COL_SURFACE_LOW    0x1B1B1D  // module card background
#define COL_SURFACE_HIGH   0x2A2A2C  // inactive button / knob
#define COL_BORDER         0x353437  // surface-variant bezel
#define COL_TEXT_PRIMARY   0xE5E1E4  // on-surface off-white
#define COL_TEXT_SEC       0xCBC6BB  // secondary vintage beige-grey
#define COL_AKAI_RED       0xE52521  // primary-container (LOW KILL)
#define COL_PHOSPHOR_GRN   0x00E479  // tertiary (MID PASS & FFT)
#define COL_AIR_CYAN       0x00E5FF  // HIGH PASS & air shelf

/* =========================================================================
 * 2. HARDWARE DEVICE PATHS & KEYCODES (Verify against nrgroove_hw_dump.txt)
 * ========================================================================= */
#define FB_DEVICE          "/dev/fb0"
#define EVDEV_BUTTONS      "/dev/input/event0"
#define RAWMIDI_PADS       "/dev/snd/midiC0D0"

// Placeholder Linux evdev keycodes for B1, B2, B3 (update after dump if needed)
#define KEY_MPC_B1         KEY_F1
#define KEY_MPC_B2         KEY_F2
#define KEY_MPC_B3         KEY_F3
#define KEY_MPC_EXIT       KEY_ESC

// Standard Bank A MIDI Note numbers for Pads 13-16 (C3=48 to D#3=51)
#define PAD_13_NOTE        48
#define PAD_14_NOTE        49
#define PAD_15_NOTE        50
#define PAD_16_NOTE        51

/* =========================================================================
 * 3. APPLICATION STATE
 * ========================================================================= */
typedef struct {
    bool low_killed;
    bool mid_killed;
    bool high_killed;
    int  lfo_rate_idx;   // 0=1/4, 1=1/8, 2=1/16T, 3=1/32
    int  routing_target; // 0=TRK 01, 1=MASTER, 2=SUBMIX 1
    int  active_macro;   // -1=None, 0=Pad13, 1=Pad14, 2=Pad15, 3=Pad16
} EqState;

typedef struct {
    int fd;
    uint8_t *ptr;
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    size_t screensize;
} Framebuffer;

/* Minimal 5x7 Bitmap Font (Uppercase ASCII 32-95) */
static const uint8_t FONT5X7[64][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00}, //  ! "
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, // # $ %
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00}, // & ' (
    {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08}, // ) * +
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00}, // , - .
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, // / 0 1
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10}, // 2 3 4
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, // 5 6 7
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00}, // 8 9 :
    {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14}, // ; < =
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E}, // > ? @
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22}, // A B C
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01}, // D E F
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, // G H I
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40}, // J K L
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E}, // M N O
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, // P Q R
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F}, // S T U
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63}, // V W X
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00}, // Y Z [
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04}, // \ ] ^
    {0x40,0x40,0x40,0x40,0x40}                                                          // _
};

/* =========================================================================
 * 4. FRAMEBUFFER PRIMITIVES (Supports 16-bit RGB565 and 32-bit ARGB8888)
 * ========================================================================= */
static inline void put_pixel(Framebuffer *fb, int x, int y, uint32_t rgb) {
    if (x < 0 || y < 0 || x >= (int)fb->vinfo.xres || y >= (int)fb->vinfo.yres) return;
    long loc = (x + fb->vinfo.xoffset) * (fb->vinfo.bits_per_pixel / 8) +
               (y + fb->vinfo.yoffset) * fb->finfo.line_length;

    if (fb->vinfo.bits_per_pixel == 32) {
        *((uint32_t *)(fb->ptr + loc)) = (0xFF000000 | rgb);
    } else if (fb->vinfo.bits_per_pixel == 16) {
        uint16_t r = (rgb >> 19) & 0x1F;
        uint16_t g = (rgb >> 10) & 0x3F;
        uint16_t b = (rgb >> 3)  & 0x1F;
        *((uint16_t *)(fb->ptr + loc)) = (r << 11) | (g << 5) | b;
    }
}

static void fill_rect(Framebuffer *fb, int x, int y, int w, int h, uint32_t rgb) {
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            put_pixel(fb, px, py, rgb);
        }
    }
}

static void draw_rect_outline(Framebuffer *fb, int x, int y, int w, int h, uint32_t rgb) {
    fill_rect(fb, x, y, w, 1, rgb);
    fill_rect(fb, x, y + h - 1, w, 1, rgb);
    fill_rect(fb, x, y, 1, h, rgb);
    fill_rect(fb, x + w - 1, y, 1, h, rgb);
}

static void draw_line(Framebuffer *fb, int x0, int y0, int x1, int y1, uint32_t rgb) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    while (true) {
        put_pixel(fb, x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_text(Framebuffer *fb, int x, int y, const char *str, uint32_t rgb, int scale) {
    int cursor_x = x;
    for (size_t i = 0; str[i] != '\0'; i++) {
        char c = str[i];
        if (c >= 'a' && c <= 'z') c -= 32; // Force uppercase for compact font
        if (c < 32 || c > 95) c = ' ';
        const uint8_t *glyph = FONT5X7[c - 32];
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (glyph[col] & (1 << row)) {
                    fill_rect(fb, cursor_x + col * scale, y + row * scale, scale, scale, rgb);
                }
            }
        }
        cursor_x += (6 * scale);
    }
}

/* =========================================================================
 * 5. RENDER THE NRGROOVE STITCH 3-BAND KILL EQ SCREEN
 * ========================================================================= */
static void render_ui(Framebuffer *fb, const EqState *st) {
    int W = fb->vinfo.xres;
    int H = fb->vinfo.yres;

    // 1. Clear background (#131315)
    fill_rect(fb, 0, 0, W, H, COL_BG_DARK);

    // 2. Top App Bar (NRGroove v0.1.0 // AKAI-DSP)
    int top_h = 24;
    fill_rect(fb, 0, 0, W, top_h, COL_SURFACE_LOW);
    fill_rect(fb, 0, top_h - 1, W, 1, COL_BORDER);
    fill_rect(fb, 4, 4, 52, 16, COL_AKAI_RED);
    draw_text(fb, 7, 8, "AKAI-DSP", COL_TEXT_PRIMARY, 1);
    draw_text(fb, 62, 8, "NRGROOVE V0.1.0 [KILL EQ]", COL_TEXT_SEC, 1);

    const char *targets[] = {"TRK 01", "MASTER", "SUBMIX"};
    draw_text(fb, W - 44, 8, targets[st->routing_target], COL_PHOSPHOR_GRN, 1);

    // 3. Three EQ Kill Modules (LOW, MID, HIGH)
    int mod_top = top_h + 6;
    int mod_h = (H - top_h - 32) * 0.54;
    int gap = 6;
    int mod_w = (W - (gap * 4)) / 3;

    // --- MODULE 1: LOW BAND (< 120 HZ) ---
    int x1 = gap;
    fill_rect(fb, x1, mod_top, mod_w, mod_h, COL_SURFACE_LOW);
    draw_rect_outline(fb, x1, mod_top, mod_w, mod_h, st->low_killed ? COL_AKAI_RED : COL_BORDER);
    draw_text(fb, x1 + 6, mod_top + 6, "LOW <120HZ", COL_TEXT_SEC, 1);
    fill_rect(fb, x1 + 6, mod_top + 18, mod_w - 12, 24, st->low_killed ? COL_AKAI_RED : COL_SURFACE_HIGH);
    draw_text(fb, x1 + 10, mod_top + 26, st->low_killed ? "LOW KILL" : "LOW PASS", COL_TEXT_PRIMARY, 1);
    draw_text(fb, x1 + 6, mod_top + 48, st->low_killed ? "-INF DB" : "0.0 DB",
              st->low_killed ? COL_AKAI_RED : COL_TEXT_SEC, 1);

    // --- MODULE 2: MID BAND (120HZ - 2.5KHZ) ---
    int x2 = gap * 2 + mod_w;
    fill_rect(fb, x2, mod_top, mod_w, mod_h, COL_SURFACE_LOW);
    draw_rect_outline(fb, x2, mod_top, mod_w, mod_h, st->mid_killed ? COL_AKAI_RED : COL_BORDER);
    draw_text(fb, x2 + 6, mod_top + 6, "MID 1.1KHZ", COL_TEXT_SEC, 1);
    fill_rect(fb, x2 + 6, mod_top + 18, mod_w - 12, 24, st->mid_killed ? COL_AKAI_RED : COL_SURFACE_HIGH);
    draw_text(fb, x2 + 10, mod_top + 26, st->mid_killed ? "MID KILL" : "MID PASS",
              st->mid_killed ? COL_TEXT_PRIMARY : COL_PHOSPHOR_GRN, 1);
    draw_text(fb, x2 + 6, mod_top + 48, st->mid_killed ? "-INF DB" : "0.0 DB", COL_PHOSPHOR_GRN, 1);

    // --- MODULE 3: HIGH BAND (> 2.5KHZ) ---
    int x3 = gap * 3 + mod_w * 2;
    fill_rect(fb, x3, mod_top, mod_w, mod_h, COL_SURFACE_LOW);
    draw_rect_outline(fb, x3, mod_top, mod_w, mod_h, st->high_killed ? COL_AKAI_RED : COL_BORDER);
    draw_text(fb, x3 + 6, mod_top + 6, "HIGH >3.2K", COL_TEXT_SEC, 1);
    fill_rect(fb, x3 + 6, mod_top + 18, mod_w - 12, 24, st->high_killed ? COL_AKAI_RED : COL_SURFACE_HIGH);
    draw_text(fb, x3 + 10, mod_top + 26, st->high_killed ? "HI KILL" : "HI PASS",
              st->high_killed ? COL_TEXT_PRIMARY : COL_AIR_CYAN, 1);
    draw_text(fb, x3 + 6, mod_top + 48, st->high_killed ? "-INF DB" : "+1.2 DB", COL_AIR_CYAN, 1);

    // 4. Dual-Trace FFT & Filter Curve Box
    int fft_top = mod_top + mod_h + 6;
    int bot_h = 24;
    int fft_h = H - fft_top - bot_h - 6;
    fill_rect(fb, gap, fft_top, W - gap * 2, fft_h, 0x0E0E10);
    draw_rect_outline(fb, gap, fft_top, W - gap * 2, fft_h, COL_BORDER);

    const char *lfo_labels[] = {"LFO: 1/4", "LFO: 1/8", "LFO: 1/16T", "LFO: 1/32"};
    draw_text(fb, gap + 6, fft_top + 5, "FFT NOTCH RESPONSE", COL_PHOSPHOR_GRN, 1);
    draw_text(fb, W - 76, fft_top + 5, lfo_labels[st->lfo_rate_idx], COL_TEXT_SEC, 1);

    // Plot Filter Curve across FFT box
    int curve_x0 = gap + 8;
    int curve_w  = W - (gap * 2) - 16;
    int y_zero   = fft_top + (fft_h / 2) + 4;
    int y_kill   = fft_top + fft_h - 6;
    int y_air    = y_zero - 6;

    // Zero-dB dashed reference line
    for (int lx = curve_x0; lx < curve_x0 + curve_w; lx += 6) {
        draw_line(fb, lx, y_zero, lx + 2, y_zero, COL_BORDER);
    }

    int x_low_end  = curve_x0 + (curve_w * 0.25);
    int x_mid_end  = curve_x0 + (curve_w * 0.70);
    int x_high_end = curve_x0 + curve_w;

    int y_low  = st->low_killed  ? y_kill : y_zero;
    int y_mid  = st->mid_killed  ? y_kill : y_zero;
    int y_high = st->high_killed ? y_kill : y_air;

    draw_line(fb, curve_x0, y_low, x_low_end - 10, y_low, COL_PHOSPHOR_GRN);
    draw_line(fb, x_low_end - 10, y_low, x_low_end + 10, y_mid, COL_PHOSPHOR_GRN);
    draw_line(fb, x_low_end + 10, y_mid, x_mid_end - 10, y_mid, COL_PHOSPHOR_GRN);
    draw_line(fb, x_mid_end - 10, y_mid, x_mid_end + 10, y_high, COL_PHOSPHOR_GRN);
    draw_line(fb, x_mid_end + 10, y_high, x_high_end, y_high, COL_PHOSPHOR_GRN);

    // 5. Bottom Soft-Keys Bar ([B1] ROUTING | [B2] KILL ALL | [B3] LFO)
    int bot_top = H - bot_h;
    fill_rect(fb, 0, bot_top, W, bot_h, COL_SURFACE_LOW);
    int btn_w = (W - (gap * 4)) / 3;

    // [B1]
    fill_rect(fb, gap, bot_top + 3, btn_w, bot_h - 6, COL_SURFACE_HIGH);
    draw_text(fb, gap + 6, bot_top + 8, "[B1] ROUTING", COL_TEXT_SEC, 1);

    // [B2]
    bool any_killed = st->low_killed || st->mid_killed || st->high_killed;
    fill_rect(fb, gap * 2 + btn_w, bot_top + 3, btn_w, bot_h - 6,
              any_killed ? COL_AKAI_RED : COL_SURFACE_HIGH);
    draw_text(fb, gap * 2 + btn_w + 6, bot_top + 8,
              any_killed ? "[B2] RESET ALL" : "[B2] KILL ALL", COL_TEXT_PRIMARY, 1);

    // [B3]
    fill_rect(fb, gap * 3 + btn_w * 2, bot_top + 3, btn_w, bot_h - 6, COL_SURFACE_HIGH);
    draw_text(fb, gap * 3 + btn_w * 2 + 6, bot_top + 8, "[B3] LFO SYNC", COL_TEXT_SEC, 1);
}

/* =========================================================================
 * 6. MAIN EVENT LOOP (Buttons via evdev + Pads 13-16 via ALSA RawMIDI)
 * ========================================================================= */
int main(void) {
    Framebuffer fb = {0};
    fb.fd = open(FB_DEVICE, O_RDWR);
    if (fb.fd < 0) {
        perror("Failed to open /dev/fb0");
        return 1;
    }

    if (ioctl(fb.fd, FBIOGET_FSCREENINFO, &fb.finfo) < 0 ||
        ioctl(fb.fd, FBIOGET_VSCREENINFO, &fb.vinfo) < 0) {
        perror("Failed to read framebuffer info");
        close(fb.fd);
        return 1;
    }

    fb.screensize = fb.finfo.line_length * fb.vinfo.yres;
    fb.ptr = (uint8_t *)mmap(0, fb.screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
    if (fb.ptr == MAP_FAILED) {
        perror("Failed to mmap framebuffer");
        close(fb.fd);
        return 1;
    }

    // Open hardware buttons & MIDI pads in non-blocking mode
    int ev_fd   = open(EVDEV_BUTTONS, O_RDONLY | O_NONBLOCK);
    int midi_fd = open(RAWMIDI_PADS, O_RDONLY | O_NONBLOCK);

    // Initial state matches your Stitch screenshot (Low Killed, Mid/High Active)
    EqState state = {
        .low_killed = true,
        .mid_killed = false,
        .high_killed = false,
        .lfo_rate_idx = 2, // 1/16T
        .routing_target = 0,
        .active_macro = 0
    };

    bool dirty = true;
    bool running = true;
    uint8_t midi_buf[3];
    struct input_event ev;

    while (running) {
        // 1. Poll physical buttons (B1, B2, B3, Exit)
        if (ev_fd >= 0) {
            while (read(ev_fd, &ev, sizeof(ev)) == sizeof(ev)) {
                if (ev.type == EV_KEY && ev.value == 1) { // Key press
                    if (ev.code == KEY_MPC_B1) {
                        state.routing_target = (state.routing_target + 1) % 3;
                        dirty = true;
                    } else if (ev.code == KEY_MPC_B2) {
                        bool any = state.low_killed || state.mid_killed || state.high_killed;
                        state.low_killed = state.mid_killed = state.high_killed = !any;
                        dirty = true;
                    } else if (ev.code == KEY_MPC_B3) {
                        state.lfo_rate_idx = (state.lfo_rate_idx + 1) % 4;
                        dirty = true;
                    } else if (ev.code == KEY_MPC_EXIT) {
                        running = false;
                    }
                }
            }
        }

        // 2. Poll Pads 13-16 via ALSA RawMIDI Note-On (0x90)
        if (midi_fd >= 0) {
            while (read(midi_fd, midi_buf, sizeof(midi_buf)) == 3) {
                uint8_t status = midi_buf[0] & 0xF0;
                uint8_t note   = midi_buf[1];
                uint8_t vel    = midi_buf[2];

                if (status == 0x90 && vel > 0) {
                    if (note == PAD_13_NOTE) {        // Pad 13: Bass Drop Kill (Toggle Low)
                        state.low_killed = !state.low_killed;
                        dirty = true;
                    } else if (note == PAD_14_NOTE) { // Pad 14: Telephone Filter (Low + High Kill)
                        bool tel = !(state.low_killed && state.high_killed && !state.mid_killed);
                        state.low_killed = tel;
                        state.mid_killed = false;
                        state.high_killed = tel;
                        dirty = true;
                    } else if (note == PAD_15_NOTE) { // Pad 15: Vinyl Isolate (Low + Mid Kill)
                        bool vin = !(state.low_killed && state.mid_killed && !state.high_killed);
                        state.low_killed = vin;
                        state.mid_killed = vin;
                        state.high_killed = false;
                        dirty = true;
                    } else if (note == PAD_16_NOTE) { // Pad 16: Toggle Mid Kill
                        state.mid_killed = !state.mid_killed;
                        dirty = true;
                    }
                }
            }
        }

        if (dirty) {
            render_ui(&fb, &state);
            dirty = false;
        }

        usleep(16000); // ~60 FPS polling cap
    }

    if (ev_fd >= 0) close(ev_fd);
    if (midi_fd >= 0) close(midi_fd);
    munmap(fb.ptr, fb.screensize);
    close(fb.fd);
    return 0;
}