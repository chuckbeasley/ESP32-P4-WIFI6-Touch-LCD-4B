/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared LVGL building blocks for the toolbox apps.
 *
 * These began as file-local statics inside ToolboxApp.cpp. They moved here when the
 * BLE Toolbox needed the same panels, because several of them encode fixes that are
 * not obvious from reading them and would be quietly lost in a copy — above all
 * make_screen's nesting, which is the difference between working navigation and a
 * blank screen (see the note in its body).
 *
 * Header-only with `inline`, deliberately: an ESP-IDF component is the alternative,
 * but that would mean a CMakeLists and a dependency edge for what is four small
 * functions, and the callers are the only ones who ever want them. `inline` gives
 * them one definition across translation units without the extra component.
 *
 * Nothing here holds state or touches the radio.
 */
#pragma once

#include "lvgl.h"

/* Text sizes for the toolbox screens, in one place.
 *
 * These are larger than the theme's default (montserrat_18, see sdkconfig.defaults) because
 * a screen title and a log of captured data are read at different distances: the title is
 * scanned, the data is studied. Keeping the numbers here rather than scattered as literals
 * across two apps means they stay consistent, which they were not before - the BLE app had
 * its body text at 12 while the Wi-Fi app used the theme's 14, so the two toolboxes did not
 * match each other.
 *
 * kToolboxFontLog stays comparatively small on purpose: the Observer's log is a scrolling
 * list of MAC addresses, and at title size a handful of frames would fill the screen. */
#define TOOLBOX_FONT_TITLE   (&lv_font_montserrat_26)
#define TOOLBOX_FONT_BODY    (&lv_font_montserrat_20)
#define TOOLBOX_FONT_DETAIL  (&lv_font_montserrat_16)
#define TOOLBOX_FONT_LOG     (&lv_font_montserrat_16)

/* A titled panel, built INSIDE the container it belongs to.
 *
 * The parent argument is not cosmetic. A screen is switched by hiding and showing its
 * container, so a panel created anywhere else is not switched at all — it simply
 * stays on top of everything. The first version of the Wi-Fi app built its panels on
 * lv_screen_active(), which made them siblings of the containers rather than children:
 * opening a sub-screen drew a second panel over the menu, and the back button then
 * hid a container that had never been visible, leaving a blank screen.
 *
 * A null back_cb means no back button, which is right for a menu — the top of an app
 * has nowhere to go back to. */
inline lv_obj_t *toolbox_make_screen(lv_obj_t *parent, lv_area_t area, const char *title,
                                     const char *back_label, lv_event_cb_t back_cb, void *user)
{
    const int w = area.x2 - area.x1 + 1;
    const int h = area.y2 - area.y1 + 1;

    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_size(panel, w, h);
    lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_set_style_pad_row(panel, 6, 0);

    /* Scroll when the content does not fit, with the bar always visible rather than
     * only mid-drag: the point is to show at a glance that there is more below. On the
     * Wi-Fi app this was not cosmetic — its injection screen needed 788 px in a 670 px
     * panel, and 118 px of controls were simply unreachable with scrolling off.
     *
     * LV_SCROLLBAR_MODE_ON overlays the panel's right edge without reserving a strip,
     * so nothing shifts sideways when the bar appears. */
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_ON);
    lv_obj_set_style_width(panel, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(panel, lv_color_make(190, 60, 60), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(panel, LV_OPA_70, LV_PART_SCROLLBAR);

    lv_obj_t *header = lv_obj_create(panel);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    if (back_cb != NULL) {
        lv_obj_t *back = lv_btn_create(header);
        lv_obj_t *back_txt = lv_label_create(back);
        lv_label_set_text(back_txt, back_label);
        lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, user);
    }

    lv_obj_t *title_label = lv_label_create(header);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, TOOLBOX_FONT_TITLE, 0);

    return panel;
}

/* A label on the left and a value on the right. */
inline lv_obj_t *toolbox_make_label_row(lv_obj_t *parent, const char *label_text, lv_obj_t **out_value)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, label_text);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text(value, "-");

    if (out_value != NULL) {
        *out_value = value;
    }

    return row;
}

/* A scrolling list of lines.
 *
 * *** This returns a CONTAINER, not a label, and that is the whole point. ***
 *
 * It used to return a single label with LV_LABEL_LONG_WRAP. A label wraps to its width and
 * then stops: anything past its box is clipped and there is no scrolling back to it. A list
 * built that way therefore grows silently unreachable content — reported as the BLE
 * Observer "not being scrollable" once it held more frames than fitted.
 *
 * A container with LV_DIR_VER scrolls, so callers add one line per entry with
 * toolbox_make_log_line() and the whole list stays reachable. */
