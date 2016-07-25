#ifndef PICAM_TEXT_H
#define PICAM_TEXT_H

#include <stdint.h>

// layout alignment in screen
typedef enum LAYOUT_ALIGN {
  // horizontal align bitmask group
  LAYOUT_ALIGN_LEFT = 1,
  LAYOUT_ALIGN_CENTER = 2,
  LAYOUT_ALIGN_RIGHT = 3,
  LAYOUT_ALIGN_HORIZONTAL_MASK = 3,

  // vertical align bitmask group
  LAYOUT_ALIGN_TOP = 4,
  LAYOUT_ALIGN_MIDDLE = 8,
  LAYOUT_ALIGN_BOTTOM = 12,
  LAYOUT_ALIGN_VERTICAL_MASK = 12,
} LAYOUT_ALIGN;

// text alignment inside bounding box
typedef enum TEXT_ALIGN {
  TEXT_ALIGN_LEFT = 1,
  TEXT_ALIGN_CENTER = 2,
  TEXT_ALIGN_RIGHT = 3,
} TEXT_ALIGN;

// Represents a bounding box for the text
typedef struct text_bounds {
  int left;
  int right;
  int top;
  int bottom;
  int width;
  int height;
} text_bounds;

/**
 * Initializes text library.
 */
void text_init();

/**
 * Destroys resources used by text library.
 */
void text_teardown();

/**
 * Creates a new text object and returns the text id.
 */
int text_create(const char *font_file, long face_index, float point, int dpi);

/**
 * Sets letter spacing.
 */
int text_set_letter_spacing(int text_id, int pixels);

/**
 * Sets stroke color.
 */
int text_set_stroke_color(int text_id, uint32_t color);

/**
 * Sets stroke border width in points.
 */
int text_set_stroke_width(int text_id, float stroke_width);

/**
 * Sets text fill color.
 */
int text_set_color(int text_id, int color);

/**
 * Sets text visibility
 */
int text_set_visibility(int text_id, int in_preview, int in_video);

/**
 * Sets multiplying factor for line spacing.
 * If this is set to 1, default line spacing is used.
 */
int text_set_line_height_multiply(int text_id, float multiply);

/**
 * Sets the scale of a tab (\t) character.
 * Tab width will be multiplied by the given number.
 */
int text_set_tab_scale(int text_id, float multiply);

/**
 * Returns the default line spacing in pixels.
 */
float text_get_line_height(int text_id);

/**
 * Returns the default ascender (distance from baseline to top)
 * in pixels.
 */
float text_get_ascender(int text_id);

/**
 * Sets the absolute position for the text.
 */
int text_set_position(int text_id, int x, int y);

/**
 * Sets the relative layout for the text in the screen.
 */
int text_set_layout(int text_id, LAYOUT_ALIGN layout_align, int horizontal_margin, int vertical_margin);

/**
 * Sets the absolute position for the text based on the current
 * relative layout and canvas size.
 */
int text_fix_position(int text_id, int canvas_width, int canvas_height);

/**
 * Sets the text alignment inside a positioned box.
 */
int text_set_align(int text_id, TEXT_ALIGN text_align);

/**
 * Sets the text in UTF-8 encoded chars.
 */
int text_set_text(int text_id, const char *utf8_text, const size_t text_bytes);

/**
 * Destroys the text object.
 */
int text_destroy(int text_id);

/**
 * Calculates a bounding box for the text object.
 */
int text_get_bounds(int text_id, const char *text, size_t text_len, text_bounds *bounds);

/**
 * Draw glyphs to internal buffer. Once this is called, the bitmap
 * will appear when text_draw_all() is called.
 */
int redraw_text(int text_id);

/**
 * Draw all text objects to the canvas.
 * we support writing on two types of canvas: ARGB8888 (is_video = 0) and YUV420PackedPlanar (is_video = 1)
 *
 * returns: nonzero if the canvas content has been changed
 */
int text_draw_all(uint8_t *canvas, int canvas_width, int canvas_height, int is_video);

/**
 * Clear the text. Once this is called, the bitmap will not be drawn
 * until text_set_text() is called.
 */
int text_clear(int text_id);

/**
 * Calculates the top-left corner position for the text object on the canvas.
 */
int text_get_position(int text_id, int canvas_width, int canvas_height, int *x, int *y);

/**
 * Select an appropriate font file and face index by a font name.
 */
int text_select_font_file(const char *name, char **font_file, int *face_index);

#endif // PICAM_TEXT_H
