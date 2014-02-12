/*
 *  Capture video from Raspberry Pi Camera and audio from ALSA,
 *  encode them to H.264/AAC, and mux them to MPEG-TS.
 *
 *  H.264 encoder: Raspberry Pi H.264 hardware encoder (via OpenMAX IL)
 *  AAC encoder  : fdk-aac (via libavcodec)
 *  MPEG-TS muxer: libavformat
 */

// Some part of this code is derived from:
// https://github.com/raspberrypi/firmware/blob/master/hardfp/opt/vc/src/hello_pi/hello_encode/encode.c
// Copyright notice of encode.c is as follows
/*
Copyright (c) 2012, Broadcom Europe Ltd
Copyright (c) 2012, Kalle Vahlman <zuh@iki>
                    Tuomas Kulve <tuomas@kulve.fi>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <math.h>
#include <linux/videodev2.h>
#include <alsa/asoundlib.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>

#include "bcm_host.h"
#include "ilclient.h"

#include "config.h"
#include "hooks.h"
#include "mpegts.h"
#include "httplivestreaming.h"
#include "state.h"

// What color should we use to fill borders? (in YUV)
#define FILL_COLOR_Y 0
#define FILL_COLOR_U 128
#define FILL_COLOR_V 128

// ALSA buffer size (frames) is multiplied by this number
#define ALSA_BUFFER_MULTIPLY 20

#define AVAIL_AUDIO 2

// Enable video preview output
#define ENABLE_PREVIEW 0

// Enable clock OMX component
#define ENABLE_CLOCK 1

// Both PTS and DTS are 33 bit and wraps around to zero.
#define PTS_MODULO 8589934592

#define AUDIO_PTS_START 0
#define VIDEO_PTS_START 0

#define STATE_DIR "state"

#define RECORD_BUFFER_KEYFRAMES 5

#define ENABLE_HLS_ENCRYPTION 0

static int64_t video_current_pts = 0;
static int64_t audio_current_pts = 0;

typedef enum {
  PTS_SPEED_NORMAL,
  PTS_SPEED_UP,
  PTS_SPEED_DOWN,
} pts_mode_t;

pts_mode_t pts_mode = PTS_SPEED_NORMAL;

// Each video frame's PTS is incremented by this in normal condition
// 90000 / 3014 = 29.860650299 FPS
#define VIDEO_PTS_STEP 3014

static int audio_pts_step_base;
static int speed_up_count = 0;
static int speed_down_count = 0;

// If we increase this value, audio gets faster than video
#define N_BUFFER_COUNT_ACTUAL 1  // For 1280x720 with new mic

// If we increase this value, video gets faster than audio
#define AUDIO_BUFFER_CHUNKS 0

#if AUDIO_BUFFER_CHUNKS > 0
uint16_t *audio_buffer[AUDIO_BUFFER_CHUNKS];
int audio_buffer_index = 0;
int is_audio_buffer_filled = 0;
#endif

#define PTS_DIFF_TOO_LARGE 45000  // 90000 == 1 second

// If this value is 1, audio capturing is always disabled.
static int disable_audio_capturing = 0;

static pthread_t audio_nop_thread;

#define ENABLE_AUDIO_AMPLIFICATION 0

#define AUDIO_VOLUME_MULTIPLY 2.0
#define AUDIO_MIN_VALUE -16384
#define AUDIO_MAX_VALUE 16383

static const int fr_q16 = (TARGET_FPS) * 65536;

// function prototypes
static void encode_and_send_image();
static void encode_and_send_audio();
void start_record();
void stop_record();

static long long video_frame_count = 0;
static long long audio_frame_count = 0;
static int video_frame_advantage = 0;
static int64_t video_start_time;
static int64_t audio_start_time;
static int is_video_recording_started = 0;
static int is_audio_recording_started = 0;
static uint8_t *last_video_buffer = NULL;
static size_t last_video_buffer_size = 0;
static int keyframes_count = 0;

static int video_pending_drop_frames = 0;
static int audio_pending_drop_frames = 0;

static COMPONENT_T *video_encode = NULL;
static COMPONENT_T *component_list[5];
static int n_component_list = 0;
static ILCLIENT_T *ilclient;

static ILCLIENT_T *cam_client;
static COMPONENT_T *camera_component = NULL;
#if ENABLE_PREVIEW
static COMPONENT_T *render_component = NULL;
#endif
#if ENABLE_CLOCK
static COMPONENT_T *clock_component = NULL;
#endif
#if ENABLE_PREVIEW || ENABLE_CLOCK
static TUNNEL_T tunnel[4];
static int n_tunnel = 0;
#endif

#define USE_AUTO_EXPOSURE 0
#define EXPOSURE_AUTO 0
#define EXPOSURE_NIGHT 1
#define EXPOSURE_NIGHT_Y_THRESHOLD 40
#define EXPOSURE_AUTO_Y_THRESHOLD 50

#define REC_CHASE_PACKETS 10

// UNIX domain sockets provided by node-rtsp-rtmp-server
#define SOCK_PATH_VIDEO "/tmp/node_rtsp_rtmp_videoReceiver"
#define SOCK_PATH_VIDEO_CONTROL "/tmp/node_rtsp_rtmp_videoControl"
#define SOCK_PATH_AUDIO "/tmp/node_rtsp_rtmp_audioReceiver"
#define SOCK_PATH_AUDIO_CONTROL "/tmp/node_rtsp_rtmp_audioControl"

// Disable output to RTSP server via UNIX domain sockets
#define DISABLE_UNIX_SOCKETS_OUTPUT 1  // default: 0
// Enable MPEG-TS output via TCP socket
#define ENABLE_TCP_OUTPUT 1
// Where to send MPEG-TS stream
#define TCP_OUTPUT_DEST "tcp://127.0.0.1:8181"

#if ENABLE_TCP_OUTPUT
static AVFormatContext *tcp_ctx;
static pthread_mutex_t tcp_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

#define HOOKS_DIR "hooks"

static int current_exposure_mode = EXPOSURE_AUTO;
static long long previous_capture_frame = 0;
static long long previous_previous_capture_frame = 0;

static int keepRunning = 1;
static int frame_count = 0;
static int current_audio_frames = 0;
static uint8_t *codec_configs[2];
static int codec_config_sizes[2];
static int codec_config_total_size = 0;
static int n_codec_configs = 0;
static struct timespec tsBegin = { .tv_sec = 0, .tv_nsec = 0 };

static AVFormatContext *rec_format_ctx;
static HTTPLiveStreaming *hls;
static int flush_recording_seconds = 5; // Flush recording data every 5 seconds
static time_t rec_start_time;

// NAL unit type 9
static uint8_t access_unit_delimiter[] = {
  0x00, 0x00, 0x00, 0x01, 0x09, 0xf0,
};
static int access_unit_delimiter_length = 6;

// sound
static snd_pcm_t *capture_handle;
static uint16_t *samples;
static AVFrame *av_frame;
static int audio_fd_count;
static struct pollfd *ufds;//file descriptor array used by pool
static int is_first_audio;
static int period_size;
static int channels = 1;

// threads
static pthread_mutex_t mutex_writing = PTHREAD_MUTEX_INITIALIZER;

// UNIX domain sockets
#if !(DISABLE_UNIX_SOCKETS_OUTPUT)
static int sockfd_video;
static int sockfd_video_control;
static int sockfd_audio;
static int sockfd_audio_control;
#endif // !(DISABLE_UNIX_SOCKETS_OUTPUT)

static uint8_t *encbuf = NULL;
static int encbuf_size = -1;

static pthread_t rec_thread;
static pthread_cond_t rec_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t rec_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rec_write_mutex = PTHREAD_MUTEX_INITIALIZER;
static int rec_thread_needs_write = 0;
static int rec_thread_needs_exit = 0;
static int rec_thread_frame = 0;
static int rec_thread_needs_flush = 0;

typedef struct EncodedPacket {
  int64_t pts;
  uint8_t *data;
  int size;
  int stream_index;
  int flags;
} EncodedPacket;

EncodedPacket **encoded_packets; // circular buffer
static int current_encoded_packet = -1;
static int keyframe_pointers[RECORD_BUFFER_KEYFRAMES] = { 0 };
static int current_keyframe_pointer = -1;
static int is_keyframe_pointers_filled = 0;
static int encoded_packets_size;

// hooks
static pthread_t hooks_thread;
char recording_filepath[50];
char recording_tmp_filepath[50];
char recording_archive_filepath[50];
int is_recording = 0;

static MpegTSCodecSettings codec_settings;

static int is_audio_muted = 0;

static void unmute_audio() {
  fprintf(stderr, "unmute");
  is_audio_muted = 0;
}

static void mute_audio() {
  fprintf(stderr, "mute");
  is_audio_muted = 1;
}

// Check if disk usage is >= 95%
static int is_disk_almost_full() {
  struct statvfs stat;
  statvfs("/", &stat);
  int used_percent = ceil( (stat.f_blocks - stat.f_bfree) * 100.0 / stat.f_blocks);
  fprintf(stderr, "disk usage=%d%% ", used_percent);
  if (used_percent >= 95) {
    return 1;
  } else {
    return 0;
  }
}

static void mark_keyframe_packet() {
  current_keyframe_pointer++;
  if (current_keyframe_pointer >= RECORD_BUFFER_KEYFRAMES) {
    current_keyframe_pointer = 0;
    if (!is_keyframe_pointers_filled) {
      is_keyframe_pointers_filled = 1;
    }
  }
  keyframe_pointers[current_keyframe_pointer] = current_encoded_packet;
}

static void prepare_encoded_packets() {
  int audio_fps = AUDIO_SAMPLE_RATE / 1 / period_size;
  encoded_packets_size = TARGET_FPS * RECORD_BUFFER_KEYFRAMES * 2 +
    (audio_fps + 1) * RECORD_BUFFER_KEYFRAMES * 2 + 100;

  fprintf(stderr, "prepare_encoded_packets: limit=%d\n", encoded_packets_size);
  int malloc_size = sizeof(EncodedPacket *) * encoded_packets_size;
  encoded_packets = malloc(malloc_size);
  if (encoded_packets == NULL) {
    fprintf(stderr, "Can't allocate memory for encodedPackets\n");
    exit(1);
  }
  memset(encoded_packets, 0, malloc_size);
}

static int write_encoded_packets(int max_packets, int origin_pts) {
  int ret;
  AVPacket avpkt;
  EncodedPacket *enc_pkt;

  av_init_packet(&avpkt);
  int wrote_packets = 0;

  pthread_mutex_lock(&rec_write_mutex);
  while (1) {
    wrote_packets++;
    enc_pkt = encoded_packets[rec_thread_frame];
    avpkt.pts = avpkt.dts = enc_pkt->pts - origin_pts;
    avpkt.data = enc_pkt->data;
    avpkt.size = enc_pkt->size;
    avpkt.stream_index = enc_pkt->stream_index;
    avpkt.flags = enc_pkt->flags;
    ret = av_write_frame(rec_format_ctx, &avpkt);
    if (ret < 0) {
      fprintf(stderr, "write_encoded_packets: av_write_frame error: ret=%d\n", ret);
    }
    if (++rec_thread_frame == encoded_packets_size) {
      rec_thread_frame = 0;
    }
    if (rec_thread_frame == current_encoded_packet) {
      break;
    }
    if (wrote_packets == max_packets) {
      break;
    }
  }
  pthread_mutex_unlock(&rec_write_mutex);
  av_free_packet(&avpkt);

  return wrote_packets;
}

static void add_encoded_packet(int64_t pts, uint8_t *data, int size, int stream_index, int flags) {
  EncodedPacket *packet;

  if (++current_encoded_packet == encoded_packets_size) {
    current_encoded_packet = 0;
  }
  packet = encoded_packets[current_encoded_packet];
  if (packet != NULL) {
    av_free(packet->data);
  } else {
    packet = malloc(sizeof(EncodedPacket));
    if (packet == NULL) {
      perror("Can't allocate memory for EncodedPacket");
      return;
    }
    encoded_packets[current_encoded_packet] = packet;
  }
  packet->pts = pts;
  packet->data = data;
  packet->size = size;
  packet->stream_index = stream_index;
  packet->flags = flags;
}

static void free_encoded_packets() {
  int i;
  EncodedPacket *packet;

  for (i = 0; i < encoded_packets_size; i++) {
    packet = encoded_packets[i];
    if (packet != NULL) {
      av_free(packet->data);
    }
    free(packet);
  }
}

void setup_av_frame(AVFormatContext *format_ctx) {
  AVCodecContext *audio_codec_ctx;
  int ret;
  int buffer_size;

#if AUDIO_ONLY
  audio_codec_ctx = format_ctx->streams[0]->codec;
#else
  audio_codec_ctx = format_ctx->streams[1]->codec;
#endif

  av_frame = avcodec_alloc_frame();
  if (!av_frame) {
    fprintf(stderr, "avcodec_alloc_frame failed\n");
    exit(1);
  }

  av_frame->sample_rate = audio_codec_ctx->sample_rate;
  av_frame->nb_samples = audio_codec_ctx->frame_size;
  fprintf(stderr, "audio nb_samples: %d\n", av_frame->nb_samples);
  av_frame->format = audio_codec_ctx->sample_fmt;
  av_frame->channel_layout = audio_codec_ctx->channel_layout;

  buffer_size = av_samples_get_buffer_size(NULL, audio_codec_ctx->channels,
      audio_codec_ctx->frame_size, audio_codec_ctx->sample_fmt, 0);
  samples = av_malloc(buffer_size);
  if (!samples) {
    fprintf(stderr, "av_malloc for samples failed\n");
    exit(1);
  }
#if AUDIO_BUFFER_CHUNKS > 0
  int i;
  for (i = 0; i < AUDIO_BUFFER_CHUNKS; i++) {
    audio_buffer[i] = av_malloc(buffer_size);
    if (!audio_buffer[i]) {
      fprintf(stderr, "av_malloc for audio_buffer[%d] failed\n", i);
      exit(1);
    }
  }
#endif

  period_size = buffer_size / channels / sizeof(short);
  audio_pts_step_base = 90000.0 * period_size / AUDIO_SAMPLE_RATE;
  fprintf(stderr, "audio_pts_step_base: %d\n", audio_pts_step_base);
  fprintf(stderr, "buffer_size=%d period_size=%d\n", buffer_size, period_size);
  fprintf(stderr, "channels=%d frame_size=%d sample_fmt=%d buffer_size=%d period_size=%d sizeof(short)=%d\n", channels, audio_codec_ctx->frame_size, audio_codec_ctx->sample_fmt, buffer_size, period_size, sizeof(short));

  ret = avcodec_fill_audio_frame(av_frame, audio_codec_ctx->channels, audio_codec_ctx->sample_fmt,
      (const uint8_t*)samples, buffer_size, 0);
  if (ret < 0) {
    fprintf(stderr, "avcodec_fill_audio_frame failed: ret=%d\n", ret);
    exit(1);
  }
}

void check_record_directory() {
  struct stat st;
  int err;
  char *dir;

  dir = "rec";
  err = stat(dir, &st);
  if (err == -1) {
    if (errno == ENOENT) {
      fprintf(stderr, "rec directory does not exist\n");
    } else {
      perror("stat error");
    }
    exit(1);
  } else {
    if (!S_ISDIR(st.st_mode)) {
      fprintf(stderr, "rec is not a directory\n");
      exit(1);
    }
  }

  if (access(dir, R_OK) != 0) {
    perror("Can't access rec directory");
    exit(1);
  }
}

void *rec_thread_stop() {
  FILE *fsrc, *fdest;
  int read_len;
  uint8_t *copy_buf;

  copy_buf = malloc(BUFSIZ);
  if (copy_buf == NULL) {
    perror("malloc for copy_buf failed");
    pthread_exit(0);
  }

  pthread_mutex_lock(&rec_write_mutex);
  mpegts_close_stream(rec_format_ctx);
  mpegts_destroy_context(rec_format_ctx);
  fprintf(stderr, "stop rec");
  state_set(STATE_DIR, "record", "false");
  pthread_mutex_unlock(&rec_write_mutex);

  fprintf(stderr, "copy ");
  fsrc = fopen(recording_tmp_filepath, "r");
  if (fsrc == NULL) {
    perror("fopen recording_tmp_filepath failed");
  }
  fdest = fopen(recording_archive_filepath, "a");
  if (fdest == NULL) {
    perror("fopen recording_archive_filepath failed");
  }
  while (1) {
    read_len = fread(copy_buf, 1, BUFSIZ, fsrc);
    if (read_len > 0) {
      fwrite(copy_buf, 1, read_len, fdest);
    }
    if (read_len != BUFSIZ) {
      break;
    }
  }
  if (feof(fsrc)) {
    fclose(fsrc);
    fclose(fdest);
  } else {
    perror("not an EOF?");
  }

  // link
  fprintf(stderr, "symlink");
  if (symlink(recording_archive_filepath + 4, recording_filepath) != 0) { // +4 is for trimming "rec/"
    perror("symlink failed");
  }

  // unlink tmp file
  fprintf(stderr, "unlink");
  unlink(recording_tmp_filepath);

  state_set(STATE_DIR, "last_rec", recording_filepath);

  free(copy_buf);
  is_recording = 0;

  pthread_exit(0);
}

void flush_record() {
  rec_thread_needs_flush = 1;
}

void stop_record() {
  rec_thread_needs_exit = 1;
}

void check_record_duration() {
  time_t now;

  if (is_recording) {
    now = time(NULL);
    if (now - rec_start_time > flush_recording_seconds) {
      flush_record();
    }
  }
}

void *rec_thread_start() {
  time_t rawtime;
  struct tm *timeinfo;
  AVPacket av_pkt;
  int wrote_packets;
  int is_caught_up = 0;
  int unique_number = 1;
  int64_t rec_start_pts, rec_end_pts;
  char diff_pts[11];
  EncodedPacket *enc_pkt;
  char filename_base[20];
  int filename_decided = 0;
  uint8_t *copy_buf;
  FILE *fsrc, *fdest;
  int read_len;

  copy_buf = malloc(BUFSIZ);
  if (copy_buf == NULL) {
    perror("malloc for copy_buf failed");
    pthread_exit(0);
  }

  time(&rawtime);
  timeinfo = localtime(&rawtime);

  rec_start_time = time(NULL);
  rec_start_pts = -1;

  strftime(filename_base, 80, "%Y-%m-%d_%H-%M-%S", timeinfo);
  snprintf(recording_filepath, 50, "rec/%s.ts", filename_base);
  if (access(recording_filepath, F_OK) != 0) {
    snprintf(recording_archive_filepath, 50, "rec/archive/%s.ts", filename_base);
    snprintf(recording_tmp_filepath, 50, "rec/tmp/%s.ts", filename_base);
    filename_decided = 1;
  }
  while (!filename_decided) {
    unique_number++;
    snprintf(recording_filepath, 50, "rec/%s-%d.ts", filename_base, unique_number);
    if (access(recording_filepath, F_OK) != 0) {
      snprintf(recording_archive_filepath, 50, "rec/archive/%s.ts", filename_base);
      snprintf(recording_tmp_filepath, 50, "rec/tmp/%s-%d.ts", filename_base, unique_number);
      filename_decided = 1;
    }
  }

  pthread_mutex_lock(&rec_write_mutex);
  rec_format_ctx = mpegts_create_context(&codec_settings);
  mpegts_open_stream(rec_format_ctx, recording_tmp_filepath, 0);
  is_recording = 1;
  fprintf(stderr, "start rec to %s", recording_tmp_filepath);
  state_set(STATE_DIR, "record", "true");
  pthread_mutex_unlock(&rec_write_mutex);

  int start_keyframe_pointer;
  if (!is_keyframe_pointers_filled) {
    start_keyframe_pointer = 0;
  } else {
    start_keyframe_pointer = current_keyframe_pointer - RECORD_BUFFER_KEYFRAMES + 1;
  }
  while (start_keyframe_pointer < 0) {
    start_keyframe_pointer += RECORD_BUFFER_KEYFRAMES;
  }
  rec_thread_frame = keyframe_pointers[start_keyframe_pointer];
  enc_pkt = encoded_packets[rec_thread_frame];
  rec_start_pts = enc_pkt->pts;

  write_encoded_packets(REC_CHASE_PACKETS, rec_start_pts);

  av_init_packet(&av_pkt);
  while (!rec_thread_needs_exit) {
    pthread_mutex_lock(&rec_mutex);
    while (!rec_thread_needs_write) {
      pthread_cond_wait(&rec_cond, &rec_mutex);
    }
    pthread_mutex_unlock(&rec_mutex);

    if (rec_thread_frame != current_encoded_packet) {
      wrote_packets = write_encoded_packets(REC_CHASE_PACKETS, rec_start_pts);
      if (wrote_packets <= 2) {
        if (!is_caught_up) {
          fprintf(stderr, "caught up");
          is_caught_up = 1;
        }
      }
    }
    check_record_duration();
    if (rec_thread_needs_flush) {
      fprintf(stderr, "F");
      mpegts_close_stream_without_trailer(rec_format_ctx);

      fsrc = fopen(recording_tmp_filepath, "r");
      if (fsrc == NULL) {
        perror("fopen recording_tmp_filepath failed");
      }
      fdest = fopen(recording_archive_filepath, "a");
      if (fdest == NULL) {
        perror("fopen recording_archive_filepath failed");
      }
      while (1) {
        read_len = fread(copy_buf, 1, BUFSIZ, fsrc);
        if (read_len > 0) {
          fwrite(copy_buf, 1, read_len, fdest);
        }
        if (read_len != BUFSIZ) {
          break;
        }
      }
      if (feof(fsrc)) {
        fclose(fsrc);
        fclose(fdest);
      } else {
        perror("not an EOF?");
      }

      mpegts_open_stream_without_header(rec_format_ctx, recording_tmp_filepath, 0);
      rec_thread_needs_flush = 0;
      rec_start_time = time(NULL);
    }
    rec_thread_needs_write = 0;
  }
  free(copy_buf);
  av_free_packet(&av_pkt);
  int prev_frame = rec_thread_frame - 1;
  if (prev_frame == -1) {
    prev_frame = encoded_packets_size - 1;
  }
  enc_pkt = encoded_packets[prev_frame];
  rec_end_pts = enc_pkt->pts;
  snprintf(diff_pts, 11, "%" PRId64, rec_end_pts - rec_start_pts);
  state_set(STATE_DIR, recording_filepath + 4, diff_pts);

  return rec_thread_stop();
}

void start_record() {
  if (is_recording) {
    fprintf(stderr, "Recording is already started\n");
    return;
  }

  if (is_disk_almost_full()) {
    fprintf(stderr, "disk is almost full, recording not started\n");
    return;
  }

  rec_thread_needs_exit = 0;
  pthread_create(&rec_thread, NULL, rec_thread_start, NULL);
}

void on_file_create(char *filename, char *content) {
  if (strcmp(filename, "start_record") == 0) {
    start_record();
  } else if (strcmp(filename, "stop_record") == 0) {
    stop_record();
  } else if (strcmp(filename, "mute") == 0) {
    mute_audio();
  } else if (strcmp(filename, "unmute") == 0) {
    unmute_audio();
  }
}

// Send audio packet to node-rtsp-rtmp-server
static void send_audio_control_info() {
#if !(DISABLE_UNIX_SOCKETS_OUTPUT)
  int64_t logical_start_time = audio_start_time;
  uint8_t sendbuf[9] = {
    // header
    0x1,
    // payload
    logical_start_time >> 56,
    (logical_start_time >> 48) & 0xff,
    (logical_start_time >> 40) & 0xff,
    (logical_start_time >> 32) & 0xff,
    (logical_start_time >> 24) & 0xff,
    (logical_start_time >> 16) & 0xff,
    (logical_start_time >> 8) & 0xff,
    logical_start_time & 0xff,
  };
  if (send(sockfd_audio_control, sendbuf, 9, 0) == -1) {
    perror("send audio_control");
    exit(1);
  }
  fprintf(stderr, "audio control info sent: %lld\n", logical_start_time);
#endif // !(DISABLE_UNIX_SOCKETS_OUTPUT)
}

// Send video packet to node-rtsp-rtmp-server
static void send_video_control_info() {
#if !(DISABLE_UNIX_SOCKETS_OUTPUT)
  int64_t logical_start_time = video_start_time;
  uint8_t sendbuf[9] = {
    // header
    0x1,
    // payload
    logical_start_time >> 56,
    (logical_start_time >> 48) & 0xff,
    (logical_start_time >> 40) & 0xff,
    (logical_start_time >> 32) & 0xff,
    (logical_start_time >> 24) & 0xff,
    (logical_start_time >> 16) & 0xff,
    (logical_start_time >> 8) & 0xff,
    logical_start_time & 0xff,
  };
  if (send(sockfd_video_control, sendbuf, 9, 0) == -1) {
    perror("send video_control");
    exit(1);
  }
  fprintf(stderr, "video control info sent: %lld\n", logical_start_time);
#endif // !(DISABLE_UNIX_SOCKETS_OUTPUT)
}

static void setup_socks() {
#if !(DISABLE_UNIX_SOCKETS_OUTPUT)
  struct sockaddr_un remote_video;
  struct sockaddr_un remote_audio;

  int len;
  struct sockaddr_un remote_video_control;
  struct sockaddr_un remote_audio_control;

  // Setup sockfd_video
  if ((sockfd_video = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("socket video");
    exit(1);
  }
  remote_video.sun_family = AF_UNIX;
  strcpy(remote_video.sun_path, SOCK_PATH_VIDEO);
  len = strlen(remote_video.sun_path) + sizeof(remote_video.sun_family);
  if (connect(sockfd_video, (struct sockaddr *)&remote_video, len) == -1) {
    perror("Failed to connect to RTSP video socket (perhaps RTSP server is not running?)");
    exit(1);
  }
  fprintf(stderr, "video server connected\n");

  // Setup sockfd_video_control
  if ((sockfd_video_control = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("socket video_control");
    exit(1);
  }
  remote_video_control.sun_family = AF_UNIX;
  strcpy(remote_video_control.sun_path, SOCK_PATH_VIDEO_CONTROL);
  len = strlen(remote_video_control.sun_path) + sizeof(remote_video_control.sun_family);
  if (connect(sockfd_video_control, (struct sockaddr *)&remote_video_control, len) == -1) {
    perror("connect video_control");
    exit(1);
  }
  fprintf(stderr, "video control server connected\n");

  // Setup sockfd_audio
  if ((sockfd_audio = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("socket audio");
    exit(1);
  }
  remote_audio.sun_family = AF_UNIX;
  strcpy(remote_audio.sun_path, SOCK_PATH_AUDIO);
  len = strlen(remote_audio.sun_path) + sizeof(remote_audio.sun_family);
  if (connect(sockfd_audio, (struct sockaddr *)&remote_audio, len) == -1) {
    perror("connect audio");
    exit(1);
  }
  fprintf(stderr, "audio server connected\n");

  // Setup sockfd_audio_control
  if ((sockfd_audio_control = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("socket audio_control");
    exit(1);
  }
  remote_audio_control.sun_family = AF_UNIX;
  strcpy(remote_audio_control.sun_path, SOCK_PATH_AUDIO_CONTROL);
  len = strlen(remote_audio_control.sun_path) + sizeof(remote_audio_control.sun_family);
  if (connect(sockfd_audio_control, (struct sockaddr *)&remote_audio_control, len) == -1) {
    perror("connect audio_control");
    exit(1);
  }
  fprintf(stderr, "audio_control server connected\n");
#endif // !(DISABLE_UNIX_SOCKETS_OUTPUT)
}

static void teardown_socks() {
#if !(DISABLE_UNIX_SOCKETS_OUTPUT)
  close(sockfd_video);
  close(sockfd_video_control);
  close(sockfd_audio);
  close(sockfd_audio_control);
#endif // !(DISABLE_UNIX_SOCKETS_OUTPUT)
}

static int64_t get_next_audio_pts() {
  int64_t pts;
  audio_frame_count++;

  // We use audio timing as the base clock,
  // so we do not modify PTS here.
  pts = audio_current_pts + audio_pts_step_base;

  audio_current_pts = pts;

  return pts;
}

static int64_t get_video_pts_for_frame(int frame_number) {
  // To play on QuickTime correctly, align PTS at regular intervals like this
  return VIDEO_PTS_START +
    (int64_t)((frame_number + video_frame_advantage) * (double)90000 / (double)TARGET_FPS);
}

static int64_t get_next_video_pts() {
  int64_t pts;
  video_frame_count++;

  int pts_diff = audio_current_pts - video_current_pts - VIDEO_PTS_STEP;
  int tolerance = (VIDEO_PTS_STEP + audio_pts_step_base) * 2;
  if (pts_diff >= PTS_DIFF_TOO_LARGE) {
    fprintf(stderr, "vR%d", pts_diff);
    pts = audio_current_pts;
  } else if (pts_diff >= tolerance) {
    if (pts_mode != PTS_SPEED_UP) {
      speed_up_count++;
      pts_mode = PTS_SPEED_UP;
      fprintf(stderr, "vSPEED_UP(%d)", pts_diff);
    }
    // Catch up with audio PTS if the delay is too large.
    pts = video_current_pts + VIDEO_PTS_STEP + 150;
  } else if (pts_diff <= -tolerance) {
    if (pts_mode != PTS_SPEED_DOWN) {
      pts_mode = PTS_SPEED_DOWN;
      speed_down_count++;
      fprintf(stderr, "vSPEED_DOWN(%d)", pts_diff);
    }
    pts = video_current_pts + VIDEO_PTS_STEP - 150;
  } else {
    pts = video_current_pts + VIDEO_PTS_STEP;
    if (pts_diff < 2000 && pts_diff > -2000) {
      if (pts_mode != PTS_SPEED_NORMAL) {
        fprintf(stderr, "vNORMAL");
        pts_mode = PTS_SPEED_NORMAL;
      }
    } else {
      if (pts_mode == PTS_SPEED_UP) {
        pts += 150;
      } else if (pts_mode == PTS_SPEED_DOWN) {
        pts -= 150;
      }
    }
  }

  video_current_pts = pts;

  return pts;
}

static int64_t get_next_audio_write_time() {
  if (audio_frame_count == 0) {
    return LLONG_MIN;
  }
  return audio_start_time + audio_frame_count * 1000000000.0 / ((float)AUDIO_SAMPLE_RATE / (float)period_size);
}

static void print_audio_timing() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  int64_t cur_time = ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
  int64_t video_pts = video_current_pts;
  int64_t audio_pts = audio_current_pts;
  int64_t avdiff = audio_pts - video_pts;

  // The following equation causes int64 overflow:
  // (cur_time - audio_start_time) * INT64_C(90000) / INT64_C(1000000000);
  int64_t clock_pts = (cur_time - audio_start_time) * 90000.0 / 1000000000.0;

  fprintf(stderr, " vp=%lld ap=%lld a-v=%lld c-a=%lld u=%d d=%d\n",
      video_pts, audio_pts, avdiff, clock_pts - audio_pts, speed_up_count, speed_down_count);
}

static void send_audio_frame(uint8_t *databuf, int databuflen, int64_t pts) {
#if !(DISABLE_UNIX_SOCKETS_OUTPUT)
  int payload_size = databuflen - 1;  // -7(ADTS header) +6(pts)
  int total_size = payload_size + 3;  // more 3 bytes for payload length
  uint8_t *sendbuf = malloc(total_size);
  if (sendbuf == NULL) {
    fprintf(stderr, "Can't alloc for audio sendbuf: size=%d", total_size);
    return;
  }
  sendbuf[0] = (payload_size >> 16) & 0xff;
  sendbuf[1] = (payload_size >> 8) & 0xff;
  sendbuf[2] = payload_size & 0xff;
  sendbuf[3] = (pts >> 40) & 0xff;
  sendbuf[4] = (pts >> 32) & 0xff;
  sendbuf[5] = (pts >> 24) & 0xff;
  sendbuf[6] = (pts >> 16) & 0xff;
  sendbuf[7] = (pts >> 8) & 0xff;
  sendbuf[8] = pts & 0xff;
  memcpy(sendbuf + 9, databuf + 7, databuflen - 7); // omit ADTS header (7 bytes)
  if (send(sockfd_audio, sendbuf, total_size, 0) == -1) {
    perror("send audio");
    exit(1);
  }
  free(sendbuf);
#endif // !(DISABLE_UNIX_SOCKETS_OUTPUT)
}

static void send_video_frame(uint8_t *databuf, int databuflen, int64_t pts) {
#if !(DISABLE_UNIX_SOCKETS_OUTPUT)
  int payload_size = databuflen + 2;  // -4(start code) +6(pts)
  int total_size = payload_size + 3;  // more 3 bytes for payload length
  uint8_t *sendbuf = malloc(total_size);
  if (sendbuf == NULL) {
    fprintf(stderr, "Can't alloc for video sendbuf: size=%d", total_size);
    return;
  }
  sendbuf[0] = (payload_size >> 16) & 0xff;
  sendbuf[1] = (payload_size >> 8) & 0xff;
  sendbuf[2] = payload_size & 0xff;
  sendbuf[3] = (pts >> 40) & 0xff;
  sendbuf[4] = (pts >> 32) & 0xff;
  sendbuf[5] = (pts >> 24) & 0xff;
  sendbuf[6] = (pts >> 16) & 0xff;
  sendbuf[7] = (pts >> 8) & 0xff;
  sendbuf[8] = pts & 0xff;
  memcpy(sendbuf + 9, databuf + 4, databuflen - 4); // omit start code (4 bytes)
  if (send(sockfd_video, sendbuf, total_size, 0) == -1) {
    perror("send video error");
  }
  free(sendbuf);
#endif // !(DISABLE_UNIX_SOCKETS_OUTPUT)
}

// send keyframe (nal_unit_type 5)
static int send_keyframe(uint8_t *data, size_t data_len, int consume_time) {
  uint8_t *buf, *ptr;
  int total_size, ret, i;
  AVPacket pkt;
  int64_t pts;

  total_size = access_unit_delimiter_length + codec_config_total_size + data_len;
  ptr = buf = av_malloc(total_size);
  if (buf == NULL) {
    fprintf(stderr, "Can't allocate buf in send_keyframe; size=%d\n", total_size);
    exit(1);
  }

  // One entire access unit should be passed to av_write_frame().
  // If access unit delimiter (AUD) is not present on top of access unit,
  // libavformat/mpegtsenc.c automatically inserts AUD.
  // Improperly inserted AUD makes whole video unplayable on QuickTime.
  // Although VLC can play those files.
  // One access unit should contain exactly one video frame (primary coded picture).
  // See spec p.5 "3.1 access unit"

  // access unit delimiter (nal_unit_type 9)
  memcpy(ptr, access_unit_delimiter, access_unit_delimiter_length);
  ptr += access_unit_delimiter_length;

  // codec configs (nal_unit_type 7 and 8)
  for (i = 0; i < n_codec_configs; i++) {
    memcpy(ptr, codec_configs[i], codec_config_sizes[i]);
    ptr += codec_config_sizes[i];
  }

  // I frame (nal_unit_type 5)
  memcpy(ptr, data, data_len);

  av_init_packet(&pkt);
  pkt.stream_index = hls->format_ctx->streams[0]->index;
  pkt.flags |= AV_PKT_FLAG_KEY;
  pkt.data = buf;
  pkt.size = total_size;

  if (consume_time) {
    pts = get_next_video_pts();
  } else {
    pts = get_video_pts_for_frame(video_frame_count);
  }

  send_video_frame(data, data_len, pts);

  pts = pts % PTS_MODULO;

  // PTS (presentation time stamp): Timestamp when a decoder should play this frame
  // DTS (decoding time stamp): Timestamp when a decoder should decode this frame
  // DTS should be smaller than or equal to PTS.
  pkt.pts = pkt.dts = pts;

  uint8_t *copied_data = av_malloc(total_size);
  memcpy(copied_data, buf, total_size);
  pthread_mutex_lock(&rec_write_mutex);
  add_encoded_packet(pts, copied_data, total_size, pkt.stream_index, pkt.flags);
  mark_keyframe_packet();
  pthread_mutex_unlock(&rec_write_mutex);

  if (is_recording) {
    pthread_mutex_lock(&rec_mutex);
    rec_thread_needs_write = 1;
    pthread_cond_signal(&rec_cond);
    pthread_mutex_unlock(&rec_mutex);
  }

#if ENABLE_TCP_OUTPUT
  pthread_mutex_lock(&tcp_mutex);
  av_write_frame(tcp_ctx, &pkt);
  pthread_mutex_unlock(&tcp_mutex);
#endif

  pthread_mutex_lock(&mutex_writing);
  int split;
  if (video_frame_count == 1) {
    split = 0;
  } else {
    split = 1;
  }
  ret = hls_write_packet(hls, &pkt, split);
  pthread_mutex_unlock(&mutex_writing);
  if (ret < 0) {
    fprintf(stderr, "keyframe write error (hls): %d\n", ret);
    fprintf(stderr, "Check if the filesystem is not full\n");
  }

  free(buf);
  av_free_packet(&pkt);
  return ret;
}

// send P frame (nal_unit_type 1)
static int send_pframe(uint8_t *data, size_t data_len, int consume_time) {
  uint8_t *buf;
  int total_size, ret;
  AVPacket pkt;
  int64_t pts;

  if (data_len == 0) {
    fprintf(stderr, "Z");
    return 0;
  }

  total_size = access_unit_delimiter_length + data_len;
  buf = av_malloc(total_size);
  if (buf == NULL) {
    fprintf(stderr, "send_pframe malloc failed: size=%d\n", total_size);
    exit(1);
  }

  // access unit delimiter (nal_unit_type 9)
  memcpy(buf, access_unit_delimiter, access_unit_delimiter_length);

  // P frame (nal_unit_type 1)
  memcpy(buf + access_unit_delimiter_length, data, data_len);

  av_init_packet(&pkt);
  pkt.stream_index = hls->format_ctx->streams[0]->index;
  pkt.data = buf;
  pkt.size = total_size;

  if (consume_time) {
    pts = get_next_video_pts();
  } else {
    pts = get_video_pts_for_frame(video_frame_count);
  }

  send_video_frame(data, data_len, pts);

  pts = pts % PTS_MODULO;

  pkt.pts = pkt.dts = pts;

  uint8_t *copied_data = av_malloc(total_size);
  memcpy(copied_data, buf, total_size);
  pthread_mutex_lock(&rec_write_mutex);
  add_encoded_packet(pts, copied_data, total_size, pkt.stream_index, pkt.flags);
  pthread_mutex_unlock(&rec_write_mutex);

  if (is_recording) {
    pthread_mutex_lock(&rec_mutex);
    rec_thread_needs_write = 1;
    pthread_cond_signal(&rec_cond);
    pthread_mutex_unlock(&rec_mutex);
  }

#if ENABLE_TCP_OUTPUT
  pthread_mutex_lock(&tcp_mutex);
  av_write_frame(tcp_ctx, &pkt);
  pthread_mutex_unlock(&tcp_mutex);
#endif

  pthread_mutex_lock(&mutex_writing);
  ret = hls_write_packet(hls, &pkt, 0);
  pthread_mutex_unlock(&mutex_writing);
  if (ret < 0) {
    fprintf(stderr, "P frame write error (hls): %d\n", ret);
    fprintf(stderr, "Check if the filesystem is not full\n");
  }

  free(buf);
  av_free_packet(&pkt);
  return ret;
}

/* methods for audio capturing */

