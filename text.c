#include <stdint.h>
#include <math.h>
#include <time.h> // clock()
#include "log.h"

// FreeType
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_STROKER_H

// HarfBuzz
#include <hb.h>
#include <hb-ft.h>

// Fontconfig
#include <fontconfig.h>

#include "text.h"

static const int BYTES_PER_PIXEL = 4;
static const int DEFAULT_TAB_WIDTH = 80;

#ifndef unlikely
#ifdef __GNUC__
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define unlikely(x) (x)
#endif
#endif

#ifndef likely
#ifdef __GNUC__
#define likely(x) __builtin_expect(!!(x), 1)
#else
#define likely(x) (x)
#endif
#endif

// Layout modes for the box
typedef enum LAYOUT_MODE {
  LAYOUT_MODE_ABSOLUTE = 1,
  LAYOUT_MODE_ALIGN = 2,
} LAYOUT_MODE;

typedef enum BlendMode {
  BLEND_MODE_NORMAL = 0,
  BLEND_MODE_MULTIPLY = 1,
  BLEND_MODE_OVERLAY = 2,
  BLEND_MODE_LIGHTEN_ONLY = 3,
} BlendMode;

//NOTE: enable if you need to blend multiple text overlays in preview and can trade off lower FPS because of that
// If You really need it, use multiple dispmanx layers (or EGL overlay) and let the hardware do the blending
//#define USE_ARGB_PIXEL_BLENDING

// Represents a text object
typedef struct TextData {
  int id;
  uint8_t *bitmap; // ARGB linear array (4 bytes per pixel)
  struct TextData *next_textdata;

  LAYOUT_MODE layout_mode;

  // variables for absolute layout
  int x;
  int y;

  // variables for relative layout
  TEXT_ALIGN text_align;
  LAYOUT_ALIGN layout_align;
  int horizontal_margin;
  int vertical_margin;

  int is_bitmap_ready;
  int will_dispose_bitmap;
  int has_changed; // do we need to redraw the overlay
  int width;
  int height;
  int is_stroke;
  uint32_t color;
  uint32_t stroke_color;
  int letter_spacing;
  float stroke_width;
  BlendMode blend_mode;
  char *text;
  int32_t text_len;
  FT_Face face;
  float line_height_multiply;
  float tab_scale;

  // baton
  int pen_x;
  int pen_y;
  int bounds_left;
  int bounds_right;
  int bounds_top;
  int bounds_bottom;

  // text visibility
  int in_preview;
  int in_video;
} TextData;

static TextData **textdata_list = NULL;
static FT_Library ft_library;
static int max_text_id = 0;

static FT_Int32 ft_load_flags = FT_LOAD_FORCE_AUTOHINT;
// FT_LOAD_DEFAULT -> crisp but bad shape
// FT_LOAD_NO_HINTING -> blurrier but better shape
// FT_LOAD_FORCE_AUTOHINT -> blurrier but readable, better shape than default

typedef struct span_sizer_data {
  int min_span_x;
  int max_span_x;
  int min_y;
  int max_y;
} span_sizer_data;

// function prototypes
static void text_destroy_real(int text_id);

/**
 * Initialize text library.
 */
void text_init() {
  FT_Error      fterr;

  fterr = FT_Init_FreeType( &ft_library );
  if (fterr) {
    fprintf(stderr, "error: freetype initialization failed: %d\n", fterr);
    return;
  }
}

/**
 * Shutdown text library.
 */
void text_teardown() {
  int i;
  for (i = 0; i < max_text_id; i++) {
    if (textdata_list[i] != NULL)  {
      text_destroy(i+1);
    }
  }

  FT_Done_FreeType(ft_library);
}

/**
 * Creates a new text object and returns the text id.
 */
