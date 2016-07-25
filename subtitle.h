#ifndef PICAM_SUBTITLE_H
#define PICAM_SUBTITLE_H

/**
 * Initializes the timestamp library with a font name.
 */
void subtitle_init_with_font_name(const char *font_name, int points, int dpi);

/**
 * Initializes the timestamp library with a font file and face index.
 */
void subtitle_init(const char *font_file, long face_index, int points, int dpi);

/**
 * Destroys the resources used by timestamp library.
 */
void subtitle_shutdown();

/**
 * Sets text color.
 */
void subtitle_set_color(int color);

/**
 * Sets text visibility
 */
void subtitle_set_visibility(int in_preview, int in_video);

/**
 * Sets text stroke color.
 */
void subtitle_set_stroke_color(uint32_t color);

/**
 * Sets text stroke border width in points.
 */
void subtitle_set_stroke_width(float stroke_width);

/**
 * Sets letter spacing in pixels.
 */
void subtitle_set_letter_spacing(int letter_spacing);

/**
 * Sets multiplying factor for line spacing.
 * If this is set to 1, default line spacing is used.
 */
void subtitle_set_line_height_multiply(float line_height_multiply);

/**
 * Sets the scale of a tab (\t) character.
 * Tab width will be multiplied by the given number.
 */
void subtitle_set_tab_scale(float tab_scale);

/**
 * Sets the absolute position for the timestamp.
 */
void subtitle_set_position(int x, int y);

/**
 * Sets the relative layout for the text in the screen.
 */
void subtitle_set_layout(LAYOUT_ALIGN layout_align, int horizontal_margin, int vertical_margin);

/**
 * Sets the text alignment inside a positioned box.
 */
void subtitle_set_align(TEXT_ALIGN text_align);

/**
 * Call this function before calling text_draw_all().
 */
void subtitle_update();

/**
 * Show subtitle text for duration_sec.
 */
void subtitle_show(const char *text, size_t text_len, float duration_sec);

/**
 * Hide the subtitle.
 */
void subtitle_clear();

#endif // PICAM_SUBTITLE_H
