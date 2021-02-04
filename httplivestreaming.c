#include <unistd.h>
#include <openssl/evp.h>
#include <openssl/aes.h>

#include "httplivestreaming.h"

// Derived the typedefs from libavformat/mpegtsenc.c in FFmpeg source
// because this is the only way to manipulate continuity counters.
// Original license claim is as follows.
/*
 * MPEG2 transport stream (aka DVB) muxer
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
/** START OF COPY **/
typedef struct MpegTSSection {
    int pid;
    int cc;
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(57, 57, 100)
    int discontinuity;
#endif
    void (*write_packet)(struct MpegTSSection *s, const uint8_t *packet);
    void *opaque;
} MpegTSSection;

typedef struct MpegTSService {
    MpegTSSection pmt; /* MPEG2 pmt table context */
    //
    // NOTE: there are more fields but we don't need them
    //
} MpegTSService;

typedef struct MpegTSWrite {
    const AVClass *av_class;
    MpegTSSection pat; /* MPEG2 pat table */
    MpegTSSection sdt; /* MPEG2 sdt table context */
    //
    // NOTE: there are more fields but we don't need them
    //
} MpegTSWrite;

typedef struct MpegTSWriteStream {
    int pid; /* stream associated pid */
    int cc;
    //
    // NOTE: there are more fields but we don't need them
    //
} MpegTSWriteStream;
/** END OF COPY **/

void encrypt_most_recent_file(HTTPLiveStreaming *hls) {
  char filepath[1024];
  FILE *fp;
  uint8_t *input_data;
  uint8_t *encrypted_data;
  int input_size;
  EVP_CIPHER_CTX *enc_ctx;

  // init cipher context
  enc_ctx = EVP_CIPHER_CTX_new();
  if (enc_ctx == NULL) {
    fprintf(stderr, "Error: encrypt_most_recent_file: EVP_CIPHER_CTX_new failed\n");
    return;
  }

  if (hls->encryption_key == NULL) {
    fprintf(stderr, "Warning: encryption_key is not set\n");
  }
  if (hls->encryption_iv == NULL) {
    fprintf(stderr, "Warning: encryption_iv is not set\n");
  }
  EVP_EncryptInit_ex(enc_ctx, EVP_aes_128_cbc(), NULL, hls->encryption_key, hls->encryption_iv);

  // read original data
  snprintf(filepath, 1024, "%s/%d.ts", hls->dir, hls->most_recent_number);
  fp = fopen(filepath, "rb+");
  if (fp == NULL) {
    perror("Read ts file failed");
    return;
  }
  fseek(fp, 0, SEEK_END);
  input_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  input_data = malloc(input_size);
  if (input_data == NULL) {
    perror("Can't malloc for input_data");
    return;
  }
  fread(input_data, 1, input_size, fp);

  // encrypt the data
  int c_len = input_size + AES_BLOCK_SIZE;
  int f_len;
  int encrypted_size;
  encrypted_data = malloc(c_len);
  if (encrypted_data == NULL) {
    perror("Can't malloc for encrypted_data");
    return;
  }
  EVP_EncryptUpdate(enc_ctx, encrypted_data, &c_len, input_data, input_size);
  EVP_EncryptFinal_ex(enc_ctx, encrypted_data+c_len, &f_len);
  encrypted_size = c_len + f_len;

  // write data to the same file
  fseek(fp, 0, SEEK_SET);
  fwrite(encrypted_data, 1, encrypted_size, fp);
  ftruncate(fileno(fp), encrypted_size);
  fclose(fp);

  // free up variables
  free(encrypted_data);
  free(input_data);
  EVP_CIPHER_CTX_free(enc_ctx);
}

static float max_float(float a, float b) {
  return (a >= b) ? a : b;
}

int calc_target_duration(HTTPLiveStreaming *hls) {
  float max = 0;
  int i;

  // segments
  int from_seq = hls->most_recent_number - hls->num_recent_files + 1;
  if (from_seq < 1) {
    from_seq = 1;
  }
  int num_segments = hls->most_recent_number - from_seq + 1;
  int segment_durations_idx = hls->segment_durations_idx - num_segments + 1;
  if (segment_durations_idx < 0) {
    segment_durations_idx += hls->num_recent_files;
  }
  for (i = 0; i < num_segments; i++) {
    max = max_float(max, hls->segment_durations[segment_durations_idx]);
    if (++segment_durations_idx == hls->num_recent_files) {
      segment_durations_idx = 0;
    }
  }

  // Round target duration to nearest integer
  return (int) (max + 0.5f);
}