int text_create(const char *font_file, long face_index, float point, int dpi) {
  FT_Error fterr;
  FT_Face face;

  if (ft_library == NULL) {
    text_init();
  }

  fterr = FT_New_Face( ft_library, font_file, face_index, &face );
  if (fterr == FT_Err_Unknown_File_Format) {
    fprintf(stderr, "text_create() failed: font format is unsupported\n");
    return -1;
  } else if (fterr == FT_Err_Cannot_Open_Resource) {
    fprintf(stderr, "text_create() failed: cannot open the font file\n");
    return -1;
  } else if (fterr == FT_Err_Invalid_Argument) {
    fprintf(stderr, "text_create() failed: maybe the font face index is invalid\n");
    return -1;
  } else if (fterr) {
    fprintf(stderr, "text_create() failed: failed to open the font file; error=%d\n", fterr);
    return -1;
  } else if (face == NULL) {
    fprintf(stderr, "text_create() failed: failed to open the font file\n");
    return -1;
  }

  fterr = FT_Set_Char_Size(
    face,       // FT_Face
    point * 64, // char_width in 1/64th of points
    0,          // char_height in 1/64th of points (0 == same as width)
    dpi,        // horizontal device resolution (dpi)
    0);         // vertical device resolution (0 == same as horizontal resolution)
  if (fterr) {
    fprintf(stderr, "error: failed to set font size\n");
    return -1;
  }

  // find available text id
  int text_id = 0;
  int i;
  for (i = 0; i < max_text_id; i++) {
    if (textdata_list[i] == NULL) {
      text_id = i + 1;
      break;
    }
  }

  if (text_id == 0) { // no available slots
    // expand textdata_list
    max_text_id++;
    textdata_list = realloc(textdata_list, sizeof(TextData) * max_text_id);
    if (textdata_list == NULL) {
      fprintf(stderr, "cannot allocate memory for textdata_list: %d bytes\n",
          sizeof(TextData) * max_text_id);
      exit(EXIT_FAILURE);
    }
    text_id = max_text_id;
  }

  TextData *textdata = malloc(sizeof(TextData));
  if (textdata == NULL) {
    fprintf(stderr, "cannot allocate memory for textdata: %d bytes\n",
        sizeof(TextData));
    exit(EXIT_FAILURE);
  }
  // initialize values
  textdata->id = text_id;
  textdata->bitmap = NULL;
  textdata->is_bitmap_ready = 0;
  textdata->has_changed = 0;
  textdata->will_dispose_bitmap = 0;
  textdata->width = 0;
  textdata->height = 0;
  textdata->color = 0xffffff;
  textdata->stroke_color = 0x000000;
  textdata->stroke_width = 1.0f;
  textdata->letter_spacing = 0;
  textdata->blend_mode = BLEND_MODE_NORMAL;
  textdata->text = NULL;
  textdata->face = face;
  textdata->line_height_multiply = 1.0f;
  textdata->tab_scale = 1.0f;
  textdata->layout_mode = LAYOUT_MODE_ABSOLUTE;
  textdata->x = 0;
  textdata->y = 0;
  textdata->pen_x = 0;
  textdata->pen_y = 0;
  textdata->in_preview = 1;
  textdata->in_video = 1;
  textdata->next_textdata = NULL;
  textdata_list[text_id-1] = textdata;

  return text_id;
}

/**
 * Returns the width of a tab for the FT_Face.
 */
int text_get_tab_width(TextData *textdata) {
  FT_Error error;

  // Get glyph_index of character 'm'
  FT_UInt glyph_index = FT_Get_Char_Index(textdata->face, (FT_ULong) 'm');
  if (glyph_index == 0) { // undefined character code
    fprintf(stderr, "warn: character 'm' not found in the font file; "
        "using default tab_width (%dpx)\n", DEFAULT_TAB_WIDTH);
    return DEFAULT_TAB_WIDTH;
  }

  // Load the glyph into slot (textdata->face->glyph)
  error = FT_Load_Glyph(textdata->face, glyph_index, FT_LOAD_DEFAULT);
  if (error) {
    fprintf(stderr, "load glyph error: %d\n", error);
    fprintf(stderr, "warn: using default tab_width (%dpx)\n", DEFAULT_TAB_WIDTH);
    return DEFAULT_TAB_WIDTH;
  }
  float m_width = textdata->face->glyph->linearHoriAdvance / 65536.0f;
  m_width *= textdata->tab_scale;
  int tab_width = roundf(m_width * 4.5f);
  if (tab_width < 0) {
    tab_width = 0;
  }
  return tab_width;
}

/**
 * Sets letter spacing.
 */
int text_set_letter_spacing(int text_id, int pixels) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  textdata->letter_spacing = pixels;
  return 0;
}

/**
 * Sets stroke color.
 */
int text_set_stroke_color(int text_id, uint32_t color) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  textdata->stroke_color = color;
  return 0;
}

/**
 * Sets stroke border width in points.
 */
int text_set_stroke_width(int text_id, float stroke_width) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  textdata->stroke_width = stroke_width;
  return 0;
}

/**
 * Sets text fill color.
 */
int text_set_color(int text_id, int color) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  textdata->color = color;
  return 0;
}

/**
 * Sets text visibility
 */
int text_set_visibility(int text_id, int in_preview, int in_video) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  textdata->in_preview = in_preview;
  textdata->in_video = in_video;
  return 0;
}

/**
 * Sets multiplying factor for line spacing.
 * If this is set to 1, default line spacing is used.
 */
int text_set_line_height_multiply(int text_id, float multiply) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  textdata->line_height_multiply = multiply;
  return 0;
}

/**
 * Sets the scale of a tab (\t) character.
 * Tab width will be multiplied by the given number.
 */
int text_set_tab_scale(int text_id, float multiply) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  textdata->tab_scale = multiply;
  return 0;
}

/**
 * Returns the default line spacing in pixels.
 */
float text_get_line_height(int text_id) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];

  return ((textdata->face->size->metrics.height >> 6) +
      ((textdata->face->size->metrics.height & 0x3f) / 64.0f));
}

/**
 * Returns the default ascender (distance from baseline to top)
 * in pixels.
 */
float text_get_ascender(int text_id) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];

  return (textdata->face->size->metrics.ascender >> 6) +
    ((textdata->face->size->metrics.ascender & 0x3f) / 64.0f);
}

