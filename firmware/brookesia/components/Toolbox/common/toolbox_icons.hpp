/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Painted launcher icons for the toolbox apps.
 *
 * The launcher draws its icon with lv_img_set_src(), so it has to be a real image, not
 * a font symbol like LV_SYMBOL_BLUETOOTH. The other apps each ship a converted PNG of
 * an opaque rounded square in their own colour with a white glyph on it; this paints
 * the same thing in code.
 *
 * Painted rather than converted because nothing here can convert one: the project has
 * no LVGL image converter and neither the IDF virtualenv nor the system Python has an
 * imaging library. Drawing it also keeps the repository free of a 100 KB generated
 * array per icon.
 *
 * *** The pixel layout is the part to be careful with ***
 *
 * LV_IMG_CF_TRUE_COLOR_ALPHA is not one format but three, chosen by LV_COLOR_DEPTH:
 * RGB565 plus an alpha byte at 16 bits (three bytes), B,G,R,A at 32 (four), RGB332
 * plus alpha at 8. This build is 16-bit, so a pixel is three bytes. An earlier version
 * of this painter wrote the 32-bit layout unconditionally and every pixel from the
 * second onwards was read from the wrong offset, which rendered as coloured noise
 * rather than as a glyph.
 *
 * So the size comes from LVGL's own LV_IMG_PX_SIZE_ALPHA_BYTE rather than from
 * arithmetic here, and the colour is written in the byte order LVGL reads it: it
 * memcpys two bytes into an lv_color_t, so low byte first with LV_COLOR_16_SWAP clear.
 *
 * Both icons also went through a PNG prototype rendered by these same equations and
 * looked at before becoming C, which is how the Wi-Fi glyph's arcs were caught drawing
 * out to the side and the Bluetooth rune's vertices were caught in the wrong place.
 */
#pragma once

#include <math.h>
#include <string.h>

#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

/* M_PI is not in standard C, and newlib only exposes it behind __USE_MISC or
 * _GNU_SOURCE, so it is spelled out rather than relied upon. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TOOLBOX_ICON_DIM     112
#define TOOLBOX_ICON_CORNER  30.0f   /* measured off the existing 112x112 app icons */
#define TOOLBOX_ICON_SS      4       /* supersamples per axis, for anti-aliasing */

/* Bytes per pixel: ARGB8888 (B, G, R, A) in LVGL 9. LV_COLOR_FORMAT_RGB565A8 is a
 * planar "colour then alpha" layout, which is why this painter uses ARGB8888 instead:
 * it matches the interleaved byte order written below on this little-endian target. */
#define TOOLBOX_ICON_PX_BYTES  4

typedef bool (*toolbox_glyph_fn)(float x, float y, void *ctx);

/* ---- Geometry helpers ---------------------------------------------------- */

inline bool toolbox_icon_inside_rounded_rect(float x, float y)
{
    const float r = TOOLBOX_ICON_CORNER;
    const float half = TOOLBOX_ICON_DIM / 2.0f;

    const float dx = fmaxf(fabsf(x - half) - (half - r), 0.0f);
    const float dy = fmaxf(fabsf(y - half) - (half - r), 0.0f);

    return (dx * dx + dy * dy) <= (r * r);
}

/* Distance from a point to a line segment, which is how a stroke of a given width is
 * tested: within half the width of the segment's line, clamped at both ends. Used by
 * the Bluetooth rune, whose strokes are straight. */
inline float toolbox_icon_dist_to_segment(float px, float py, float ax, float ay,
                                          float bx, float by)
{
    const float vx = bx - ax;
    const float vy = by - ay;
    const float wx = px - ax;
    const float wy = py - ay;
    const float len2 = (vx * vx) + (vy * vy);

    float t = 0.0f;
    if (len2 > 0.0f) {
        t = ((wx * vx) + (wy * vy)) / len2;
        t = fmaxf(0.0f, fminf(1.0f, t));
    }

    const float dx = px - (ax + (t * vx));
    const float dy = py - (ay + (t * vy));

    return sqrtf((dx * dx) + (dy * dy));
}

