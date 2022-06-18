#ifdef __cplusplus
extern "C" {
#endif

#ifndef _CLIB_HTTPLIVESTREAMING_H_
#define _CLIB_HTTPLIVESTREAMING_H_

#include <libavformat/avformat.h>
#include "mpegts/mpegts.h"

typedef struct HTTPLiveStreaming {
  AVFormatContext *format_ctx;
  AVCodecContext *audio_ctx;
  AVCodecContext *video_ctx;
  char *index_filename;
  int num_recent_files;
  int num_retained_old_files;
  int most_recent_number;
  char *dir;
  int is_started;
  int use_encryption;
  char *encryption_key_uri;
  uint8_t *encryption_key;
  uint8_t *encryption_iv;
  int64_t segment_start_pts;
  int64_t last_packet_pts;
  float *segment_durations;
  int segment_durations_idx;
  int is_audio_only;
} HTTPLiveStreaming;

HTTPLiveStreaming *hls_create(int num_recent_files, MpegTSCodecSettings *settings);
HTTPLiveStreaming *hls_create_audio_only(int num_recent_files, MpegTSCodecSettings *settings);
int hls_write_packet(HTTPLiveStreaming *hls, AVPacket *pkt, int split);
void hls_destroy(HTTPLiveStreaming *hls);

#endif

#ifdef __cplusplus
}
#endif