/**
 * Sets the absolute position for the text.
 */
int text_set_position(int text_id, int x, int y) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  textdata->layout_mode = LAYOUT_MODE_ABSOLUTE;
  textdata->x = x;
  textdata->y = y;
  return 0;
}

/**
 * Sets the relative layout for the text in the screen.
 */
int text_set_layout(int text_id, LAYOUT_ALIGN layout_align,
    int horizontal_margin, int vertical_margin) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  textdata->layout_mode = LAYOUT_MODE_ALIGN;
  textdata->layout_align = layout_align;
  textdata->horizontal_margin = horizontal_margin;
  textdata->vertical_margin = vertical_margin;
  return 0;
}

/**
 * Sets the absolute position for the text based on the current
 * relative layout and canvas size.
 */
int text_fix_position(int text_id, int canvas_width, int canvas_height) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  if (textdata->layout_mode == LAYOUT_MODE_ALIGN) {
    int pen_x, pen_y;
    text_get_position(textdata->id, canvas_width, canvas_height, &pen_x, &pen_y);
    textdata->layout_mode = LAYOUT_MODE_ABSOLUTE;
    textdata->x = pen_x;
    textdata->y = pen_y;
  }
  return 0;
}

/**
 * Sets the text alignment inside a positioned box.
 */
int text_set_align(int text_id, TEXT_ALIGN text_align) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  textdata->text_align = text_align;
  return 0;
}

/**
 * Sets the text in UTF-8 encoded chars.
 */
int text_set_text(int text_id, const char *utf8_text, const size_t text_bytes) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  if (textdata->text != NULL) {
    free(textdata->text);
  }
  textdata->text = malloc(text_bytes);
  if (textdata->text == NULL) {
    fprintf(stderr,
        "cannot allocate memory for textdata->text: %d bytes\n",
        text_bytes);
    exit(EXIT_FAILURE);
  }
  textdata->text_len = text_bytes;
  memcpy(textdata->text, utf8_text, text_bytes);
  return 0;
}

/**
 * Actually destroyes the resources used by the text object.
 */
static void text_destroy_real(int text_id) {
  if (text_id <= 0 || text_id > max_text_id) {
    return; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  if (textdata->bitmap != NULL) {
    free(textdata->bitmap);
  }
  if (textdata->text != NULL) {
    free(textdata->text);
  }
  if (textdata->face != NULL) {
    FT_Done_Face(textdata->face);
  }
  free(textdata);
  textdata_list[text_id-1] = NULL;
}

/**
 * Destroys the text object.
 *
 * Actual destruction will be performed at the next text_draw_all() call.
 */
int text_destroy(int text_id) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  textdata->has_changed = 1;
  textdata->will_dispose_bitmap = 1;
  return 0;
}

// callback function for measuring the bounding box for the text.
static void span_sizer_callback(int y, int count, const FT_Span* spans, void *user) {
  span_sizer_data *sizer_data = (span_sizer_data *) user;

  if (y < sizer_data->min_y) {
    sizer_data->min_y = y;
  }
  if (y > sizer_data->max_y) {
    sizer_data->max_y = y;
  }
  int i;
  for (i = 0 ; i < count; i++) {
    if (spans[i].x + spans[i].len > sizer_data->max_span_x) {
      sizer_data->max_span_x = spans[i].x + spans[i].len;
    }
    if (spans[i].x < sizer_data->min_span_x) {
      sizer_data->min_span_x = spans[i].x;
    }
  }
}

/**
 * Calculates a bounding box for the text object.
 */