inline lv_obj_t *toolbox_make_log(lv_obj_t *parent)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    /* ACTIVE rather than ON: this sits inside a panel that scrolls too, and two always-on
     * bars side by side is noise. The bar appears when the list can actually move. */
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);

    return list;
}

/* One line in a toolbox_make_log() list. */
inline lv_obj_t *toolbox_make_log_line(lv_obj_t *list, const char *text)
{
    lv_obj_t *line = lv_label_create(list);
    lv_obj_set_style_text_font(line, TOOLBOX_FONT_LOG, 0);
    lv_label_set_text(line, text);

    return line;
}

/* Measure a menu's buttons against the width they have to fit in.
 *
 * Menu entries are text, so raising their font is a change that can silently clip: a label
 * wider than its button does not shrink, it overflows, and the entry stops being readable.
 * This reports each entry's text width next to the space available, so the size is chosen
 * from a measurement.
 *
 * Three things had to be right for the numbers to mean anything, and a first version got
 * all three wrong:
 *
 *   - the text width comes from lv_txt_get_size() with the button's own font, NOT from
 *     lv_obj_get_width() on the label. At this point layout has not run, and an
 *     unlaid-out label reports width 0 — so the first version compared 0 against the
 *     available width and declared every entry too wide.
 *   - the available width comes from the button's own content area, not the panel's.
 *     lv_obj_get_content_width() on a flex container with padding and a border returns a
 *     NEGATIVE number, which the first version printed as "-20 px available".
 *   - it is called once per menu build, not per screen switch. The first version logged on
 *     every switch to the menu, which is several times a second while navigating.
 *
 * Returns the number of entries that do not fit. */
inline int toolbox_measure_menu(lv_obj_t *panel, const char *tag)
{
    /* Layout must be resolved first. Button widths here are LV_PCT(100), and until a
     * layout pass runs a percentage width is unresolved — lv_obj_get_width() then returns
     * 0, which after subtracting the button's padding reads as a NEGATIVE available width
     * and makes every entry look too wide. A first version of this measured before the
     * layout and reported "-40 px" for all three. */
    lv_obj_update_layout(panel);

    int too_wide = 0;
    int entries = 0;

    const uint32_t n = lv_obj_get_child_cnt(panel);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *btn = lv_obj_get_child(panel, i);
        if (btn == NULL || !lv_obj_check_type(btn, &lv_button_class)) {
            continue;
        }

        lv_obj_t *label = lv_obj_get_child(btn, 0);
        if (label == NULL) {
            continue;
        }

        /* The button's inner width: its resolved width less its own padding. */
        const lv_coord_t box = lv_obj_get_width(btn)
                               - lv_obj_get_style_pad_left(btn, LV_PART_MAIN)
                               - lv_obj_get_style_pad_right(btn, LV_PART_MAIN);

        const char *text = lv_label_get_text(label);
        const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);

        lv_point_t size = { 0, 0 };
        lv_txt_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

        const bool fits = size.x <= box;
        if (!fits) {
            too_wide++;
        }

        ESP_LOGI(tag, "  menu entry %d: text %d px in %d px  %s",
                 ++entries, (int)size.x, (int)box, fits ? "fits" : "TOO WIDE");
    }

    /* Height as well as width: the menu is a column of fixed-height buttons, and an
     * overlong column scrolls rather than clipping, so it is usable — but worth knowing. */
    int column = 0;
    for (uint32_t i = 0; i < n; i++) {
        column += lv_obj_get_height(lv_obj_get_child(panel, i));
    }
    column += (int)((n > 0 ? n - 1 : 0) * lv_obj_get_style_pad_row(panel, LV_PART_MAIN));

    ESP_LOGI(tag, "menu fit: column %d px in %d px of panel%s",
             column, (int)lv_obj_get_height(panel),
             column > lv_obj_get_height(panel) ? "  (scrolls)" : "");

    return too_wide;
}

/* A full-width action button with the given handler, and optionally a font.
 *
 * A NULL font leaves the label on the theme's font, which is what most callers want. The
 * menus pass one because their entries are the app's primary navigation and are read at a
 * glance, whereas an in-screen Start/Stop pair sits next to its own context. */
inline lv_obj_t *toolbox_make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                                     void *user, int height,
                                     const lv_font_t *font = nullptr)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, height);

    lv_obj_t *txt = lv_label_create(btn);
    lv_label_set_text(txt, text);
    if (font != nullptr) {
        lv_obj_set_style_text_font(txt, font, 0);
    }
    lv_obj_center(txt);

    if (cb != NULL) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    }

    return btn;
}