// Callback function that is called when an error has occurred
static int xrun_recovery(snd_pcm_t *handle, int error) {
  switch(error) {
    case -EPIPE: // Buffer overrun
      fprintf(stderr, "microphone error: Buffer overrun\n");
      if ( (error = snd_pcm_prepare(handle)) < 0) {
        fprintf(stderr, "microphone error: Buffer overrrun cannot be recovered, "
            "snd_pcm_prepare failed: %s\n", snd_strerror(error));
      }
      return 0;
      break;

    case -ESTRPIPE: // Microphone is suspended
      fprintf(stderr, "microphone error: ESTRPIPE\n");
      // Wait until the suspend flag is cleared
      while ((error = snd_pcm_resume(handle)) == -EAGAIN) {
        sleep(1);
      }

      if (error < 0) {
        if ( (error = snd_pcm_prepare(handle)) < 0) {
          fprintf(stderr, "microphone: Suspend cannot be recovered, "
              "snd_pcm_prepare failed: %s\n", snd_strerror(error));
        }
      }
      return 0;
      break;

    case -EBADFD: // PCM descriptor is wrong
      fprintf(stderr, "microphone error: EBADFD\n");
      break;

    default:
      fprintf(stderr, "microphone error: unknown, error = %d\n",error);
      break;
  }
  return error;
}