int text_get_bounds(int text_id, const char *text, size_t text_len, text_bounds *bounds) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }

  if (text_len == 0) {
    bounds->left = 0;
    bounds->right = 0;
    bounds->top = 0;
    bounds->bottom = 0;
    bounds->width = 0;
    bounds->height = 0;
    return 0;
  }

  TextData *textdata = textdata_list[text_id-1];

  hb_font_t *hb_ft_font = hb_ft_font_create(textdata->face, NULL);
  hb_buffer_t *buf = hb_buffer_create();
  hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
  hb_buffer_set_script(buf, HB_SCRIPT_COMMON);
  hb_buffer_set_language(buf, hb_language_get_default());

  hb_buffer_add_utf8(buf, text, text_len, 0, text_len);
  hb_shape(hb_ft_font, buf, NULL, 0);

  unsigned int glyph_count;
  hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buf, &glyph_count);
  hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(buf, &glyph_count);

  FT_Error fterr;
  span_sizer_data sizer_data;

  FT_Raster_Params ftr_params;
  memset(&ftr_params, 0, sizeof(ftr_params));
  ftr_params.target = 0; // we use callback instead of this
  ftr_params.flags = FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_AA; // antialiasing with callback
  ftr_params.user = &sizer_data; // user data
  ftr_params.black_spans = 0; // unused
  ftr_params.bit_set = 0; // unused
  ftr_params.bit_test = 0; // unused
  ftr_params.gray_spans = span_sizer_callback; // callback func

  int max_x = INT_MIN;
  int min_x = INT_MAX;
  int max_y = INT_MIN;
  int min_y = INT_MAX;
  float total_advance_x = 0.0f;
  float total_advance_y = 0.0f;

  int tab_width = -1;
  int j;
  for (j = 0; j < glyph_count; j++) {
    char current_char = *(text + glyph_info[j].cluster);
    if (current_char == '\t') { // Tab character found
      if (tab_width == -1) {
        // Calculate tab width
        tab_width = text_get_tab_width(textdata);
      }
      if (tab_width > 0) {
        // Advance total_advance_x to the next tab position
        total_advance_x += tab_width - (((int)total_advance_x) % tab_width);
      }
      continue;
    }

    fterr = FT_Load_Glyph(textdata->face, glyph_info[j].codepoint, ft_load_flags);
    if (fterr) {
      fprintf(stderr, "failed to load %08x (freetype error code=%d)\n",  glyph_info[j].codepoint, fterr);
    } else {
      if (textdata->face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
        fprintf(stderr, "unsupported glyph format: %4s\n", (char *)&textdata->face->glyph->format);
      } else {
        float gx = total_advance_x + (glyph_pos[j].x_offset / 64.0f);
        float gy = total_advance_y + (glyph_pos[j].y_offset / 64.0f);

        sizer_data.min_span_x = INT_MAX;
        sizer_data.max_span_x = INT_MIN;
        sizer_data.min_y = INT_MAX;
        sizer_data.max_y = INT_MIN;

        FT_Stroker stroker;
        FT_Stroker_New(ft_library, &stroker);
        FT_Fixed radius = textdata->stroke_width * 64;
        FT_Stroker_Set(stroker, radius, FT_STROKER_LINECAP_ROUND,
            FT_STROKER_LINEJOIN_ROUND, 0);

        FT_Glyph glyph;
        FT_Get_Glyph(textdata->face->glyph, &glyph);
        FT_Glyph_StrokeBorder(&glyph, stroker, 0, 1);

        fterr = FT_Outline_Render(ft_library,
            &((FT_OutlineGlyph)glyph)->outline, &ftr_params);
        if (fterr) {
          fprintf(stderr, "FT_Outline_Render() failed; err=%d\n", fterr);
        }

        // Tidy up
        FT_Stroker_Done(stroker);
        FT_Done_Glyph(glyph);

        if (sizer_data.min_span_x != INT_MAX) {
          if (min_x > sizer_data.min_span_x + (int)gx) {
            min_x = sizer_data.min_span_x + (int)gx;
          }
          if (max_x < sizer_data.max_span_x + ceil(gx)) {
            max_x = sizer_data.max_span_x + ceil(gx);
          }
          if (min_y > sizer_data.min_y + (int)gy) {
            min_y = sizer_data.min_y + (int)gy;
          }
          if (max_y < sizer_data.max_y + ceil(gy)) {
            max_y = sizer_data.max_y + ceil(gy);
          }
        } else { // empty glyph
          if (min_x > gx) {
            min_x = (int)gx;
          }
          if (max_x < gx) {
            max_x = ceil(gx);
          }
          if (min_y > gy) {
            min_y = (int)gy;
          }
          if (max_y < gy) {
            max_y = ceil(gy);
          }
        }
      }
    }

    total_advance_x += glyph_pos[j].x_advance / 64.0f;
    total_advance_y += glyph_pos[j].y_advance / 64.0f;
    if (j + 1 < glyph_count) {
      total_advance_x += textdata->letter_spacing;
    }
  }

  if (min_x > total_advance_x) {
    min_x = (int)total_advance_x;
  }
  if (max_x < total_advance_x) {
    max_x = ceil(total_advance_x);
  }
  if (min_y > total_advance_y) {
    min_y = (int)total_advance_y;
  }
  if (max_y < total_advance_y) {
    max_y = ceil(total_advance_y);
  }

  int bbox_w = max_x - min_x;
  int bbox_h = max_y - min_y;

  int left = min_x;
  int right = bbox_w + min_x;
  int top = - max_y;
  int bottom = - max_y + bbox_h;

  hb_buffer_destroy(buf);
  hb_font_destroy(hb_ft_font);

  bounds->left = left;
  bounds->right = right;
  bounds->top = top;
  bounds->bottom = bottom;
  int width = right;
  if (left < 0) {
    width -= left;
  }
  bounds->width = width;
  bounds->height = bbox_h + 1;

  return 0;
}

// NOTE: PI is little-endian so the actual color order in memory is as below
typedef union {
  uint32_t x;
  struct {
    uint8_t b;
    uint8_t g;
    uint8_t r;
    uint8_t a;
  } c;
} color_argb_t;

