/*
 *  Capture video from Raspberry Pi Camera and audio from ALSA,
 *  encode them to H.264/AAC, and mux them to MPEG-TS.
 *
 *  H.264 encoder: Raspberry Pi H.264 hardware encoder (via OpenMAX IL)
 *  AAC encoder  : fdk-aac (via libavcodec)
 *  MPEG-TS muxer: libavformat
 */

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/videodev2.h>
#include <alsa/asoundlib.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <getopt.h>

#include "bcm_host.h"
#include "ilclient.h"

#include "hooks.h"
#include "mpegts.h"
#include "httplivestreaming.h"
#include "state.h"
#include "log.h"

#define PROGRAM_NAME     "picam"
#define PROGRAM_VERSION  "1.1.1"

// Audio-only stream is created if this is 1 (for debugging)
#define AUDIO_ONLY 0

// ALSA buffer size (frames) is multiplied by this number
#define ALSA_BUFFER_MULTIPLY 100

// Both PTS and DTS are 33 bit and wraps around to zero
#define PTS_MODULO 8589934592

// Initial values for audio/video PTS
#define AUDIO_PTS_START 0
#define VIDEO_PTS_START 0

// Internal flag indicates that audio is available for read
#define AVAIL_AUDIO 2

// Each video frame's PTS is incremented by this in normal condition
// 90000 / 2955 = 30.46 FPS
#define VIDEO_PTS_STEP 2955

// If this value is increased, audio gets faster than video
#define N_BUFFER_COUNT_ACTUAL 1

// If this value is increased, video gets faster than audio
#define AUDIO_BUFFER_CHUNKS 0

// How much PTS difference between audio and video is
// considered to be too large
#define PTS_DIFF_TOO_LARGE 45000  // 90000 == 1 second

// enum
#define EXPOSURE_AUTO 0
#define EXPOSURE_NIGHT 1

// Number of packets to chase recording for each cycle
#define REC_CHASE_PACKETS 10

// Which color (YUV) is used to fill blank borders
#define FILL_COLOR_Y 0
#define FILL_COLOR_U 128
#define FILL_COLOR_V 128

// Whether or not to pass pBuffer from camera to video_encode directly
#define ENABLE_PBUFFER_OPTIMIZATION_HACK 1

#if ENABLE_PBUFFER_OPTIMIZATION_HACK
static OMX_BUFFERHEADERTYPE *video_encode_input_buf = NULL;
static OMX_U8 *video_encode_input_buf_pBuffer_orig = NULL;
#endif

// OpenMAX IL ports
static const int CAMERA_PREVIEW_PORT      = 70;
static const int CAMERA_CAPTURE_PORT      = 71;
static const int CAMERA_INPUT_PORT        = 73;
static const int CLOCK_OUTPUT_1_PORT      = 80;
static const int VIDEO_RENDER_INPUT_PORT  = 90;
static const int VIDEO_ENCODE_INPUT_PORT  = 200;
static const int VIDEO_ENCODE_OUTPUT_PORT = 201;

// Directory to put recorded MPEG-TS files
static const char *rec_dir = "rec";
static const char *rec_tmp_dir = "rec/tmp";
static const char *rec_archive_dir = "rec/archive";

// Whether or not to enable clock OMX component
static const int is_clock_enabled = 1;

// Pace of PTS
typedef enum {
  PTS_SPEED_NORMAL,
  PTS_SPEED_UP,
  PTS_SPEED_DOWN,
} pts_mode_t;

typedef struct EncodedPacket {
  int64_t pts;
  uint8_t *data;
  int size;
  int stream_index;
  int flags;
} EncodedPacket;

static const int log_level_default = LOG_LEVEL_INFO;
static int video_width;
static const int video_width_default = 1280;
static int video_height;
static const int video_height_default = 720;
static float video_fps;
static const float video_fps_default = 30.0;
static int video_gop_size;
static const int video_gop_size_default = 30;
static long video_bitrate;
static const long video_bitrate_default = 2000 * 1000; // 2 Mbps
static char alsa_dev[256];
static const char *alsa_dev_default = "hw:0,0";
static long audio_bitrate;
static const long audio_bitrate_default = 40000; // 40 Kbps
static int audio_sample_rate;
static const int audio_sample_rate_default = 48000;
static int is_hlsout_enabled;
static const int is_hlsout_enabled_default = 0;
static char hls_output_dir[256];
static const char *hls_output_dir_default = "/run/shm/video";
static int is_rtspout_enabled;
static const int is_rtspout_enabled_default = 0;
static char rtsp_video_control_path[256];
static const char *rtsp_video_control_path_default = "/tmp/node_rtsp_rtmp_videoControl";
static char rtsp_audio_control_path[256];
static const char *rtsp_audio_control_path_default = "/tmp/node_rtsp_rtmp_audioControl";
static char rtsp_video_data_path[256];
static const char *rtsp_video_data_path_default = "/tmp/node_rtsp_rtmp_videoData";
static char rtsp_audio_data_path[256];
static const char *rtsp_audio_data_path_default = "/tmp/node_rtsp_rtmp_audioData";
static int is_tcpout_enabled;
static const int is_tcpout_enabled_default = 0;
static char tcp_output_dest[256];
static int is_auto_exposure_enabled;
static const int is_auto_exposure_enabled_default = 0;
static int exposure_night_y_threshold;
static const int exposure_night_y_threshold_default = 40;
static int exposure_auto_y_threshold;
static const int exposure_auto_y_threshold_default = 50;
static char state_dir[256];
static const char *state_dir_default = "state";
static char hooks_dir[256];
static const char *hooks_dir_default = "hooks";
static float audio_volume_multiply;
static const float audio_volume_multiply_default = 1.0;
static int audio_min_value;
static int audio_max_value;
static int is_hls_encryption_enabled;
static const int is_hls_encryption_enabled_default = 0;
static char hls_encryption_key_uri[256];
static const char *hls_encryption_key_uri_default = "stream.key";
static uint8_t hls_encryption_key[16] = {
  0x75, 0xb0, 0xa8, 0x1d, 0xe1, 0x74, 0x87, 0xc8,
  0x8a, 0x47, 0x50, 0x7a, 0x7e, 0x1f, 0xdf, 0x73,
};
static uint8_t hls_encryption_iv[16] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};
static int is_preview_enabled;
static const int is_preview_enabled_default = 0;
static int record_buffer_keyframes;
static const int record_buffer_keyframes_default = 5;

static int64_t video_current_pts = 0;
static int64_t audio_current_pts = 0;

pts_mode_t pts_mode = PTS_SPEED_NORMAL;

// Counter for PTS speed up/down
static int speed_up_count = 0;
static int speed_down_count = 0;

static int audio_pts_step_base;

#if AUDIO_BUFFER_CHUNKS > 0
uint16_t *audio_buffer[AUDIO_BUFFER_CHUNKS];
int audio_buffer_index = 0;
int is_audio_buffer_filled = 0;
#endif

// If this value is 1, audio capturing is always disabled.
static int disable_audio_capturing = 0;

static pthread_t audio_nop_thread;

static int fr_q16;

// Function prototypes
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
static COMPONENT_T *render_component = NULL;
static COMPONENT_T *clock_component = NULL;
static TUNNEL_T tunnel[3]; // Must be null-terminated
static int n_tunnel = 0;

static AVFormatContext *tcp_ctx;
static pthread_mutex_t tcp_mutex = PTHREAD_MUTEX_INITIALIZER;

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
static int flush_recording_seconds = 5; // Flush recording data every 5 seconds
static time_t rec_start_time;

static HTTPLiveStreaming *hls;

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
static struct pollfd *poll_fds; // file descriptors for polling audio
static int is_first_audio;
static int period_size;
static int channels = 1;

// threads
static pthread_mutex_t mutex_writing = PTHREAD_MUTEX_INITIALIZER;

// UNIX domain sockets
static int sockfd_video;
static int sockfd_video_control;
static int sockfd_audio;
static int sockfd_audio_control;

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

EncodedPacket **encoded_packets; // circular buffer
static int current_encoded_packet = -1;
static int *keyframe_pointers;
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

// Will be set to 1 when the camera finishes capturing
static int is_camera_finished = 0;

static pthread_mutex_t camera_finish_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t camera_finish_cond = PTHREAD_COND_INITIALIZER;

static void unmute_audio() {
  log_info("unmute");
  is_audio_muted = 0;
}

static void mute_audio() {
  log_info("mute");
  is_audio_muted = 1;
}

// Check if disk usage is >= 95%
static int is_disk_almost_full() {
  struct statvfs stat;
  statvfs("/", &stat);
  int used_percent = ceil( (stat.f_blocks - stat.f_bfree) * 100.0 / stat.f_blocks);
  log_info("disk_usage=%d%% ", used_percent);
  if (used_percent >= 95) {
    return 1;
  } else {
    return 0;
  }
}

static void mark_keyframe_packet() {
  current_keyframe_pointer++;
  if (current_keyframe_pointer >= record_buffer_keyframes) {
    current_keyframe_pointer = 0;
    if (!is_keyframe_pointers_filled) {
      is_keyframe_pointers_filled = 1;
    }
  }
  keyframe_pointers[current_keyframe_pointer] = current_encoded_packet;
}

static void prepare_encoded_packets() {
  int audio_fps = audio_sample_rate / 1 / period_size;
  encoded_packets_size = (video_fps + 1) * record_buffer_keyframes * 2 +
    (audio_fps + 1) * record_buffer_keyframes * 2 + 100;

  int malloc_size = sizeof(EncodedPacket *) * encoded_packets_size;
  encoded_packets = malloc(malloc_size);
  if (encoded_packets == NULL) {
    log_error("cannot allocate memory for encoded_packets\n");
    exit(EXIT_FAILURE);
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
      log_error("write_encoded_packets: av_write_frame error: ret=%d\n", ret);
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
      perror("malloc for EncodedPacket");
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
    log_error("avcodec_alloc_frame failed\n");
    exit(EXIT_FAILURE);
  }

  av_frame->sample_rate = audio_codec_ctx->sample_rate;
  av_frame->nb_samples = audio_codec_ctx->frame_size;
  av_frame->format = audio_codec_ctx->sample_fmt;
  av_frame->channel_layout = audio_codec_ctx->channel_layout;

  buffer_size = av_samples_get_buffer_size(NULL, audio_codec_ctx->channels,
      audio_codec_ctx->frame_size, audio_codec_ctx->sample_fmt, 0);
  samples = av_malloc(buffer_size);
  if (!samples) {
    log_error("av_malloc for samples failed\n");
    exit(EXIT_FAILURE);
  }
#if AUDIO_BUFFER_CHUNKS > 0
  int i;
  for (i = 0; i < AUDIO_BUFFER_CHUNKS; i++) {
    audio_buffer[i] = av_malloc(buffer_size);
    if (!audio_buffer[i]) {
      log_error("av_malloc for audio_buffer[%d] failed\n", i);
      exit(EXIT_FAILURE);
    }
  }
#endif

  period_size = buffer_size / channels / sizeof(short);
  audio_pts_step_base = 90000.0 * period_size / audio_sample_rate;
  log_debug("audio_pts_step_base: %d\n", audio_pts_step_base);

  ret = avcodec_fill_audio_frame(av_frame, audio_codec_ctx->channels, audio_codec_ctx->sample_fmt,
      (const uint8_t*)samples, buffer_size, 0);
  if (ret < 0) {
    log_error("avcodec_fill_audio_frame failed: ret=%d\n", ret);
    exit(EXIT_FAILURE);
  }
}

