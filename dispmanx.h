#ifndef PICAM_DISPMANX_H
#define PICAM_DISPMANX_H

#include <stdint.h>

// dispmanx layers consts for displaying multiple accelerated overlays in preview
#define DISP_LAYER_BACKGROUD     0xe
#define DISP_LAYER_VIDEO_PREVIEW 0xf
#define DISP_LAYER_TEXT          0x1f

// default ARGB color for preview background (black)
#define BLANK_BACKGROUND_DEFAULT    0xff000000

// display to which we will output the preview overlays
#define DISP_DISPLAY_DEFAULT     0

void dispmanx_init(uint32_t bg_color, uint32_t video_width, uint32_t video_height);
void dispmanx_destroy(void);
void dispmanx_update_text_overlay(void);

#endif