static color_argb_t blend_colors_argb(color_argb_t bg_color, color_argb_t fg_color, BlendMode blend_mode)
{
  color_argb_t out;

  if (blend_mode != BLEND_MODE_NORMAL) {
    // TODO: Implement other blending modes
    fprintf(stderr, "blend_colors_argb: blending mode not implemented: %d\n", blend_mode);
    blend_mode = BLEND_MODE_NORMAL;
  }

  // shortcut
  if (bg_color.c.a == 0 || fg_color.c.a == 0xff) {
    return fg_color;
  }

  // alpha blending two ARGB from wikipedia:
  out.c.r = fg_color.c.r * fg_color.c.a / 255 + (bg_color.c.r * bg_color.c.a * (255 - fg_color.c.a) / (255 * 255));
  out.c.g = fg_color.c.g * fg_color.c.a / 255 + (bg_color.c.g * bg_color.c.a * (255 - fg_color.c.a) / (255 * 255));
  out.c.b = fg_color.c.b * fg_color.c.a / 255 + (bg_color.c.b * bg_color.c.a * (255 - fg_color.c.a) / (255 * 255));
  out.c.a = fg_color.c.a + (bg_color.c.a * (255 - fg_color.c.a) / 255);

  return out;
}

// callback function for drawing the glyphs of the text.
// NOTE: we're creating full 32-bit bitmap
void span_writer_callback(int y, int count, const FT_Span* spans, void *user) {
  TextData *textdata = (TextData *) user;
  int row = -y - textdata->bounds_top;
  uint8_t *scanline = textdata->bitmap + ((row + textdata->pen_y)
      * textdata->width + textdata->pen_x) * BYTES_PER_PIXEL;
  int remaining_pixels = textdata->width - textdata->pen_x;
  if (unlikely(remaining_pixels <= 0)) { // out of bounds
    return;
  }
  int i;
  int total_length = 0;
  for (i = 0; i < count; i++) {
    uint8_t opacity = spans[i].coverage;
    uint32_t* start = (uint32_t*)(scanline + spans[i].x * BYTES_PER_PIXEL);

    int x;
    for (x = 0; x < spans[i].len; x++) {
      if (unlikely(total_length + x >= remaining_pixels)) { // out of bounds
        return;
      }
      if (opacity == 0) {
        ++start;
      } else {
        color_argb_t fg_color, bg_color, new_color;
        bg_color.x = *start;
        fg_color.x = opacity << 24 | ((textdata->is_stroke) ? textdata->stroke_color : textdata->color);
        new_color = blend_colors_argb(bg_color, fg_color, BLEND_MODE_NORMAL);

        *start = new_color.x;
        //printf("0x%08x + 0x%08x = 0x%08x\n", prev_color.x, curr_color.x, new_color.x);

        ++start;
      }
    }
    total_length = spans[i].x + spans[i].len;
  }
}

/**
 * Draws glyphs to internal bitmap.
 */