// Create dir if it does not exist
int create_dir(const char *dir) {
  struct stat st;
  int err;

  err = stat(dir, &st);
  if (err == -1) {
    if (errno == ENOENT) {
      // create directory
      if (mkdir(dir, 0755) == 0) { // success
        log_info("created directory: ./%s\n", dir);
      } else { // error
        log_error("error creating directory ./%s: %s\n",
            dir, strerror(errno));
        return -1;
      }
    } else {
      perror("stat directory");
      return -1;
    }
  } else {
    if (!S_ISDIR(st.st_mode)) {
      log_error("./%s is not a directory\n", dir);
      return -1;
    }
  }

  if (access(dir, R_OK) != 0) {
    log_error("cannot access directory ./%s: %s\n",
        dir, strerror(errno));
    return -1;
  }

  return 0;
}

void *rec_thread_stop() {
  FILE *fsrc, *fdest;
  int read_len;
  uint8_t *copy_buf;

  copy_buf = malloc(BUFSIZ);
  if (copy_buf == NULL) {
    perror("malloc for copy_buf");
    pthread_exit(0);
  }

  pthread_mutex_lock(&rec_write_mutex);
  mpegts_close_stream(rec_format_ctx);
  mpegts_destroy_context(rec_format_ctx);
  log_info("stop rec");
  state_set(state_dir, "record", "false");
  pthread_mutex_unlock(&rec_write_mutex);

  log_debug("copy ");
  fsrc = fopen(recording_tmp_filepath, "r");
  if (fsrc == NULL) {
    perror("fopen recording_tmp_filepath");
  }
  fdest = fopen(recording_archive_filepath, "a");
  if (fdest == NULL) {
    perror("fopen recording_archive_filepath");
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
    log_error("rec_thread_stop: not an EOF?: %s\n", strerror(errno));
  }

  // link
  log_debug("symlink");
  if (symlink(recording_archive_filepath + 4, recording_filepath) != 0) { // +4 is for trimming "rec/"
    perror("symlink recording_archive_filepath");
  }

  // unlink tmp file
  log_debug("unlink");
  unlink(recording_tmp_filepath);

  state_set(state_dir, "last_rec", recording_filepath);

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
    perror("malloc for copy_buf");
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
  log_info("start rec to %s", recording_tmp_filepath);
  state_set(state_dir, "record", "true");
  pthread_mutex_unlock(&rec_write_mutex);

  int start_keyframe_pointer;
  if (!is_keyframe_pointers_filled) {
    start_keyframe_pointer = 0;
  } else {
    start_keyframe_pointer = current_keyframe_pointer - record_buffer_keyframes + 1;
  }
  while (start_keyframe_pointer < 0) {
    start_keyframe_pointer += record_buffer_keyframes;
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
          log_info("caught up");
          is_caught_up = 1;
        }
      }
    }
    check_record_duration();
    if (rec_thread_needs_flush) {
      log_debug("F");
      mpegts_close_stream_without_trailer(rec_format_ctx);

      fsrc = fopen(recording_tmp_filepath, "r");
      if (fsrc == NULL) {
        perror("fopen recording_tmp_filepath");
      }
      fdest = fopen(recording_archive_filepath, "a");
      if (fdest == NULL) {
        perror("fopen recording_archive_filepath");
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
        log_error("rec_thread_start: not an EOF?: %s\n", strerror(errno));
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
  state_set(state_dir, recording_filepath + 4, diff_pts);

  return rec_thread_stop();
}

void start_record() {
  if (is_recording) {
    log_warn("recording is already started\n");
    return;
  }

  if (is_disk_almost_full()) {
    log_error("disk is almost full, recording not started\n");
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
static void send_audio_start_time() {
  if (is_rtspout_enabled) {
    int payload_size = 9;
    int64_t logical_start_time = audio_start_time;
    uint8_t sendbuf[12] = {
      // payload size
      (payload_size >> 16) & 0xff,
      (payload_size >> 8) & 0xff,
      payload_size & 0xff,
      // packet type (0x01 == audio start time)
      0x01,
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
    if (send(sockfd_audio_control, sendbuf, 12, 0) == -1) {
      perror("send audio start time");
      exit(EXIT_FAILURE);
    }
  } // if (is_rtspout_enabled)
}

// Send video packet to node-rtsp-rtmp-server
static void send_video_start_time() {
  if (is_rtspout_enabled) {
    int payload_size = 9;
    int64_t logical_start_time = video_start_time;
    uint8_t sendbuf[12] = {
      // payload size
      (payload_size >> 16) & 0xff,
      (payload_size >> 8) & 0xff,
      payload_size & 0xff,
      // packet type (0x00 == video start time)
      0x00,
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
    if (send(sockfd_video_control, sendbuf, 12, 0) == -1) {
      perror("send video start time");
      exit(EXIT_FAILURE);
    }
  } // if (is_rtspout_enabled)
}

static void setup_socks() {
  if (is_rtspout_enabled) {
    struct sockaddr_un remote_video;
    struct sockaddr_un remote_audio;

    int len;
    struct sockaddr_un remote_video_control;
    struct sockaddr_un remote_audio_control;

    log_debug("connecting to UNIX domain sockets\n");

    // Setup sockfd_video
    if ((sockfd_video = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
      perror("socket video");
      exit(EXIT_FAILURE);
    }
    remote_video.sun_family = AF_UNIX;
    strcpy(remote_video.sun_path, rtsp_video_data_path);
    len = strlen(remote_video.sun_path) + sizeof(remote_video.sun_family);
    if (connect(sockfd_video, (struct sockaddr *)&remote_video, len) == -1) {
      log_error("error: failed to connect to video data socket (%s): %s\n"
          "perhaps RTSP server (https://github.com/iizukanao/node-rtsp-rtmp-server) is not running?\n",
          rtsp_video_data_path, strerror(errno));
      exit(EXIT_FAILURE);
    }

    // Setup sockfd_video_control
    if ((sockfd_video_control = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
      perror("socket video_control");
      exit(EXIT_FAILURE);
    }
    remote_video_control.sun_family = AF_UNIX;
    strcpy(remote_video_control.sun_path, rtsp_video_control_path);
    len = strlen(remote_video_control.sun_path) + sizeof(remote_video_control.sun_family);
    if (connect(sockfd_video_control, (struct sockaddr *)&remote_video_control, len) == -1) {
      log_error("error: failed to connect to video control socket (%s): %s\n"
          "perhaps RTSP server (https://github.com/iizukanao/node-rtsp-rtmp-server) is not running?\n",
          rtsp_video_control_path, strerror(errno));
      exit(EXIT_FAILURE);
    }

    // Setup sockfd_audio
    if ((sockfd_audio = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
      perror("socket audio");
      exit(EXIT_FAILURE);
    }
    remote_audio.sun_family = AF_UNIX;
    strcpy(remote_audio.sun_path, rtsp_audio_data_path);
    len = strlen(remote_audio.sun_path) + sizeof(remote_audio.sun_family);
    if (connect(sockfd_audio, (struct sockaddr *)&remote_audio, len) == -1) {
      log_error("error: failed to connect to audio data socket (%s): %s\n"
          "perhaps RTSP server (https://github.com/iizukanao/node-rtsp-rtmp-server) is not running?\n",
          rtsp_audio_data_path, strerror(errno));
      exit(EXIT_FAILURE);
    }

    // Setup sockfd_audio_control
    if ((sockfd_audio_control = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
      perror("socket audio_control");
      exit(EXIT_FAILURE);
    }
    remote_audio_control.sun_family = AF_UNIX;
    strcpy(remote_audio_control.sun_path, rtsp_audio_control_path);
    len = strlen(remote_audio_control.sun_path) + sizeof(remote_audio_control.sun_family);
    if (connect(sockfd_audio_control, (struct sockaddr *)&remote_audio_control, len) == -1) {
      log_error("error: failed to connect to audio control socket (%s): %s\n"
          "perhaps RTSP server (https://github.com/iizukanao/node-rtsp-rtmp-server) is not running?\n",
          rtsp_audio_control_path, strerror(errno));
      exit(EXIT_FAILURE);
    }
  } // if (is_rtspout_enabled)
}

static void teardown_socks() {
  if (is_rtspout_enabled) {
    close(sockfd_video);
    close(sockfd_video_control);
    close(sockfd_audio);
    close(sockfd_audio_control);
  }
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
    (int64_t)((frame_number + video_frame_advantage) * (double)90000 / (double)video_fps);
}

static int64_t get_next_video_pts() {
  int64_t pts;
  video_frame_count++;

  int pts_diff = audio_current_pts - video_current_pts - VIDEO_PTS_STEP;
  int tolerance = (VIDEO_PTS_STEP + audio_pts_step_base) * 2;
  if (pts_diff >= PTS_DIFF_TOO_LARGE) {
    // video PTS is too slow
    log_debug("vR%d", pts_diff);
    pts = audio_current_pts;
  } else if (pts_diff >= tolerance) {
    if (pts_mode != PTS_SPEED_UP) {
      // speed up video PTS
      speed_up_count++;
      pts_mode = PTS_SPEED_UP;
      log_debug("vSPEED_UP(%d)", pts_diff);
    }
    // Catch up with audio PTS if the delay is too large.
    pts = video_current_pts + VIDEO_PTS_STEP + 150;
  } else if (pts_diff <= -tolerance) {
    if (pts_mode != PTS_SPEED_DOWN) {
      // speed down video PTS
      pts_mode = PTS_SPEED_DOWN;
      speed_down_count++;
      log_debug("vSPEED_DOWN(%d)", pts_diff);
    }
    pts = video_current_pts + VIDEO_PTS_STEP - 150;
  } else {
    pts = video_current_pts + VIDEO_PTS_STEP;
    if (pts_diff < 2000 && pts_diff > -2000) {
      if (pts_mode != PTS_SPEED_NORMAL) {
        // video PTS has caught up with audio PTS
        log_debug("vNORMAL");
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
  return audio_start_time + audio_frame_count * 1000000000.0 / ((float)audio_sample_rate / (float)period_size);
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

  log_debug(" a-v=%lld c-a=%lld u=%d d=%d\n",
      avdiff, clock_pts - audio_pts, speed_up_count, speed_down_count);
}

static void send_audio_frame(uint8_t *databuf, int databuflen, int64_t pts) {
  if (is_rtspout_enabled) {
    int payload_size = databuflen + 7;  // +1(packet type) +6(pts)
    int total_size = payload_size + 3;  // more 3 bytes for payload length
    uint8_t *sendbuf = malloc(total_size);
    if (sendbuf == NULL) {
      log_error("cannot allocate memory for audio sendbuf: size=%d", total_size);
      return;
    }
    // payload header
    sendbuf[0] = (payload_size >> 16) & 0xff;
    sendbuf[1] = (payload_size >> 8) & 0xff;
    sendbuf[2] = payload_size & 0xff;
    // payload
    sendbuf[3] = 0x03;  // packet type (0x03 == audio data)
    sendbuf[4] = (pts >> 40) & 0xff;
    sendbuf[5] = (pts >> 32) & 0xff;
    sendbuf[6] = (pts >> 24) & 0xff;
    sendbuf[7] = (pts >> 16) & 0xff;
    sendbuf[8] = (pts >> 8) & 0xff;
    sendbuf[9] = pts & 0xff;
    memcpy(sendbuf + 10, databuf, databuflen);
    if (send(sockfd_audio, sendbuf, total_size, 0) == -1) {
      perror("send audio data");
    }
    free(sendbuf);
  } // if (is_rtspout_enabled)
}

static void send_video_frame(uint8_t *databuf, int databuflen, int64_t pts) {
  if (is_rtspout_enabled) {
    int payload_size = databuflen + 7;  // +1(packet type) +6(pts)
    int total_size = payload_size + 3;  // more 3 bytes for payload length
    uint8_t *sendbuf = malloc(total_size);
    if (sendbuf == NULL) {
      log_error("cannot allocate memory for video sendbuf: size=%d", total_size);
      return;
    }
    // payload header
    sendbuf[0] = (payload_size >> 16) & 0xff;
    sendbuf[1] = (payload_size >> 8) & 0xff;
    sendbuf[2] = payload_size & 0xff;
    // payload
    sendbuf[3] = 0x02;  // packet type (0x02 == video data)
    sendbuf[4] = (pts >> 40) & 0xff;
    sendbuf[5] = (pts >> 32) & 0xff;
    sendbuf[6] = (pts >> 24) & 0xff;
    sendbuf[7] = (pts >> 16) & 0xff;
    sendbuf[8] = (pts >> 8) & 0xff;
    sendbuf[9] = pts & 0xff;
    memcpy(sendbuf + 10, databuf, databuflen);
    if (send(sockfd_video, sendbuf, total_size, 0) == -1) {
      perror("send video data");
    }
    free(sendbuf);
  } // if (is_rtspout_enabled)
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
    log_error("send_keyframe: cannot allocate memory for buf (%d bytes)\n", total_size);
    exit(EXIT_FAILURE);
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

  if (is_tcpout_enabled) {
    pthread_mutex_lock(&tcp_mutex);
    av_write_frame(tcp_ctx, &pkt);
    pthread_mutex_unlock(&tcp_mutex);
  }

  if (is_hlsout_enabled) {
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
      log_error("keyframe write error (hls): %d\n", ret);
      log_error("check if the filesystem is not full\n");
    }
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
    log_debug("Z");
    return 0;
  }

  total_size = access_unit_delimiter_length + data_len;
  buf = av_malloc(total_size);
  if (buf == NULL) {
    log_fatal("send_pframe malloc failed: size=%d\n", total_size);
    return 0;
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

  if (is_tcpout_enabled) {
    pthread_mutex_lock(&tcp_mutex);
    av_write_frame(tcp_ctx, &pkt);
    pthread_mutex_unlock(&tcp_mutex);
  }

  if (is_hlsout_enabled) {
    pthread_mutex_lock(&mutex_writing);
    ret = hls_write_packet(hls, &pkt, 0);
    pthread_mutex_unlock(&mutex_writing);
    if (ret < 0) {
      log_error("P frame write error (hls): %d\n", ret);
      log_error("check if the filesystem is not full\n");
    }
  }

  free(buf);
  av_free_packet(&pkt);
  return ret;
}

// Callback function that is called when an error has occurred
static int xrun_recovery(snd_pcm_t *handle, int error) {
  switch(error) {
    case -EPIPE: // Buffer overrun
      log_error("microphone error: buffer overrun\n");
      if ((error = snd_pcm_prepare(handle)) < 0) {
        log_error("microphone error: buffer overrrun cannot be recovered, "
            "snd_pcm_prepare failed: %s\n", snd_strerror(error));
      }
      return 0;
      break;

    case -ESTRPIPE: // Microphone is suspended
      log_error("microphone error: suspended\n");
      // Wait until the suspend flag is cleared
      while ((error = snd_pcm_resume(handle)) == -EAGAIN) {
        sleep(1);
      }

      if (error < 0) {
        if ((error = snd_pcm_prepare(handle)) < 0) {
          log_error("microphone: suspend cannot be recovered, "
              "snd_pcm_prepare failed: %s\n", snd_strerror(error));
        }
      }
      return 0;
      break;

    case -EBADFD: // PCM descriptor is wrong
      log_error("microphone error: EBADFD\n");
      break;

    default:
      log_error("microphone error: unknown, error = %d\n",error);
      break;
  }
  return error;
}

// Wait for data using poll
static int wait_for_poll(snd_pcm_t *device, struct pollfd *target_fds, unsigned int audio_fd_count) {
  unsigned short revents;
  int avail_flags = 0;
  int ret;

  while (1) {
    ret = poll(target_fds, audio_fd_count, -1); // -1 means block
    if (ret < 0) {
      if (keepRunning) {
        log_error("audio poll error: %d\n", ret);
      }
      return ret;
    } else {
      snd_pcm_poll_descriptors_revents(device, target_fds, audio_fd_count, &revents);
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

  log_debug("opening ALSA device: %s\n", alsa_dev);
  err = snd_pcm_open(&capture_handle, alsa_dev, SND_PCM_STREAM_CAPTURE, 0);
  if (err < 0) {
    log_error("cannot open audio device '%s' (%s)\n",
        alsa_dev, snd_strerror(err));
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

  // ALSA poll mmap
  snd_pcm_uframes_t real_buffer_size; // real buffer size in frames
  int dir;

  buffer_size = av_samples_get_buffer_size(NULL, ctx->channels,
      ctx->frame_size, ctx->sample_fmt, 0);

  err = snd_pcm_hw_params_malloc(&hw_params);
  if (err < 0) {
    log_fatal("cannot allocate hardware parameter structure (%s)\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  err = snd_pcm_hw_params_any(capture_handle, hw_params);
  if (err < 0) {
    log_fatal("cannot initialize hardware parameter structure (%s)\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // use mmap
  err = snd_pcm_hw_params_set_access(capture_handle, hw_params,
      SND_PCM_ACCESS_MMAP_INTERLEAVED);
  if (err < 0) {
    log_fatal("cannot set access type (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // SND_PCM_FORMAT_S16_LE => PCM 16 bit signed little endian
  err = snd_pcm_hw_params_set_format(capture_handle, hw_params, SND_PCM_FORMAT_S16_LE);
  if (err < 0) {
    log_fatal("cannot set sample format (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // set the sample rate
  unsigned int rate = audio_sample_rate;
  err = snd_pcm_hw_params_set_rate_near(capture_handle, hw_params, &rate, 0);
  if (err < 0) {
    log_fatal("cannot set sample rate (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  unsigned int actual_rate;
  int actual_dir;
  err = snd_pcm_hw_params_get_rate(hw_params, &actual_rate, &actual_dir);
  if (err < 0) {
    log_fatal("microphone: failed to get rate (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }
  log_debug("actual sample rate=%u dir=%d\n", actual_rate, actual_dir);
  if (actual_rate != audio_sample_rate) {
    log_fatal("error: failed to set the sample rate of microphone to %d (got %d)\n",
        audio_sample_rate, actual_rate);
    exit(EXIT_FAILURE);
  }

  // set the number of channels
  err = snd_pcm_hw_params_set_channels(capture_handle, hw_params, channels);
  if (err < 0) {
    log_fatal("cannot set channel count (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // set the buffer size
  err = snd_pcm_hw_params_set_buffer_size(capture_handle, hw_params,
      buffer_size * ALSA_BUFFER_MULTIPLY);
  if (err < 0) {
    log_fatal("microphone: failed to set buffer size (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // check the value of the buffer size
  err = snd_pcm_hw_params_get_buffer_size(hw_params, &real_buffer_size);
  if (err < 0) {
    log_fatal("microphone: failed to get buffer size (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }
  log_debug("microphone: buffer size: %d frames\n", (int)real_buffer_size);

  dir = 0;
  // set the period size
  err = snd_pcm_hw_params_set_period_size_near(capture_handle, hw_params,
      (snd_pcm_uframes_t *)&period_size, &dir);
  if (err < 0) {
    log_fatal("microphone: period size cannot be configured (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  snd_pcm_uframes_t actual_period_size;
  err = snd_pcm_hw_params_get_period_size(hw_params, &actual_period_size, &dir);
  if (err < 0) {
    log_fatal("microphone: period size cannot be configured (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }
  log_debug("actual_period_size=%lu dir=%d\n", actual_period_size, dir);

  // apply the hardware configuration
  err = snd_pcm_hw_params (capture_handle, hw_params);
  if (err < 0) {
    log_fatal("cannot set PCM hardware parameters (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // end of configuration
  snd_pcm_hw_params_free (hw_params);

  err = snd_pcm_prepare(capture_handle);
  if (err < 0) {
    log_fatal("cannot prepare audio interface for use (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  audio_fd_count = snd_pcm_poll_descriptors_count(capture_handle);
  if (audio_fd_count <= 0) {
    log_error("microphone: invalid poll descriptors count\n");
    return audio_fd_count;
  }
  poll_fds = malloc(sizeof(struct pollfd) * audio_fd_count);
  if (poll_fds == NULL) {
    log_fatal("cannot allocate memory for poll_fds\n");
    exit(EXIT_FAILURE);
  }
  // get poll descriptors
  err = snd_pcm_poll_descriptors(capture_handle, poll_fds, audio_fd_count);
  if (err < 0) {
    log_error("microphone: unable to obtain poll descriptors for capture: %s\n", snd_strerror(err));
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

  // get the delayed frames
  for (got_output = 1; got_output; i++) {
    av_init_packet(&pkt);
    pkt.data = NULL; // packet data will be allocated by the encoder
    pkt.size = 0;

    ret = avcodec_encode_audio2(ctx, &pkt, NULL, &got_output);
    av_free_packet(&pkt);
    if (ret < 0) {
      log_error("error encoding frame\n");
      break;
    }
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

  free(poll_fds);
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
  log_debug("stop requested (signal=%d)\n", signo);
}

static void shutdown_video() {
  int i;

  log_debug("shutdown_video\n");
  for (i = 0; i < n_codec_configs; i++) {
    free(codec_configs[i]);
  }
}

static void shutdown_openmax() {
  int i;

  if (is_preview_enabled || is_clock_enabled) {
    log_debug("shutdown_openmax: ilclient_flush_tunnels\n");
    ilclient_flush_tunnels(tunnel, 0);
  }

  // Disable port buffers
  log_debug("shutdown_openmax: disable port buffer for camera %d\n", CAMERA_CAPTURE_PORT);
  ilclient_disable_port_buffers(camera_component, CAMERA_CAPTURE_PORT, NULL, NULL, NULL);
  log_debug("shutdown_openmax: disable port buffer for video_encode %d\n", VIDEO_ENCODE_INPUT_PORT);
  ilclient_disable_port_buffers(video_encode, VIDEO_ENCODE_INPUT_PORT, NULL, NULL, NULL);
  log_debug("shutdown_openmax: disable port buffer for video_encode %d\n", VIDEO_ENCODE_OUTPUT_PORT);
  ilclient_disable_port_buffers(video_encode, VIDEO_ENCODE_OUTPUT_PORT, NULL, NULL, NULL);

  if (is_preview_enabled || is_clock_enabled) {
    for (i = 0; i < n_tunnel; i++) {
      log_debug("shutdown_openmax: disable tunnel[%d]\n", i);
      ilclient_disable_tunnel(&tunnel[i]);
    }
    log_debug("shutdown_openmax: teardown tunnels\n");
    ilclient_teardown_tunnels(tunnel);
  }

  log_debug("shutdown_openmax: state transition to idle\n");
  ilclient_state_transition(component_list, OMX_StateIdle);
  log_debug("shutdown_openmax: state transition to loaded\n");
  ilclient_state_transition(component_list, OMX_StateLoaded);

  log_debug("shutdown_openmax: ilclient_cleanup_components\n");
  ilclient_cleanup_components(component_list);

  log_debug("shutdown_openmax: OMX_Deinit\n");
  OMX_Deinit();

  log_debug("shutdown_openmax: ilclient_destroy cam_client\n");
  ilclient_destroy(cam_client);
  log_debug("shutdown_openmax: ilclient_destroy ilclient\n");
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

  log_debug("exposure mode: auto\n");
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigCommonExposure, &exposure_type);
  if (error != OMX_ErrorNone) {
    log_error("failed to set camera exposure to auto: 0x%x\n", error);
  }
  current_exposure_mode = EXPOSURE_AUTO;
}

static void set_exposure_to_night() {
  OMX_CONFIG_EXPOSURECONTROLTYPE exposure_type;
  OMX_ERRORTYPE error;

  memset(&exposure_type, 0, sizeof(OMX_CONFIG_EXPOSURECONTROLTYPE));
  exposure_type.nSize = sizeof(OMX_CONFIG_EXPOSURECONTROLTYPE);
  exposure_type.nVersion.nVersion = OMX_VERSION;
  exposure_type.nPortIndex = OMX_ALL;
  exposure_type.eExposureControl = OMX_ExposureControlNight;

  log_debug("exposure mode: night\n");
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigCommonExposure, &exposure_type);
  if (error != OMX_ErrorNone) {
    log_error("failed to set camera exposure to night: 0x%x\n", error);
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
  log_debug("y=%d(%d) ", average_y, diff_frames);
  if (average_y <= exposure_night_y_threshold) { // dark
    if (current_exposure_mode == EXPOSURE_AUTO) {
      set_exposure_to_night();
    }
  } else if (average_y >= exposure_auto_y_threshold) { // in the light
    if (current_exposure_mode == EXPOSURE_NIGHT) {
      set_exposure_to_auto();
    }
  }
}

static void cam_fill_buffer_done(void *data, COMPONENT_T *comp) {
  OMX_BUFFERHEADERTYPE *out;
  OMX_ERRORTYPE error;

  out = ilclient_get_output_buffer(camera_component, CAMERA_CAPTURE_PORT, 1);
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
            send_audio_start_time();
            send_video_start_time();
          }
        }

        if (is_audio_recording_started == 1) {
          if (video_pending_drop_frames > 0) {
            log_debug("dV");
            video_pending_drop_frames--;
          } else {
            log_info(".");
            encode_and_send_image();
            previous_previous_capture_frame = previous_capture_frame;
            previous_capture_frame = video_frame_count;
          }
        }
      } else {
        log_warn("\nnot an end of a frame\n");
      }
    } else {
      log_warn("got zero bytes\n");
    }
    out->nFilledLen = 0;
  } else {
    log_warn("out is NULL\n");
  }

  if (keepRunning) {
    error = OMX_FillThisBuffer(ILC_GET_HANDLE(camera_component), out);
    if (error != OMX_ErrorNone) {
      log_error("error filling camera buffer (2): 0x%x\n", error);
    }
  } else {
    // Return the buffer (without this, ilclient_disable_port_buffers will hang)
    error = OMX_FillThisBuffer(ILC_GET_HANDLE(camera_component), out);
    if (error != OMX_ErrorNone) {
      log_error("error filling camera buffer (3): 0x%x\n", error);
    }

    // Clear the callback
    ilclient_set_fill_buffer_done_callback(cam_client, NULL, 0);

#if ENABLE_PBUFFER_OPTIMIZATION_HACK
    // Revert pBuffer value of video_encode input buffer
    if (video_encode_input_buf != NULL) {
      log_debug("Reverting pBuffer to its original value\n");
      video_encode_input_buf->pBuffer = video_encode_input_buf_pBuffer_orig;
    }
#endif

    // Notify the main thread that the camera is stopped
    pthread_mutex_lock(&camera_finish_mutex);
    is_camera_finished = 1;
    pthread_cond_signal(&camera_finish_cond);
    pthread_mutex_unlock(&camera_finish_mutex);
  }
}

static int openmax_cam_open() {
  OMX_PARAM_PORTDEFINITIONTYPE cam_def;
  OMX_CONFIG_FRAMERATETYPE framerate;
  OMX_ERRORTYPE error;
  OMX_PARAM_PORTDEFINITIONTYPE portdef;
  OMX_PARAM_TIMESTAMPMODETYPE timestamp_mode;
  int r;

  cam_client = ilclient_init();
  if (cam_client == NULL) {
    log_error("openmax_cam_open: ilclient_init failed\n");
    return -1;
  }

  ilclient_set_fill_buffer_done_callback(cam_client, cam_fill_buffer_done, 0);

  // create camera_component
  error = ilclient_create_component(cam_client, &camera_component, "camera",
      ILCLIENT_DISABLE_ALL_PORTS |
      ILCLIENT_ENABLE_OUTPUT_BUFFERS);
  if (error != 0) {
    log_fatal("failed to create camera component: 0x%x\n", error);
    exit(EXIT_FAILURE);
  }
  component_list[n_component_list++] = camera_component;

  memset(&cam_def, 0, sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
  cam_def.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
  cam_def.nVersion.nVersion = OMX_VERSION;
  cam_def.nPortIndex = CAMERA_CAPTURE_PORT;

  error = OMX_GetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexParamPortDefinition, &cam_def);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to get camera %d port definition: 0x%x\n", CAMERA_CAPTURE_PORT, error);
    exit(EXIT_FAILURE);
  }

  // Configure port 71 (camera capture output)
  cam_def.format.video.nFrameWidth = video_width;
  cam_def.format.video.nFrameHeight = video_height;

  // nStride must be a multiple of 32 and equal to or larger than nFrameWidth.
  cam_def.format.video.nStride = (video_width+31)&~31;

  // nSliceHeight must be a multiple of 16.
  cam_def.format.video.nSliceHeight = (video_height+15)&~15;

  cam_def.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
  cam_def.format.video.xFramerate = fr_q16; // specify the frame rate in Q.16 (framerate * 2^16)

  // This specifies the input pixel format.
  // See http://www.khronos.org/files/openmax_il_spec_1_0.pdf for details.
  cam_def.format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
  cam_def.nBufferCountActual = N_BUFFER_COUNT_ACTUAL; // Affects to audio/video sync

  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexParamPortDefinition, &cam_def);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to set camera %d port definition: 0x%x\n", CAMERA_CAPTURE_PORT, error);
    exit(EXIT_FAILURE);
  }

  // Set frame rate
  memset(&framerate, 0, sizeof(OMX_CONFIG_FRAMERATETYPE));
  framerate.nSize = sizeof(OMX_CONFIG_FRAMERATETYPE);
  framerate.nVersion.nVersion = OMX_VERSION;
  framerate.nPortIndex = CAMERA_CAPTURE_PORT; // capture port
  framerate.xEncodeFramerate = fr_q16;
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigVideoFramerate, &framerate);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to set camera %d framerate: 0x%x\n", CAMERA_CAPTURE_PORT, error);
    exit(EXIT_FAILURE);
  }

  // Set timestamp mode (unnecessary?)
  memset(&timestamp_mode, 0, sizeof(OMX_PARAM_TIMESTAMPMODETYPE));
  timestamp_mode.nSize = sizeof(OMX_PARAM_TIMESTAMPMODETYPE);
  timestamp_mode.nVersion.nVersion = OMX_VERSION;
  timestamp_mode.eTimestampMode = OMX_TimestampModeRawStc;
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexParamCommonUseStcTimestamps, &timestamp_mode);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to set camera timestamp mode: 0x%x\n", error);
    exit(EXIT_FAILURE);
  }

  set_exposure_to_auto();

  // Set camera component to idle state
  if (ilclient_change_component_state(camera_component, OMX_StateIdle) == -1) {
    log_fatal("failed to set camera to idle state (perhaps you need to reboot the machine)\n");
    exit(EXIT_FAILURE);
  }

  if (is_clock_enabled) {
    // create clock component
    error = ilclient_create_component(cam_client, &clock_component, "clock",
        ILCLIENT_DISABLE_ALL_PORTS);
    if (error != 0) {
      log_fatal("failed to create clock component: 0x%x\n", error);
      exit(EXIT_FAILURE);
    }
    component_list[n_component_list++] = clock_component;

    // Set clock state
    OMX_TIME_CONFIG_CLOCKSTATETYPE clock_state;
    memset(&clock_state, 0, sizeof(OMX_TIME_CONFIG_CLOCKSTATETYPE));
    clock_state.nSize = sizeof(OMX_TIME_CONFIG_CLOCKSTATETYPE);
    clock_state.nVersion.nVersion = OMX_VERSION;
    clock_state.eState = OMX_TIME_ClockStateWaitingForStartTime;
    clock_state.nWaitMask = 1;
    error = OMX_SetParameter(ILC_GET_HANDLE(clock_component), OMX_IndexConfigTimeClockState, &clock_state);
    if (error != OMX_ErrorNone) {
      log_error("failed to set clock state: 0x%x\n", error);
    }

    // Set up tunnel from clock to camera
    set_tunnel(tunnel+n_tunnel,
        clock_component, CLOCK_OUTPUT_1_PORT,
        camera_component, CAMERA_INPUT_PORT);

    if (ilclient_setup_tunnel(tunnel+(n_tunnel++), 0, 0) != 0) {
      log_fatal("failed to setup tunnel from clock to camera\n");
      exit(EXIT_FAILURE);
    }
  } // if (is_clock_enabled)

  if (is_preview_enabled) {
    // Set up preview port
    memset(&portdef, 0, sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
    portdef.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
    portdef.nVersion.nVersion = OMX_VERSION;
    portdef.nPortIndex = CAMERA_PREVIEW_PORT;

    error = OMX_GetParameter(ILC_GET_HANDLE(camera_component),
        OMX_IndexParamPortDefinition, &portdef);
    if (error != OMX_ErrorNone) {
      log_fatal("failed to get camera preview %d port definition: 0x%x\n", CAMERA_PREVIEW_PORT, error);
      exit(EXIT_FAILURE);
    }

    portdef.format.video.nFrameWidth = video_width;
    portdef.format.video.nFrameHeight = video_height;
    portdef.format.video.nStride = (video_width+31)&~31;
    portdef.format.video.nSliceHeight = (video_height+15)&~15;
    portdef.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
    portdef.format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;

    error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
        OMX_IndexParamPortDefinition, &portdef);
    if (error != OMX_ErrorNone) {
      log_fatal("failed to set camera preview %d port definition: 0x%x\n", CAMERA_PREVIEW_PORT, error);
      exit(EXIT_FAILURE);
    }

    memset(&framerate, 0, sizeof(OMX_CONFIG_FRAMERATETYPE));
    framerate.nSize = sizeof(OMX_CONFIG_FRAMERATETYPE);
    framerate.nVersion.nVersion = OMX_VERSION;
    framerate.nPortIndex = CAMERA_PREVIEW_PORT;
    framerate.xEncodeFramerate = fr_q16;
    error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
        OMX_IndexConfigVideoFramerate, &framerate);
    if (error != OMX_ErrorNone) {
      log_fatal("failed to set camera preview %d framerate: 0x%x\n", CAMERA_PREVIEW_PORT, error);
      exit(EXIT_FAILURE);
    }

    // create render_component
    r = ilclient_create_component(cam_client, &render_component, "video_render",
        ILCLIENT_DISABLE_ALL_PORTS);
    if (r != 0) {
      log_fatal("failed to create render component: 0x%x\n", r);
      exit(EXIT_FAILURE);
    }
    component_list[n_component_list++] = render_component;

    // Set up tunnel from camera to video_render
    set_tunnel(tunnel+n_tunnel,
        camera_component, CAMERA_PREVIEW_PORT,
        render_component, VIDEO_RENDER_INPUT_PORT);

    if (ilclient_setup_tunnel(tunnel+(n_tunnel++), 0, 0) != 0) {
      log_fatal("failed to setup tunnel from camera to render\n");
      exit(EXIT_FAILURE);
    }

    // Set render component to executing state
    ilclient_change_component_state(render_component, OMX_StateExecuting);
  } // if (is_preview_enabled)

  if (is_clock_enabled) {
    // Set clock component to executing state
    ilclient_change_component_state(clock_component, OMX_StateExecuting);
  }

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
    log_error("cannot get video encode buffer\n");
    return 0;
  }
  if (encbuf != NULL) {
    // merge the previous buffer
    concat_buf = realloc(encbuf, encbuf_size + out->nFilledLen);
    if (concat_buf == NULL) {
      log_fatal("cannot allocate memory for concat_buf (%d bytes)\n", encbuf_size + out->nFilledLen);
      free(encbuf);
      exit(EXIT_FAILURE);
    }
    memcpy(concat_buf + encbuf_size, out->pBuffer, out->nFilledLen);
    buf = concat_buf;
    buf_len = encbuf_size + out->nFilledLen;
  } else {
    buf = out->pBuffer;
    buf_len = out->nFilledLen;
  }
  if (!(out->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) &&
      !(out->nFlags & OMX_BUFFERFLAG_CODECCONFIG)) {
    // There is remaining buffer for the current frame
    nal_unit_type = buf[4] & 0x1f;
    encbuf_size = buf_len;
    if (concat_buf != NULL) {
      encbuf = concat_buf;
      concat_buf = NULL;
    } else {
      encbuf = malloc(buf_len);
      if (encbuf == NULL) {
        log_fatal("cannot allocate memory for encbuf (%d bytes)\n", buf_len);
        exit(EXIT_FAILURE);
      }
      memcpy(encbuf, buf, buf_len);
    }
    is_endofnal = 0;
  } else {
    encbuf = NULL;
    encbuf_size = -1;

    nal_unit_type = buf[4] & 0x1f;
    if (nal_unit_type != 1 && nal_unit_type != 5) {
      log_debug("%d", nal_unit_type);
    }
    if (out->nFlags != 0x480 && out->nFlags != 0x490 &&
        out->nFlags != 0x430 && out->nFlags != 0x410 &&
        out->nFlags != 0x400 && out->nFlags != 0x510 &&
        out->nFlags != 0x530) {
      log_warn("\nnew flag (%d,nal=%d)\n", out->nFlags, nal_unit_type);
    }
    if (out->nFlags & OMX_BUFFERFLAG_DATACORRUPT) {
      log_warn("\n=== OMX_BUFFERFLAG_DATACORRUPT ===\n");
    }
    if (out->nFlags & OMX_BUFFERFLAG_EXTRADATA) {
      log_warn("\n=== OMX_BUFFERFLAG_EXTRADATA ===\n");
    }
    if (out->nFlags & OMX_BUFFERFLAG_FRAGMENTLIST) {
      log_warn("\n=== OMX_BUFFERFLAG_FRAGMENTLIST ===\n");
    }
    if (out->nFlags & OMX_BUFFERFLAG_DISCONTINUITY) {
      log_warn("\n=== OMX_BUFFERFLAG_DISCONTINUITY ===\n");
    }
    if (out->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
      // Insert timing info to nal_unit_type 7
      codec_configs[n_codec_configs] = malloc(buf_len);
      if (codec_configs[n_codec_configs] == NULL) {
        log_fatal("cannot allocate memory for codec config (%d bytes)\n", buf_len);
        exit(EXIT_FAILURE);
      }
      codec_config_sizes[n_codec_configs] = buf_len;
      memcpy(codec_configs[n_codec_configs], buf, buf_len);

      codec_config_total_size += codec_config_sizes[n_codec_configs];
      n_codec_configs++;

      send_video_frame(buf, buf_len, 0);
    } else { // video frame
      frame_count++; // will be used for printing stats about FPS, etc.

      if (out->nFlags & OMX_BUFFERFLAG_SYNCFRAME) { // keyframe
        if (nal_unit_type != 5) {
          log_debug("SYNCFRAME nal_unit_type=%d len=%d\n", nal_unit_type, buf_len);
        }
        int consume_time = 0;
        if (nal_unit_type == 1 || nal_unit_type == 2 ||
            nal_unit_type == 3 || nal_unit_type == 4 ||
            nal_unit_type == 5) {
          consume_time = 1;
        } else {
          log_debug("(nosl)");
        }
#if !(AUDIO_ONLY)
        send_keyframe(buf, buf_len, consume_time);
#endif

        // calculate FPS and display it
        if (tsBegin.tv_sec != 0 && tsBegin.tv_nsec != 0) {
          keyframes_count++;
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
          log_info(" %5.2f fps k=%d", fps, keyframes_count);
          if (log_get_level() <= LOG_LEVEL_DEBUG) {
            print_audio_timing();
          } else {
            log_info("\n");
          }
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
            log_debug("(nosl)");
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
  OMX_PARAM_PORTDEFINITIONTYPE portdef, portdef_encode_output;
  OMX_ERRORTYPE error;
  int r;

  ilclient = ilclient_init();
  if (ilclient == NULL) {
    log_fatal("video_encode_startup: ilclient_init failed\n");
    return -1;
  }

  // create video_encode component
  r = ilclient_create_component(ilclient, &video_encode, "video_encode",
      ILCLIENT_DISABLE_ALL_PORTS |
      ILCLIENT_ENABLE_INPUT_BUFFERS |
      ILCLIENT_ENABLE_OUTPUT_BUFFERS);
  if (r != 0) {
    log_fatal("failed to create video_encode component: 0x%x\n", r);
    exit(EXIT_FAILURE);
  }
  component_list[n_component_list++] = video_encode;

  memset(&portdef, 0, sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
  portdef.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
  portdef.nVersion.nVersion = OMX_VERSION;
  portdef.nPortIndex = VIDEO_ENCODE_INPUT_PORT;

  error = OMX_GetParameter(ILC_GET_HANDLE(video_encode), OMX_IndexParamPortDefinition, &portdef);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to get video_encode %d port definition: 0x%x\n", VIDEO_ENCODE_INPUT_PORT, error);
    exit(EXIT_FAILURE);
  }

  // Configure port 200 (video_encode input)
  portdef.format.video.nFrameWidth = video_width;
  portdef.format.video.nFrameHeight = video_height;
  portdef.format.video.xFramerate = fr_q16; // specify the frame rate in Q.16 (framerate * 2^16)
  portdef.format.video.nBitrate = 0x0;
  portdef.format.video.nSliceHeight = portdef.format.video.nFrameHeight;
  portdef.format.video.nStride = portdef.format.video.nFrameWidth;
  portdef.nBufferCountActual = N_BUFFER_COUNT_ACTUAL;

  // This specifies the input pixel format.
  // See http://www.khronos.org/files/openmax_il_spec_1_0.pdf for details.
  portdef.format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;

  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamPortDefinition, &portdef);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to set video_encode %d port definition: 0x%x\n",
        VIDEO_ENCODE_INPUT_PORT, error);
    exit(EXIT_FAILURE);
  }

  memset(&portdef_encode_output, 0, sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
  portdef_encode_output.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
  portdef_encode_output.nVersion.nVersion = OMX_VERSION;
  portdef_encode_output.nPortIndex = VIDEO_ENCODE_OUTPUT_PORT;

  error = OMX_GetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamPortDefinition, &portdef_encode_output);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to get video_encode %d port definition: 0x%x\n",
        VIDEO_ENCODE_OUTPUT_PORT, error);
    exit(EXIT_FAILURE);
  }

  // Configure port 201 (video_encode output)
  portdef_encode_output.nBufferCountActual = N_BUFFER_COUNT_ACTUAL;

  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamPortDefinition, &portdef_encode_output);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to set video_encode %d port definition: 0x%x\n",
        VIDEO_ENCODE_OUTPUT_PORT, error);
    exit(EXIT_FAILURE);
  }

  memset(&format, 0, sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE));
  format.nSize = sizeof(OMX_VIDEO_PARAM_PORTFORMATTYPE);
  format.nVersion.nVersion = OMX_VERSION;
  format.nPortIndex = VIDEO_ENCODE_OUTPUT_PORT;
  format.eCompressionFormat = OMX_VIDEO_CodingAVC;

  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamVideoPortFormat, &format);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to set video_encode %d port format: 0x%x\n",
        VIDEO_ENCODE_OUTPUT_PORT, error);
    exit(EXIT_FAILURE);
  }

  memset(&avctype, 0, sizeof(OMX_VIDEO_PARAM_AVCTYPE));
  avctype.nSize = sizeof(OMX_VIDEO_PARAM_AVCTYPE);
  avctype.nVersion.nVersion = OMX_VERSION;
  avctype.nPortIndex = VIDEO_ENCODE_OUTPUT_PORT;

  error = OMX_GetParameter (ILC_GET_HANDLE(video_encode), OMX_IndexParamVideoAvc, &avctype);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to get video_encode %d AVC: 0x%x\n", VIDEO_ENCODE_OUTPUT_PORT, error);
    exit(EXIT_FAILURE);
  }

  // Number of P frames between I frames
  avctype.nPFrames = video_gop_size - 1;

  // Number of B frames between I frames
  avctype.nBFrames = 0;

  // Set the profile to Constrained Baseline Profile, Level 3.1
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

  // Set AVC parameter
  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamVideoAvc, &avctype);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to set video_encode %d AVC: 0x%x\n", VIDEO_ENCODE_OUTPUT_PORT, error);
    exit(EXIT_FAILURE);
  }

  // Set the bitrate
  memset(&bitrate_type, 0, sizeof(OMX_VIDEO_PARAM_BITRATETYPE));
  bitrate_type.nSize = sizeof(OMX_VIDEO_PARAM_BITRATETYPE);
  bitrate_type.nVersion.nVersion = OMX_VERSION;
  bitrate_type.nPortIndex = VIDEO_ENCODE_OUTPUT_PORT;
  bitrate_type.eControlRate = OMX_Video_ControlRateVariable; // TODO: Is this OK?
  bitrate_type.nTargetBitrate = video_bitrate; // in bits per second

  // Set bitrate
  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamVideoBitrate, &bitrate_type);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to set video_encode %d bitrate: 0x%x\n", VIDEO_ENCODE_OUTPUT_PORT, error);
    exit(EXIT_FAILURE);
  }

  // Separate buffers for each NAL unit
  memset(&boolean_type, 0, sizeof(OMX_CONFIG_BOOLEANTYPE));
  boolean_type.nSize = sizeof(OMX_CONFIG_BOOLEANTYPE);
  boolean_type.nVersion.nVersion = OMX_VERSION;
  boolean_type.bEnabled = OMX_TRUE;

  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamBrcmNALSSeparate, &boolean_type);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to set video_encode NAL separate: 0x%x\n", error);
    exit(EXIT_FAILURE);
  }

  // Set video_encode component to idle state
  log_debug("Set video_encode state to idle\n");
  if (ilclient_change_component_state(video_encode, OMX_StateIdle) == -1) {
    log_fatal("failed to set video_encode to idle state\n");
    exit(EXIT_FAILURE);
  }

  // Enable port buffers for port 71 (camera capture output)
  log_debug("Enable port buffers for camera %d\n", CAMERA_CAPTURE_PORT);
  if (ilclient_enable_port_buffers(camera_component, CAMERA_CAPTURE_PORT, NULL, NULL, NULL) != 0) {
    log_fatal("failed to enable port buffers for camera %d\n", CAMERA_CAPTURE_PORT);
    exit(EXIT_FAILURE);
  }

  // Enable port buffers for port 200 (video_encode input)
  log_debug("Enable port buffers for video_encode %d\n", VIDEO_ENCODE_INPUT_PORT);
  if (ilclient_enable_port_buffers(video_encode, VIDEO_ENCODE_INPUT_PORT, NULL, NULL, NULL) != 0) {
    log_fatal("failed to enable port buffers for video_encode %d\n", VIDEO_ENCODE_INPUT_PORT);
    exit(EXIT_FAILURE);
  }

  // Enable port buffers for port 201 (video_encode output)
  log_debug("Enable port buffers for video_encode %d\n", VIDEO_ENCODE_OUTPUT_PORT);
  if (ilclient_enable_port_buffers(video_encode, VIDEO_ENCODE_OUTPUT_PORT, NULL, NULL, NULL) != 0) {
    log_fatal("failed to enable port buffers for video_encode %d\n", VIDEO_ENCODE_OUTPUT_PORT);
    exit(EXIT_FAILURE);
  }

  log_debug("Set camera state to executing\n");
  // Set camera component to executing state
  ilclient_change_component_state(camera_component, OMX_StateExecuting);

  log_debug("Set video_encode state to executing\n");
  // Set video_encode component to executing state
  ilclient_change_component_state(video_encode, OMX_StateExecuting);

  return 0;
}

static void encode_and_send_image() {
  OMX_BUFFERHEADERTYPE *buf;
  OMX_BUFFERHEADERTYPE *out;
  OMX_ERRORTYPE error;

  buf = ilclient_get_input_buffer(video_encode, VIDEO_ENCODE_INPUT_PORT, 1);
  if (buf == NULL) {
    log_error("cannot get the encoded video buffer\n");
    exit(EXIT_FAILURE);
  }

#if ENABLE_PBUFFER_OPTIMIZATION_HACK
  if (video_encode_input_buf == NULL) {
    video_encode_input_buf = buf;
    video_encode_input_buf_pBuffer_orig = buf->pBuffer;
    buf->pBuffer = last_video_buffer;
    // buf->pBuffer has to be reverted back to its
    // original value before disabling port at the end.
  }
  // Both camera output and video_encode input have only one buffer
  assert(buf->pBuffer == last_video_buffer);
#else
  memcpy(buf->pBuffer, last_video_buffer, last_video_buffer_size);
#endif
  buf->nFilledLen = last_video_buffer_size;

  // OMX_EmptyThisBuffer takes 22000-27000 usec at 1920x1080
  error = OMX_EmptyThisBuffer(ILC_GET_HANDLE(video_encode), buf);
  if (error != OMX_ErrorNone) {
    log_error("error emptying buffer: 0x%x\n", error);
  }

  out = ilclient_get_output_buffer(video_encode, VIDEO_ENCODE_OUTPUT_PORT, 1);

  while (1) {
    error = OMX_FillThisBuffer(ILC_GET_HANDLE(video_encode), out);
    if (error != OMX_ErrorNone) {
      log_error("error filling video_encode buffer: 0x%x\n", error);
    }

    if (out->nFilledLen > 0) {
      video_encode_fill_buffer_done(out);
    } else {
      log_debug("E(%d)", out->nFlags);
      break;
    }

    if (out->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) {
      break;
      // If out->nFlags doesn't have ENDOFFRAME,
      // there is remaining buffer for this frame.
    }
  }

  if (is_auto_exposure_enabled) {
    if (video_frame_count > 0 && video_frame_count % (int)video_fps == 0) {
      auto_select_exposure(video_width, video_height, last_video_buffer);
    }
  }
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
    log_error("error encoding audio frame\n");
    exit(EXIT_FAILURE);
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

    if (is_tcpout_enabled) {
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
    }

    if (is_hlsout_enabled) {
      pthread_mutex_lock(&mutex_writing);
      ret = hls_write_packet(hls, &pkt, 0);
      pthread_mutex_unlock(&mutex_writing);
      if (ret < 0) {
        log_error("audio frame write error (hls): %d\n", ret);
        log_error("check if the filesystem is not full\n");
      }
    }

    av_free_packet(&pkt);

    current_audio_frames++;
  } else {
    log_error("not getting audio output");
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
      log_fatal("microphone: SUSPEND recovery failed: %s\n", snd_strerror(error));
      exit(EXIT_FAILURE);
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
        log_debug("S");
        if ( (error = snd_pcm_start(capture_handle)) < 0) {
          log_fatal("microphone: start error: %s\n", snd_strerror(error));
          exit(EXIT_FAILURE);
        }
        break;

      case 0:
        log_debug("not first audio");
        // wait for pcm to become ready
        if ( (error = snd_pcm_wait(capture_handle, -1)) < 0) {
          if ((error = xrun_recovery(capture_handle, error)) < 0) {
            log_fatal("microphone: snd_pcm_wait error: %s\n", snd_strerror(error));
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
        log_fatal("microphone: mmap begin avail error: %s\n", snd_strerror(error));
        exit(EXIT_FAILURE);
      }
      is_first_audio = 1;
    } 
    memcpy(this_samples + read_size, (my_areas[0].addr)+(offset*sizeof(short)*channels), frames*sizeof(short)*channels);
    read_size += frames;

    commitres = snd_pcm_mmap_commit(capture_handle, offset, frames);
    if (commitres < 0 || (snd_pcm_uframes_t)commitres != frames) {
      if ((error = xrun_recovery(capture_handle, commitres >= 0 ? commitres : -EPIPE)) < 0) {
        log_fatal("microphone: mmap commit error: %s\n", snd_strerror(error));
        exit(EXIT_FAILURE);
      }
      is_first_audio = 1;
    }
    size -= frames; // needed in the condition of the while loop to check if period is filled
  }

  if (audio_volume_multiply != 1.0) {
    int total_samples = period_size * channels;
    int i;
    for (i = 0; i < total_samples; i++) {
      int16_t value = (int16_t)this_samples[i];
      if (value < audio_min_value) { // negative overflow of audio volume
        log_info("o-");
        value = -32768;
      } else if (value > audio_max_value) { // positive overflow audio volume
        log_info("o+");
        value = 32767;
      } else {
        value = (int16_t)(value * audio_volume_multiply);
      }
      this_samples[i] = (uint16_t)value;
    }
  }

#if AUDIO_BUFFER_CHUNKS > 0
  if (++audio_buffer_index == AUDIO_BUFFER_CHUNKS) {
    audio_buffer_index = 0;
    if (!is_audio_buffer_filled) {
      log_debug("audio buffer filled\n");
      is_audio_buffer_filled = 1;
    }
  }
#endif

  return 0;
}

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
    log_fatal("failed to start clock: 0x%x\n", error);
    exit(EXIT_FAILURE);
  }
}

static void stop_openmax_clock() {
  OMX_TIME_CONFIG_CLOCKSTATETYPE clock_state;
  OMX_ERRORTYPE error;

  memset(&clock_state, 0, sizeof(OMX_TIME_CONFIG_CLOCKSTATETYPE));
  clock_state.nSize = sizeof(OMX_TIME_CONFIG_CLOCKSTATETYPE);
  clock_state.nVersion.nVersion = OMX_VERSION;
  clock_state.eState = OMX_TIME_ClockStateStopped;

  error = OMX_SetParameter(ILC_GET_HANDLE(clock_component),
      OMX_IndexConfigTimeClockState, &clock_state);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to stop clock: 0x%x\n", error);
    exit(EXIT_FAILURE);
  }
}

static void start_openmax_capturing() {
  OMX_CONFIG_PORTBOOLEANTYPE boolean;
  OMX_ERRORTYPE error;

  memset(&boolean, 0, sizeof(OMX_CONFIG_PORTBOOLEANTYPE));
  boolean.nSize = sizeof(OMX_CONFIG_PORTBOOLEANTYPE);
  boolean.nVersion.nVersion = OMX_VERSION;
  boolean.nPortIndex = CAMERA_CAPTURE_PORT;
  boolean.bEnabled = OMX_TRUE;

  log_debug("start capturing video\n");
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigPortCapturing, &boolean);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to start capturing video: 0x%x\n", error);
    exit(EXIT_FAILURE);
  }

  if (is_clock_enabled) {
    start_openmax_clock();
  }
}

static void stop_openmax_capturing() {
  OMX_CONFIG_PORTBOOLEANTYPE boolean;
  OMX_ERRORTYPE error;

  if (is_clock_enabled) {
    stop_openmax_clock();
  }

  memset(&boolean, 0, sizeof(OMX_CONFIG_PORTBOOLEANTYPE));
  boolean.nSize = sizeof(OMX_CONFIG_PORTBOOLEANTYPE);
  boolean.nVersion.nVersion = OMX_VERSION;
  boolean.nPortIndex = CAMERA_CAPTURE_PORT;
  boolean.bEnabled = OMX_FALSE;

  log_debug("stop capturing video\n");
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigPortCapturing, &boolean);
  if (error != OMX_ErrorNone) {
    log_fatal("failed to stop capturing video: 0x%x\n", error);
    exit(EXIT_FAILURE);
  }
}

static void openmax_cam_loop() {
  OMX_BUFFERHEADERTYPE *out;
  OMX_ERRORTYPE error;

  start_openmax_capturing();

  log_debug("waiting for the first video buffer\n");
  out = ilclient_get_output_buffer(camera_component, CAMERA_CAPTURE_PORT, 1);

  error = OMX_FillThisBuffer(ILC_GET_HANDLE(camera_component), out);
  if (error != OMX_ErrorNone) {
    log_error("error filling camera buffer (1): 0x%x\n", error);
  }
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
          log_error("nanosleep error:%d\n", ret);
        }
      }
    } else {
      // sleep 100ms
      ts.tv_sec = 0;
      ts.tv_nsec = 100000000;
      ret = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
      if (ret != 0) {
        log_error("nanosleep error:%d\n", ret);
      }
    }
  }
  pthread_exit(0);
}

static void audio_loop_poll_mmap() {
  int avail_flags;

  while (keepRunning) {
    if (is_first_audio) {
      read_audio_poll_mmap();
      // We ignore the first audio frame.
      // Because there is always a big delay after
      // the first and the second frame.
    }

    avail_flags = wait_for_poll(capture_handle, poll_fds, audio_fd_count);
    if (avail_flags < 0) { // try to recover from error
      if (keepRunning) {
        log_error("trying to recover from error\n");
      }
      if (snd_pcm_state(capture_handle) == SND_PCM_STATE_XRUN ||
          snd_pcm_state(capture_handle) == SND_PCM_STATE_SUSPENDED) {
        avail_flags = snd_pcm_state(capture_handle) == SND_PCM_STATE_XRUN ? -EPIPE : -ESTRPIPE;
        if (xrun_recovery(capture_handle, avail_flags) < 0) {
          log_fatal("microphone: write error: %s\n", snd_strerror(avail_flags));
          exit(EXIT_FAILURE);
        }
        is_first_audio = 1;
      } else {
        if (keepRunning) {
          log_error("microphone: wait for poll failed\n");
        }
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
            send_audio_start_time();
            send_video_start_time();
          }
        }
        if (is_video_recording_started == 1) {
          if (audio_pending_drop_frames > 0) {
            log_debug("dA");
            audio_pending_drop_frames--;
          } else {
            if (is_audio_muted) {
              memset(samples, 0, period_size * sizeof(short) * channels);
            }
            encode_and_send_audio();
          }
        }
      }
    }
  } // end of while loop (keepRunning)
}