// Write m3u8 file
int write_index(HTTPLiveStreaming *hls, int is_end) {
  FILE *file;
  char buf[128];
  char tmp_filepath[1024];
  char filepath[1024];
  int i;

  snprintf(tmp_filepath, 1024, "%s/_%s", hls->dir, hls->index_filename);
  file = fopen(tmp_filepath, "w");
  if (!file) {
    perror("fopen");
    return -1;
  }

  int target_duration = calc_target_duration(hls);
  // header
  // What features are available in each version can be found on:
  // https://developer.apple.com/library/ios/qa/qa1752/_index.html
  snprintf(buf, 128, "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:%d\n#EXT-X-MEDIA-SEQUENCE:%d\n#EXT-X-ALLOW-CACHE:NO\n",
      target_duration, hls->most_recent_number);
  fwrite(buf, 1, strlen(buf), file);

  // insert encryption header if needed
  if (hls->use_encryption) {
    if (hls->encryption_key_uri == NULL) {
      fprintf(stderr, "Error: encryption_key_uri is not set\n");
    } else {
      snprintf(buf, 128, "#EXT-X-KEY:METHOD=AES-128,URI=\"%s\",IV=0x",
          hls->encryption_key_uri);
      fwrite(buf, 1, strlen(buf), file);
      for (i = 0; i < 16; i++) {
        snprintf(buf + i * 2, 3, "%02x", hls->encryption_iv[i]);
      }
      snprintf(buf + 32, 2, "\n");
      fwrite(buf, 1, 33, file);
    }
  }

  // segments
  int from_seq = hls->most_recent_number - hls->num_recent_files + 1;
  if (from_seq < 1) {
    from_seq = 1;
  }
  int num_segments = hls->most_recent_number - from_seq + 1;
  int segment_durations_idx = hls->segment_durations_idx - num_segments + 1;
  if (segment_durations_idx < 0) {
    segment_durations_idx += hls->num_recent_files;
  }
  for (i = 0; i < num_segments; i++) {
    snprintf(buf, 128, "#EXTINF:%.5f,\n%d.ts\n",
        hls->segment_durations[segment_durations_idx],
        from_seq + i);
    fwrite(buf, 1, strlen(buf), file);
    if (++segment_durations_idx == hls->num_recent_files) {
      segment_durations_idx = 0;
    }
  }

  if (is_end) {
    // end mark
    fwrite("#EXT-X-ENDLIST\n", 1, 15, file);
  }

  fclose(file);

  snprintf(filepath, 1024, "%s/%s", hls->dir, hls->index_filename);
  rename(tmp_filepath, filepath);

  int last_seq = hls->most_recent_number - hls->num_recent_files - hls->num_retained_old_files;
  if (last_seq >= 1) {
    snprintf(filepath, 1024, "%s/%d.ts", hls->dir, last_seq);
    unlink(filepath);
  }

  return 0;
}

void hls_destroy(HTTPLiveStreaming *hls) {
  if (hls->is_started) {
    mpegts_close_stream(hls->format_ctx);
    if (hls->use_encryption) {
      encrypt_most_recent_file(hls);
      if (hls->encryption_key_uri != NULL) {
        free(hls->encryption_key_uri);
      }
      if (hls->encryption_key != NULL) {
        free(hls->encryption_key);
      }
      if (hls->encryption_iv != NULL) {
        free(hls->encryption_iv);
      }
    }

    if (++hls->segment_durations_idx == hls->num_recent_files) {
      hls->segment_durations_idx = 0;
    }
    hls->segment_durations[hls->segment_durations_idx] =
      (hls->last_packet_pts - hls->segment_start_pts) / 90000.0;

    write_index(hls, 1);
  }
  mpegts_destroy_context(hls->format_ctx);
  free(hls->segment_durations);
  free(hls);
}