static int draw_glyphs(TextData *textdata) {
  // count \n in the string
  int *line_start_pos = malloc(sizeof(int));
  if (line_start_pos == NULL) {
    fprintf(stderr, "cannot allocate memory for line_start_pos: %d bytes\n",
        sizeof(int));
    exit(EXIT_FAILURE);
  }
  line_start_pos[0] = 0; // stores the starting positions of each line
  int lines = 1;
  int i;
  int contains_tab = 0;
  for (i = 0; i < textdata->text_len; i++) {
    if (textdata->text[i] == '\n') {
      if (i < textdata->text_len) {
        int *more_line_start_pos = realloc(line_start_pos, sizeof(int) * (lines + 1));
        if (more_line_start_pos == NULL) {
          free(line_start_pos);
          fprintf(stderr, "cannot allocate memory for line_start_pos: %d bytes\n",
              sizeof(int) * (lines + 1));
          exit(EXIT_FAILURE);
        }
        line_start_pos = more_line_start_pos;
        line_start_pos[lines] = i + 1;
      }
      lines++;
    } else if (textdata->text[i] == '\t') {
      if (!contains_tab) {
        contains_tab = 1;
      }
    }
  }

  int tab_width;
  if (contains_tab) {
    // Calculate tab width
    tab_width = text_get_tab_width(textdata);
  } else {
    tab_width = 0; // We will not use this variable
  }

  // calculate the maximum width among all lines
  int max_width = 0;
  int max_height = 0;
  text_bounds *text_bounds_list = NULL;
  text_bounds_list = calloc(1, sizeof(text_bounds) * lines);
  if (text_bounds_list == NULL) {
    fprintf(stderr, "cannot allocate memory for text_bounds_list: %d bytes\n",
        sizeof(text_bounds) * lines);
    exit(EXIT_FAILURE);
  }
  int min_left = 0;
  for (i = 0; i < lines; i++) {
    size_t len;
    if (i + 1 == lines) {
      len = textdata->text_len - line_start_pos[i];
    } else {
      len = line_start_pos[i+1] - line_start_pos[i] - 1;
    }
    if (len > 0) {
      text_get_bounds(textdata->id,
          textdata->text + line_start_pos[i], len, &text_bounds_list[i]);
      if (text_bounds_list[i].width > max_width) {
        max_width = text_bounds_list[i].width;
      }
      if (text_bounds_list[i].height > max_height) {
        max_height = text_bounds_list[i].height;
      }
      if (text_bounds_list[i].left < min_left) {
        min_left = text_bounds_list[i].left;
      }
    } else {
      text_bounds_list[i].width = 0;
    }
  }

  // adjust line height when the stroke width is large
  float line_height = text_get_line_height(textdata->id);
  if (max_height > line_height) {
    line_height = max_height;
  }
  float box_height = line_height +
    (lines - 1) * line_height * textdata->line_height_multiply;

  max_width += 0 - min_left; // XXX: Is this compensation OK?
  if (max_width < 0) { // integer overflow
    max_width = 0;
  }

  TextData *tmp_textdata = malloc(sizeof(TextData));
  if (tmp_textdata == NULL) {
    fprintf(stderr, "cannot allocate memory for textdata: %d bytes\n",
        sizeof(TextData));
    exit(EXIT_FAILURE);
  }
  memcpy(tmp_textdata, textdata, sizeof(TextData));

  tmp_textdata->width = max_width;
  tmp_textdata->height = ceil(box_height);
  tmp_textdata->bitmap = calloc(1, tmp_textdata->width * tmp_textdata->height * BYTES_PER_PIXEL);
  if (tmp_textdata->bitmap == NULL) {
    fprintf(stderr, "cannot allocate memory for text bitmap: %d bytes\n",
        tmp_textdata->width * tmp_textdata->height * BYTES_PER_PIXEL);
    exit(EXIT_FAILURE);
  }

  hb_font_t *hb_ft_font = hb_ft_font_create(tmp_textdata->face, NULL);
  hb_buffer_t *buf = hb_buffer_create();

  // draw text line by line
  float start_x = 0;
  float start_y = 0;
  for (i = 0; i < lines; i++) {
    hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
    hb_buffer_set_script(buf, HB_SCRIPT_COMMON);
    hb_buffer_set_language(buf, hb_language_get_default());

    tmp_textdata->bounds_left = text_bounds_list[i].left;
    tmp_textdata->bounds_right = text_bounds_list[i].right;
    tmp_textdata->bounds_top = text_bounds_list[i].top;
    tmp_textdata->bounds_bottom = text_bounds_list[i].bottom;

    float x;
    float y = start_y + line_height * tmp_textdata->line_height_multiply * i;
    float center = 0.0f;
    if (tmp_textdata->text_align == TEXT_ALIGN_CENTER) {
      if (center == 0.0f) {
        center = start_x + max_width / 2.0f;
      }
      x = center - text_bounds_list[i].width / 2.0f;
    } else if (tmp_textdata->text_align == TEXT_ALIGN_RIGHT) {
      x = start_x + max_width - text_bounds_list[i].width;
    } else { // TEXT_ALIGN_LEFT (default)
      x = start_x;
    }
    int len;
    if (i + 1 == lines) {
      len = tmp_textdata->text_len - line_start_pos[i];
    } else {
      len = line_start_pos[i+1] - line_start_pos[i] - 1;
    }

    FT_Error fterr;
    hb_buffer_add_utf8(buf, tmp_textdata->text + line_start_pos[i], len, 0, len);
    hb_shape(hb_ft_font, buf, NULL, 0);

    unsigned int glyph_count;
    hb_glyph_info_t *glyph_info = hb_buffer_get_glyph_infos(buf, &glyph_count);
    hb_glyph_position_t *glyph_pos = hb_buffer_get_glyph_positions(buf, &glyph_count);

    FT_Raster_Params ftr_params;
    memset(&ftr_params, 0, sizeof(ftr_params));
    ftr_params.target = 0; // we use callback instead of this
    ftr_params.flags = FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_AA; // antialiasing with callback
    ftr_params.user = tmp_textdata; // user data
    ftr_params.black_spans = 0; // unused
    ftr_params.bit_set = 0; // unused
    ftr_params.bit_test = 0; // unused
    ftr_params.gray_spans = span_writer_callback; // callback func

    x += 0 - min_left; // XXX: OK?

    int j;
    for (j = 0; j < glyph_count; j++) {
      // glyph_info.cluster indicates the index of the character in the input text
      char current_char = *(tmp_textdata->text +
          line_start_pos[i] + glyph_info[j].cluster);
      if (current_char == '\t') { // Tab character found
        if (tab_width > 0) {
          // Advance x to the next tab position
          x += tab_width - (((int)x) % tab_width);
        }
        continue;
      }
      fterr = FT_Load_Glyph(tmp_textdata->face, glyph_info[j].codepoint, ft_load_flags);
      if (fterr) {
        fprintf(stderr, "failed to load %08x (freetype error code=%d)\n",  glyph_info[j].codepoint, fterr);
      } else {
        if (tmp_textdata->face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
          fprintf(stderr, "unsupported glyph format: %4s\n", (char *)&tmp_textdata->face->glyph->format);
        } else {
          int gx = x + (glyph_pos[j].x_offset/64);
          int gy = y - (glyph_pos[j].y_offset/64);
          tmp_textdata->pen_x = gx;
          tmp_textdata->pen_y = gy;

          if (tmp_textdata->stroke_width > 0.0f) {
            FT_Stroker stroker;
            FT_Stroker_New(ft_library, &stroker);
            FT_Fixed radius = tmp_textdata->stroke_width * 64;
            FT_Stroker_Set(stroker, radius, FT_STROKER_LINECAP_ROUND,
                FT_STROKER_LINEJOIN_ROUND, 0);

            FT_Glyph glyph;
            FT_Get_Glyph(tmp_textdata->face->glyph, &glyph);
            FT_Glyph_StrokeBorder(&glyph, stroker, 0, 1);

            tmp_textdata->is_stroke = 1;
            fterr = FT_Outline_Render(ft_library, &((FT_OutlineGlyph)glyph)->outline, &ftr_params);
            if (fterr) {
              fprintf(stderr, "FT_Outline_Render() failed; err=%d\n", fterr);
            }

            // Tidy up
            FT_Stroker_Done(stroker);
            FT_Done_Glyph(glyph);
          }

          tmp_textdata->is_stroke = 0;
          fterr = FT_Outline_Render(ft_library, &tmp_textdata->face->glyph->outline, &ftr_params);
          if (fterr) {
            fprintf(stderr, "FT_Outline_Render() failed; err=%d\n", fterr);
          }
        }
      }

      x += glyph_pos[j].x_advance/64 + tmp_textdata->letter_spacing;
      y -= glyph_pos[j].y_advance/64;
    }

    hb_buffer_clear_contents(buf);
  }

  if (textdata->is_bitmap_ready) {
    // queue next textdata
    tmp_textdata->is_bitmap_ready = 1;
    tmp_textdata->has_changed = 1;
    textdata->next_textdata = tmp_textdata;
  } else {
    // use existing textdata
    textdata->bitmap = tmp_textdata->bitmap;
    textdata->bounds_top = tmp_textdata->bounds_top;
    textdata->bounds_left = tmp_textdata->bounds_left;
    textdata->bounds_right = tmp_textdata->bounds_right;
    textdata->bounds_bottom = tmp_textdata->bounds_bottom;
    textdata->width = tmp_textdata->width;
    textdata->height = tmp_textdata->height;
    textdata->is_bitmap_ready = 1;
    textdata->has_changed = 1;
    free(tmp_textdata);
  }

  hb_buffer_destroy(buf);
  hb_font_destroy(hb_ft_font);

  free(line_start_pos);
  if (text_bounds_list != NULL) {
    free(text_bounds_list);
  }

  return 0;
}