static void setup_tcp_output() {
  avformat_network_init();
  tcp_ctx = mpegts_create_context(&codec_settings);
  mpegts_open_stream(tcp_ctx, tcp_output_dest, 0);
}

static void teardown_tcp_output() {
  mpegts_close_stream(tcp_ctx);
  mpegts_destroy_context(tcp_ctx);
  pthread_mutex_destroy(&tcp_mutex);
}

// Check if hls_output_dir is accessible.
// Also create HLS output directory if it doesn't exist.
static void ensure_hls_dir_exists() {
  struct stat st;
  int err;

  err = stat(hls_output_dir, &st);
  if (err == -1) {
    if (errno == ENOENT) {
      // create directory
      if (mkdir(hls_output_dir, 0755) == 0) { // success
        log_info("created HLS output directory: %s\n", hls_output_dir);
      } else { // error
        log_error("error creating hls_output_dir (%s): %s\n",
            hls_output_dir, strerror(errno));
        exit(EXIT_FAILURE);
      }
    } else {
      perror("stat hls_output_dir");
      exit(EXIT_FAILURE);
    }
  } else {
    if (!S_ISDIR(st.st_mode)) {
      log_error("hls_output_dir (%s) is not a directory\n",
          hls_output_dir);
      exit(EXIT_FAILURE);
    }
  }

  if (access(hls_output_dir, R_OK) != 0) {
    log_error("cannot access hls_output_dir (%s): %s\n",
        hls_output_dir, strerror(errno));
    exit(EXIT_FAILURE);
  }
}

