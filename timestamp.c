#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

#include "text.h"
#include "timestamp.h"

const static char *default_time_format = "%Y-%m-%d %H:%M:%S";
const static char *default_font_name = "Nimbus Mono L,monospace";

static int text_id = -1;
static char time_format[128];
static time_t last_time_drawn;

/**
 * Initializes the timestamp library with a font name.
 */
void timestamp_init_with_font_name(const char *font_name, int points, int dpi) {
  char *font_file;
  int face_index;

  if (font_name != NULL) {
    text_select_font_file(font_name, &font_file, &face_index);
  } else {
    text_select_font_file(default_font_name, &font_file, &face_index);
  }
  timestamp_init(font_file, face_index, points, dpi);
  free(font_file);
}

/**
 * Initializes the timestamp library with a font file and face index.
 */
void timestamp_init(const char *font_file, long face_index, int points, int dpi) {
  text_id = text_create(
    font_file, face_index,
    points,
    dpi
  );
  timestamp_set_format(default_time_format);
  text_set_stroke_color(text_id, 0x000000);
  text_set_stroke_width(text_id, 1.0f);
  text_set_color(text_id, 0xffffff);
  text_set_layout(text_id,
      LAYOUT_ALIGN_BOTTOM | LAYOUT_ALIGN_RIGHT, // layout alignment for the box
      5, // horizontal margin from the right edge
      5); // vertical margin from the bottom edge
  text_set_align(text_id, TEXT_ALIGN_LEFT); // text alignment inside the box
  last_time_drawn = 0;
}

/**
 * Sets timestamp text format.
 */
void timestamp_set_format(const char *format) {
  strncpy(time_format, format, sizeof(time_format) - 1);
  time_format[sizeof(time_format) - 1] = '\0';
}

/**
 * Sets text color.
 */
void timestamp_set_color(int color) {
  text_set_color(text_id, color);
}

/**
 * Sets text stroke color.
 */
void timestamp_set_stroke_color(uint32_t color) {
  text_set_stroke_color(text_id, color);
}

/**
 * Sets text stroke border width in points.
 */
void timestamp_set_stroke_width(float stroke_width) {
  text_set_stroke_width(text_id, stroke_width);
}

/**
 * Sets letter spacing in pixels.
 */
void timestamp_set_letter_spacing(int pixels) {
  text_set_letter_spacing(text_id, pixels);
}

/**
 * Sets multiplying factor for line spacing.
 * If this is set to 1, default line spacing is used.
 */
void timestamp_set_line_height_multiply(float multiply) {
  text_set_line_height_multiply(text_id, multiply);
}

/**
 * Sets the absolute position for the timestamp.
 */
void timestamp_set_position(int x, int y) {
  text_set_position(text_id, x, y);
}

/**
 * Sets the relative layout for the text in the screen.
 */
void timestamp_set_layout(LAYOUT_ALIGN layout_align, int horizontal_margin, int vertical_margin) {
  text_set_layout(text_id, layout_align, horizontal_margin, vertical_margin);
}

/**
 * Sets the text alignment inside a positioned box.
 */
void timestamp_set_align(TEXT_ALIGN text_align) {
  text_set_align(text_id, text_align);
}

/**
 * Assigns an absolute position for the timestamp based on
 * the relative layout constraints that is currently set.
 */
void timestamp_fix_position(int canvas_width, int canvas_height) {
  struct tm *timeinfo;
  char str[128];

  time_t rawtime = 0;
  timeinfo = gmtime(&rawtime);
  strftime(str, sizeof(str)-1, time_format, timeinfo);
  str[sizeof(str)-1] = '\0';

  text_set_text(text_id, str, strlen(str));
  redraw_text(text_id);
  text_fix_position(text_id, canvas_width, canvas_height);
  text_clear(text_id);
}

/**
 * Call this function before calling text_draw_all().
 */
void timestamp_update() {
  time_t rawtime;
  struct tm *timeinfo;
  char str[128];

  time(&rawtime);

  if (rawtime > last_time_drawn) {
    timeinfo = localtime(&rawtime);
    strftime(str, sizeof(str)-1, time_format, timeinfo);
    str[sizeof(str)-1] = '\0';

    text_set_text(text_id, str, strlen(str));
    redraw_text(text_id);
    last_time_drawn = rawtime;
  }
}

/**
 * Destroys the resources used by timestamp library.
 */
void timestamp_shutdown() {
  text_destroy(text_id);
}
