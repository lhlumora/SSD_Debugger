/*
 * Test screen of the display
 *
 * See lcd_ui.h for the description of the module.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "lcd_display.h"
#include "lcd_ui.h"
#include "tft_panel.h"

static const char *TAG = "lcd_ui";

/* Colors of the test screen */
#define COLOR_BACKGROUND lv_color_hex(0x0E1621)
#define COLOR_HEADER     lv_color_hex(0x1B2A3A)
#define COLOR_TITLE      lv_color_hex(0xE8F1F8)
#define COLOR_LABEL      lv_color_hex(0x7A8CA0)
#define COLOR_VALUE      lv_color_hex(0xE8F1F8)
#define COLOR_OK         lv_color_hex(0x4CD964)
#define COLOR_WARN       lv_color_hex(0xFF9F0A)
#define COLOR_ERROR      lv_color_hex(0xFF453A)
#define COLOR_BORDER     lv_color_hex(0x506070)
#define COLOR_AREA       lv_color_hex(0x16222E)

/* The colors of the marks in the corners of the screen. They are the
   primary colors, they show at a glance if the image is shifted, turned or
   mirrored. */
#define COLOR_MARK_TOP_LEFT     lv_color_hex(0xFF0000)   /* red */
#define COLOR_MARK_TOP_RIGHT    lv_color_hex(0x00FF00)   /* green */
#define COLOR_MARK_BOTTOM_LEFT  lv_color_hex(0x0000FF)   /* blue */
#define COLOR_MARK_BOTTOM_RIGHT lv_color_hex(0xFFFF00)   /* yellow */

/* The size of the elements of the screen is computed from the size of the
   screen, they are the values below for a 128x160 screen. */
#define REFERENCE_WIDTH  128
#define HEADER_HEIGHT    15
#define SWATCH_HEIGHT   14
#define RAMP_HEIGHT      6
#define WIDGET_HEIGHT   28
#define BALL_SIZE       12
#define MARK_SIZE        4

/* The size of the elements of the screen */
static int s_scale = 1;
static int s_header_height;
static int s_swatch_height;
static int s_ramp_height;
static int s_widget_height;
static int s_info_height;
static int s_ball_size;
static int s_mark_size;

/* The objects of the screen */
static lv_obj_t    *s_ball;
static lv_obj_t    *s_bar;
static lv_obj_t    *s_arc;
static lv_obj_t    *s_fps;
static lv_obj_t    *s_info;
static lv_timer_t  *s_timer;
static bool         s_created;

/* The time when the counter of the frames was reset */
static int64_t s_frames_time;

/* The font of the title and of the information, they depend on the size of
   the screen */
static const lv_font_t *title_font(void)
{
    return s_scale > 1 ? &lv_font_montserrat_16 : &lv_font_montserrat_14;
}

static const lv_font_t *info_font(void)
{
    return s_scale > 1 ? &lv_font_montserrat_14 : &lv_font_montserrat_10;
}

/* Compute the size of the elements of the screen from its size */
static void compute_sizes(int width, int height)
{
    /* The scale is the number of times the screen is bigger than the
       reference one, it is rounded to the nearest integer */
    s_scale = (width + REFERENCE_WIDTH / 2) / REFERENCE_WIDTH;
    if (s_scale < 1) {
        s_scale = 1;
    }

    s_header_height = HEADER_HEIGHT * s_scale;
    s_swatch_height = SWATCH_HEIGHT * s_scale;
    s_ramp_height = RAMP_HEIGHT * s_scale;
    s_widget_height = WIDGET_HEIGHT * s_scale;
    s_ball_size = BALL_SIZE * s_scale;
    s_mark_size = MARK_SIZE * s_scale;

    /* The block of the information holds three lines of text */
    s_info_height = info_font()->line_height * 3 + 4;

    /* The block of the information must fit in the screen */
    const int fixed = s_header_height + s_swatch_height + s_ramp_height +
                      s_widget_height + s_info_height;
    if (fixed > height) {
        s_info_height = height - fixed + s_info_height;
        if (s_info_height < 0) {
            s_info_height = 0;
        }
    }
}

/* Move the ball horizontally */
static void ball_x_cb(void *obj, int32_t value)
{
    lv_obj_set_x(obj, value);
}

/* Move the ball vertically */
static void ball_y_cb(void *obj, int32_t value)
{
    lv_obj_set_y(obj, value);
}

/* Move the indicator of the bar */
static void bar_anim_cb(void *obj, int32_t value)
{
    lv_bar_set_value((lv_obj_t *)obj, value, LV_ANIM_OFF);
}

/* Move the indicator of the arc */
static void arc_anim_cb(void *obj, int32_t value)
{
    lv_arc_set_value((lv_obj_t *)obj, value);
}