static void print_usage() {
  log_info(PROGRAM_NAME " version " PROGRAM_VERSION "\n");
  log_info("Usage: " PROGRAM_NAME " [options]\n");
  log_info("\n");
  log_info("Options:\n");
  log_info(" [video]\n");
  log_info("  -w, --width         Width in pixels (default: %d)\n", video_width_default);
  log_info("  -h, --height        Height in pixels (default: %d)\n", video_height_default);
//  log_info("  -f, --fps           Frame rate (default: %d)\n", video_fps_default);
  log_info("  -v, --videobitrate  Video bit rate (default: %ld)\n", video_bitrate_default);
  log_info("  -g, --gopsize       GOP size (default: %d)\n", video_gop_size_default);
  log_info(" [audio]\n");
  log_info("  -r, --samplerate    Audio sample rate (default: %d)\n", audio_sample_rate_default);
  log_info("  -a, --audiobitrate  Audio bit rate (default: %ld)\n", audio_bitrate_default);
  log_info("  --alsadev <dev>     ALSA microphone device (default: %s)\n", alsa_dev_default);
  log_info("  --volume <num>      Amplify audio by multiplying the volume by <num>\n");
  log_info("                      (default: %.1f)\n", audio_volume_multiply_default);
  log_info(" [HTTP Live Streaming (HLS)]\n");
  log_info("  -o, --hlsdir <dir>  Generate HTTP Live Streaming files in <dir>\n");
  log_info("  --hlsenc            Enable HLS encryption\n");
  log_info("  --hlsenckeyuri <uri>  Set HLS encryption key URI (default: %s)\n", hls_encryption_key_uri_default);
  log_info("  --hlsenckey <hex>   Set HLS encryption key in hex string\n");
  log_info("                      (default: ");
  log_hex(LOG_LEVEL_INFO, hls_encryption_key, sizeof(hls_encryption_key));
  log_info(")\n");
  log_info("  --hlsenciv <hex>    Set HLS encryption IV in hex string\n");
  log_info("                      (default: ");
  log_hex(LOG_LEVEL_INFO, hls_encryption_iv, sizeof(hls_encryption_iv));
  log_info(")\n");
  log_info(" [output for node-rtsp-rtmp-server]\n");
  log_info("  --rtspout           Enable output for node-rtsp-rtmp-server\n");
  log_info("  --rtspvideocontrol <path>  Set video control socket path\n");
  log_info("                      (default: %s)\n", rtsp_video_control_path_default);
  log_info("  --rtspaudiocontrol <path>  Set audio control socket path\n");
  log_info("                      (default: %s)\n", rtsp_audio_control_path_default);
  log_info("  --rtspvideodata <path>  Set video data socket path\n");
  log_info("                      (default: %s)\n", rtsp_video_data_path_default);
  log_info("  --rtspaudiodata <path>  Set audio data socket path\n");
  log_info("                      (default: %s)\n", rtsp_audio_data_path_default);
  log_info(" [MPEG-TS output via TCP]\n");
  log_info("  --tcpout <url>      Enable TCP output to <url>\n");
  log_info("                      (e.g. --tcpout tcp://127.0.0.1:8181)\n");
  log_info(" [camera]\n");
  log_info("  --autoexposure      Enable automatic changing of exposure\n");
  log_info("  --expnight <num>    Change the exposure to night mode if the average\n");
  log_info("                      value of Y (brightness) is <= <num> while in\n");
  log_info("                      daylight mode (default: %d)\n", exposure_night_y_threshold_default);
  log_info("  --expday <num>      Change the exposure to daylight mode if the average\n");
  log_info("                      value of Y (brightness) is >= <num> while in\n");
  log_info("                      night mode (default: %d)\n", exposure_auto_y_threshold_default);
  log_info("  -p, --preview       Display a preview window for video\n");
  log_info(" [misc]\n");
  log_info("  --recordbuf <num>   Start recording from <num> keyframes ago\n");
  log_info("                      (default: %d)\n", record_buffer_keyframes_default);
  log_info("  --statedir <dir>    Set state dir (default: %s)\n", state_dir_default);
  log_info("  --hooksdir <dir>    Set hooks dir (default: %s)\n", hooks_dir_default);
  log_info("  -q, --quiet         Turn off most of the log messages\n");
  log_info("  --help              Print this help\n");
}