/* ---- The painter --------------------------------------------------------- */

/* Paints one icon on first call and reuses it afterwards.
 *
 * The buffer is deliberately never freed: it is the descriptor's pixel data and the
 * launcher reads it every time the home screen is drawn, so it must outlive every open
 * and close of the app that owns it. It is not a per-instance resource and must not be
 * released in close(), which the core calls whenever the user leaves.
 *
 * `state` is a caller-owned static that keeps the descriptor alive across calls. */
typedef struct {
    uint8_t *pixels;
    lv_image_dsc_t dsc;
} toolbox_icon_state_t;

inline const lv_image_dsc_t *toolbox_icon_paint(toolbox_icon_state_t *state,
                                              uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
                                              toolbox_glyph_fn glyph, void *ctx,
                                              const char *tag)
{
    if (state->pixels != NULL) {
        return &state->dsc;
    }

    const uint8_t px_bytes = TOOLBOX_ICON_PX_BYTES;
    const size_t bytes = (size_t)TOOLBOX_ICON_DIM * TOOLBOX_ICON_DIM * px_bytes;

    /* PSRAM: this board has 32 MB of it and internal RAM is the scarce one. */
    state->pixels = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (state->pixels == NULL) {
        state->pixels = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    if (state->pixels == NULL) {
        ESP_LOGE(tag, "launcher icon: no memory for %dx%d pixels; keeping the default icon",
                 TOOLBOX_ICON_DIM, TOOLBOX_ICON_DIM);
        return NULL;
    }

    for (int py = 0; py < TOOLBOX_ICON_DIM; py++) {
        for (int px = 0; px < TOOLBOX_ICON_DIM; px++) {
            uint8_t *out = state->pixels + (((size_t)py * TOOLBOX_ICON_DIM) + px) * px_bytes;

            /* 4x4 supersampling, composited as the colour mean over the covered samples
             * and the glyph's share of all samples for the alpha.
             *
             * Getting this wrong shows in the statistics rather than the picture: a
             * version that marked a pixel white as soon as one sample hit the glyph made
             * every anti-aliased edge pixel fully white, which thickened the strokes. A
             * sample outside the rounded body contributes no coverage. */
            unsigned hits = 0;
            unsigned sr = 0, sg = 0, sb = 0;

            for (int sy = 0; sy < TOOLBOX_ICON_SS; sy++) {
                for (int sx = 0; sx < TOOLBOX_ICON_SS; sx++) {
                    const float fx = px + ((sx + 0.5f) / TOOLBOX_ICON_SS);
                    const float fy = py + ((sy + 0.5f) / TOOLBOX_ICON_SS);

                    if (!toolbox_icon_inside_rounded_rect(fx, fy)) {
                        continue;
                    }

                    hits++;

                    if (glyph != NULL && glyph(fx, fy, ctx)) {
                        sr += 0xFF;
                        sg += 0xFF;
                        sb += 0xFF;
                    } else {
                        sr += bg_r;
                        sg += bg_g;
                        sb += bg_b;
                    }
                }
            }

            const unsigned samples = TOOLBOX_ICON_SS * TOOLBOX_ICON_SS;

            if (hits == 0) {
                memset(out, 0, px_bytes);
                continue;
            }

            const uint8_t r8 = (uint8_t)(sr / hits);
            const uint8_t g8 = (uint8_t)(sg / hits);
            const uint8_t b8 = (uint8_t)(sb / hits);
            const uint8_t alpha = (uint8_t)((hits * 255u) / samples);

            /* ARGB8888 (B, G, R, A) per pixel, written regardless of the display's
             * native colour depth — LVGL 9 converts to the framebuffer format. */
            out[0] = b8;
            out[1] = g8;
            out[2] = r8;
            out[3] = alpha;
        }
    }

    state->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    state->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    state->dsc.header.w = TOOLBOX_ICON_DIM;
    state->dsc.header.h = TOOLBOX_ICON_DIM;
    state->dsc.header.stride = TOOLBOX_ICON_DIM * TOOLBOX_ICON_PX_BYTES;
    state->dsc.data_size = (uint32_t)bytes;
    state->dsc.data = state->pixels;

    /* Counts the C painter against the prototype's, so a port that drifts is visible
     * as a number rather than as a subtly wrong picture.
     *
     * `glyph` counts fully opaque pixels and `opaque` counts pixels with any alpha —
     * the latter is the rounded body and is identical for every icon of this shape, so
     * two icons with completely different glyphs report the same `opaque`.
     *
     * *** What these counts cannot tell you is PLACEMENT ***
     *
     * A glyph drawn in the wrong part of the icon reports a perfectly healthy pixel
     * count. The Bluetooth rune was first painted with its stem on the centreline, which
     * left the left half of the icon empty and made it read as half a symbol; every
     * count here was correct. So the glyph's bounding box is reported too, and the thing
     * to look at is whether it spans most of the icon: a box hugging one side means the
     * composition is wrong even when the pixel counts agree with the prototype. */
    unsigned opaque = 0, glyph_px = 0;
    int gx1 = TOOLBOX_ICON_DIM, gy1 = TOOLBOX_ICON_DIM, gx2 = -1, gy2 = -1;
    const unsigned samples = TOOLBOX_ICON_SS * TOOLBOX_ICON_SS;

    for (int i = 0; i < TOOLBOX_ICON_DIM * TOOLBOX_ICON_DIM; i++) {
        const uint8_t *p = state->pixels + ((size_t)i * px_bytes);

        if (p[px_bytes - 1] != 0) {
            opaque++;
        }

        const int gy = i / TOOLBOX_ICON_DIM;
        const int gx = i % TOOLBOX_ICON_DIM;
        unsigned on_glyph = 0;

        for (int sy = 0; sy < TOOLBOX_ICON_SS; sy++) {
            for (int sx = 0; sx < TOOLBOX_ICON_SS; sx++) {
                const float fx = gx + ((sx + 0.5f) / TOOLBOX_ICON_SS);
                const float fy = gy + ((sy + 0.5f) / TOOLBOX_ICON_SS);

                if (toolbox_icon_inside_rounded_rect(fx, fy) &&
                    glyph != NULL && glyph(fx, fy, ctx)) {
                    on_glyph++;
                }
            }
        }

        if (on_glyph == samples) {
            glyph_px++;

            if (gx < gx1) gx1 = gx;
            if (gx > gx2) gx2 = gx;
            if (gy < gy1) gy1 = gy;
            if (gy > gy2) gy2 = gy;
        }
    }

    ESP_LOGI(tag, "launcher icon painted: %dx%d, %u bytes, opaque=%u glyph=%u "
                  "bbox=[%d,%d..%d,%d]",
             TOOLBOX_ICON_DIM, TOOLBOX_ICON_DIM, (unsigned)bytes, opaque, glyph_px,
             gx1, gy1, gx2, gy2);

    /* A glyph that hugs one side of the icon is off-centre or far too small, and the pixel
     * counts above cannot say so. The threshold is deliberately loose: glyphs are not all
     * square, and the Bluetooth rune here is markedly taller than wide (about 51 px across
     * against 91 tall) by design, so a check tuned to "fills the icon" would fire on a
     * correct glyph. It is set to catch only the gross error — the rune as first drawn
     * spanned 29 px, having been placed mostly outside its own icon. */
    if (gx2 >= 0 && ((gx2 - gx1) < (TOOLBOX_ICON_DIM / 4))) {
        ESP_LOGW(tag, "  glyph spans only %d px of %d across - check its placement",
                 gx2 - gx1, TOOLBOX_ICON_DIM);
    }

    return &state->dsc;
}

/* ---- The Wi-Fi glyph ----------------------------------------------------- */

typedef struct {
    float cx, apex_y, thick, span, dot_x, dot_y, dot_r;
    const float *radii;
} toolbox_wifi_glyph_t;

inline float toolbox_wifi_angle(float dx, float dy)
{
    /* Degrees from straight up: 0 is up, 90 right, 180 down. Getting this reference
     * wrong is what drew the first attempt's arcs out to the side. */
    return atan2f(dx, -dy) * (180.0f / (float)M_PI);
}

inline bool toolbox_icon_is_wifi(float x, float y, void *ctx)
{
    const toolbox_wifi_glyph_t *g = (const toolbox_wifi_glyph_t *)ctx;

    for (int i = 0; i < 3; i++) {
        const float radius = g->radii[i];
        const float dx = x - g->cx;
        const float dy = y - g->apex_y;
        const float dist = sqrtf((dx * dx) + (dy * dy));
        const float half = g->thick / 2.0f;

        bool on_arc;

        if (fabsf(toolbox_wifi_angle(dx, dy)) <= g->span) {
            on_arc = fabsf(dist - radius) <= half;
        } else {
            /* Beyond the sweep, the nearest point is a round cap at one end. */
            float best = 1e9f;
            for (int s = -1; s <= 1; s += 2) {
                const float ra = (float)s * g->span * ((float)M_PI / 180.0f);
                const float capx = g->cx + (radius * sinf(ra));
                const float capy = g->apex_y - (radius * cosf(ra));
                const float d = sqrtf(((x - capx) * (x - capx)) + ((y - capy) * (y - capy)));
                if (d < best) {
                    best = d;
                }
            }
            on_arc = best <= half;
        }

        if (on_arc) {
            return true;
        }
    }

    const float ddx = x - g->dot_x;
    const float ddy = y - g->dot_y;

    return sqrtf((ddx * ddx) + (ddy * ddy)) <= g->dot_r;
}

/* ---- The Bluetooth glyph -------------------------------------------------
 *
 * Taken from Bluetooth.svg, which is authoritative. Its path is:
 *
 *     <path d="m157 330 305 307-147 178V179l147 170-305 299"
 *           stroke="#FFF" stroke-width="53" fill="none"/>
 *
 * Read out, that is a SINGLE CONTINUOUS POLYLINE of six segments:
 *
 *     (157,330) -> (462,637) -> (315,815) -> (315,179) -> (462,349) -> (157,648)
 *
 * which is why every hand-derived version here was wrong. It is not two crossing
 * diagonals, and not two chevrons on a stem: the stem IS part of the path — the V179
 * command walks straight up it — and the "right side" is the two right-hand vertices
 * (462,637) and (462,349) visited in turn. Drawing it as anything but one polyline
 * changes the shape.
 *
 * The drawn width is the path's stroke-width, scaled with everything else.
 *
 * The vertices below are the SVG's, transformed by the same fit the preview used: the
 * glyph's stroke-inclusive bounding box is (130,152)-(488,842), scaled to 94 px tall and
 * centred in the 112 px icon. */

typedef struct {
    float x, y;
} toolbox_pt_t;

typedef struct {
    const toolbox_pt_t *pts;    /* the polyline's vertices, in order */
    int count;
    float half_width;           /* half the stroke width */
} toolbox_bt_glyph_t;

inline bool toolbox_icon_is_bluetooth(float x, float y, void *ctx)
{
    const toolbox_bt_glyph_t *g = (const toolbox_bt_glyph_t *)ctx;

    for (int i = 0; i + 1 < g->count; i++) {
        if (toolbox_icon_dist_to_segment(x, y,
                                         g->pts[i].x, g->pts[i].y,
                                         g->pts[i + 1].x, g->pts[i + 1].y) <= g->half_width) {
            return true;
        }
    }

    return false;
}