// Wait for data using poll
static int wait_for_poll(snd_pcm_t *device, struct pollfd *ufds, unsigned int audio_fd_count) {
  unsigned short revents;
  int avail_flags = 0;
  int ret;

  while (1) {
    ret = poll(ufds, audio_fd_count, -1); // -1 means block, +1 for video
    if (ret < 0) {
      fprintf(stderr, "poll error: %d\n", ret);
      return ret;
    } else {
      snd_pcm_poll_descriptors_revents(device, ufds, audio_fd_count, &revents);
      if (revents & POLLERR) {
        return -EIO;
      }
      if (revents & POLLIN) { // Data is ready for read
        avail_flags |= AVAIL_AUDIO;
      }
      if (avail_flags) {
        return avail_flags;
      }
    }
  }
}

static int open_audio_capture_device() {
  int err;

  char *input_device = "hw:0,0";
  fprintf(stderr, "opening ALSA device: %s\n", input_device);
  if ((err = snd_pcm_open (&capture_handle, input_device, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
    fprintf (stderr, "cannot open audio device %s (%s)\n",
        input_device,
        snd_strerror (err));
    return -1;
  }

  return 0;
}

static int configure_audio_capture_device() {
  // ALSA
  int err;
  snd_pcm_hw_params_t *hw_params;

  // libavcodec
#if AUDIO_ONLY
  AVCodecContext *ctx = hls->format_ctx->streams[0]->codec;
#else
  AVCodecContext *ctx = hls->format_ctx->streams[1]->codec;
#endif
  int buffer_size;

  // alsa poll mmap
  snd_pcm_uframes_t real_buffer_size; // real buffer size in frames
  int dir;

  buffer_size = av_samples_get_buffer_size(NULL, ctx->channels,
      ctx->frame_size, ctx->sample_fmt, 0);

  if ((err = snd_pcm_hw_params_malloc (&hw_params)) < 0) {
    fprintf (stderr, "cannot allocate hardware parameter structure (%s)\n",
        snd_strerror (err));
    exit (1);
  }

  if ((err = snd_pcm_hw_params_any (capture_handle, hw_params)) < 0) {
    fprintf (stderr, "cannot initialize hardware parameter structure (%s)\n",
        snd_strerror (err));
    exit (1);
  }

  // use mmap
  if ((err = snd_pcm_hw_params_set_access (capture_handle, hw_params, SND_PCM_ACCESS_MMAP_INTERLEAVED)) < 0) {
    fprintf (stderr, "cannot set access type (%s)\n",
        snd_strerror (err));
    exit (1);
  }

  // SND_PCM_FORMAT_S16_LE => PCM 16 bit signed little endian
  if ((err = snd_pcm_hw_params_set_format (capture_handle, hw_params, SND_PCM_FORMAT_S16_LE)) < 0) {
    fprintf (stderr, "cannot set sample format (%s)\n",
        snd_strerror (err));
    exit (1);
  }

  // set the sample rate
  unsigned int rate = AUDIO_SAMPLE_RATE;
  if ((err = snd_pcm_hw_params_set_rate_near (capture_handle, hw_params, &rate, 0)) < 0) {
    fprintf (stderr, "cannot set sample rate (%s)\n",
        snd_strerror (err));
    exit (1);
  }

  unsigned int actual_rate;
  int actual_dir;
  if ((err = snd_pcm_hw_params_get_rate(hw_params, &actual_rate, &actual_dir)) < 0) {
    fprintf(stderr, "microphone: Failed to get rate (%s)\n", snd_strerror(err));
    exit(1);
  }
  fprintf(stderr, "actual rate=%u dir=%d\n", actual_rate, actual_dir);

  if ((err = snd_pcm_hw_params_get_rate_max(hw_params, &actual_rate, &actual_dir)) < 0) {
    fprintf(stderr, "microphone: Failed to get rate max (%s)\n", snd_strerror(err));
    exit(1);
  }
  fprintf(stderr, "max rate=%u dir=%d\n", actual_rate, actual_dir);

  if ((err = snd_pcm_hw_params_get_rate_min(hw_params, &actual_rate, &actual_dir)) < 0) {
    fprintf(stderr, "microphone: Failed to get rate min (%s)\n", snd_strerror(err));
    exit(1);
  }
  fprintf(stderr, "min rate=%u dir=%d\n", actual_rate, actual_dir);

  // set the number of channels
  if ((err = snd_pcm_hw_params_set_channels (capture_handle, hw_params, channels)) < 0) {
    fprintf (stderr, "cannot set channel count (%s)\n",
        snd_strerror (err));
    exit(1);
  }

  // set the buffer size
  if ( (err = snd_pcm_hw_params_set_buffer_size(capture_handle, hw_params, buffer_size * ALSA_BUFFER_MULTIPLY)) < 0) {
    fprintf (stderr, "microphone: Failed to set buffer size (%s)\n",
        snd_strerror (err));
    exit(1);
  }

  // check the value of the buffer size
  if ( (err = snd_pcm_hw_params_get_buffer_size(hw_params, &real_buffer_size)) < 0) {
    fprintf (stderr, "microphone: Failed to get buffer size (%s)\n",
        snd_strerror (err));
    exit(1);
  }
  fprintf (stderr, "microphone: Buffer size = %d [frames]\n", (int)real_buffer_size);

  dir = 0;
  // set the period size
  if ( (err = snd_pcm_hw_params_set_period_size_near(capture_handle, hw_params, (snd_pcm_uframes_t *)&period_size, &dir)) < 0) {
    fprintf (stderr, "microphone: Period size cannot be configured (%s)\n",
        snd_strerror (err));
    exit(1);
  }

  snd_pcm_uframes_t actual_period_size;
  if ( (err = snd_pcm_hw_params_get_period_size(hw_params, &actual_period_size, &dir)) < 0) {
    fprintf (stderr, "microphone: Period size cannot be configured (%s)\n",
        snd_strerror (err));
    exit(1);
  }
  fprintf(stderr, "actual_period_size: %lu\n", actual_period_size);

  // apply the hardware configuration
  if ((err = snd_pcm_hw_params (capture_handle, hw_params)) < 0) {
    fprintf (stderr, "cannot set parameters (%s)\n",
        snd_strerror (err));
    exit(1);
  }

  // end of configuration
  snd_pcm_hw_params_free (hw_params);

  if ((err = snd_pcm_prepare (capture_handle)) < 0) {
    fprintf (stderr, "cannot prepare audio interface for use (%s)\n",
        snd_strerror (err));
    exit (1);
  }

  audio_fd_count = snd_pcm_poll_descriptors_count (capture_handle);
  if (audio_fd_count <= 0) {
    fprintf(stderr,"microphone: Invalid poll descriptors count\n");
    return audio_fd_count;
  }
  ufds = malloc(sizeof(struct pollfd) * audio_fd_count); // +1 for video capture
  if (ufds == NULL) {
    fprintf(stderr, "Can't allocate ufds\n");
    exit(1);
  }
  // get poll descriptors
  if ((err = snd_pcm_poll_descriptors(capture_handle, ufds, audio_fd_count)) < 0) { // +1 for video
    fprintf(stderr,"microphone: Unable to obtain poll descriptors for capture: %s\n", snd_strerror(err));
    return err;
  }
  is_first_audio = 1; 

  return 0;
}

static void teardown_audio_encode() {
#if AUDIO_ONLY
  AVCodecContext *ctx = hls->format_ctx->streams[0]->codec;
#else
  AVCodecContext *ctx = hls->format_ctx->streams[1]->codec;
#endif
  int got_output, i, ret;
  AVPacket pkt;

  fprintf(stderr, "teardown_audio\n");

  // get the delayed frames
  fprintf(stderr, "waiting for the delayed frames\n");
  for (got_output = 1; got_output; i++) {
    av_init_packet(&pkt);
    pkt.data = NULL; // packet data will be allocated by the encoder
    pkt.size = 0;

    ret = avcodec_encode_audio2(ctx, &pkt, NULL, &got_output);
    if (ret < 0) {
      fprintf(stderr, "Error encoding frame\n");
      exit(1);
    }
    fprintf(stderr, "ret=%d got_output=%d\n", ret, got_output);
    av_free_packet(&pkt);
  }

  av_freep(&samples);
#if AUDIO_BUFFER_CHUNKS > 0
  for (i = 0; i < AUDIO_BUFFER_CHUNKS; i++) {
    av_freep(&audio_buffer[i]);
  }
#endif
  avcodec_free_frame(&av_frame);
}

static void teardown_audio_capture_device() {
  snd_pcm_close (capture_handle);

  free(ufds);
}

/* Return 1 if the difference is negative, otherwise 0.  */
static int timespec_subtract(struct timespec *result, struct timespec *t2, struct timespec *t1) {
  long long diff = (t2->tv_nsec + INT64_C(1000000000) * t2->tv_sec) - (t1->tv_nsec + INT64_C(1000000000) * t1->tv_sec);
  result->tv_sec = diff / 1000000000;
  result->tv_nsec = diff % 1000000000;

  return (diff<0);
}

void stopSignalHandler(int signo) {
  keepRunning = 0;
  fprintf(stderr, "[%d] stop requested\n", signo);
}

static void shutdown_video() {
  int i;
  for (i = 0; i < n_codec_configs; i++) {
    free(codec_configs[i]);
  }
}

static void shutdown_openmax() {
#if ENABLE_PREVIEW || ENABLE_CLOCK
  fprintf(stderr, "flush_tunnels\n");
  ilclient_flush_tunnels(tunnel, 0);
#endif

  fprintf(stderr, "disabling port buffers for 71...\n");
  ilclient_disable_port_buffers(camera_component, 71, NULL, NULL, NULL);
  fprintf(stderr, "disabling port buffers for 200...\n");
  ilclient_disable_port_buffers(video_encode, 200, NULL, NULL, NULL);
  fprintf(stderr, "disabling port buffers for 201...\n");
  ilclient_disable_port_buffers(video_encode, 201, NULL, NULL, NULL);

#if ENABLE_PREVIEW || ENABLE_CLOCK
  fprintf(stderr, "disable_tunnel\n");
  ilclient_disable_tunnel(tunnel);
  fprintf(stderr, "teardown_tunnels\n");
  ilclient_teardown_tunnels(tunnel);
#endif

  fprintf(stderr, "ilclient_state_transition to idle\n");
  ilclient_state_transition(component_list, OMX_StateIdle);
  fprintf(stderr, "ilclient_state_transition to loaded\n");
  ilclient_state_transition(component_list, OMX_StateLoaded);

  fprintf(stderr, "ilclient_cleanup_components\n");
  ilclient_cleanup_components(component_list);

  fprintf(stderr, "OMX_Deinit\n");
  OMX_Deinit();

  fprintf(stderr, "destroy cam_client\n");
  ilclient_destroy(cam_client);

  fprintf(stderr, "ilclient_destroy\n");
  ilclient_destroy(ilclient);
}

static void set_exposure_to_auto() {
  OMX_CONFIG_EXPOSURECONTROLTYPE exposure_type;
  OMX_ERRORTYPE error;

  memset(&exposure_type, 0, sizeof(OMX_CONFIG_EXPOSURECONTROLTYPE));
  exposure_type.nSize = sizeof(OMX_CONFIG_EXPOSURECONTROLTYPE);
  exposure_type.nVersion.nVersion = OMX_VERSION;
  exposure_type.nPortIndex = OMX_ALL;
  exposure_type.eExposureControl = OMX_ExposureControlAuto;

  fprintf(stderr, "set to auto exposure mode\n");
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigCommonExposure, &exposure_type);
  if (error != OMX_ErrorNone) {
    fprintf(stderr, "%s:%d: OMX_SetParameter() for camera_component port 71 "
        "exposure type failed with %x!\n",
        __FUNCTION__, __LINE__, error);
  }
  current_exposure_mode = EXPOSURE_AUTO;
}

#if USE_AUTO_EXPOSURE
static void set_exposure_to_night() {
  OMX_CONFIG_EXPOSURECONTROLTYPE exposure_type;
  OMX_ERRORTYPE error;

  memset(&exposure_type, 0, sizeof(OMX_CONFIG_EXPOSURECONTROLTYPE));
  exposure_type.nSize = sizeof(OMX_CONFIG_EXPOSURECONTROLTYPE);
  exposure_type.nVersion.nVersion = OMX_VERSION;
  exposure_type.nPortIndex = OMX_ALL;
  exposure_type.eExposureControl = OMX_ExposureControlNight;

  fprintf(stderr, "set to night exposure mode\n");
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigCommonExposure, &exposure_type);
  if (error != OMX_ErrorNone) {
    fprintf
      (stderr, "%s:%d: OMX_SetParameter() for camera_component port 71 exposure type failed with %x!\n",
       __FUNCTION__, __LINE__, error);
  }
  current_exposure_mode = EXPOSURE_NIGHT;
}