int main(int argc, char **argv) {
  int ret;

  static struct option long_options[] = {
    { "width", required_argument, NULL, 'w' },
    { "height", required_argument, NULL, 'h' },
//    { "fps", required_argument, NULL, 'f' },
    { "gopsize", required_argument, NULL, 'g' },
    { "videobitrate", required_argument, NULL, 'v' },
    { "alsadev", required_argument, NULL, 0 },
    { "audiobitrate", required_argument, NULL, 'a' },
    { "samplerate", required_argument, NULL, 'r' },
    { "hlsdir", required_argument, NULL, 'o' },
    { "rtspout", no_argument, NULL, 0 },
    { "rtspvideocontrol", required_argument, NULL, 0 },
    { "rtspvideodata", required_argument, NULL, 0 },
    { "rtspaudiocontrol", required_argument, NULL, 0 },
    { "rtspaudiodata", required_argument, NULL, 0 },
    { "tcpout", required_argument, NULL, 0 },
    { "autoexposure", no_argument, NULL, 0 },
    { "expnight", required_argument, NULL, 0 },
    { "expday", required_argument, NULL, 0 },
    { "statedir", required_argument, NULL, 0 },
    { "hooksdir", required_argument, NULL, 0 },
    { "volume", required_argument, NULL, 0 },
    { "hlsenc", no_argument, NULL, 0 },
    { "hlsenckeyuri", required_argument, NULL, 0 },
    { "hlsenckey", required_argument, NULL, 0 },
    { "hlsenciv", required_argument, NULL, 0 },
    { "preview", no_argument, NULL, 'p' },
    { "quiet", no_argument, NULL, 'q' },
    { "recordbuf", required_argument, NULL, 0 },
    { "verbose", no_argument, NULL, 0 },
    { "help", no_argument, NULL, 0 },
    { 0, 0, 0, 0 },
  };
  int option_index = 0;
  int opt;

  // Turn off buffering for stdout
  setvbuf(stdout, NULL, _IONBF, 0);

  log_set_level(LOG_LEVEL_INFO);
  log_set_stream(stdout);

  // Set default values of options
  video_width = video_width_default;
  video_height = video_height_default;
  video_fps = video_fps_default;
  video_gop_size = video_gop_size_default;
  video_bitrate = video_bitrate_default;
  strncpy(alsa_dev, alsa_dev_default, sizeof(alsa_dev));
  audio_bitrate = audio_bitrate_default;
  audio_sample_rate = audio_sample_rate_default;
  is_hlsout_enabled = is_hlsout_enabled_default;
  strncpy(hls_output_dir, hls_output_dir_default, sizeof(hls_output_dir));
  is_rtspout_enabled = is_rtspout_enabled_default;
  strncpy(rtsp_video_control_path, rtsp_video_control_path_default,
      sizeof(rtsp_video_control_path));
  strncpy(rtsp_audio_control_path, rtsp_audio_control_path_default,
      sizeof(rtsp_audio_control_path));
  strncpy(rtsp_video_data_path, rtsp_video_data_path_default,
      sizeof(rtsp_video_data_path));
  strncpy(rtsp_audio_data_path, rtsp_audio_data_path_default,
      sizeof(rtsp_audio_data_path));
  is_tcpout_enabled = is_tcpout_enabled_default;
  is_auto_exposure_enabled = is_auto_exposure_enabled_default;
  exposure_night_y_threshold = exposure_night_y_threshold_default;
  exposure_auto_y_threshold = exposure_auto_y_threshold_default;
  strncpy(state_dir, state_dir_default, sizeof(state_dir));
  strncpy(hooks_dir, hooks_dir_default, sizeof(hooks_dir));
  audio_volume_multiply = audio_volume_multiply_default;
  is_hls_encryption_enabled = is_hls_encryption_enabled_default;
  strncpy(hls_encryption_key_uri, hls_encryption_key_uri_default,
      sizeof(hls_encryption_key_uri));
  is_preview_enabled = is_preview_enabled_default;
  record_buffer_keyframes = record_buffer_keyframes_default;

  while ((opt = getopt_long(argc, argv, "w:h:g:v:a:r:o:pq", long_options, &option_index)) != -1) {
    switch (opt) {
      case 0:
        if (long_options[option_index].flag != 0) {
          break;
        }
        if (strcmp(long_options[option_index].name, "alsadev") == 0) {
          strncpy(alsa_dev, optarg, sizeof(alsa_dev));
        } else if (strcmp(long_options[option_index].name, "rtspout") == 0) {
          is_rtspout_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "rtspvideocontrol") == 0) {
          strncpy(rtsp_video_control_path, optarg, sizeof(rtsp_video_control_path));
        } else if (strcmp(long_options[option_index].name, "rtspaudiocontrol") == 0) {
          strncpy(rtsp_audio_control_path, optarg, sizeof(rtsp_audio_control_path));
        } else if (strcmp(long_options[option_index].name, "rtspvideodata") == 0) {
          strncpy(rtsp_video_data_path, optarg, sizeof(rtsp_video_data_path));
        } else if (strcmp(long_options[option_index].name, "rtspaudiodata") == 0) {
          strncpy(rtsp_audio_data_path, optarg, sizeof(rtsp_audio_data_path));
        } else if (strcmp(long_options[option_index].name, "tcpout") == 0) {
          is_tcpout_enabled = 1;
          strncpy(tcp_output_dest, optarg, sizeof(tcp_output_dest));
        } else if (strcmp(long_options[option_index].name, "autoexposure") == 0) {
          is_auto_exposure_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "expnight") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("invalid expnight: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("invalid expnight: %ld (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          exposure_night_y_threshold = value;
        } else if (strcmp(long_options[option_index].name, "expday") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("invalid expday: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("invalid expday: %ld (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          exposure_auto_y_threshold = value;
        } else if (strcmp(long_options[option_index].name, "statedir") == 0) {
          strncpy(state_dir, optarg, sizeof(state_dir));
        } else if (strcmp(long_options[option_index].name, "hooksdir") == 0) {
          strncpy(hooks_dir, optarg, sizeof(hooks_dir));
        } else if (strcmp(long_options[option_index].name, "volume") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("invalid volume: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value < 0.0) {
            log_fatal("invalid volume: %.1f (must be >= 0.0)\n", value);
            return EXIT_FAILURE;
          }
          audio_volume_multiply = value;
        } else if (strcmp(long_options[option_index].name, "hlsenc") == 0) {
          is_hls_encryption_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "hlsenckeyuri") == 0) {
          strncpy(hls_encryption_key_uri, optarg, sizeof(hls_encryption_key_uri));
        } else if (strcmp(long_options[option_index].name, "hlsenckey") == 0) {
          int i;
          for (i = 0; i < 16; i++) {
            int value;
            if (sscanf(optarg + i * 2, "%02x", &value) == 1) {
              hls_encryption_key[i] = value;
            } else {
              log_fatal("invalid hlsenckey: %s\n", optarg);
              print_usage();
              return EXIT_FAILURE;
            }
          }
        } else if (strcmp(long_options[option_index].name, "hlsenciv") == 0) {
          int i;
          for (i = 0; i < 16; i++) {
            int value;
            if (sscanf(optarg + i * 2, "%02x", &value) == 1) {
              hls_encryption_iv[i] = value;
            } else {
              log_fatal("invalid hlsenciv: %s\n", optarg);
              print_usage();
              return EXIT_FAILURE;
            }
          }
        } else if (strcmp(long_options[option_index].name, "recordbuf") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("invalid recordbuf: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("invalid recordbuf: %ld (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          record_buffer_keyframes = value;
        } else if (strcmp(long_options[option_index].name, "verbose") == 0) {
          log_set_level(LOG_LEVEL_DEBUG);
        } else if (strcmp(long_options[option_index].name, "help") == 0) {
          print_usage();
          return EXIT_SUCCESS;
        }
        break;
      case 'w':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("invalid width: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("invalid width: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          video_width = value;
          break;
        }
      case 'h':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("invalid height: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("invalid height: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          video_height = value;
          break;
        }
      case 'f':
        {
          char *end;
          double value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("invalid fps: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0.0) {
            log_fatal("invalid fps: %.1f (must be > 0.0)\n", value);
            return EXIT_FAILURE;
          }
          video_fps = value;
          break;
        }
      case 'g':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("invalid gopsize: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("invalid gopsize: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          video_gop_size = value;
          break;
        }
      case 'v':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("invalid videobitrate: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("invalid videobitrate: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          video_bitrate = value;
          break;
        }
      case 'a':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("invalid audiobitrate: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("invalid audiobitrate: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          audio_bitrate = value;
          break;
        }
      case 'r':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("invalid samplerate: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("invalid samplerate: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          audio_sample_rate = value;
          break;
        }
      case 'o':
        is_hlsout_enabled = 1;
        strncpy(hls_output_dir, optarg, sizeof(hls_output_dir));
        break;
      case 'p':
        is_preview_enabled = 1;
        break;
      case 'q':
        log_set_level(LOG_LEVEL_ERROR);
        break;
      default:
        print_usage();
        return EXIT_FAILURE;
    }
  }

  fr_q16 = video_fps * 65536;
  mpegts_set_config(video_bitrate, video_width, video_height);
  audio_min_value = (int) (-32768 / audio_volume_multiply);
  audio_max_value = (int) (32767 / audio_volume_multiply);
  keyframe_pointers = calloc(sizeof(int) * record_buffer_keyframes, 1);
  if (keyframe_pointers == NULL) {
    log_fatal("cannot allocate memory for keyframe_pointers\n");
    exit(EXIT_FAILURE);
  }

  log_debug("video_width=%d\n", video_width);
  log_debug("video_height=%d\n", video_height);
  log_debug("video_fps=%.1f\n", video_fps);
  log_debug("gop_size=%d\n", video_gop_size);
  log_debug("video_bitrate=%ld\n", video_bitrate);
  log_debug("alsa_dev=%s\n", alsa_dev);
  log_debug("audio_sample_rate=%d\n", audio_sample_rate);
  log_debug("audio_bitrate=%ld\n", audio_bitrate);
  log_debug("audio_volume_multiply=%f\n", audio_volume_multiply);
  log_debug("is_hlsout_enabled=%d\n", is_hlsout_enabled);
  log_debug("is_hls_encryption_enabled=%d\n", is_hls_encryption_enabled);
  log_debug("hls_encryption_key_uri=%s\n", hls_encryption_key_uri);
  log_debug("hls_encryption_key=0x");
  log_hex(LOG_LEVEL_DEBUG, hls_encryption_key, sizeof(hls_encryption_key));
  log_debug("\n");
  log_debug("hls_encryption_iv=0x");
  log_hex(LOG_LEVEL_DEBUG, hls_encryption_iv, sizeof(hls_encryption_iv));
  log_debug("\n");
  log_debug("hls_output_dir=%s\n", hls_output_dir);
  log_debug("rtsp_enabled=%d\n", is_rtspout_enabled);
  log_debug("rtsp_video_control_path=%s\n", rtsp_video_control_path);
  log_debug("rtsp_audio_control_path=%s\n", rtsp_audio_control_path);
  log_debug("rtsp_video_data_path=%s\n", rtsp_video_data_path);
  log_debug("rtsp_audio_data_path=%s\n", rtsp_audio_data_path);
  log_debug("tcp_enabled=%d\n", is_tcpout_enabled);
  log_debug("tcp_output_dest=%s\n", tcp_output_dest);
  log_debug("auto_exposure_enabled=%d\n", is_auto_exposure_enabled);
  log_debug("exposure_night_y_threshold=%d\n", exposure_night_y_threshold);
  log_debug("exposure_auto_y_threshold=%d\n", exposure_auto_y_threshold);
  log_debug("is_preview_enabled=%d\n", is_preview_enabled);
  log_debug("record_buffer_keyframes=%d\n", record_buffer_keyframes);
  log_debug("state_dir=%s\n", state_dir);
  log_debug("hooks_dir=%s\n", hooks_dir);

  if (state_create_dir(state_dir) != 0) {
    return EXIT_FAILURE;
  }
  if (hooks_create_dir(hooks_dir) != 0) {
    return EXIT_FAILURE;
  }

  codec_settings.audio_sample_rate = audio_sample_rate;
  codec_settings.audio_bit_rate = audio_bitrate;
  codec_settings.audio_channels = 1;
  codec_settings.audio_profile = FF_PROFILE_AAC_LOW;

  create_dir(rec_dir);
  create_dir(rec_tmp_dir);
  create_dir(rec_archive_dir);

  if (is_hlsout_enabled) {
    ensure_hls_dir_exists();
  }

  state_set(state_dir, "record", "false");

  if (clear_hooks(hooks_dir) != 0) {
    log_error("clear_hooks() failed\n");
  }
  start_watching_hooks(&hooks_thread, hooks_dir, on_file_create, 1);

  setup_socks();

  if (is_tcpout_enabled) {
    setup_tcp_output();
  }

  if (is_preview_enabled || is_clock_enabled) {
    memset(tunnel, 0, sizeof(tunnel));
  }

  bcm_host_init();

  ret = OMX_Init();
  if (ret != OMX_ErrorNone) {
    log_fatal("OMX_Init failed: 0x%x\n", ret);
    ilclient_destroy(ilclient);
    return 1;
  }
  memset(component_list, 0, sizeof(component_list));

  ret = openmax_cam_open();
  if (ret != 0) {
    log_fatal("openmax_cam_open failed: %d\n", ret);
    return ret;
  }
  ret = video_encode_startup();
  if (ret != 0) {
    log_fatal("video_encode_startup failed: %d\n", ret);
    return ret;
  }

  av_log_set_level(AV_LOG_INFO);

  if (!disable_audio_capturing) {
    ret = open_audio_capture_device();
    if (ret == -1) {
      log_warn("### WARNING: audio stream is disabled ###\n");
      disable_audio_capturing = 1;
    } else if (ret < 0) {
      log_fatal("init_audio failed: %d\n", ret);
      exit(EXIT_FAILURE);
    }
  }

  if (disable_audio_capturing) {
    codec_settings.audio_bit_rate = 1000;
  }

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

  if (is_hlsout_enabled) {
    hls->dir = hls_output_dir;
    hls->target_duration = 1;
    hls->num_retained_old_files = 10;
    if (is_hls_encryption_enabled) {
      hls->use_encryption = 1;

      int uri_len = strlen(hls_encryption_key_uri) + 1;
      hls->encryption_key_uri = malloc(uri_len);
      if (hls->encryption_key_uri == NULL) {
        perror("malloc for hls->encryption_key_uri");
        return 1;
      }
      memcpy(hls->encryption_key_uri, hls_encryption_key_uri, uri_len);

      hls->encryption_key = malloc(16);
      if (hls->encryption_key == NULL) {
        perror("malloc for hls->encryption_key");
        return 1;
      }
      memcpy(hls->encryption_key, hls_encryption_key, 16);

      hls->encryption_iv = malloc(16);
      if (hls->encryption_iv == NULL) {
        perror("malloc for hls->encryption_iv");
        return 1;
      }
      memcpy(hls->encryption_iv, hls_encryption_iv, 16);
    } // if (enable_hls_encryption)
  }

  setup_av_frame(hls->format_ctx);

  if (disable_audio_capturing) {
    memset(samples, 0, period_size * sizeof(short) * channels);
    is_audio_recording_started = 1;
  } else {
    ret = configure_audio_capture_device();
    if (ret != 0) {
      log_fatal("configure_audio_capture_device error: ret=%d\n", ret);
      exit(EXIT_FAILURE);
    }
  }

  prepare_encoded_packets();

  struct sigaction int_handler = {.sa_handler = stopSignalHandler};
  sigaction(SIGINT, &int_handler, NULL);
  sigaction(SIGTERM, &int_handler, NULL);

  openmax_cam_loop();

  if (disable_audio_capturing) {
    pthread_create(&audio_nop_thread, NULL, audio_nop_loop, NULL);
    pthread_join(audio_nop_thread, NULL);
  } else {
    audio_loop_poll_mmap();
  }

  log_debug("shutdown sequence start\n");

  if (is_recording) {
    rec_thread_needs_write = 1;
    pthread_cond_signal(&rec_cond);

    stop_record();
    pthread_join(rec_thread, NULL);
  }

  pthread_mutex_lock(&camera_finish_mutex);
  // Wait for the camera to finish
  while (!is_camera_finished) {
    log_debug("waiting for the camera to finish\n");
    pthread_cond_wait(&camera_finish_cond, &camera_finish_mutex);
  }
  pthread_mutex_unlock(&camera_finish_mutex);

  stop_openmax_capturing();
  shutdown_openmax();
  shutdown_video();

  log_debug("teardown_audio_encode\n");
  teardown_audio_encode();

  if (!disable_audio_capturing) {
    log_debug("teardown_audio_capture_device\n");
    teardown_audio_capture_device();
  }

  log_debug("hls_destroy\n");
  hls_destroy(hls);

  log_debug("pthread_mutex_destroy\n");
  pthread_mutex_destroy(&mutex_writing);
  pthread_mutex_destroy(&rec_mutex);
  pthread_mutex_destroy(&rec_write_mutex);
  pthread_mutex_destroy(&camera_finish_mutex);
  pthread_cond_destroy(&rec_cond);
  pthread_cond_destroy(&camera_finish_cond);

  if (is_tcpout_enabled) {
    teardown_tcp_output();
  }

  log_debug("teardown_socks\n");
  teardown_socks();

  log_debug("free_encoded_packets\n");
  free_encoded_packets();
  log_debug("free keyframe_pointers\n");
  free(keyframe_pointers);

  log_debug("stop_watching_hooks\n");
  stop_watching_hooks();
  log_debug("pthread_join hooks_thread\n");
  pthread_join(hooks_thread, NULL);

  log_debug("shutdown successful");
  log_info("\n");
  return 0;
}

#if defined(__cplusplus)
}
#endif