void create_new_ts(HTTPLiveStreaming *hls) {
  char filepath[1024];

  hls->most_recent_number++;
  snprintf(filepath, 1024, "%s/%d.ts", hls->dir, hls->most_recent_number);
  mpegts_open_stream(hls->format_ctx, filepath, 0);
}

int hls_write_packet(HTTPLiveStreaming *hls, AVPacket *pkt, int split) {
  MpegTSWrite *ts;
  MpegTSWriteStream *ts_st;
  uint8_t pat_cc;
  uint8_t sdt_cc;
  int *stream_cc;
  int nb_streams;
  int i;

  if ( ! hls->is_started ) {
    hls->is_started = 1;
    create_new_ts(hls);
    hls->segment_start_pts = pkt->pts;
    hls->segment_durations_idx = 0;
  }

  if (split) {
    // Store the last segment duration
    if (++hls->segment_durations_idx == hls->num_recent_files) {
      hls->segment_durations_idx = 0;
    }
    hls->segment_durations[hls->segment_durations_idx] =
      (pkt->pts - hls->segment_start_pts) / 90000.0;
    hls->segment_start_pts = pkt->pts;

    // Flush remaining packets
    av_write_frame(hls->format_ctx, NULL);

    // Retain continuity counters
    ts = hls->format_ctx->priv_data;
    pat_cc = ts->pat.cc;
    sdt_cc = ts->sdt.cc;

    nb_streams = hls->format_ctx->nb_streams;
    stream_cc = malloc(sizeof(int) * nb_streams);
    if (stream_cc == NULL) {
      perror("malloc failed for stream_cc");
      exit(EXIT_FAILURE);
    }
    for (i = 0; i < nb_streams; i++) {
      ts_st = hls->format_ctx->streams[i]->priv_data;
      stream_cc[i] = ts_st->cc;
    }

    mpegts_close_stream(hls->format_ctx);
    if (hls->use_encryption) {
      encrypt_most_recent_file(hls);
    }
    write_index(hls, 0);
    create_new_ts(hls);

    // Restore continuity counters
    ts = hls->format_ctx->priv_data;
    ts->pat.cc = pat_cc;
    ts->sdt.cc = sdt_cc;
    for (i = 0; i < nb_streams; i++) {
      ts_st = hls->format_ctx->streams[i]->priv_data;
      ts_st->cc = stream_cc[i];
    }
    free(stream_cc);
  }

  if (hls->is_audio_only) {
    hls->last_packet_pts = pkt->pts;
  } else if (pkt->stream_index == 0) { // video frame
    hls->last_packet_pts = pkt->pts;
  }

  return av_write_frame(hls->format_ctx, pkt);
}

HTTPLiveStreaming *_hls_create(int num_recent_files, int is_audio_only, MpegTSCodecSettings *settings) {
  HTTPLiveStreaming *hls = malloc(sizeof(HTTPLiveStreaming));
  if (hls == NULL) {
    perror("no memory for hls");
    return NULL;
  }
  AVFormatContext *format_ctx;
  // HTTP Live Streaming does not allow video-only stream
  if (is_audio_only) {
    format_ctx = mpegts_create_context_audio_only(settings);
  } else {
    format_ctx = mpegts_create_context(settings);
  }
  hls->is_audio_only = is_audio_only;
  hls->format_ctx = format_ctx;
  hls->index_filename = "index.m3u8";
  hls->num_recent_files = num_recent_files;
  hls->num_retained_old_files = 10;
  hls->most_recent_number = 0;
  hls->dir = ".";
  hls->is_started = 0;
  hls->use_encryption = 0;
  hls->encryption_key_uri = NULL;
  hls->encryption_key = NULL;
  hls->encryption_iv = NULL;
  hls->segment_durations = malloc(sizeof(float) * num_recent_files);
  if (hls->segment_durations == NULL) {
    perror("no memory for hls->segment_durations");
    free(hls);
    return NULL;
  }
  hls->segment_start_pts = 0;
  hls->last_packet_pts = 0;
  return hls;
}

HTTPLiveStreaming *hls_create(int num_recent_files, MpegTSCodecSettings *settings) {
  return _hls_create(num_recent_files, 0, settings);
}

HTTPLiveStreaming *hls_create_audio_only(int num_recent_files, MpegTSCodecSettings *settings) {
  return _hls_create(num_recent_files, 1, settings);
}