static void auto_select_exposure(int width, int height, uint8_t *data) {
  if (previous_previous_capture_frame == 0) {
    return;
  }
  const int width32 = ((width + 31) & ~31); // nearest smaller number that is multiple of 32
  const int height16 = ((height + 15) & ~15); // nearest smaller number that is multiple of 16
  int i = width32 * height16 / 4; // * (3 / 2) / 6
  uint8_t *py = data;
  int total_y = 0;
  int read_width = 0;
  int line_num = 1;
  int count = 0;
  while (i--) {
    total_y += *py++;
    count++;
    if (++read_width >= width) {
      if (width32 > width) {
        py += width32 - width;
      }
      read_width = 0;
      if (++line_num > height) {
        break;
      }
    }
  }
  if (count == 0) {
    return;
  }
  int average_y = total_y / count;

  int diff_frames = previous_capture_frame - previous_previous_capture_frame;
  if (diff_frames > 2) {
    diff_frames = 2;
  }
  if (diff_frames == 0) { // Avoid SIGFPE
    return;
  }
  if (current_exposure_mode == EXPOSURE_NIGHT) {
    average_y /= diff_frames;
  }
  fprintf(stderr, "y=%d(%d) ", average_y, diff_frames);
  if (average_y <= EXPOSURE_NIGHT_Y_THRESHOLD) { // dark
    if (current_exposure_mode == EXPOSURE_AUTO) {
      set_exposure_to_night();
    }
  } else if (average_y >= EXPOSURE_AUTO_Y_THRESHOLD) { // in the light
    if (current_exposure_mode == EXPOSURE_NIGHT) {
      set_exposure_to_auto();
    }
  }
}
#endif

