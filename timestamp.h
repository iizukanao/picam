#ifndef PICAM_TIMESTAMP_H
#define PICAM_TIMESTAMP_H

#include <stdint.h>

/**
 * Initializes the timestamp library with a font name.
 */
void timestamp_init_with_font_name(const char *font_name, int points, int dpi);

/**
 * Initializes the timestamp library with a font file and face index.
 */
void timestamp_init(const char *font_file, long face_index, int points, int dpi);

/**
 * Destroys the resources used by timestamp library.
 */
void timestamp_shutdown();

/**
 * Sets timestamp text format.
 */
void timestamp_set_format(const char *format);

/**
 * Sets text color.
 */
void timestamp_set_color(int color);

/**
 * Sets text stroke color.
 */
void timestamp_set_stroke_color(uint32_t color);

/**
 * Sets text stroke border width in points.
 */
void timestamp_set_stroke_width(float stroke_width);

/**
 * Sets letter spacing in pixels.
 */
void timestamp_set_letter_spacing(int pixels);

/**
 * Sets multiplying factor for line spacing.
 * If this is set to 1, default line spacing is used.
 */
void timestamp_set_line_height_multiply(float line_height_multiply);

/**
 * Sets the absolute position for the timestamp.
 */
void timestamp_set_position(int x, int y);

/**
 * Sets the relative layout for the text in the screen.
 */
void timestamp_set_layout(LAYOUT_ALIGN layout_align, int horizontal_margin, int vertical_margin);

/**
 * Sets the text alignment inside a positioned box.
 */
void timestamp_set_align(TEXT_ALIGN text_align);

/**
 * Assigns an absolute position for the timestamp based on
 * the relative layout constraints that is currently set.
 */
void timestamp_fix_position(int canvas_width, int canvas_height);

/**
 * Call this function before calling text_draw_all().
 */
void timestamp_update();

#endif // PICAM_TIMESTAMP_H