/**
 * Draw glyphs to internal buffer. Once this is called, the bitmap
 * will appear when text_draw_all() is called.
 */
int redraw_text(int text_id) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  draw_glyphs(textdata);
  return 0;
}

/**
 * Clear the text. Once this is called, the bitmap will not be drawn
 * until text_set_text() is called.
 */
int text_clear(int text_id) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  textdata->is_bitmap_ready = 0;
  textdata->has_changed = 1;
  return 0;
}

/**
 * Calculates the top-left corner position for the text object on the canvas.
 */
int text_get_position(int text_id, int canvas_width, int canvas_height, int *x, int *y) {
  if (text_id <= 0 || text_id > max_text_id) {
    return -1; // non-existent text id
  }
  TextData *textdata = textdata_list[text_id-1];
  if (textdata->layout_mode == LAYOUT_MODE_ALIGN) {
    float start_x, start_y;
    int horizontal_align = textdata->layout_align & LAYOUT_ALIGN_HORIZONTAL_MASK;
    if (horizontal_align == LAYOUT_ALIGN_RIGHT) {
      start_x = canvas_width - textdata->horizontal_margin - textdata->width;
    } else if (horizontal_align == LAYOUT_ALIGN_LEFT) {
      start_x = textdata->horizontal_margin;
    } else { // // LAYOUT_ALIGN_CENTER (default)
      start_x = canvas_width / 2.0f - textdata->width / 2.0f;
    }
    int vertical_align = textdata->layout_align & LAYOUT_ALIGN_VERTICAL_MASK;
    if (vertical_align == LAYOUT_ALIGN_TOP) {
      start_y = textdata->vertical_margin;
    } else if (vertical_align == LAYOUT_ALIGN_MIDDLE) {
      start_y = canvas_height / 2.0f - textdata->height / 2.0f;
    } else { // LAYOUT_ALIGN_BOTTOM (default)
      start_y = canvas_height - textdata->vertical_margin - textdata->height;
    }
    *x = roundf(start_x);
    *y = roundf(start_y);
  } else {
    *x = textdata->x;
    *y = textdata->y;
  }
  return 0;
}

/**
 * Draw all text objects to the canvas.
 *
 * returns: nonzero if the canvas content has been changed
 */