static void cam_fill_buffer_done(void *data, COMPONENT_T *comp) {
  OMX_BUFFERHEADERTYPE *out;
  OMX_ERRORTYPE error;

  out = ilclient_get_output_buffer(camera_component, 71, 1);
  if (out != NULL) {
    if (out->nFilledLen > 0) {
      last_video_buffer = out->pBuffer;
      last_video_buffer_size = out->nFilledLen;
      if (out->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) {
        if (is_video_recording_started == 0) {
          is_video_recording_started = 1;
          if (is_audio_recording_started == 1) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            video_start_time = audio_start_time = ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
            send_audio_control_info();
            send_video_control_info();
            fprintf(stderr, "audio/video start_time (V): tv_sec=%lld tv_nsec=%lld\n", (long long)ts.tv_sec, (long long)ts.tv_nsec);
          }
        }

        if (is_audio_recording_started == 1) {
          if (video_pending_drop_frames > 0) {
            fprintf(stderr, "dV");
            video_pending_drop_frames--;
          } else {
            fprintf(stderr, ".");
            encode_and_send_image();
            previous_previous_capture_frame = previous_capture_frame;
            previous_capture_frame = video_frame_count;
          }
        } else {
          fprintf(stderr, "audio recording is not started yet\n");
        }
      } else {
        fprintf(stderr, "\nNot an end of a frame\n");
      }
    } else {
      fprintf(stderr, "Got zero bytes\n");
    }
  } else {
    fprintf(stderr, "out is NULL\n");
  }

  out->nFilledLen = 0;

  if (keepRunning) {
    error = OMX_FillThisBuffer(ILC_GET_HANDLE(camera_component), out);
    if (error != OMX_ErrorNone) {
      fprintf(stderr, "Error filling buffer (camera-2): %x\n", error);
    }
  } else {
    shutdown_openmax();
    shutdown_video();
    pthread_exit(0);
  }
}

static int openmax_cam_open() {
  OMX_PARAM_PORTDEFINITIONTYPE cam_def;
  OMX_CONFIG_FRAMERATETYPE framerate;
  OMX_ERRORTYPE error;
#if ENABLE_PREVIEW
  OMX_PARAM_PORTDEFINITIONTYPE portdef;
  int r;
#endif
  OMX_PARAM_TIMESTAMPMODETYPE timestamp_mode;

  if ((cam_client = ilclient_init()) == NULL) {
    fprintf(stderr, "ilclient_init returned NULL\n");
    return -3;
  }

  ilclient_set_fill_buffer_done_callback(cam_client, cam_fill_buffer_done, 0);

  // create camera_component
  error = ilclient_create_component(cam_client, &camera_component, "camera",
      ILCLIENT_DISABLE_ALL_PORTS |
      ILCLIENT_ENABLE_INPUT_BUFFERS |
      ILCLIENT_ENABLE_OUTPUT_BUFFERS);
  if (error != 0) {
    fprintf
      (stderr, "ilclient_create_component() for camera_component failed with %x!\n",
       error);
    exit(1);
  }
  component_list[n_component_list++] = camera_component;

  memset(&cam_def, 0, sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
  cam_def.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
  cam_def.nVersion.nVersion = OMX_VERSION;
  cam_def.nPortIndex = 71;

  if (OMX_GetParameter
      (ILC_GET_HANDLE(camera_component), OMX_IndexParamPortDefinition, &cam_def) != OMX_ErrorNone) {
    fprintf(stderr, "%s:%d: OMX_GetParameter() for camera_component port 71 port definition failed!\n",
        __FUNCTION__, __LINE__);
    exit(1);
  }

  // Port 71
  fprintf(stderr, "portdefinition 71\n");

  cam_def.format.video.nFrameWidth = WIDTH;
  cam_def.format.video.nFrameHeight = HEIGHT;

  // nStride must be a multiple of 32 and equal to or larger than nFrameWidth.
  cam_def.format.video.nStride = (WIDTH+31)&~31;

  // nSliceHeight must be a multiple of 16.
  cam_def.format.video.nSliceHeight = (HEIGHT+15)&~15;

  cam_def.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
  cam_def.format.video.xFramerate = fr_q16; // specify the frame rate in Q.16 (framerate * 2^16)

  // This specifies the input pixel format.
  // See http://www.khronos.org/files/openmax_il_spec_1_0.pdf for details.
  cam_def.format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
  cam_def.nBufferCountActual = N_BUFFER_COUNT_ACTUAL; // Affects to audio/video sync

  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexParamPortDefinition, &cam_def);
  if (error != OMX_ErrorNone) {
    fprintf
      (stderr, "%s:%d: OMX_SetParameter() for camera_component port 71 port definition failed with %x!\n",
       __FUNCTION__, __LINE__, error);
    exit(1);
  }

  fprintf(stderr, "framerate\n");
  memset(&framerate, 0, sizeof(OMX_CONFIG_FRAMERATETYPE));
  framerate.nSize = sizeof(OMX_CONFIG_FRAMERATETYPE);
  framerate.nVersion.nVersion = OMX_VERSION;
  framerate.nPortIndex = 71; // capture port
  framerate.xEncodeFramerate = (90000.0 / VIDEO_PTS_STEP) * 65536;
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigVideoFramerate, &framerate);
  if (error != OMX_ErrorNone) {
    fprintf
      (stderr, "%s:%d: OMX_SetParameter() for camera_component port 71 video framrate failed with %x!\n",
       __FUNCTION__, __LINE__, error);
    exit(1);
  }

  fprintf(stderr, "timestamp\n");
  memset(&timestamp_mode, 0, sizeof(OMX_PARAM_TIMESTAMPMODETYPE));
  timestamp_mode.nSize = sizeof(OMX_PARAM_TIMESTAMPMODETYPE);
  timestamp_mode.nVersion.nVersion = OMX_VERSION;
  timestamp_mode.eTimestampMode = OMX_TimestampModeRawStc;
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexParamCommonUseStcTimestamps, &timestamp_mode);
  if (error != OMX_ErrorNone) {
    fprintf
      (stderr, "%s:%d: OMX_SetParameter() for camera_component port 71 timestamp mode failed with %x!\n",
       __FUNCTION__, __LINE__, error);
    exit(1);
  }

  set_exposure_to_auto();

  fprintf(stderr, "capture to idle...\n");
  if (ilclient_change_component_state(camera_component, OMX_StateIdle) == -1) {
    fprintf
      (stderr, "%s:%d: ilclient_change_component_state(camera_component, OMX_StateIdle) failed\n",
       __FUNCTION__, __LINE__);
  }

#if ENABLE_CLOCK
  // create clock component
  error = ilclient_create_component(cam_client, &clock_component, "clock",
      ILCLIENT_DISABLE_ALL_PORTS);
  if (error != 0) {
    fprintf
      (stderr, "ilclient_create_component() for clock failed with %x!\n",
       error);
    exit(1);
  }
  component_list[n_component_list++] = clock_component;

  fprintf(stderr, "clock state\n");
  OMX_TIME_CONFIG_CLOCKSTATETYPE clock_state;
  memset(&clock_state, 0, sizeof(OMX_TIME_CONFIG_CLOCKSTATETYPE));
  clock_state.nSize = sizeof(OMX_TIME_CONFIG_CLOCKSTATETYPE);
  clock_state.nVersion.nVersion = OMX_VERSION;
  clock_state.eState = OMX_TIME_ClockStateWaitingForStartTime;
  clock_state.nWaitMask = 1;
  if(OMX_SetParameter(ILC_GET_HANDLE(clock_component), OMX_IndexConfigTimeClockState, &clock_state) != OMX_ErrorNone) {
    fprintf(stderr, "set parameter for clock (clock state) failed\n");
  }

  set_tunnel(tunnel+n_tunnel, clock_component, 80, camera_component, 73);

  if (ilclient_setup_tunnel(tunnel+(n_tunnel++), 0, 0) != 0) {
    fprintf(stderr, "ilclient_setup_tunnel error\n");
    exit(1);
  }
#endif

#if ENABLE_PREVIEW
  // Preview port
  fprintf(stderr, "portdefinition 70\n");
  memset(&portdef, 0, sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
  portdef.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
  portdef.nVersion.nVersion = OMX_VERSION;
  portdef.nPortIndex = 70;

  if (OMX_GetParameter
      (ILC_GET_HANDLE(camera_component), OMX_IndexParamPortDefinition, &portdef) != OMX_ErrorNone) {
    fprintf(stderr, "%s:%d: OMX_GetParameter() for camera_component port 70 failed!\n",
        __FUNCTION__, __LINE__);
    exit(1);
  }

  portdef.format.video.nFrameWidth = WIDTH;
  portdef.format.video.nFrameHeight = HEIGHT;
  portdef.format.video.nStride = (WIDTH+31)&~31;
  portdef.format.video.nSliceHeight = (HEIGHT+15)&~15;
  portdef.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
  portdef.format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;

  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexParamPortDefinition, &portdef);
  if (error != OMX_ErrorNone) {
    fprintf
      (stderr, "%s:%d: OMX_SetParameter() for camera_component port 70 failed with %x!\n",
       __FUNCTION__, __LINE__, error);
    exit(1);
  }

  fprintf(stderr, "framerate\n");
  memset(&framerate, 0, sizeof(OMX_CONFIG_FRAMERATETYPE));
  framerate.nSize = sizeof(OMX_CONFIG_FRAMERATETYPE);
  framerate.nVersion.nVersion = OMX_VERSION;
  framerate.nPortIndex = 70; // preview port
  framerate.xEncodeFramerate = fr_q16;
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigVideoFramerate, &framerate);
  if (error != OMX_ErrorNone) {
    fprintf
      (stderr, "%s:%d: OMX_SetParameter() for camera_component port 70 failed with %x!\n",
       __FUNCTION__, __LINE__, error);
    exit(1);
  }

  // create render_component
  r = ilclient_create_component(cam_client, &render_component, "video_render",
      ILCLIENT_DISABLE_ALL_PORTS);
  if (r != 0) {
    fprintf
      (stderr, "ilclient_create_component() for render_component failed with %x!\n",
       r);
    exit(1);
  }
  component_list[n_component_list++] = render_component;

  set_tunnel(tunnel+n_tunnel, camera_component, 70, render_component, 90);

  if (ilclient_setup_tunnel(tunnel+(n_tunnel++), 0, 0) != 0) {
    fprintf(stderr, "ilclient_setup_tunnel error\n");
    exit(1);
  }

