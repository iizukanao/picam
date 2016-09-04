#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

#include "text.h"
#include "subtitle.h"

const static char *default_font_name = "sans";

static int text_id = -1;
static int64_t hide_time = 0;

/**
 * Initializes the timestamp library with a font name.
 */
void subtitle_init_with_font_name(const char *font_name, int points, int dpi) {
  char *font_file;
  int face_index;

  if (font_name != NULL) {
    text_select_font_file(font_name, &font_file, &face_index);
  } else {
    text_select_font_file(default_font_name, &font_file, &face_index);
  }
  subtitle_init(font_file, face_index, points, dpi);
  free(font_file);
}

/**
 * Initializes the timestamp library with a font file and face index.
 */
void subtitle_init(const char *font_file, long face_index, int points, int dpi) {
  if (text_id != -1) {
    text_destroy(text_id);
  }

  text_id = text_create(
    font_file, face_index,
    points,
    dpi
  );
  text_set_stroke_color(text_id, 0x000000);
  text_set_letter_spacing(text_id, 1);
  text_set_color(text_id, 0xffffff);
  text_set_layout(text_id,
      LAYOUT_ALIGN_BOTTOM | LAYOUT_ALIGN_CENTER, // layout alignment for the box
      0, // horizontal margin from the right edge
      30); // vertical margin from the bottom edge
  text_set_align(text_id, TEXT_ALIGN_CENTER); // text alignment inside the box
}

/**
 * Destroys the resources used by timestamp library.
 */
void subtitle_shutdown() {
  if (text_id != -1) {
    text_destroy(text_id);
    text_id = -1;
  }
}

/**
 * Sets text color.
 */
void subtitle_set_color(int color) {
  text_set_color(text_id, color);
}

/**
 * Sets text visibility
 */
void subtitle_set_visibility(int in_preview, int in_video) {
  text_set_visibility(text_id, in_preview, in_video);
}

/**
 * Sets text stroke color.
 */
void subtitle_set_stroke_color(uint32_t color) {
  text_set_stroke_color(text_id, color);
}

/**
 * Sets text stroke border width in points.
 */
void subtitle_set_stroke_width(float stroke_width) {
  text_set_stroke_width(text_id, stroke_width);
}

/**
 * Sets letter spacing in pixels.
 */
void subtitle_set_letter_spacing(int letter_spacing) {
  text_set_letter_spacing(text_id, letter_spacing);
}

/**
 * Sets multiplying factor for line spacing.
 * If this is set to 1, default line spacing is used.
 */
void subtitle_set_line_height_multiply(float multiply) {
  text_set_line_height_multiply(text_id, multiply);
}

/**
 * Sets the scale of a tab (\t) character.
 * Tab width will be multiplied by the given number.
 */
void subtitle_set_tab_scale(float multiply) {
  text_set_tab_scale(text_id, multiply);
}

/**
 * Sets the absolute position for the timestamp.
 */
void subtitle_set_position(int x, int y) {
  text_set_position(text_id, x, y);
}

/**
 * Sets the relative layout for the text in the screen.
 */
void subtitle_set_layout(LAYOUT_ALIGN layout_align, int horizontal_margin, int vertical_margin) {
  text_set_layout(text_id, layout_align, horizontal_margin, vertical_margin);
}

/**
 * Sets the text alignment inside a positioned box.
 */
void subtitle_set_align(TEXT_ALIGN text_align) {
  text_set_align(text_id, text_align);
}

/**
 * Call this function before calling text_draw_all().
 */
void subtitle_update() {
  struct timespec ts;

  if (hide_time > 0) {
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t current_time = ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
    if (current_time > hide_time) {
      text_clear(text_id);
      hide_time = 0;
    }
  }
}

/**
 * Show subtitle text for duration_sec.
 * When duration_sec is 0, the text will be displayed indefinitely.
 */
void subtitle_show(const char *text, size_t text_len, float duration_sec) {
  struct timespec ts;

  text_set_text(text_id, text, text_len);
  redraw_text(text_id);

  if (duration_sec > 0.0f) {
    // hide the text after duration_sec
    clock_gettime(CLOCK_MONOTONIC, &ts);
    hide_time = ts.tv_sec * INT64_C(1000000000) +
      ts.tv_nsec + duration_sec * INT64_C(1000000000);
  } else {
    // show the text indefinitely
    hide_time = 0;
  }
}

/**
 * Hide the subtitle.
 */
void subtitle_clear() {
  if (text_id != -1) {
    text_clear(text_id);
  }
}