int text_draw_all(uint8_t *canvas, int canvas_width, int canvas_height, int is_video) {
  int i;
  int has_anything_changed = 0;
  int canvas_bytes_per_pixel = (is_video) ? 1 : BYTES_PER_PIXEL; // note: in YUV we're poinig to Y planar pixel
  //clock_t start_time = clock();

  for (i = 0; i < max_text_id; i++) {
    TextData *textdata = textdata_list[i];
    if (textdata != NULL) {
      if (textdata->next_textdata != NULL) {
        // Copy the contents of next_textdata to textdata
        free(textdata->bitmap);
        TextData *next_textdata = textdata->next_textdata;
        memcpy(textdata, next_textdata, sizeof(TextData));
        free(next_textdata);
        has_anything_changed = 1; // we're replacing old textdata with new one
      }
      has_anything_changed |= textdata->has_changed;
      textdata->has_changed = 0;
      if (textdata->will_dispose_bitmap) {
        text_destroy_real(textdata->id);
        continue;
      }
      if (textdata->is_bitmap_ready) {
        if ((is_video && !textdata->in_video)
            || (!is_video && !textdata->in_preview)) {
          continue; // skip this textdata if we don't want to show it on this medium
        }
        int pen_x, pen_y;
        text_get_position(textdata->id, canvas_width, canvas_height, &pen_x, &pen_y);
        int row, col;
        for (row = 0; row < textdata->height; row++) {
          if (pen_y + row < 0) {
            continue;
          }
          if (pen_y + row >= canvas_height) { // out of bounds
            break;
          }
          for (col = 0; col < textdata->width; col++) {
            if (pen_x + col < 0) {
              continue;
            }
            if (pen_x + col >= canvas_width) { // out of bounds
              break;
            }
            int offset = (row * textdata->width + col) * BYTES_PER_PIXEL;
            color_argb_t color;
            color.x = *((uint32_t*) (textdata->bitmap + offset));
            uint8_t* canvas_pixel = canvas + ((pen_y + row) * canvas_width + (pen_x + col)) * canvas_bytes_per_pixel;

            if (is_video) { // YUV420PackedPlanar video frame
              uint8_t opacity = color.c.a;
              uint8_t y = ( (  66 * color.c.r + 129 * color.c.g +  25 * color.c.b + 128) >> 8) + 16;
#if 0
              uint8_t u = ( ( -38 * color.c.r -  74 * color.c.g + 112 * color.c.b + 128) >> 8) + 128;
              uint8_t v = ( ( 112 * color.c.r -  94 * color.c.g -  18 * color.c.b + 128) >> 8) + 128;
              (void)u; (void)v;
#endif
              if (opacity == 255) {
                if (textdata->blend_mode == BLEND_MODE_NORMAL) {
                  *canvas_pixel = y;
                  // TODO: update U and V
                } else {
                  // TODO: Implement other blending modes
                  fprintf(stderr, "blending mode not implemented: %d\n",
                      textdata->blend_mode);
                }
              } else if (opacity > 0) {
                // Blend colors
                uint8_t orig_color_y = *canvas_pixel;
                float intensity = opacity / 255.0f;
                if (textdata->blend_mode == BLEND_MODE_NORMAL) {
                  *canvas_pixel = orig_color_y * (1 - intensity) + y * intensity;
                } else {
                  // TODO: Implement other blending modes
                  fprintf(stderr, "blending mode not implemented: %d\n",
                      textdata->blend_mode);
                }
              }
            } else { // ARGB preview canvas
#ifdef USE_ARGB_PIXEL_BLENDING
              color_argb_t bg_color, new_color;
              bg_color.x = *((uint32_t*) canvas_pixel);
              new_color = blend_colors_argb(bg_color, color, textdata->blend_mode);
              *((uint32_t*) canvas_pixel) = new_color.x;
#else
              *((uint32_t*) canvas_pixel) = color.x;
#endif
            }
          }
        }
      }
    }
  }
  //log_debug("\n\ntext_draw_all(is_video=%d) took %d ms, has_changed=%d\n", is_video, (clock() - start_time) * 1000 / CLOCKS_PER_SEC, has_anything_changed);
  return has_anything_changed;
}

/**
 * Select an appropriate font file and face index by a font name.
 */
int text_select_font_file(const char *name, char **font_file, int *face_index) {
  FcInit();

  FcPattern *pattern = FcNameParse((const FcChar8 *)name);
  FcConfigSubstitute(0, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);

  FcResult result = FcResultMatch;
  FcPattern *match = FcFontMatch(NULL, pattern, &result);
  FcPatternDestroy(pattern);
  if (result != FcResultMatch) {
    return -1;
  }

  FcChar8 *path;
  if (FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch) {
    size_t path_len = strlen((const char *)path) + 1;
    *font_file = malloc(path_len);
    memcpy(*font_file, path, path_len);
  } else {
    return -1;
  }
  int index;
  if (FcPatternGetInteger(match, FC_INDEX, 0, &index) == FcResultMatch) {
    *face_index = index;
  } else {
    return -1;
  }

  FcPatternDestroy(match);

  FcFini();

  return 0;
}
