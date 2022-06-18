#ifndef _CLIB_MPEGTS_H_
#define _CLIB_MPEGTS_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

typedef struct MpegTSCodecSettings {
  int audio_sample_rate; // e.g. 22050
  int audio_bit_rate;    // e.g. 24000
  int audio_channels;    // e.g. 1
  int audio_profile;     // e.g. FF_PROFILE_AAC_LOW
} MpegTSCodecSettings;

typedef struct MpegTSContext {
  AVFormatContext *format_context;
  AVCodecContext *codec_context_video;
  AVCodecContext *codec_context_audio;
} MpegTSContext;

MpegTSContext mpegts_create_context(MpegTSCodecSettings *settings);
MpegTSContext mpegts_create_context_video_only(MpegTSCodecSettings *settings);
MpegTSContext mpegts_create_context_audio_only(MpegTSCodecSettings *settings);
void mpegts_set_config(long bitrate, int width, int height);
void mpegts_open_stream(AVFormatContext *format_ctx, char *filename, int dump_format);
void mpegts_open_stream_without_header(AVFormatContext *format_ctx, char *filename, int dump_format);
void mpegts_close_stream(AVFormatContext *format_ctx);
void mpegts_close_stream_without_trailer(AVFormatContext *format_ctx);
void mpegts_destroy_context(AVFormatContext *format_ctx);

#ifdef __cplusplus
}
#endif

#endif