#endif

#if ENABLE_PREVIEW
  fprintf(stderr, "render to executing...\n");
  ilclient_change_component_state(render_component, OMX_StateExecuting);
#endif

#if ENABLE_CLOCK
  fprintf(stderr, "clock to executing...\n");
  ilclient_change_component_state(clock_component, OMX_StateExecuting);
#endif

  return 0;
}

// This function is called after the video encoder produces each frame
static int video_encode_fill_buffer_done(OMX_BUFFERHEADERTYPE *out) {
  int nal_unit_type;
  uint8_t *buf;
  int buf_len;
  int is_endofnal = 1;
  uint8_t *concat_buf = NULL;
  struct timespec tsEnd, tsDiff;

  // out->nTimeStamp is useless as the value is always zero

  if (out == NULL) {
    fprintf(stderr, "FATAL: Not getting it :(\n");
    return 0;
  }
  if (encbuf != NULL) {
    fprintf(stderr, "m(%d,%d)", out->nFlags, out->nFilledLen + encbuf_size);
    concat_buf = realloc(encbuf, encbuf_size + out->nFilledLen);
    if (concat_buf == NULL) {
      fprintf(stderr, "Can't allocate concat_buf; size=%d\n", encbuf_size + out->nFilledLen);
      free(encbuf);
      exit(1);
    }
    memcpy(concat_buf + encbuf_size, out->pBuffer, out->nFilledLen);
    buf = concat_buf;
    buf_len = encbuf_size + out->nFilledLen;
  } else {
    buf = out->pBuffer;
    buf_len = out->nFilledLen;
  }
  if (!(out->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) && !(out->nFlags & OMX_BUFFERFLAG_CODECCONFIG)) { // incomplete buffer
    nal_unit_type = buf[4] & 0x1f;
    fprintf(stderr, "~(%d,%d,%d,%d)", out->nFlags, nal_unit_type, out->nFilledLen, buf_len);
    encbuf_size = buf_len;
    if (concat_buf != NULL) {
      encbuf = concat_buf;
      concat_buf = NULL;
    } else {
      encbuf = malloc(buf_len);
      if (encbuf == NULL) {
        fprintf(stderr, "Can't allocate encbuf; size=%d\n", buf_len);
        exit(1);
      }
      memcpy(encbuf, buf, buf_len);
    }
    is_endofnal = 0;
  } else {
    encbuf = NULL;
    encbuf_size = -1;

    nal_unit_type = buf[4] & 0x1f;
    if (nal_unit_type != 1 && nal_unit_type != 5) {
      fprintf(stderr, "%d", nal_unit_type);
    }
    if (out->nFlags != 0x480 && out->nFlags != 0x490 &&
        out->nFlags != 0x430 && out->nFlags != 0x410 &&
        out->nFlags != 0x400 && out->nFlags != 0x510 &&
        out->nFlags != 0x530) {
      fprintf(stderr, "\nNew flag (%d,nal=%d)\n", out->nFlags, nal_unit_type);
    }
    if (out->nFlags & OMX_BUFFERFLAG_DATACORRUPT) {
      fprintf(stderr, "\n=== OMX_BUFFERFLAG_DATACORRUPT ===\n");
    }
    if (out->nFlags & OMX_BUFFERFLAG_EXTRADATA) {
      fprintf(stderr, "\n=== OMX_BUFFERFLAG_EXTRADATA ===\n");
    }
    if (out->nFlags & OMX_BUFFERFLAG_FRAGMENTLIST) {
      fprintf(stderr, "\n=== OMX_BUFFERFLAG_FRAGMENTLIST ===\n");
    }
    if (out->nFlags & OMX_BUFFERFLAG_DISCONTINUITY) {
      fprintf(stderr, "\n=== OMX_BUFFERFLAG_DISCONTINUITY ===\n");
    }
    if (out->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
      // Insert timing info to nal_unit_type 7
      codec_configs[n_codec_configs] = malloc(buf_len);
      if (codec_configs[n_codec_configs] == NULL) {
        fprintf(stderr, "Fatal: Can't allocate memory for codec config; size=%d\n", buf_len);
        exit(1);
      }
      codec_config_sizes[n_codec_configs] = buf_len;
      memcpy(codec_configs[n_codec_configs], buf, buf_len);

      codec_config_total_size += codec_config_sizes[n_codec_configs];
      n_codec_configs++;

      send_video_frame(buf, buf_len, 0);
    } else { // video frame
      frame_count++; // will be used for printing stats about FPS, etc.

      if (out->nFlags & OMX_BUFFERFLAG_SYNCFRAME) { // keyframe
        // TODO: Send out nal_unit_type 6 (SEI) before the first I frame
        if (nal_unit_type != 5) {
          fprintf(stderr, "SYNCFRAME nal_unit_type=%d len=%d\n", nal_unit_type, buf_len);
        }
        int consume_time = 0;
        if (nal_unit_type == 1 || nal_unit_type == 2 ||
            nal_unit_type == 3 || nal_unit_type == 4 ||
            nal_unit_type == 5) {
          consume_time = 1;
        } else {
          fprintf(stderr, "(nosl)");
        }
#if !(AUDIO_ONLY)
        send_keyframe(buf, buf_len, consume_time);
#endif

        // calculate FPS and display it
        if (tsBegin.tv_sec != 0 && tsBegin.tv_nsec != 0) {
          clock_gettime(CLOCK_MONOTONIC, &tsEnd);
          timespec_subtract(&tsDiff, &tsEnd, &tsBegin);
          unsigned long long wait_nsec = tsDiff.tv_sec * INT64_C(1000000000) + tsDiff.tv_nsec;
          float divisor = (float)wait_nsec / (float)frame_count / 1000000000;
          float fps;
          if (divisor == 0.0) { // This won't cause SIGFPE because of float, but just to be safe.
            fps = 99999.0;
          } else {
            fps = 1 / divisor;
          }
          keyframes_count++;
          fprintf(stderr, " v=%d a=%d (%5.2f fps) k=%d",
              frame_count, current_audio_frames, fps, keyframes_count);
          print_audio_timing();
          current_audio_frames = 0;
          frame_count = 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &tsBegin);
      } else { // inter frame
        if (nal_unit_type != 9) { // Exclude nal_unit_type 9 (Access unit delimiter)
          int consume_time = 0;
          if (nal_unit_type == 1 || nal_unit_type == 2 ||
              nal_unit_type == 3 || nal_unit_type == 4 ||
              nal_unit_type == 5) {
            consume_time = 1;
          } else {
            fprintf(stderr, "(nosl)");
          }
#if !(AUDIO_ONLY)
          send_pframe(buf, buf_len, consume_time);
#endif
        }
      }
    }
  }
  if (concat_buf != NULL) {
    free(concat_buf);
  }

  return is_endofnal;
}

static int video_encode_startup() {
  OMX_VIDEO_PARAM_PORTFORMATTYPE format;
  OMX_VIDEO_PARAM_AVCTYPE avctype;
  OMX_VIDEO_PARAM_BITRATETYPE bitrate_type;
  OMX_CONFIG_BOOLEANTYPE boolean_type;
  OMX_PARAM_PORTDEFINITIONTYPE portdef, portdef_201;
  OMX_ERRORTYPE error;
  int r;

  if ((ilclient = ilclient_init()) == NULL) {
    fprintf(stderr, "ilclient_init returned NULL\n");
    return -3;
  }

  // create video_encode
  r = ilclient_create_component(ilclient, &video_encode, "video_encode",
      ILCLIENT_DISABLE_ALL_PORTS |
      ILCLIENT_ENABLE_INPUT_BUFFERS |
      ILCLIENT_ENABLE_OUTPUT_BUFFERS);
  if (r != 0) {
    fprintf
      (stderr, "ilclient_create_component() for video_encode failed with %x!\n",
       r);
    exit(1);
  }
  component_list[n_component_list++] = video_encode;

  memset(&portdef, 0, sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
  portdef.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
  portdef.nVersion.nVersion = OMX_VERSION;
  portdef.nPortIndex = 200;

  if (OMX_GetParameter
      (ILC_GET_HANDLE(video_encode), OMX_IndexParamPortDefinition,
       &portdef) != OMX_ErrorNone) {
    fprintf(stderr, "%s:%d: OMX_GetParameter() for video_encode port 200 failed!\n",
        __FUNCTION__, __LINE__);
    exit(1);
  }

  // Port 200
  portdef.format.video.nFrameWidth = WIDTH;
  portdef.format.video.nFrameHeight = HEIGHT;
  portdef.format.video.xFramerate = fr_q16; // specify the frame rate in Q.16 (framerate * 2^16)
  portdef.format.video.nBitrate = 0x0;
  portdef.format.video.nSliceHeight = portdef.format.video.nFrameHeight;
  portdef.format.video.nStride = portdef.format.video.nFrameWidth;
  portdef.nBufferCountActual = N_BUFFER_COUNT_ACTUAL;

  // This specifies the input pixel format.
  // See http://www.khronos.org/files/openmax_il_spec_1_0.pdf for details.
  portdef.format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;

  fprintf(stderr, "portdefinition\n");
  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamPortDefinition, &portdef);
  if (error != OMX_ErrorNone) {
    fprintf
      (stderr, "%s:%d: OMX_SetParameter() for video_encode port 200 failed with %x!\n",
       __FUNCTION__, __LINE__, error);
    exit(1);
  }

  memset(&portdef_201, 0, sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
  portdef_201.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
  portdef_201.nVersion.nVersion = OMX_VERSION;
  portdef_201.nPortIndex = 201;

  if (OMX_GetParameter
      (ILC_GET_HANDLE(video_encode), OMX_IndexParamPortDefinition,
       &portdef_201) != OMX_ErrorNone) {
    fprintf(stderr, "%s:%d: OMX_GetParameter() for video_encode port 200 failed!\n",
        __FUNCTION__, __LINE__);
    exit(1);
  }

  // Port 201
  portdef_201.nBufferCountActual = N_BUFFER_COUNT_ACTUAL;

  fprintf(stderr, "portdefinition 201\n");
  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamPortDefinition, &portdef_201);
  if (error != OMX_ErrorNone) {
    fprintf
      (stderr, "%s:%d: OMX_SetParameter() for video_encode port 200 failed with %x!\n",
       __FUNCTION__, __LINE__, error);
    exit(1);
  }

  fprintf(stderr, "portformat\n");
  memset(&format, 0, sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
  format.nSize = sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE);
  format.nVersion.nVersion = OMX_VERSION;
  format.nPortIndex = 201;
  format.eCompressionFormat = OMX_VIDEO_CodingAVC;

  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamVideoPortFormat, &format);
  if (error != OMX_ErrorNone) {
    fprintf
      (stderr, "%s:%d: OMX_SetParameter() for video_encode port 201 port format failed with %x!\n",
       __FUNCTION__, __LINE__, error);
    exit(1);
  }

  // Set the profile to Baseline 3.0
  memset(&avctype, 0, sizeof(OMX_VIDEO_PARAM_AVCTYPE));
  avctype.nSize = sizeof(OMX_VIDEO_PARAM_AVCTYPE);
  avctype.nVersion.nVersion = OMX_VERSION;
  avctype.nPortIndex = 201;

  if (OMX_GetParameter
      (ILC_GET_HANDLE(video_encode), OMX_IndexParamVideoAvc, &avctype) != OMX_ErrorNone) {
    fprintf(stderr, "%s:%d: OMX_GetParameter() for video_encode port 201 avctype failed!\n",
        __FUNCTION__, __LINE__);
    exit(1);
  }

  avctype.nPFrames = GOP_SIZE - 1;
  avctype.nBFrames = 0;

  avctype.eProfile = OMX_VIDEO_AVCProfileConstrainedBaseline; // Main profile is not playable on Android
  avctype.eLevel = OMX_VIDEO_AVCLevel31; // Level 4.1 is not supported on Raspberry Pi
  // Level 3.1 allows up to 1280x720 @ 30.0 FPS (720p)

  avctype.nAllowedPictureTypes = OMX_VIDEO_PictureTypeI | OMX_VIDEO_PictureTypeP;
  avctype.bUseHadamard = OMX_TRUE; // not sure