/* Create an object that is only a rectangle of a color */
static lv_obj_t *create_rect(lv_obj_t *parent, int x, int y, int width, int height,
                             lv_color_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_bg_color(obj, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(obj, false);
    return obj;
}

/* The colors of the eight squares of the first row. They show if the order of
   the colors of the panel is the expected one. */
static const uint32_t swatch_colors[] = {
    0xFF0000,   /* red */
    0x00FF00,   /* green */
    0x0000FF,   /* blue */
    0xFFFFFF,   /* white */
    0x000000,   /* black */
    0xFFFF00,   /* yellow */
    0x00FFFF,   /* cyan */
    0xFF00FF,   /* magenta */
};

/* The shades of gray of the second row, from black to white */
static void create_gray_ramp(lv_obj_t *parent, int x, int y, int width, int height)
{
    const int steps = 16;
    const int step_width = width / steps;

    for (int i = 0; i < steps; i++) {
        const uint8_t level = (uint8_t)(i * 255 / (steps - 1));
        create_rect(parent, x + i * step_width, y, step_width, height,
                    lv_color_make(level, level, level));
    }
}

/* The four marks in the corners of the screen. They show if the image is
   shifted or turned. */
static void create_corner_marks(lv_obj_t *parent, int width, int height)
{
    const int size = s_mark_size;
    create_rect(parent, 0, 0, size, size, COLOR_MARK_TOP_LEFT);
    create_rect(parent, width - size, 0, size, size, COLOR_MARK_TOP_RIGHT);
    create_rect(parent, 0, height - size, size, size, COLOR_MARK_BOTTOM_LEFT);
    create_rect(parent, width - size, height - size, size, size, COLOR_MARK_BOTTOM_RIGHT);
}

/* Format the running time as hh:mm:ss */
static void format_uptime(char *buf, size_t len)
{
    const int64_t seconds = esp_timer_get_time() / 1000000;
    const int hours = (int)(seconds / 3600);
    const int minutes = (int)((seconds % 3600) / 60);
    const int secs = (int)(seconds % 60);
    snprintf(buf, len, "%02d:%02d:%02d", hours, minutes, secs);
}

/* Update the information shown at the bottom of the screen */
static void update_info(lv_timer_t *timer)
{
    const int64_t now = esp_timer_get_time();
    const int64_t elapsed = now - s_frames_time;

    /* The number of frames drawn during the last second */
    int fps = 0;
    if (elapsed >= 1000000) {
        fps = (int)(lcd_display_take_frame_count() * 1000000 / elapsed);
        s_frames_time = now;
    }

    lv_label_set_text_fmt(s_fps, "%d FPS", fps);

    char uptime[16] = { 0 };
    format_uptime(uptime, sizeof(uptime));

    int width = 0;
    int height = 0;
    lcd_display_get_size(&width, &height);

    int col_start = 0;
    int row_start = 0;
    tft_panel_get_offset(&col_start, &row_start);

    lv_label_set_text_fmt(s_info,
                          "%dx%d %s %dMHz\n"
                          "off %d/%d rot %d\n"
                          "LVGL %d.%d.%d BL %d%% %s",
                          width, height,
                          (tft_panel_get_madctl() & 0x08) ? "BGR" : "RGB",
                          tft_panel_get_clock() / 1000000,
                          col_start, row_start,
                          lcd_display_get_rotation(),
                          LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
                          lcd_display_get_brightness(), uptime);
}

/* Create the screen of the test */
esp_err_t lcd_ui_create(void)
{
    if (s_created) {
        return ESP_OK;
    }

    lv_display_t *display = lv_display_get_default();
    const int width = lv_display_get_horizontal_resolution(display);
    const int height = lv_display_get_vertical_resolution(display);

    /* The size of the elements depends on the size of the screen */
    compute_sizes(width, height);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, COLOR_BACKGROUND, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scroll_dir(screen, LV_DIR_NONE);

    /* The header shows the name of the test and the number of the frames */
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, width, s_header_height);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, COLOR_HEADER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "LVGL TEST");
    lv_obj_set_style_text_color(title, COLOR_TITLE, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, title_font(), LV_PART_MAIN);
    /* The title is shifted to the right, the mark of the top left corner is
       drawn over the header */
    lv_obj_set_pos(title, s_mark_size + 2, 0);

    s_fps = lv_label_create(header);
    lv_label_set_text(s_fps, "0 FPS");
    lv_obj_set_style_text_color(s_fps, COLOR_OK, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_fps, s_scale > 1 ? &lv_font_montserrat_14
                                                  : &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_pos(s_fps, width - 48 * s_scale, s_scale);

    /* The row of the colors, it shows if the order of the colors is the
       expected one */
    int y = s_header_height;
    const int swatch_width = width / (int)(sizeof(swatch_colors) / sizeof(swatch_colors[0]));
    for (size_t i = 0; i < sizeof(swatch_colors) / sizeof(swatch_colors[0]); i++) {
        create_rect(screen, (int)i * swatch_width, y, swatch_width, s_swatch_height,
                    lv_color_hex(swatch_colors[i]));
    }
    y += s_swatch_height;

    /* The row of the shades of gray */
    create_gray_ramp(screen, 0, y, width, s_ramp_height);
    y += s_ramp_height;

    /* The area of the animation, it takes all the space that is left */
    int area_height = height - y - s_widget_height - s_info_height;
    if (area_height < 0) {
        area_height = 0;
    }
    lv_obj_t *area = create_rect(screen, 0, y, width, area_height, COLOR_AREA);

    /* The ball moves in the area */
    s_ball = create_rect(area, 0, 0, s_ball_size, s_ball_size, COLOR_WARN);
    lv_obj_set_style_radius(s_ball, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    lv_anim_t anim_x;
    lv_anim_init(&anim_x);
    lv_anim_set_var(&anim_x, s_ball);
    lv_anim_set_exec_cb(&anim_x, ball_x_cb);
    lv_anim_set_values(&anim_x, 0, area_height > 0 ? width - s_ball_size : 0);
    lv_anim_set_duration(&anim_x, 1600);
    lv_anim_set_path_cb(&anim_x, lv_anim_path_bounce);
    lv_anim_set_repeat_count(&anim_x, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim_x);

    lv_anim_t anim_y;
    lv_anim_init(&anim_y);
    lv_anim_set_var(&anim_y, s_ball);
    lv_anim_set_exec_cb(&anim_y, ball_y_cb);
    lv_anim_set_values(&anim_y, 0, area_height > 0 ? area_height - s_ball_size : 0);
    lv_anim_set_duration(&anim_y, 2400);
    lv_anim_set_path_cb(&anim_y, lv_anim_path_bounce);
    lv_anim_set_repeat_count(&anim_y, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim_y);

    y += area_height;

    /* The arc and the bar show that the animations work */
    const int arc_size = s_widget_height;
    s_arc = lv_arc_create(screen);
    lv_obj_set_size(s_arc, arc_size, arc_size);
    lv_obj_set_pos(s_arc, 2, y);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 25);
    lv_arc_set_rotation(s_arc, 270);
    lv_obj_set_style_arc_color(s_arc, COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, COLOR_OK, LV_PART_INDICATOR);

    lv_anim_t anim_arc;
    lv_anim_init(&anim_arc);
    lv_anim_set_var(&anim_arc, s_arc);
    lv_anim_set_exec_cb(&anim_arc, arc_anim_cb);
    lv_anim_set_values(&anim_arc, 0, 100);
    lv_anim_set_duration(&anim_arc, 2000);
    lv_anim_set_repeat_count(&anim_arc, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim_arc);

    const int bar_margin = arc_size + 6;
    const int bar_height = 10 * s_scale;
    s_bar = lv_bar_create(screen);
    lv_obj_set_size(s_bar, width - bar_margin - 4, bar_height);
    lv_obj_set_pos(s_bar, bar_margin, y + (arc_size - bar_height) / 2);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 50, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, COLOR_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, COLOR_OK, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 2, LV_PART_MAIN);

    lv_anim_t anim_bar;
    lv_anim_init(&anim_bar);
    lv_anim_set_var(&anim_bar, s_bar);
    lv_anim_set_exec_cb(&anim_bar, bar_anim_cb);
    lv_anim_set_values(&anim_bar, 0, 100);
    lv_anim_set_duration(&anim_bar, 1500);
    lv_anim_set_repeat_count(&anim_bar, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim_bar);

    y += s_widget_height;

    /* The information about the panel */
    s_info = lv_label_create(screen);
    lv_label_set_text(s_info, "");
    lv_obj_set_style_text_color(s_info, COLOR_VALUE, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_info, info_font(), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(s_info, 0, LV_PART_MAIN);
    lv_obj_set_pos(s_info, 3, y);

    /* The border and the marks in the corners, they show if the image is
       shifted or turned */
    lv_obj_t *border = create_rect(screen, 1, 1, width - 2, height - 2, COLOR_BACKGROUND);
    lv_obj_set_style_bg_opa(border, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(border, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(border, COLOR_BORDER, LV_PART_MAIN);

    create_corner_marks(screen, width, height);

    /* The timer that refreshes the information */
    s_frames_time = esp_timer_get_time();
    s_timer = lv_timer_create(update_info, 500, NULL);

    s_created = true;
    ESP_LOGI(TAG, "Test screen created (%dx%d, scale %d)", width, height, s_scale);
    return ESP_OK;
}

void lcd_ui_recreate(void)
{
    if (!s_created) {
        return;
    }

    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }

    /* The screen of LVGL is emptied and the test screen is built again with
       the new size of the screen */
    lv_obj_clean(lv_screen_active());
    s_created = false;

    lcd_ui_create();
}