//  avctype.nRefFrames = 1;          // not sure
  avctype.bEnableFMO = OMX_FALSE;  // not allowed in CBP (Constrained Baseline profile)
  avctype.bEnableASO = OMX_FALSE;  // not allowed in CBP
  avctype.bEnableRS = OMX_FALSE;   // not allowed in CBP
  // See A.2.1 in H.264 Annex A
  // See also https://en.wikipedia.org/wiki/H.264/MPEG-4_AVC
  avctype.bWeightedPPrediction = OMX_FALSE; // FALSE is required by BP
  avctype.bconstIpred = OMX_FALSE;          // not sure
  avctype.bFrameMBsOnly = OMX_TRUE;         // TRUE is required by BP
  avctype.bEntropyCodingCABAC = OMX_FALSE;  // FALSE is required by BP
//  avctype.nWeightedBipredicitonMode = 0;    // not sure

  fprintf(stderr, "videoavc\n");
  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamVideoAvc, &avctype);
  if (error != OMX_ErrorNone) {
    fprintf
      (stderr, "%s:%d: OMX_SetParameter() for video_encode port 201 video avc failed with %x!\n",
       __FUNCTION__, __LINE__, error);
    exit(1);
  }

  // Set the bitrate
  memset(&bitrate_type, 0, sizeof(OMX_VIDEO_PARAM_BITRATETYPE));
  bitrate_type.nSize = sizeof(OMX_VIDEO_PARAM_BITRATETYPE);
  bitrate_type.nVersion.nVersion = OMX_VERSION;
  bitrate_type.nPortIndex = 201;
  bitrate_type.eControlRate = OMX_Video_ControlRateVariable; // TODO: Is this OK?
  bitrate_type.nTargetBitrate = H264_BIT_RATE; // in bits per second

  fprintf(stderr, "bitrate\n");
  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamVideoBitrate, &bitrate_type);
  if (error != OMX_ErrorNone) {
    fprintf
      (stderr, "%s:%d: OMX_SetParameter() for video_encode port 201 bitrate failed with %x!\n",
       __FUNCTION__, __LINE__, error);
    exit(1);
  }

  // Separate buffers for each NAL unit
  memset(&boolean_type, 0, sizeof(OMX_CONFIG_BOOLEANTYPE));
  boolean_type.nSize = sizeof(OMX_CONFIG_BOOLEANTYPE);
  boolean_type.nVersion.nVersion = OMX_VERSION;
  boolean_type.bEnabled = 1;

  fprintf(stderr, "nalseparate\n");
  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamBrcmNALSSeparate, &boolean_type);
  if (error != OMX_ErrorNone) {
    fprintf
      (stderr, "%s:%d: OMX_SetParameter() for video_encode port 201 nal separate failed with %x!\n",
       __FUNCTION__, __LINE__, error);
    exit(1);
  }

  fprintf(stderr, "encode to idle...\n");
  if (ilclient_change_component_state(video_encode, OMX_StateIdle) == -1) {
    fprintf
      (stderr, "%s:%d: ilclient_change_component_state(video_encode, OMX_StateIdle) failed\n",
       __FUNCTION__, __LINE__);
  }

  fprintf(stderr, "enabling port buffers for 71...\n");
  if (ilclient_enable_port_buffers(camera_component, 71, NULL, NULL, NULL) != 0) {
    fprintf(stderr, "enabling port buffers for 71 failed!\n");
    exit(1);
  }

  fprintf(stderr, "enabling port buffers for 200...\n");
  if (ilclient_enable_port_buffers(video_encode, 200, NULL, NULL, NULL) != 0) {
    fprintf(stderr, "enabling port buffers for 200 failed!\n");
    exit(1);
  }

  fprintf(stderr, "enabling port buffers for 201...\n");
  if (ilclient_enable_port_buffers(video_encode, 201, NULL, NULL, NULL) != 0) {
    fprintf(stderr, "enabling port buffers for 201 failed!\n");
    exit(1);
  }

  fprintf(stderr, "camera to executing...\n");
  ilclient_change_component_state(camera_component, OMX_StateExecuting);

  fprintf(stderr, "encode to executing...\n");
  ilclient_change_component_state(video_encode, OMX_StateExecuting);

  return 0;
}

static void encode_and_send_image() {
  OMX_BUFFERHEADERTYPE *buf;
  OMX_BUFFERHEADERTYPE *out;
  OMX_ERRORTYPE error;

  buf = ilclient_get_input_buffer(video_encode, 200, 1);
  if (buf == NULL) {
    fprintf(stderr, "Doh, no buffers for me!\n");
    exit(1);
  }

  buf->pBuffer = last_video_buffer;
  buf->nFilledLen = last_video_buffer_size;

  // OMX_EmptyThisBuffer takes 22000-27000 usec at 1920x1080
  error = OMX_EmptyThisBuffer(ILC_GET_HANDLE(video_encode), buf);
  if (error != OMX_ErrorNone) {
    fprintf(stderr, "Error emptying buffer: %x\n", error);
  }

  out = ilclient_get_output_buffer(video_encode, 201, 1);

  while (1) {
    error = OMX_FillThisBuffer(ILC_GET_HANDLE(video_encode), out);
    if (error != OMX_ErrorNone) {
      fprintf(stderr, "Error filling buffer (video_encode-4): %x\n", error);
    }

    if (out->nFilledLen > 0) {
      video_encode_fill_buffer_done(out);
    } else {
      fprintf(stderr, "E(%d)", out->nFlags);
      break;
    }

    if (out->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) {
      break;
    } else {
      fprintf(stderr, "T(%d)", out->nFlags);
    }
  }

#if USE_AUTO_EXPOSURE
  if (video_frame_count > 0 && video_frame_count % TARGET_FPS == 0) {
    auto_select_exposure(WIDTH, HEIGHT, last_video_buffer);
  }
#endif
}

static void encode_and_send_audio() {
  AVPacket pkt;
  int ret, got_output;
  int64_t pts;
#if AUDIO_ONLY
  AVCodecContext *ctx = hls->format_ctx->streams[0]->codec;
#else
  AVCodecContext *ctx = hls->format_ctx->streams[1]->codec;
#endif

  av_init_packet(&pkt);
  pkt.data = NULL; // packet data will be allocated by the encoder
  pkt.size = 0;

  // encode the samples
  ret = avcodec_encode_audio2(ctx, &pkt, av_frame, &got_output);
  if (ret < 0) {
    fprintf(stderr, "Error encoding audio frame\n");
    exit(1);
  }
  if (got_output) {
#if AUDIO_ONLY
    pkt.stream_index = hls->format_ctx->streams[0]->index; // This must be done after avcodec_encode_audio2
#else
    pkt.stream_index = hls->format_ctx->streams[1]->index; // This must be done after avcodec_encode_audio2
#endif // AUDIO_ONLY

    pts = get_next_audio_pts();

    send_audio_frame(pkt.data, pkt.size, pts);

    pts = pts % PTS_MODULO;
    pkt.pts = pkt.dts = pts;

    // We have to copy AVPacket before av_write_frame()
    // because it changes internal data of the AVPacket.
    // Otherwise av_write_frame() will fail with the error:
    // "AAC bitstream not in ADTS format and extradata missing".
    uint8_t *copied_data = av_malloc(pkt.size);
    memcpy(copied_data, pkt.data, pkt.size);
    pthread_mutex_lock(&rec_write_mutex);
    add_encoded_packet(pts, copied_data, pkt.size, pkt.stream_index, pkt.flags);
    pthread_mutex_unlock(&rec_write_mutex);

    if (is_recording) {
      pthread_mutex_lock(&rec_mutex);
      rec_thread_needs_write = 1;
      pthread_cond_signal(&rec_cond);
      pthread_mutex_unlock(&rec_mutex);
    }

#if ENABLE_TCP_OUTPUT
    // Setup AVPacket
    AVPacket tcp_pkt;
    av_init_packet(&tcp_pkt);
    tcp_pkt.size = pkt.size;
    tcp_pkt.data = av_malloc(pkt.size);
    memcpy(tcp_pkt.data, pkt.data, pkt.size);
    tcp_pkt.stream_index = pkt.stream_index;
    tcp_pkt.pts = tcp_pkt.dts = pkt.pts;

    // Send the AVPacket
    pthread_mutex_lock(&tcp_mutex);
    av_write_frame(tcp_ctx, &tcp_pkt);
    pthread_mutex_unlock(&tcp_mutex);

    av_free_packet(&tcp_pkt);
#endif

    pthread_mutex_lock(&mutex_writing);
    ret = hls_write_packet(hls, &pkt, 0);
    pthread_mutex_unlock(&mutex_writing);
    if (ret < 0) {
      fprintf(stderr, "audio frame write error (hls): %d\n", ret);
      fprintf(stderr, "Check if the filesystem is not full\n");
    }
    av_free_packet(&pkt);

    current_audio_frames++;
  } else {
    fprintf(stderr, "Not getting audio output");
  }
}

static int read_audio_poll_mmap() {
  const snd_pcm_channel_area_t *my_areas; // mapped memory area info
  snd_pcm_sframes_t avail, commitres; // aux for frames count
  snd_pcm_uframes_t offset, frames, size; //aux for frames count
  int error;
  uint16_t *this_samples;

#if AUDIO_BUFFER_CHUNKS > 0
  this_samples = audio_buffer[audio_buffer_index];
#else
  this_samples = samples;
#endif

  // check how many frames are ready to read or write
  avail = snd_pcm_avail_update(capture_handle);
  if (avail < 0) {
    if ( (error = xrun_recovery(capture_handle, avail)) < 0) {
      fprintf(stderr,"microphone: SUSPEND recovery failed: %s\n", snd_strerror(error));
      exit(1);
    }
    is_first_audio = 1;
    return error;
  }   
  if (avail < period_size) { // check if one period is ready to process
    switch (is_first_audio) 
    {
      case 1:
        // if the capture from PCM is started (is_first_audio=1) and one period is ready to process,
        // the stream must start 
        is_first_audio = 0;
        fprintf(stderr, "S");
        if ( (error = snd_pcm_start(capture_handle)) < 0) {
          fprintf(stderr,"microphone: Start error: %s\n", snd_strerror(error));
          exit(EXIT_FAILURE);
        }
        break;

      case 0:
        fprintf(stderr, "0");
        // wait for pcm to become ready
        if ( (error = snd_pcm_wait(capture_handle, -1)) < 0) {
          if ((error = xrun_recovery(capture_handle, error)) < 0) {
            fprintf(stderr,"microphone: snd_pcm_wait error: %s\n", snd_strerror(error));
            exit(EXIT_FAILURE);
          }
          is_first_audio = 1;
        }
    } 
    return -1;
  }
  int read_size = 0;
  size = period_size;
  while (size > 0) { // wait until we have period_size frames (in the most cases only one loop is needed)
    frames = size; // expected number of frames to be processed
    // frames is a bidirectional variable, this means that the real number of frames processed is written 
    // to this variable by the function.
    if ((error = snd_pcm_mmap_begin (capture_handle, &my_areas, &offset, &frames)) < 0) {
      if ((error = xrun_recovery(capture_handle, error)) < 0) {
        fprintf(stderr,"microphone: MMAP begin avail error: %s\n", snd_strerror(error));
        exit(EXIT_FAILURE);
      }
      is_first_audio = 1;
    } 
    memcpy(this_samples + read_size, (my_areas[0].addr)+(offset*sizeof(short)*channels), frames*sizeof(short)*channels);
    read_size += frames;

    commitres = snd_pcm_mmap_commit(capture_handle, offset, frames);
    if (commitres < 0 || (snd_pcm_uframes_t)commitres != frames) {
      if ((error = xrun_recovery(capture_handle, commitres >= 0 ? commitres : -EPIPE)) < 0) {
        fprintf(stderr,"microphone: MMAP commit error: %s\n", snd_strerror(error));
        exit(EXIT_FAILURE);
      }
      is_first_audio = 1;
    }
    size -= frames; // needed in the condition of the while loop to check if period is filled
  }

#if ENABLE_AUDIO_AMPLIFICATION
  int total_samples = period_size * channels;
  int i;
  for (i = 0; i < total_samples; i++) {
    int16_t value = (int16_t)this_samples[i];
    if (value < AUDIO_MIN_VALUE) { // negative overflow of audio volume
      fprintf(stderr, "o-");
      value = -32768;
    } else if (value > AUDIO_MAX_VALUE) { // positive overflow audio volume
      fprintf(stderr, "o+");
      value = 32767;
    } else {
      value = (int16_t)(value * AUDIO_VOLUME_MULTIPLY);
    }
    this_samples[i] = (uint16_t)value;
  }
#endif

#if AUDIO_BUFFER_CHUNKS > 0
  if (++audio_buffer_index == AUDIO_BUFFER_CHUNKS) {
    audio_buffer_index = 0;
    if (!is_audio_buffer_filled) {
      fprintf(stderr, "audio buffer filled\n");
      is_audio_buffer_filled = 1;
    }
  }
#endif

  return 0;
}

#if ENABLE_CLOCK
static void start_openmax_clock() {
  OMX_TIME_CONFIG_CLOCKSTATETYPE clock_state;
  OMX_ERRORTYPE error;

  memset(&clock_state, 0, sizeof(OMX_TIME_CONFIG_CLOCKSTATETYPE));
  clock_state.nSize = sizeof(OMX_TIME_CONFIG_CLOCKSTATETYPE);
  clock_state.nVersion.nVersion = OMX_VERSION;
  clock_state.eState = OMX_TIME_ClockStateRunning;

  error = OMX_SetParameter(ILC_GET_HANDLE(clock_component),
      OMX_IndexConfigTimeClockState, &clock_state);
  if (error != OMX_ErrorNone) {
    fprintf
      (stderr, "%s:%d: OMX_SetParameter() for clock state failed with %x!\n",
       __FUNCTION__, __LINE__, error);
    exit(1);
  }
}
#endif

static void start_openmax_capturing() {
  OMX_CONFIG_PORTBOOLEANTYPE boolean;
  OMX_ERRORTYPE error;

  memset(&boolean, 0, sizeof(OMX_CONFIG_PORTBOOLEANTYPE));
  boolean.nSize = sizeof(OMX_CONFIG_PORTBOOLEANTYPE);
  boolean.nVersion.nVersion = OMX_VERSION;
  boolean.nPortIndex = 71;
  boolean.bEnabled = 1;

  fprintf(stderr, "start capturing\n");
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigPortCapturing, &boolean);
  if (error != OMX_ErrorNone) {
    fprintf
      (stderr, "%s:%d: OMX_SetParameter() for camera_component port 71 port capturing failed with %x!\n",
       __FUNCTION__, __LINE__, error);
    exit(1);
  }

#if ENABLE_CLOCK
  fprintf(stderr, "start_openmax_clock\n");
  start_openmax_clock();
#endif
}

static void openmax_cam_loop() {
  OMX_BUFFERHEADERTYPE *out;
  OMX_ERRORTYPE error;

  start_openmax_capturing();

  fprintf(stderr, "ilclient_get_output_buffer\n");
  out = ilclient_get_output_buffer(camera_component, 71, 1);

  fprintf(stderr, "FillThisBuffer\n");
  error = OMX_FillThisBuffer(ILC_GET_HANDLE(camera_component), out);
  if (error != OMX_ErrorNone) {
    fprintf(stderr, "Error filling buffer (camera-1): %x\n", error);
  }

  fprintf(stderr, "end of openmax_cam_loop\n");
}

void *video_thread_loop() {
  openmax_cam_loop();
  return 0;
}

static void *audio_nop_loop() {
  struct timespec ts;
  int ret;

  while (keepRunning) {
    if (is_video_recording_started) {
      encode_and_send_audio();
      clock_gettime(CLOCK_MONOTONIC, &ts);
      int64_t diff_time = get_next_audio_write_time() - (ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec);
      if (diff_time > 0) {
        ts.tv_sec = diff_time / 1000000000;
        diff_time -= ts.tv_sec * 1000000000;
        ts.tv_nsec = diff_time;
        ret = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
        if (ret != 0) {
          fprintf(stderr, "nanosleep error:%d\n", ret);
        }
      }
    } else {
      // sleep 100ms
      ts.tv_sec = 0;
      ts.tv_nsec = 100000000;
      ret = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
      if (ret != 0) {
        fprintf(stderr, "nanosleep error:%d\n", ret);
      }
    }
  }
  pthread_exit(0);
}

static void audio_loop_poll_mmap() {
  int avail_flags;

  while (keepRunning) {
    if (is_first_audio) {
      fprintf(stderr, "is_first_audio\n");
      read_audio_poll_mmap();
      // We ignore the first audio frame.
      // Because there is always a big delay after
      // the first and the second frame.
    }

    avail_flags = wait_for_poll(capture_handle, ufds, audio_fd_count);
    if (avail_flags < 0) { // try to recover from error
      fprintf(stderr, "trying to recover from error\n");
      if (snd_pcm_state(capture_handle) == SND_PCM_STATE_XRUN ||
          snd_pcm_state(capture_handle) == SND_PCM_STATE_SUSPENDED) {
        avail_flags = snd_pcm_state(capture_handle) == SND_PCM_STATE_XRUN ? -EPIPE : -ESTRPIPE;
        if (xrun_recovery(capture_handle, avail_flags) < 0) {
          fprintf(stderr,"microphone: Write error: %s\n", snd_strerror(avail_flags));
          exit(EXIT_FAILURE);
        }
        is_first_audio = 1;
      } else {
        fprintf(stderr,"microphone: Wait for poll failed\n");
        continue;
      }
    }

    if (avail_flags & AVAIL_AUDIO) {
      read_audio_poll_mmap();
#if AUDIO_BUFFER_CHUNKS > 0
      if (AUDIO_BUFFER_CHUNKS == 0 || is_audio_buffer_filled) {
        memcpy(samples, audio_buffer[audio_buffer_index], period_size * sizeof(short) * channels);
#else
      if (1) {
#endif
        if (is_audio_recording_started == 0) {
          is_audio_recording_started = 1;
          if (is_video_recording_started == 1) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            video_start_time = audio_start_time = ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
            send_audio_control_info();
            send_video_control_info();
            fprintf(stderr, "audio/video start_time (A): tv_sec=%lld tv_nsec=%lld\n", (long long)ts.tv_sec, (long long)ts.tv_nsec);
          }
        }
        if (is_video_recording_started == 1) {
          if (audio_pending_drop_frames > 0) {
            fprintf(stderr, "dA");
            audio_pending_drop_frames--;
          } else {
            if (is_audio_muted) {
              memset(samples, 0, period_size * sizeof(short) * channels);
            }
            encode_and_send_audio();
          }
        } else {
          fprintf(stderr, "video recording is not started yet\n");
        }
      }
    }
  } // end of while loop (keepRunning)
}

#if ENABLE_TCP_OUTPUT
static void setup_tcp_output() {
  avformat_network_init();
  tcp_ctx = mpegts_create_context(&codec_settings);
  mpegts_open_stream(tcp_ctx, TCP_OUTPUT_DEST, 0);
}

static void teardown_tcp_output() {
  mpegts_close_stream(tcp_ctx);
  mpegts_destroy_context(tcp_ctx);
  pthread_mutex_destroy(&tcp_mutex);
}
#endif

int main(int argc, char **argv) {
  codec_settings.audio_sample_rate = AUDIO_SAMPLE_RATE;
  codec_settings.audio_bit_rate = AAC_BIT_RATE;
  codec_settings.audio_channels = 1;
  codec_settings.audio_profile = FF_PROFILE_AAC_LOW;

  check_record_directory();
  state_set(STATE_DIR, "record", "false");

  if (clear_hooks(HOOKS_DIR) != 0) {
    fprintf(stderr, "clear_hooks() failed\n");
  }
  start_watching_hooks(&hooks_thread, HOOKS_DIR, on_file_create, 1);

  fprintf(stderr, "setup_socks\n");
  setup_socks();

#if ENABLE_TCP_OUTPUT
  setup_tcp_output();
#endif

#if ENABLE_PREVIEW || ENABLE_CLOCK
  memset(tunnel, 0, sizeof(tunnel));
#endif

  int ret;
  fprintf(stderr, "bcm_host_init\n");
  bcm_host_init();

  fprintf(stderr, "OMX_Init\n");
  ret = OMX_Init();
  if (ret != OMX_ErrorNone) {
    fprintf(stderr, "OMX_Init failed with error code: 0x%x\n", ret);
    ilclient_destroy(ilclient);
    return -4;
  }
  memset(component_list, 0, sizeof(component_list));

  fprintf(stderr, "openmax_cam_open\n");
  ret = openmax_cam_open();
  if (ret != 0) {
    fprintf(stderr, "openmax_cam_open failed: %d\n", ret);
    return ret;
  }
  fprintf(stderr, "video_encode_startup\n");
  ret = video_encode_startup();
  if (ret != 0) {
    fprintf(stderr, "video_encode_startup failed: %d\n", ret);
    return ret;
  }

  av_log_set_level(AV_LOG_INFO);

  if (!disable_audio_capturing) {
    fprintf(stderr, "open_audio_capture_device\n");
    ret = open_audio_capture_device();
    if (ret == -1) {
      fprintf(stderr, "### WARNING: audio device is not available ###\n");
      disable_audio_capturing = 1;
    } else if (ret < 0) {
      fprintf(stderr, "init_audio failed with %d\n", ret);
      exit(1);
    }
  }

  if (disable_audio_capturing) {
    codec_settings.audio_bit_rate = 1000;
  }

  fprintf(stderr, "setup hls\n");

  // From http://tools.ietf.org/html/draft-pantos-http-live-streaming-12#section-6.2.1
  //
  // The server MUST NOT remove a media segment from the Playlist file if
  // the duration of the Playlist file minus the duration of the segment
  // is less than three times the target duration.
  //
  // So, 3 should be set as num_recent_files.
#if AUDIO_ONLY
  hls = hls_create_audio_only(2, &codec_settings); // 2 == num_recent_files
#else
  hls = hls_create(2, &codec_settings); // 2 == num_recent_files
#endif
  fprintf(stderr, "hls created\n");
  hls->dir = "/run/shm/video";
  hls->target_duration = 1;
  hls->num_retained_old_files = 10;
#if ENABLE_HLS_ENCRYPTION
  hls->use_encryption = 1;

  fprintf(stderr, "set hls->encryption_key_uri\n");
  hls->encryption_key_uri = malloc(11);
  if (hls->encryption_key_uri == NULL) {
    perror("Can't malloc for encryption key uri");
    return 1;
  }
  memcpy(hls->encryption_key_uri, "stream.key", 11);

  fprintf(stderr, "set hls->encryption_key\n");
  hls->encryption_key = malloc(16);
  if (hls->encryption_key == NULL) {
    perror("Can't malloc for encryption key");
    return 1;
  }
  uint8_t tmp_key[] = {
    0x75, 0xb0, 0xa8, 0x1d, 0xe1, 0x74, 0x87, 0xc8,
    0x8a, 0x47, 0x50, 0x7a, 0x7e, 0x1f, 0xdf, 0x73
  };
  memcpy(hls->encryption_key, tmp_key, 16);

  fprintf(stderr, "set hls->encryption_iv\n");
  hls->encryption_iv = malloc(16);
  if (hls->encryption_iv == NULL) {
    perror("Can't malloc for encryption iv");
    return 1;
  }
  uint8_t tmp_iv[] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16
  };
  memcpy(hls->encryption_iv, tmp_iv, 16);
#endif

  fprintf(stderr, "setup_av_frame\n");
  setup_av_frame(hls->format_ctx);

  if (disable_audio_capturing) {
    memset(samples, 0, period_size * sizeof(short) * channels);
    is_audio_recording_started = 1;
  } else {
    fprintf(stderr, "configure_audio_capture_device\n");
    ret = configure_audio_capture_device();
    if (ret != 0) {
      fprintf(stderr, "configure_audio_capture_device error: ret=%d\n", ret);
      exit(1);
    }
  }

  fprintf(stderr, "prepare_encoded_packets\n");
  prepare_encoded_packets();

  fprintf(stderr, "open_cam\n");

  fprintf(stderr, "setup signals\n");
  struct sigaction int_handler = {.sa_handler = stopSignalHandler};
  sigaction(SIGINT, &int_handler, NULL);
  sigaction(SIGTERM, &int_handler, NULL);

  fprintf(stderr, "openmax_cam_loop start\n");
  openmax_cam_loop();

  if (disable_audio_capturing) {
    pthread_create(&audio_nop_thread, NULL, audio_nop_loop, NULL);
    fprintf(stderr, "waiting for audio_nop_thread to exit\n");
    pthread_join(audio_nop_thread, NULL);
    fprintf(stderr, "audio_nop_thread has exited\n");
  } else {
    fprintf(stderr, "audio_loop_poll_mmap\n");
    audio_loop_poll_mmap();
  }

  if (is_recording) {
    rec_thread_needs_write = 1;
    pthread_cond_signal(&rec_cond);

    stop_record();
    fprintf(stderr, "waiting for rec_thread to exit\n");
    pthread_join(rec_thread, NULL);
    fprintf(stderr, "rec_thread has exited\n");
  }

  teardown_audio_encode();

  if (!disable_audio_capturing) {
    teardown_audio_capture_device();
  }

  fprintf(stderr, "hls_destroy\n");
  hls_destroy(hls);

  pthread_mutex_destroy(&mutex_writing);
  pthread_mutex_destroy(&rec_mutex);
  pthread_mutex_destroy(&rec_write_mutex);

#if ENABLE_TCP_OUTPUT
  teardown_tcp_output();
#endif

  teardown_socks();

  fprintf(stderr, "free_encoded_packets\n");
  free_encoded_packets();

  stop_watching_hooks();
  fprintf(stderr, "waiting for hooks_thread to exit\n");
  pthread_join(hooks_thread, NULL);

  fprintf(stderr, "shutdown successful\n");
  return 0;
}

#if defined(__cplusplus)
}
#endif
