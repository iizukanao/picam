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
#include "text.h"
#include "dispmanx.h"
#include "timestamp.h"
#include "subtitle.h"

#define PROGRAM_NAME     "picam"
#define PROGRAM_VERSION  "1.4.7"

// Audio-only stream is created if this is 1 (for debugging)
#define AUDIO_ONLY 0

// ALSA buffer size for capture will be multiplied by this number
#define ALSA_BUFFER_MULTIPLY 100

// ALSA buffer size for playback will be multiplied by this number (max: 16)
#define ALSA_PLAYBACK_BUFFER_MULTIPLY 10

// If this is 1, PTS will be reset to zero when it exceeds PTS_MODULO
#define ENABLE_PTS_WRAP_AROUND 0

#if ENABLE_PTS_WRAP_AROUND
// Both PTS and DTS are 33 bit and wraps around to zero
#define PTS_MODULO 8589934592
#endif

// Internal flag indicates that audio is available for read
#define AVAIL_AUDIO 2

// If this value is increased, audio gets faster than video
#define N_BUFFER_COUNT_ACTUAL 1

// The number of buffers that are required on video_encode input port
#define VIDEO_ENCODE_INPUT_BUFFER_COUNT 2

// The number of buffers that are required on video_encode output port
#define VIDEO_ENCODE_OUTPUT_BUFFER_COUNT 2

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
#define ENABLE_PBUFFER_OPTIMIZATION_HACK 0

#if ENABLE_PBUFFER_OPTIMIZATION_HACK
static OMX_BUFFERHEADERTYPE *video_encode_input_buf = NULL;
static OMX_U8 *video_encode_input_buf_pBuffer_orig = NULL;
#endif

#define ENABLE_AUTO_GOP_SIZE_CONTROL_FOR_VFR 1

// OpenMAX IL ports
static const int CAMERA_PREVIEW_PORT      = 70;
static const int CAMERA_CAPTURE_PORT      = 71;
static const int CAMERA_INPUT_PORT        = 73;
static const int CLOCK_OUTPUT_1_PORT      = 80;
static const int VIDEO_RENDER_INPUT_PORT  = 90;
static const int VIDEO_ENCODE_INPUT_PORT  = 200;
static const int VIDEO_ENCODE_OUTPUT_PORT = 201;

// Directory to put recorded MPEG-TS files
static char *rec_dir = "rec";
static char *rec_tmp_dir = "rec/tmp";
static char *rec_archive_dir = "rec/archive";

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
static int sensor_mode;
static const int sensor_mode_default = -1;
static int video_width;
static const int video_width_default = 1280;
static int video_width_32;
static int video_height;
static const int video_height_default = 720;
static int video_height_16;
static float video_fps;
static const float video_fps_default = 30.0f;
static int video_pts_step;
static const int video_pts_step_default = 0;
static int video_gop_size;
static const int video_gop_size_default = 0;
static int video_rotation;
static const int video_rotation_default = 0;
static int video_hflip;
static const int video_hflip_default = 0;
static int video_vflip;
static const int video_vflip_default = 0;
static long video_bitrate;
static const long video_bitrate_default = 2000 * 1000; // 2 Mbps

static char video_avc_profile[21];
static const char *video_avc_profile_default = "constrained_baseline";
typedef struct video_avc_profile_option {
  char *name;
  OMX_VIDEO_AVCPROFILETYPE profile;
} video_avc_profile_option;
static const video_avc_profile_option video_avc_profile_options[] = {
  { .name = "constrained_baseline", .profile = OMX_VIDEO_AVCProfileConstrainedBaseline },
  { .name = "baseline",             .profile = OMX_VIDEO_AVCProfileBaseline },
  { .name = "main",                 .profile = OMX_VIDEO_AVCProfileMain },
  { .name = "high",                 .profile = OMX_VIDEO_AVCProfileHigh },
};

static char video_avc_level[4];
static const char *video_avc_level_default = "3.1";
typedef struct video_avc_level_option {
  char *name;
  OMX_VIDEO_AVCLEVELTYPE level;
} video_avc_level_option;
static const video_avc_level_option video_avc_level_options[] = {
  { .name = "1",   .level = OMX_VIDEO_AVCLevel1 },
  { .name = "1b",  .level = OMX_VIDEO_AVCLevel1b },
  { .name = "1.1", .level = OMX_VIDEO_AVCLevel11 },
  { .name = "1.2", .level = OMX_VIDEO_AVCLevel12 },
  { .name = "1.3", .level = OMX_VIDEO_AVCLevel13 },
  { .name = "2",   .level = OMX_VIDEO_AVCLevel2 },
  { .name = "2.1", .level = OMX_VIDEO_AVCLevel21 },
  { .name = "2.2", .level = OMX_VIDEO_AVCLevel22 },
  { .name = "3",   .level = OMX_VIDEO_AVCLevel3 },
  { .name = "3.1", .level = OMX_VIDEO_AVCLevel31 },
  { .name = "3.2", .level = OMX_VIDEO_AVCLevel32 },
  { .name = "4",   .level = OMX_VIDEO_AVCLevel4 },
  { .name = "4.1", .level = OMX_VIDEO_AVCLevel41 },
  { .name = "4.2", .level = OMX_VIDEO_AVCLevel42 },
  { .name = "5",   .level = OMX_VIDEO_AVCLevel5 },
  { .name = "5.1", .level = OMX_VIDEO_AVCLevel51 },
};

static int video_qp_min;
static const int video_qp_min_default = -1;
static int video_qp_max;
static const int video_qp_max_default = -1;
static int video_qp_initial;
static const int video_qp_initial_default = -1;
static int video_slice_dquant;
static const int video_slice_dquant_default = -1;
static char alsa_dev[256];
static const char *alsa_dev_default = "hw:0,0";
static char audio_preview_dev[256];
static const char *audio_preview_dev_default = "plughw:0,0";
static long audio_bitrate;
static const long audio_bitrate_default = 40000; // 40 Kbps
static int is_audio_channels_specified = 0;
static int audio_channels;
static int audio_preview_channels;
static const int audio_channels_default = 1; // mono
static int audio_sample_rate;
static const int audio_sample_rate_default = 48000;
static int is_hlsout_enabled;
static const int is_hlsout_enabled_default = 0;
static char hls_output_dir[256];
static const char *hls_output_dir_default = "/run/shm/video";
static int hls_keyframes_per_segment;
static const int hls_keyframes_per_segment_default = 1;
static int hls_number_of_segments;
static const int hls_number_of_segments_default = 3;
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
static int is_vfr_enabled; // whether variable frame rate is enabled
static const int is_vfr_enabled_default = 0;
static float auto_exposure_threshold;
static const float auto_exposure_threshold_default = 5.0f;

static float roi_left;
static const float roi_left_default = 0.0f;
static float roi_top;
static const float roi_top_default = 0.0f;
static float roi_width;
static const float roi_width_default = 1.0f;
static float roi_height;
static const float roi_height_default = 1.0f;

static char white_balance[13];
static const char *white_balance_default = "auto";
typedef struct white_balance_option {
  char *name;
  OMX_WHITEBALCONTROLTYPE control;
} white_balance_option;
static const white_balance_option white_balance_options[] = {
  { .name = "off",          .control = OMX_WhiteBalControlOff },
  { .name = "auto",         .control = OMX_WhiteBalControlAuto },
  { .name = "sun",          .control = OMX_WhiteBalControlSunLight },
  { .name = "cloudy",       .control = OMX_WhiteBalControlCloudy },
  { .name = "shade",        .control = OMX_WhiteBalControlShade },
  { .name = "tungsten",     .control = OMX_WhiteBalControlTungsten },
  { .name = "fluorescent",  .control = OMX_WhiteBalControlFluorescent },
  { .name = "incandescent", .control = OMX_WhiteBalControlIncandescent },
  { .name = "flash",        .control = OMX_WhiteBalControlFlash },
  { .name = "horizon",      .control = OMX_WhiteBalControlHorizon },
};

static char exposure_control[14];
static const char *exposure_control_default = "auto";
typedef struct exposure_control_option {
  char *name;
  OMX_EXPOSURECONTROLTYPE control;
} exposure_control_option;
static const exposure_control_option exposure_control_options[] = {
  { .name = "off",           .control = OMX_ExposureControlOff },
  { .name = "auto",          .control = OMX_ExposureControlAuto },
  { .name = "night",         .control = OMX_ExposureControlNight },
  { .name = "nightpreview",  .control = OMX_ExposureControlNightWithPreview },
  { .name = "backlight",     .control = OMX_ExposureControlBackLight },
  { .name = "spotlight",     .control = OMX_ExposureControlSpotLight },
  { .name = "sports",        .control = OMX_ExposureControlSports },
  { .name = "snow",          .control = OMX_ExposureControlSnow },
  { .name = "beach",         .control = OMX_ExposureControlBeach },
  { .name = "verylong",      .control = OMX_ExposureControlVeryLong },
  { .name = "fixedfps",      .control = OMX_ExposureControlFixedFps },
  { .name = "antishake",     .control = OMX_ExposureControlAntishake },
  { .name = "fireworks",     .control = OMX_ExposureControlFireworks },
  { .name = "largeaperture", .control = OMX_ExposureControlLargeAperture },
  { .name = "smallaperture", .control = OMX_ExposureControlSmallAperture },
};

// Red gain used when AWB is off
static float awb_red_gain;
static const float awb_red_gain_default = 0.0f;
// Blue gain used when AWB is off
static float awb_blue_gain;
static const float awb_blue_gain_default = 0.0f;

static char exposure_metering[8];
static const char *exposure_metering_default = "average";
typedef struct exposure_metering_option {
  char *name;
  OMX_METERINGTYPE metering;
} exposure_metering_option;
static const exposure_metering_option exposure_metering_options[] = {
  { .name = "average", .metering = OMX_MeteringModeAverage },
  { .name = "spot",    .metering = OMX_MeteringModeSpot },
  { .name = "matrix",  .metering = OMX_MeteringModeMatrix },
  { .name = "backlit", .metering = OMX_MeteringModeBacklit },
};

static int manual_exposure_compensation = 0; // EV compensation
static float exposure_compensation;
static int manual_exposure_aperture = 0; // f-number
static float exposure_aperture;
static int manual_exposure_shutter_speed = 0; // in microseconds
static unsigned int exposure_shutter_speed;
static int manual_exposure_sensitivity = 0; // ISO
static unsigned int exposure_sensitivity;

static char state_dir[256];
static const char *state_dir_default = "state";
static char hooks_dir[256];
static const char *hooks_dir_default = "hooks";
static float audio_volume_multiply;
static const float audio_volume_multiply_default = 1.0f;
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
static uint8_t hls_encryption_key_default[16] = {
  0x75, 0xb0, 0xa8, 0x1d, 0xe1, 0x74, 0x87, 0xc8,
  0x8a, 0x47, 0x50, 0x7a, 0x7e, 0x1f, 0xdf, 0x73,
};
static uint8_t hls_encryption_iv[16] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};
static uint8_t hls_encryption_iv_default[16] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};
static int is_preview_enabled;
static const int is_preview_enabled_default = 0;
static int is_previewrect_enabled;
static const int is_previewrect_enabled_default = 0;
static int preview_x;
static int preview_y;
static int preview_width;
static int preview_height;
static int preview_opacity;
static uint32_t blank_background_color;
static const int preview_opacity_default = 255;
static int record_buffer_keyframes;
static const int record_buffer_keyframes_default = 5;

static int is_timestamp_enabled = 0;
static char timestamp_format[128];
static const char *timestamp_format_default = "%a %b %d %l:%M:%S %p";
static LAYOUT_ALIGN timestamp_layout;
static const LAYOUT_ALIGN timestamp_layout_default = LAYOUT_ALIGN_BOTTOM | LAYOUT_ALIGN_RIGHT;
static int timestamp_horizontal_margin;
static const int timestamp_horizontal_margin_default = 10;
static int timestamp_vertical_margin;
static const int timestamp_vertical_margin_default = 10;
static int timestamp_pos_x;
static int timestamp_pos_y;
static int is_timestamp_abs_pos_enabled = 0;
static TEXT_ALIGN timestamp_text_align;
static const TEXT_ALIGN timestamp_text_align_default = TEXT_ALIGN_LEFT;
static char timestamp_font_name[128];
static const char *timestamp_font_name_default = "FreeMono:style=Bold";
static char timestamp_font_file[1024];
static int timestamp_font_face_index;
static const int timestamp_font_face_index_default = 0;
static float timestamp_font_points;
static const float timestamp_font_points_default = 14.0f;
static int timestamp_font_dpi;
static const int timestamp_font_dpi_default = 96;
static int timestamp_color;
static const int timestamp_color_default = 0xffffff;
static int timestamp_stroke_color;
static const int timestamp_stroke_color_default = 0x000000;
static float timestamp_stroke_width;
static const float timestamp_stroke_width_default = 1.3f;
static int timestamp_letter_spacing;
static const int timestamp_letter_spacing_default = 0;

// how many keyframes should we look back for the next recording
static int recording_look_back_keyframes;

static int64_t video_current_pts = 0;
static int64_t audio_current_pts = 0;
static int64_t last_pts = 0;
static int64_t time_for_last_pts = 0; // Used in VFR mode

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
static int camera_set_white_balance(char *wb);
static int camera_set_exposure_control(char *ex);
static int camera_set_custom_awb_gains();
static void encode_and_send_image();
static void encode_and_send_audio();
void start_record();
void stop_record();
#if ENABLE_AUTO_GOP_SIZE_CONTROL_FOR_VFR
static void set_gop_size(int gop_size);
#endif

static int video_send_keyframe_count = 0;
static long long video_frame_count = 0;
static long long audio_frame_count = 0;
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
static snd_pcm_t *audio_preview_handle;
static snd_pcm_hw_params_t *alsa_hw_params;
static uint16_t *samples;
static AVFrame *av_frame;
static int audio_fd_count;
static struct pollfd *poll_fds; // file descriptors for polling audio
static int is_first_audio;
static int period_size;
static int audio_buffer_size;
static int is_audio_preview_enabled;
static const int is_audio_preview_enabled_default = 0;
static int is_audio_preview_device_opened = 0;

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
char recording_filepath[256];
char recording_tmp_filepath[256];
char recording_archive_filepath[1024];
char recording_basename[256];
char recording_dest_dir[1024];
int is_recording = 0;

static MpegTSCodecSettings codec_settings;

static int is_audio_muted = 0;

// Will be set to 1 when the camera finishes capturing
static int is_camera_finished = 0;

#if ENABLE_AUTO_GOP_SIZE_CONTROL_FOR_VFR
// Variables for variable frame rate
static int64_t last_keyframe_pts = 0;
static int frames_since_last_keyframe = 0;
#endif
static float min_fps = -1.0f;
static float max_fps = -1.0f;

// Query camera capabilities and exit
static int query_and_exit = 0;

static pthread_mutex_t camera_finish_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t camera_finish_cond = PTHREAD_COND_INITIALIZER;

static char errbuf[1024];

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
  int used_percent = ceil( (stat.f_blocks - stat.f_bfree) * 100.0f / stat.f_blocks);
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
    log_error("error: cannot allocate memory for encoded_packets\n");
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
      av_strerror(ret, errbuf, sizeof(errbuf));
      log_error("error: write_encoded_packets: av_write_frame: %s\n", errbuf);
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
    int next_keyframe_pointer = current_keyframe_pointer + 1;
    if (next_keyframe_pointer >= record_buffer_keyframes) {
      next_keyframe_pointer = 0;
    }
    if (current_encoded_packet == keyframe_pointers[next_keyframe_pointer]) {
      log_warn("warning: Record buffer is starving. Recorded file may not start from keyframe. Try reducing the value of --gopsize.\n");
    }

    av_freep(&packet->data);
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
      av_freep(&packet->data);
      free(packet);
    }
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

  av_frame = av_frame_alloc();
  if (!av_frame) {
    log_error("error: av_frame_alloc failed\n");
    exit(EXIT_FAILURE);
  }

  av_frame->sample_rate = audio_codec_ctx->sample_rate;
  log_debug("sample_rate: %d\n", audio_codec_ctx->sample_rate);
  av_frame->nb_samples = audio_codec_ctx->frame_size;
  log_debug("nb_samples: %d\n", audio_codec_ctx->frame_size);
  av_frame->format = audio_codec_ctx->sample_fmt;
  log_debug("sample_fmt: %d\n", audio_codec_ctx->sample_fmt);
  av_frame->channel_layout = audio_codec_ctx->channel_layout;
  log_debug("audio_codec_ctx->channel_layout: %" PRIu64 "\n", audio_codec_ctx->channel_layout);
  log_debug("av_frame->channel_layout: %" PRIu64 "\n", av_frame->channel_layout);
  log_debug("audio_codec_ctx->channels: %d\n", audio_codec_ctx->channels);
  log_debug("av_frame->channels: %d\n", av_frame->channels);

  buffer_size = av_samples_get_buffer_size(NULL, audio_codec_ctx->channels,
      audio_codec_ctx->frame_size, audio_codec_ctx->sample_fmt, 0);
  samples = av_malloc(buffer_size);
  if (!samples) {
    log_error("error: av_malloc for samples failed\n");
    exit(EXIT_FAILURE);
  }
  log_debug("allocated %d bytes for audio samples\n", buffer_size);
#if AUDIO_BUFFER_CHUNKS > 0
  int i;
  for (i = 0; i < AUDIO_BUFFER_CHUNKS; i++) {
    audio_buffer[i] = av_malloc(buffer_size);
    if (!audio_buffer[i]) {
      log_error("error: av_malloc for audio_buffer[%d] failed\n", i);
      exit(EXIT_FAILURE);
    }
  }
#endif

  period_size = buffer_size / audio_channels / sizeof(short);
  audio_pts_step_base = 90000.0f * period_size / audio_sample_rate;
  log_debug("audio_pts_step_base: %d\n", audio_pts_step_base);

  ret = avcodec_fill_audio_frame(av_frame, audio_codec_ctx->channels, audio_codec_ctx->sample_fmt,
      (const uint8_t*)samples, buffer_size, 0);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    log_error("error: avcodec_fill_audio_frame failed: %s\n", errbuf);
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
      log_error("error: ./%s is not a directory\n", dir);
      return -1;
    }
  }

  if (access(dir, R_OK) != 0) {
    log_error("error: cannot access directory ./%s: %s\n",
        dir, strerror(errno));
    return -1;
  }

  return 0;
}

void *rec_thread_stop(int skip_cleanup) {
  FILE *fsrc, *fdest;
  int read_len;
  uint8_t *copy_buf;

  log_info("stop rec\n");
  if (!skip_cleanup) {
    copy_buf = malloc(BUFSIZ);
    if (copy_buf == NULL) {
      perror("malloc for copy_buf");
      pthread_exit(0);
    }

    pthread_mutex_lock(&rec_write_mutex);
    mpegts_close_stream(rec_format_ctx);
    mpegts_destroy_context(rec_format_ctx);
    pthread_mutex_unlock(&rec_write_mutex);

    log_debug("copy ");
    fsrc = fopen(recording_tmp_filepath, "r");
    if (fsrc == NULL) {
      log_error("error: failed to open %s: %s\n",
          recording_tmp_filepath, strerror(errno));
    }
    fdest = fopen(recording_archive_filepath, "a");
    if (fdest == NULL) {
      log_error("error: failed to open %s: %s\n",
          recording_archive_filepath, strerror(errno));
      fclose(fsrc);
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
      log_error("error: rec_thread_stop: not an EOF?: %s\n", strerror(errno));
    }

    // Create a symlink
    char symlink_dest_path[1024];
    size_t rec_dir_len = strlen(rec_dir);
    struct stat file_stat;

    // If recording_archive_filepath starts with "rec/", then remove it
    if (strncmp(recording_archive_filepath, rec_dir, rec_dir_len) == 0 &&
        recording_archive_filepath[rec_dir_len] == '/') {
      snprintf(symlink_dest_path, sizeof(symlink_dest_path),
          recording_archive_filepath + rec_dir_len + 1);
    } else if (recording_archive_filepath[0] == '/') { // absolute path
      snprintf(symlink_dest_path, sizeof(symlink_dest_path),
          recording_archive_filepath);
    } else { // relative path
      char cwd[1024];
      if (getcwd(cwd, sizeof(cwd)) == NULL) {
        log_error("error: failed to get current working directory: %s\n",
            strerror(errno));
        cwd[0] = '.';
        cwd[1] = '.';
        cwd[2] = '\0';
      }
      snprintf(symlink_dest_path, sizeof(symlink_dest_path),
          "%s/%s", cwd, recording_archive_filepath);
    }

    log_debug("symlink(%s, %s)\n", symlink_dest_path, recording_filepath);
    if (lstat(recording_filepath, &file_stat) == 0) { // file (symlink) exists
      log_info("replacing existing symlink: %s\n", recording_filepath);
      unlink(recording_filepath);
    }
    if (symlink(symlink_dest_path, recording_filepath) != 0) {
      log_error("error: cannot create symlink from %s to %s: %s\n",
          symlink_dest_path, recording_filepath, strerror(errno));
    }

    // unlink tmp file
    log_debug("unlink");
    unlink(recording_tmp_filepath);

    state_set(state_dir, "last_rec", recording_filepath);

    free(copy_buf);
  }

  is_recording = 0;
  state_set(state_dir, "record", "false");

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
  char state_buf[256];
  EncodedPacket *enc_pkt;
  int filename_decided = 0;
  uint8_t *copy_buf;
  FILE *fsrc, *fdest;
  int read_len;
  char *dest_dir;
  int has_error;

  has_error = 0;
  copy_buf = malloc(BUFSIZ);
  if (copy_buf == NULL) {
    perror("malloc for copy_buf");
    pthread_exit(0);
  }

  time(&rawtime);
  timeinfo = localtime(&rawtime);

  rec_start_time = time(NULL);
  rec_start_pts = -1;

  if (recording_dest_dir[0] != 0) {
    dest_dir = recording_dest_dir;
  } else {
    dest_dir = rec_archive_dir;
  }

  if (recording_basename[0] != 0) { // basename is already decided
    snprintf(recording_filepath, sizeof(recording_filepath),
        "%s/%s", rec_dir, recording_basename);
    snprintf(recording_archive_filepath, sizeof(recording_archive_filepath),
        "%s/%s", dest_dir, recording_basename);
    snprintf(recording_tmp_filepath, sizeof(recording_tmp_filepath),
        "%s/%s", rec_tmp_dir, recording_basename);
    filename_decided = 1;
  } else {
    strftime(recording_basename, sizeof(recording_basename), "%Y-%m-%d_%H-%M-%S", timeinfo);
    snprintf(recording_filepath, sizeof(recording_filepath),
        "%s/%s.ts", rec_dir, recording_basename);
    if (access(recording_filepath, F_OK) != 0) { // filename is decided
      sprintf(recording_basename + strlen(recording_basename), ".ts"); // add ".ts"
      snprintf(recording_archive_filepath, sizeof(recording_archive_filepath),
          "%s/%s", dest_dir, recording_basename);
      snprintf(recording_tmp_filepath, sizeof(recording_tmp_filepath),
          "%s/%s", rec_tmp_dir, recording_basename);
      filename_decided = 1;
    }
    while (!filename_decided) {
      unique_number++;
      snprintf(recording_filepath, sizeof(recording_filepath),
          "%s/%s-%d.ts", rec_dir, recording_basename, unique_number);
      if (access(recording_filepath, F_OK) != 0) { // filename is decided
        sprintf(recording_basename + strlen(recording_basename), "-%d.ts", unique_number);
        snprintf(recording_archive_filepath, sizeof(recording_archive_filepath),
            "%s/%s", dest_dir, recording_basename);
        snprintf(recording_tmp_filepath, sizeof(recording_tmp_filepath),
            "%s/%s", rec_tmp_dir, recording_basename);
        filename_decided = 1;
      }
    }
  }

  // Remove existing file
  if (unlink(recording_archive_filepath) == 0) {
    log_info("removed existing file: %s\n", recording_archive_filepath);
  }

  pthread_mutex_lock(&rec_write_mutex);
  rec_format_ctx = mpegts_create_context(&codec_settings);
  mpegts_open_stream(rec_format_ctx, recording_tmp_filepath, 0);
  is_recording = 1;
  log_info("start rec to %s\n", recording_archive_filepath);
  state_set(state_dir, "record", "true");
  pthread_mutex_unlock(&rec_write_mutex);

  int look_back_keyframes;
  if (recording_look_back_keyframes != -1) {
    look_back_keyframes = recording_look_back_keyframes;
  } else {
    look_back_keyframes = record_buffer_keyframes;
  }

  int start_keyframe_pointer;
  if (!is_keyframe_pointers_filled) { // first cycle has not been finished
    if (look_back_keyframes - 1 > current_keyframe_pointer) { // not enough pre-start buffer
      start_keyframe_pointer = 0;
    } else {
      start_keyframe_pointer = current_keyframe_pointer - look_back_keyframes + 1;
    }
  } else { // at least one cycle has been passed
    start_keyframe_pointer = current_keyframe_pointer - look_back_keyframes + 1;
  }

  // turn negative into positive
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
          log_debug("caught up");
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
        log_error("error: failed to open %s: %s\n",
            recording_tmp_filepath, strerror(errno));
        has_error = 1;
        break;
      }
      fdest = fopen(recording_archive_filepath, "a");
      if (fdest == NULL) {
        log_error("error: failed to open %s: %s\n",
            recording_archive_filepath, strerror(errno));
        has_error = 1;
        break;
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
        log_error("error: rec_thread_start: not an EOF?: %s\n", strerror(errno));
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
  snprintf(state_buf, sizeof(state_buf), "duration_pts=%" PRId64 "\nduration_sec=%f\n",
      rec_end_pts - rec_start_pts,
      (rec_end_pts - rec_start_pts) / 90000.0f);
  state_set(state_dir, recording_basename, state_buf);

  return rec_thread_stop(has_error);
}

void start_record() {
  if (is_recording) {
    log_warn("recording is already started\n");
    return;
  }

  if (is_disk_almost_full()) {
    log_error("error: disk is almost full, recording not started\n");
    return;
  }

  rec_thread_needs_exit = 0;
  pthread_create(&rec_thread, NULL, rec_thread_start, NULL);
}

// set record_buffer_keyframes to newsize
static int set_record_buffer_keyframes(int newsize) {
  int i;
  void *result;
  int malloc_size;

  if (is_recording) {
    log_error("error: recordbuf cannot be changed while recording\n");
    return -1;
  }

  if (newsize < 1) {
    log_error("error changing recordbuf to %d (must be >= 1)\n", newsize);
    return -1;
  }

  if (newsize == record_buffer_keyframes) { // no change
    log_debug("recordbuf does not change: current=%d new=%d\n",
        record_buffer_keyframes, newsize);
    return -1;
  }

  for (i = 0; i < encoded_packets_size; i++) {
    EncodedPacket *packet = encoded_packets[i];
    if (packet != NULL) {
      av_freep(&packet->data);
      free(packet);
    }
  }

  // reset encoded_packets
  int audio_fps = audio_sample_rate / 1 / period_size;
  int new_encoded_packets_size = (video_fps + 1) * newsize * 2 +
    (audio_fps + 1) * newsize * 2 + 100;
  malloc_size = sizeof(EncodedPacket *) * new_encoded_packets_size;
  result = realloc(encoded_packets, malloc_size);
  int success = 0;
  if (result == NULL) {
    log_error("error: failed to set encoded_packets to %d while trying to allocate "
        "%d bytes of memory\n", newsize, malloc_size);
    // fallback to old size
    malloc_size = sizeof(EncodedPacket *) * encoded_packets_size;
  } else {
    encoded_packets = result;
    encoded_packets_size = new_encoded_packets_size;
    success = 1;
  }
  memset(encoded_packets, 0, malloc_size);

  if (success) {
    // reset keyframe_pointers
    malloc_size = sizeof(int) * newsize;
    result = realloc(keyframe_pointers, malloc_size);
    if (result == NULL) {
      log_error("error: failed to set keyframe_pointers to %d while trying to allocate "
          "%d bytes of memory\n", newsize, malloc_size);
      // fallback to old size
      malloc_size = sizeof(int) * record_buffer_keyframes;
    } else {
      keyframe_pointers = result;
      record_buffer_keyframes = newsize;
    }
  } else {
    malloc_size = sizeof(int) * record_buffer_keyframes;
  }
  memset(keyframe_pointers, 0, malloc_size);

  current_encoded_packet = -1;
  current_keyframe_pointer = -1;
  is_keyframe_pointers_filled = 0;

  return 0;
}

// parse the contents of hooks/start_record
static void parse_start_record_file(char *full_filename) {
  char buf[1024];

  recording_basename[0] = 0; // empties the basename used for this recording
  recording_dest_dir[0] = 0; // empties the directory the result file will be put in
  recording_look_back_keyframes = -1;

  FILE *fp = fopen(full_filename, "r");
  if (fp != NULL) {
    while (fgets(buf, sizeof(buf), fp)) {
      char *sep_p = strchr(buf, '='); // separator (name=value)
      if (sep_p == NULL) { // we couldn't find '='
        log_error("error parsing line in %s: %s\n",
            full_filename, buf);
        continue;
      }
      if (strncmp(buf, "recordbuf", sep_p - buf) == 0) {
        // read a number
        char *end;
        int value = strtol(sep_p + 1, &end, 10);
        if (end == sep_p + 1 || errno == ERANGE) { // parse error
          log_error("error parsing line in %s: %s\n",
              full_filename, buf);
          continue;
        }
        if (value > record_buffer_keyframes) {
          log_error("error: per-recording recordbuf (%d) cannot be greater than "
              "global recordbuf (%d); using %d\n"
              "hint: try increasing global recordbuf with \"--recordbuf %d\" or "
              "\"echo %d > hooks/set_recordbuf\"\n",
              value, record_buffer_keyframes, record_buffer_keyframes,
              value, value);
          continue;
        }
        recording_look_back_keyframes = value;
        log_info("using recordbuf=%d for this recording\n", recording_look_back_keyframes);
      } else if (strncmp(buf, "dir", sep_p - buf) == 0) { // directory
        size_t len = strcspn(sep_p + 1, "\r\n");
        if (len > sizeof(recording_dest_dir) - 1) {
          len = sizeof(recording_dest_dir) - 1;
        }
        strncpy(recording_dest_dir, sep_p + 1, len);
        recording_dest_dir[len] = '\0';
        // Create the directory if it does not exist
        create_dir(recording_dest_dir);
      } else if (strncmp(buf, "filename", sep_p - buf) == 0) { // basename
        size_t len = strcspn(sep_p + 1, "\r\n");
        if (len > sizeof(recording_basename) - 1) {
          len = sizeof(recording_basename) - 1;
        }
        strncpy(recording_basename, sep_p + 1, len);
        recording_basename[len] = '\0';
      } else {
        log_error("failed to parse line in %s: %s\n",
            full_filename, buf);
      }
    }
    fclose(fp);
  }
}

/**
 * Reads a file and returns the contents.
 * file_contents argument will be set to the pointer to the
 * newly-allocated memory block that holds the NULL-terminated contents.
 * It must be freed by caller after use.
 */
static int read_file(const char *filepath, char **file_contents, size_t *file_contents_len) {
  FILE *fp;
  long filesize;
  char *buf;
  size_t result;

  fp = fopen(filepath, "rb");
  if (fp == NULL) {
    return -1;
  }

  // obtain file size
  fseek(fp, 0L, SEEK_END);
  filesize = ftell(fp);
  fseek(fp, 0L, SEEK_SET);

  buf = malloc(filesize + 1);
  if (buf == NULL) {
    log_error("read_file: error reading %s: failed to allocate memory (%d bytes)", filepath, filesize + 1);
    fclose(fp);
    return -1;
  }

  result = fread(buf, 1, filesize, fp);
  if (result != filesize) {
    log_error("read_file: error reading %s", filepath);
    fclose(fp);
    free(buf);
    return -1;
  }

  fclose(fp);
  buf[filesize] = '\0';
  *file_contents = buf;
  *file_contents_len = filesize + 1;

  return 0;
}

void on_file_create(char *filename, char *content) {
  if (strcmp(filename, "start_record") == 0) {
    char buf[256];

    // parse the contents of hooks/start_record
    snprintf(buf, sizeof(buf), "%s/%s", hooks_dir, filename);
    parse_start_record_file(buf);

    start_record();
  } else if (strcmp(filename, "stop_record") == 0) {
    stop_record();
  } else if (strcmp(filename, "mute") == 0) {
    mute_audio();
  } else if (strcmp(filename, "unmute") == 0) {
    unmute_audio();
  } else if (strcmp(filename, "wbred") == 0) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s/%s", hooks_dir, filename);
    char *file_buf;
    size_t file_buf_len;
    if (read_file(buf, &file_buf, &file_buf_len) == 0) {
      if (file_buf != NULL) {
        // read a number
        char *end;
        double value = strtod(file_buf, &end);
        if (end == file_buf || errno == ERANGE) { // parse error
          log_error("error parsing file %s\n", buf);
        } else { // parse ok
          awb_red_gain = value;
          if (camera_set_custom_awb_gains() == 0) {
            log_info("changed red gain to %.2f\n", awb_red_gain);
          } else {
            log_error("error: failed to set wbred\n");
          }
        }
        free(file_buf);
      }
    }
  } else if (strcmp(filename, "wbblue") == 0) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s/%s", hooks_dir, filename);
    char *file_buf;
    size_t file_buf_len;
    if (read_file(buf, &file_buf, &file_buf_len) == 0) {
      if (file_buf != NULL) {
        // read a number
        char *end;
        double value = strtod(file_buf, &end);
        if (end == file_buf || errno == ERANGE) { // parse error
          log_error("error parsing file %s\n", buf);
        } else { // parse ok
          awb_blue_gain = value;
          if (camera_set_custom_awb_gains() == 0) {
            log_info("changed blue gain to %.2f\n", awb_blue_gain);
          } else {
            log_error("error: failed to set wbblue\n");
          }
        }
        free(file_buf);
      }
    }
  } else if (strncmp(filename, "wb_", 3) == 0) { // e.g. wb_sun
    char *wb_mode = filename + 3;
    int matched = 0;
    int i;
    for (i = 0; i < sizeof(white_balance_options) / sizeof(white_balance_option); i++) {
      if (strcmp(white_balance_options[i].name, wb_mode) == 0) {
        strncpy(white_balance, wb_mode, sizeof(white_balance) - 1);
        white_balance[sizeof(white_balance) - 1] = '\0';
        matched = 1;
        break;
      }
    }
    if (matched) {
      if (camera_set_white_balance(white_balance) == 0) {
        log_info("changed the white balance to %s\n", white_balance);
      } else {
        log_error("error: failed to set the white balance to %s\n", white_balance);
      }
    } else {
      log_error("hook error: invalid white balance: %s\n", wb_mode);
      log_error("(valid values: ");
      int size = sizeof(white_balance_options) / sizeof(white_balance_option);
      for (i = 0; i < size; i++) {
        log_error("%s", white_balance_options[i].name);
        if (i + 1 == size) { // the last item
          log_error(")\n");
        } else {
          log_error("/");
        }
      }
    }
  } else if (strncmp(filename, "ex_", 3) == 0) { // e.g. ex_night
    char *ex_mode = filename + 3;
    int matched = 0;
    int i;

    if (!is_vfr_enabled) {
      log_warn("warn: Use --vfr or --ex in order to ex_* hook to properly take effect\n");
    }

    for (i = 0; i < sizeof(exposure_control_options) / sizeof(exposure_control_option); i++) {
      if (strcmp(exposure_control_options[i].name, ex_mode) == 0) {
        strncpy(exposure_control, ex_mode, sizeof(exposure_control) - 1);
        exposure_control[sizeof(exposure_control) - 1] = '\0';
        matched = 1;
        break;
      }
    }
    if (matched) {
      if (camera_set_exposure_control(exposure_control) == 0) {
        log_info("changed the exposure control to %s\n", exposure_control);
      } else {
        log_error("error: failed to set the exposure control to %s\n", exposure_control);
      }
    } else {
      log_error("hook error: invalid exposure control: %s\n", ex_mode);
      log_error("(valid values: ");
      int size = sizeof(exposure_control_options) / sizeof(exposure_control_option);
      for (i = 0; i < size; i++) {
        log_error("%s", exposure_control_options[i].name);
        if (i + 1 == size) { // the last item
          log_error(")\n");
        } else {
          log_error("/");
        }
      }
    }
  } else if (strcmp(filename, "set_recordbuf") == 0) { // set global recordbuf
    char buf[256];
    snprintf(buf, sizeof(buf), "%s/%s", hooks_dir, filename);
    char *file_buf;
    size_t file_buf_len;
    if (read_file(buf, &file_buf, &file_buf_len) == 0) {
      if (file_buf != NULL) {
        // read a number
        char *end;
        int value = strtol(file_buf, &end, 10);
        if (end == file_buf || errno == ERANGE) { // parse error
          log_error("error parsing file %s\n", buf);
        } else { // parse ok
          if (set_record_buffer_keyframes(value) == 0) {
            log_info("recordbuf set to %d; existing record buffer cleared\n", value);
          }
        }
        free(file_buf);
      }
    }
  } else if (strcmp(filename, "subtitle") == 0) {
    // The followings are default values for the subtitle
    char line[1024];
    char text[1024];
    size_t text_len = 0;
    char font_name[128] = { 0x00 };
    long face_index = 0;
    char font_file[256] = { 0x00 };
    int color = 0xffffff;
    int stroke_color = 0x000000;
    float font_points = 28.0f;
    int font_dpi = 96;
    float stroke_width = 1.0f;
    int letter_spacing = 0;
    float line_height_multiply = 1.0f;
    float tab_scale = 1.0f;
    int abspos_x = 0;
    int abspos_y = 0;
    float duration = 7.0f;
    int is_abspos_specified = 0;
    LAYOUT_ALIGN layout_align = LAYOUT_ALIGN_BOTTOM | LAYOUT_ALIGN_CENTER;
    TEXT_ALIGN text_align = TEXT_ALIGN_CENTER;
    int horizontal_margin = 0;
    int vertical_margin = 35;
    int in_preview = 1;
    int in_video = 1;

    char filepath[256];
    snprintf(filepath, sizeof(filepath), "%s/%s", hooks_dir, filename);
    FILE *fp;
    fp = fopen(filepath, "r");
    if (fp == NULL) {
      log_error("subtitle error: cannot open file: %s\n", filepath);
    } else {
      // read key=value lines
      while (fgets(line, sizeof(line), fp)) {
        // remove newline at the end of the line
        size_t line_len = strlen(line);
        if (line[line_len-1] == '\n') {
          line[line_len-1] = '\0';
          line_len--;
        }
        if (line_len == 0) { // blank line
          continue;
        }
        if (line[0] == '#') { // comment line
          continue;
        }

        char *delimiter_p = strchr(line, '=');
        if (delimiter_p != NULL) {
          int key_len = delimiter_p - line;
          if (strncmp(line, "text=", key_len+1) == 0) {
            text_len = line_len - 5; // 5 == strlen("text") + 1
            if (text_len >= sizeof(text) - 1) {
              text_len = sizeof(text) - 1;
            }
            strncpy(text, delimiter_p + 1, text_len);
            text[text_len] = '\0';
          } else if (strncmp(line, "font_name=", key_len+1) == 0) {
            strncpy(font_name, delimiter_p + 1, sizeof(font_name) - 1);
            font_name[sizeof(font_name) - 1] = '\0';
          } else if (strncmp(line, "font_file=", key_len+1) == 0) {
            strncpy(font_file, delimiter_p + 1, sizeof(font_file) - 1);
            font_file[sizeof(font_file) - 1] = '\0';
          } else if (strncmp(line, "face_index=", key_len+1) == 0) {
            char *end;
            long value = strtol(delimiter_p+1, &end, 10);
            if (end == delimiter_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid face_index: %s\n", delimiter_p+1);
              return;
            }
            face_index = value;
          } else if (strncmp(line, "pt=", key_len+1) == 0) {
            char *end;
            double value = strtod(delimiter_p+1, &end);
            if (end == delimiter_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid pt: %s\n", delimiter_p+1);
              return;
            }
            font_points = value;
          } else if (strncmp(line, "dpi=", key_len+1) == 0) {
            char *end;
            long value = strtol(delimiter_p+1, &end, 10);
            if (end == delimiter_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid dpi: %s\n", delimiter_p+1);
              return;
            }
            font_dpi = value;
          } else if (strncmp(line, "horizontal_margin=", key_len+1) == 0) {
            char *end;
            long value = strtol(delimiter_p+1, &end, 10);
            if (end == delimiter_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid horizontal_margin: %s\n", delimiter_p+1);
              return;
            }
            horizontal_margin = value;
          } else if (strncmp(line, "vertical_margin=", key_len+1) == 0) {
            char *end;
            long value = strtol(delimiter_p+1, &end, 10);
            if (end == delimiter_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid vertical_margin: %s\n", delimiter_p+1);
              return;
            }
            vertical_margin = value;
          } else if (strncmp(line, "duration=", key_len+1) == 0) {
            char *end;
            double value = strtod(delimiter_p+1, &end);
            if (end == delimiter_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid duration: %s\n", delimiter_p+1);
              return;
            }
            duration = value;
          } else if (strncmp(line, "color=", key_len+1) == 0) {
            char *end;
            long value = strtol(delimiter_p+1, &end, 16);
            if (end == delimiter_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid color: %s\n", delimiter_p+1);
              return;
            }
            if (value < 0) {
              log_error("subtitle error: invalid color: %d (must be >= 0)\n", value);
              return;
            }
            color = value;
          } else if (strncmp(line, "stroke_color=", key_len+1) == 0) {
            char *end;
            long value = strtol(delimiter_p+1, &end, 16);
            if (end == delimiter_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid stroke_color: %s\n", delimiter_p+1);
              return;
            }
            if (value < 0) {
              log_error("subtitle error: invalid stroke_color: %d (must be >= 0)\n", value);
              return;
            }
            stroke_color = value;
          } else if (strncmp(line, "stroke_width=", key_len+1) == 0) {
            char *end;
            double value = strtod(delimiter_p+1, &end);
            if (end == delimiter_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid stroke_width: %s\n", delimiter_p+1);
              return;
            }
            stroke_width = value;
          } else if (strncmp(line, "letter_spacing=", key_len+1) == 0) {
            char *end;
            long value = strtol(delimiter_p+1, &end, 10);
            if (end == delimiter_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid letter_spacing: %s\n", delimiter_p+1);
              return;
            }
            letter_spacing = value;
          } else if (strncmp(line, "line_height=", key_len+1) == 0) {
            char *end;
            double value = strtod(delimiter_p+1, &end);
            if (end == delimiter_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid line_height: %s\n", delimiter_p+1);
              return;
            }
            line_height_multiply = value;
          } else if (strncmp(line, "tab_scale=", key_len+1) == 0) {
            char *end;
            double value = strtod(delimiter_p+1, &end);
            if (end == delimiter_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid tab_scale: %s\n", delimiter_p+1);
              return;
            }
            tab_scale = value;
          } else if (strncmp(line, "pos=", key_len+1) == 0) { // absolute position
            char *comma_p = strchr(delimiter_p+1, ',');
            if (comma_p == NULL) {
              log_error("subtitle error: invalid pos format: %s (should be <x>,<y>)\n", delimiter_p+1);
              return;
            }

            char *end;
            long value = strtol(delimiter_p+1, &end, 10);
            if (end == delimiter_p+1 || end != comma_p || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid pos x: %s\n", delimiter_p+1);
              return;
            }
            abspos_x = value;

            value = strtol(comma_p+1, &end, 10);
            if (end == comma_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid pos y: %s\n", comma_p+1);
              return;
            }
            abspos_y = value;

            is_abspos_specified = 1;
          } else if (strncmp(line, "layout_align=", key_len+1) == 0) { // layout align
            char *comma_p;
            char *search_p = delimiter_p + 1;
            int param_len;
            layout_align = 0;
            while (1) {
              comma_p = strchr(search_p, ',');
              if (comma_p == NULL) {
                param_len = line + line_len - search_p;
              } else {
                param_len = comma_p - search_p;
              }
              if (strncmp(search_p, "top", param_len) == 0) {
                layout_align |= LAYOUT_ALIGN_TOP;
              } else if (strncmp(search_p, "middle", param_len) == 0) {
                layout_align |= LAYOUT_ALIGN_MIDDLE;
              } else if (strncmp(search_p, "bottom", param_len) == 0) {
                layout_align |= LAYOUT_ALIGN_BOTTOM;
              } else if (strncmp(search_p, "left", param_len) == 0) {
                layout_align |= LAYOUT_ALIGN_LEFT;
              } else if (strncmp(search_p, "center", param_len) == 0) {
                layout_align |= LAYOUT_ALIGN_CENTER;
              } else if (strncmp(search_p, "right", param_len) == 0) {
                layout_align |= LAYOUT_ALIGN_RIGHT;
              } else {
                log_error("subtitle error: invalid layout_align found at: %s\n", search_p);
                return;
              }
              if (comma_p == NULL || line + line_len - 1 - comma_p <= 0) { // no remaining chars
                break;
              }
              search_p = comma_p + 1;
            }
          } else if (strncmp(line, "text_align=", key_len+1) == 0) { // text align
            char *comma_p;
            char *search_p = delimiter_p + 1;
            int param_len;
            text_align = 0;
            while (1) {
              comma_p = strchr(search_p, ',');
              if (comma_p == NULL) {
                param_len = line + line_len - search_p;
              } else {
                param_len = comma_p - search_p;
              }
              if (strncmp(search_p, "left", param_len) == 0) {
                text_align |= TEXT_ALIGN_LEFT;
              } else if (strncmp(search_p, "center", param_len) == 0) {
                text_align |= TEXT_ALIGN_CENTER;
              } else if (strncmp(search_p, "right", param_len) == 0) {
                text_align |= TEXT_ALIGN_RIGHT;
              } else {
                log_error("subtitle error: invalid text_align found at: %s\n", search_p);
                return;
              }
              if (comma_p == NULL || line + line_len - 1 - comma_p <= 0) { // no remaining chars
                break;
              }
              search_p = comma_p + 1;
            }
          } else if (strncmp(line, "in_preview=", key_len+1) == 0) {
            char *end;
            double value = strtod(delimiter_p+1, &end);
            if (end == delimiter_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid in_preview: %s\n", delimiter_p+1);
              return;
            }
            in_preview = (value != 0);
          } else if (strncmp(line, "in_video=", key_len+1) == 0) {
            char *end;
            double value = strtod(delimiter_p+1, &end);
            if (end == delimiter_p+1 || *end != '\0' || errno == ERANGE) { // parse error
              log_error("subtitle error: invalid in_video: %s\n", delimiter_p+1);
              return;
            }
            in_video = (value != 0);
          } else {
            log_error("subtitle error: cannot parse line: %s\n", line);
          }
        } else {
          log_error("subtitle error: cannot find delimiter: %s\n", line);
        }
      }
      if (text_len > 0) {
        // replace literal \n with newline
        int i;
        int is_escape_active = 0;
        int omitted_bytes = 0;
        char replaced_text[1024];
        char *replaced_text_ptr = replaced_text;
        for (i = 0; i < text_len; i++) {
          if (text[i] == '\\') { // escape char
            if (is_escape_active) { // double escape char
              *replaced_text_ptr++ = '\\';
            } else { // start of escape sequence
              omitted_bytes++;
            }
            is_escape_active = !is_escape_active;
          } else if (text[i] == 'n') {
            if (is_escape_active) { // n after escape char
              *replaced_text_ptr = '\n';
              is_escape_active = 0;
            } else { // n after non-escape char
              *replaced_text_ptr = text[i];
            }
            replaced_text_ptr++;
          } else if (text[i] == 't') {
            if (is_escape_active) { // t after escape char
              *replaced_text_ptr = '\t';
              is_escape_active = 0;
            } else { // t after non-escape char
              *replaced_text_ptr = text[i];
            }
            replaced_text_ptr++;
          } else {
            if (is_escape_active) {
              is_escape_active = 0;
            }
            *replaced_text_ptr++ = text[i];
          }
        }
        text_len -= omitted_bytes;

        replaced_text[text_len] = '\0';
        if (font_file[0] != 0x00) {
          subtitle_init(font_file, face_index, font_points, font_dpi);
        } else {
          subtitle_init_with_font_name(font_name, font_points, font_dpi);
        }
        subtitle_set_color(color);
        subtitle_set_stroke_color(stroke_color);
        subtitle_set_stroke_width(stroke_width);
        subtitle_set_visibility(in_preview, in_video);
        subtitle_set_letter_spacing(letter_spacing);
        subtitle_set_line_height_multiply(line_height_multiply);
        subtitle_set_tab_scale(tab_scale);
        if (is_abspos_specified) {
          subtitle_set_position(abspos_x, abspos_y);
        } else {
          subtitle_set_layout(layout_align,
              horizontal_margin, vertical_margin);
        }
        subtitle_set_align(text_align);

        // show subtitle for 7 seconds
        subtitle_show(replaced_text, text_len, duration);
      } else {
        subtitle_clear();
      }
      fclose(fp);
    }
  } else {
    log_error("error: invalid hook: %s\n", filename);
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
    int payload_size = 11;
    uint8_t sendbuf[14] = {
      // payload size
      (payload_size >> 16) & 0xff,
      (payload_size >> 8) & 0xff,
      payload_size & 0xff,

      // payload
      // packet type
      0x00,
      // stream name
      'l', 'i', 'v', 'e', '/', 'p', 'i', 'c', 'a', 'm',
    };
    if (send(sockfd_video_control, sendbuf, sizeof(sendbuf), 0) == -1) {
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

// Return next video PTS for variable frame rate
static int64_t get_next_video_pts_vfr() {
  video_frame_count++;

  if (time_for_last_pts == 0) {
    video_current_pts = 0;
  } else {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    video_current_pts = last_pts
      + (ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec - time_for_last_pts) // diff_time
      * .00009f; // nanoseconds to PTS
  }

  return video_current_pts;
}

// Return next video PTS for constant frame rate
static int64_t get_next_video_pts_cfr() {
  int64_t pts;
  video_frame_count++;

  int pts_diff = audio_current_pts - video_current_pts - video_pts_step;
  int tolerance = (video_pts_step + audio_pts_step_base) * 2;
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
    pts = video_current_pts + video_pts_step + 150;
  } else if (pts_diff <= -tolerance) {
    if (pts_mode != PTS_SPEED_DOWN) {
      // speed down video PTS
      pts_mode = PTS_SPEED_DOWN;
      speed_down_count++;
      log_debug("vSPEED_DOWN(%d)", pts_diff);
    }
    pts = video_current_pts + video_pts_step - 150;
  } else {
    pts = video_current_pts + video_pts_step;
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

// Return next PTS for video stream
static int64_t get_next_video_pts() {
  if (is_vfr_enabled) { // variable frame rate
    return get_next_video_pts_vfr();
  } else { // constant frame rate
    return get_next_video_pts_cfr();
  }
}

static int64_t get_next_audio_write_time() {
  if (audio_frame_count == 0) {
    return LLONG_MIN;
  }
  return audio_start_time + audio_frame_count * 1000000000.0f / ((float)audio_sample_rate / (float)period_size);
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
  int64_t clock_pts = (cur_time - audio_start_time) * .00009f;

  log_debug(" a-v=%lld c-a=%lld u=%d d=%d pts=%" PRId64,
      avdiff, clock_pts - audio_pts, speed_up_count, speed_down_count, last_pts);
}

static void send_audio_frame(uint8_t *databuf, int databuflen, int64_t pts) {
  if (is_rtspout_enabled) {
    int payload_size = databuflen + 7;  // +1(packet type) +6(pts)
    int total_size = payload_size + 3;  // more 3 bytes for payload length
    uint8_t *sendbuf = malloc(total_size);
    if (sendbuf == NULL) {
      log_error("error: cannot allocate memory for audio sendbuf: size=%d", total_size);
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
      log_error("error: cannot allocate memory for video sendbuf: size=%d", total_size);
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
    log_error("error: send_keyframe: cannot allocate memory for buf (%d bytes)\n", total_size);
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
    pts = video_current_pts;
  }

#if ENABLE_AUTO_GOP_SIZE_CONTROL_FOR_VFR
  if (is_vfr_enabled) {
    int64_t pts_between_keyframes = pts - last_keyframe_pts;
    if (pts_between_keyframes < 80000) { // < .89 seconds
      // Frame rate is running faster than we thought
      int ideal_video_gop_size = (frames_since_last_keyframe + 1)
        * 90000.0f / pts_between_keyframes;
      if (ideal_video_gop_size > video_gop_size) {
        video_gop_size = ideal_video_gop_size;
        log_debug("increase gop_size to %d ", ideal_video_gop_size);
        set_gop_size(video_gop_size);
      }
    }
    last_keyframe_pts = pts;
    frames_since_last_keyframe = 0;
  }
#endif

  send_video_frame(data, data_len, pts);

#if ENABLE_PTS_WRAP_AROUND
  pts = pts % PTS_MODULO;
#endif
  last_pts = pts;

  if (is_vfr_enabled) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    time_for_last_pts = ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
  }

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

    if (video_send_keyframe_count % hls_keyframes_per_segment == 0 && video_frame_count != 1) {
      split = 1;
    } else {
      split = 0;
    }

    video_send_keyframe_count = video_send_keyframe_count % hls_keyframes_per_segment;    

    // Update counter 
    video_send_keyframe_count++;

    ret = hls_write_packet(hls, &pkt, split);
    pthread_mutex_unlock(&mutex_writing);
    if (ret < 0) {
      av_strerror(ret, errbuf, sizeof(errbuf));
      log_error("keyframe write error (hls): %s\n", errbuf);
      log_error("please check if the disk is full\n");
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
    log_fatal("error: send_pframe malloc failed: size=%d\n", total_size);
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
    pts = video_current_pts;
  }

#if ENABLE_AUTO_GOP_SIZE_CONTROL_FOR_VFR
  if (is_vfr_enabled) {
    if (video_current_pts - last_keyframe_pts >= 100000) { // >= 1.11 seconds
      // Frame rate is running slower than we thought
      int ideal_video_gop_size = frames_since_last_keyframe;
      if (ideal_video_gop_size == 0) {
        ideal_video_gop_size = 1;
      }
      if (ideal_video_gop_size < video_gop_size) {
        video_gop_size = ideal_video_gop_size;
        log_debug("decrease gop_size to %d ", video_gop_size);
        set_gop_size(video_gop_size);
      }
    }
    frames_since_last_keyframe++;
  }
#endif

  send_video_frame(data, data_len, pts);

#if ENABLE_PTS_WRAP_AROUND
  pts = pts % PTS_MODULO;
#endif
  last_pts = pts;

  if (is_vfr_enabled) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    time_for_last_pts = ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
  }

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
      av_strerror(ret, errbuf, sizeof(errbuf));
      log_error("P frame write error (hls): %s\n", errbuf);
      log_error("please check if the disk is full\n");
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
          log_error("microphone error: suspend cannot be recovered, "
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

  log_debug("opening ALSA device for capture: %s\n", alsa_dev);
  err = snd_pcm_open(&capture_handle, alsa_dev, SND_PCM_STREAM_CAPTURE, 0);
  if (err < 0) {
    log_error("error: cannot open audio capture device '%s': %s\n",
        alsa_dev, snd_strerror(err));
    log_error("hint: specify correct ALSA device with '--alsadev <dev>'\n");
    return -1;
  }

  return 0;
}

static int open_audio_preview_device() {
  int err;
  snd_pcm_hw_params_t *audio_preview_params;

  log_debug("opening ALSA device for playback (preview): %s\n", audio_preview_dev);
  err = snd_pcm_open(&audio_preview_handle, audio_preview_dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
  if (err < 0) {
    log_error("error: cannot open audio playback (preview) device '%s': %s\n",
        audio_preview_dev, snd_strerror(err));
    log_error("hint: specify correct ALSA device with '--audiopreviewdev <dev>'\n");
    exit(EXIT_FAILURE);
  }

  err = snd_pcm_hw_params_malloc(&audio_preview_params);
  if (err < 0) {
    log_fatal("error: cannot allocate hardware parameter structure for audio preview: %s\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // fill hw_params with a full configuration space for a PCM.
  err = snd_pcm_hw_params_any(audio_preview_handle, audio_preview_params);
  if (err < 0) {
    log_fatal("error: cannot initialize hardware parameter structure for audio preview: %s\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // enable rate resampling
  unsigned int enable_resampling = 1;
  err = snd_pcm_hw_params_set_rate_resample(audio_preview_handle, audio_preview_params, enable_resampling);
  if (err < 0) {
    log_fatal("error: cannot enable rate resampling for audio preview: %s\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  err = snd_pcm_hw_params_set_access(audio_preview_handle, audio_preview_params,
      SND_PCM_ACCESS_MMAP_INTERLEAVED);
  if (err < 0) {
    log_fatal("error: cannot set access type for audio preview: %s\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // SND_PCM_FORMAT_S16_LE => PCM 16 bit signed little endian
  err = snd_pcm_hw_params_set_format(audio_preview_handle, audio_preview_params, SND_PCM_FORMAT_S16_LE);
  if (err < 0) {
    log_fatal("error: cannot set sample format for audio preview: %s\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  audio_preview_channels = audio_channels;
  err = snd_pcm_hw_params_set_channels(audio_preview_handle, audio_preview_params, audio_preview_channels);
  if (err < 0) {
    log_fatal("error: cannot set channel count for audio preview: %s\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // set the sample rate
  unsigned int rate = audio_sample_rate;
  err = snd_pcm_hw_params_set_rate_near(audio_preview_handle, audio_preview_params, &rate, 0);
  if (err < 0) {
    log_fatal("error: cannot set sample rate for audio preview: %s\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // set the buffer size
  err = snd_pcm_hw_params_set_buffer_size(audio_preview_handle, audio_preview_params,
      audio_buffer_size * ALSA_PLAYBACK_BUFFER_MULTIPLY);
  if (err < 0) {
    log_fatal("error: failed to set buffer size for audio preview: audio_buffer_size=%d error=%s\n",
        audio_buffer_size, snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  int dir;
  // set the period size
  err = snd_pcm_hw_params_set_period_size_near(audio_preview_handle, audio_preview_params,
      (snd_pcm_uframes_t *)&period_size, &dir);
  if (err < 0) {
    log_fatal("error: failed to set period size for audio preview: %s\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // apply the hardware configuration
  err = snd_pcm_hw_params (audio_preview_handle, audio_preview_params);
  if (err < 0) {
    log_fatal("error: cannot set PCM hardware parameters for audio preview: %s\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // end of configuration
  snd_pcm_hw_params_free(audio_preview_params);

  // dump the configuration of capture_handle
  if (log_get_level() <= LOG_LEVEL_DEBUG) {
    snd_output_t *output;
    err = snd_output_stdio_attach(&output, stdout, 0);
    if (err < 0) {
      log_error("snd_output_stdio_attach failed: %s\n", snd_strerror(err));
      return 0;
    }
    log_debug("audio preview device:\n");
    snd_pcm_dump(audio_preview_handle, output);
  }

  return 0;
}

// Configure the microphone before main setup
static void preconfigure_microphone() {
  int err;

  // allocate an invalid snd_pcm_hw_params_t using standard malloc
  err = snd_pcm_hw_params_malloc(&alsa_hw_params);
  if (err < 0) {
    log_fatal("error: cannot allocate hardware parameter structure (%s)\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // fill hw_params with a full configuration space for a PCM.
  err = snd_pcm_hw_params_any(capture_handle, alsa_hw_params);
  if (err < 0) {
    log_fatal("error: cannot initialize hardware parameter structure (%s)\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // set the number of channels
  err = snd_pcm_hw_params_set_channels(capture_handle, alsa_hw_params, audio_channels);
  if (err < 0) {
    if (audio_channels == 1) {
      if (is_audio_channels_specified) {
        log_info("cannot use mono audio; trying stereo\n");
      } else {
        log_debug("cannot use mono audio; trying stereo\n");
      }
      audio_channels = 2;
    } else {
      if (is_audio_channels_specified) {
        log_info("cannot use stereo audio; trying mono\n");
      } else {
        log_debug("cannot use stereo audio; trying mono\n");
      }
      audio_channels = 1;
    }
    err = snd_pcm_hw_params_set_channels(capture_handle, alsa_hw_params, audio_channels);
    if (err < 0) {
      log_fatal("error: cannot set channel count for microphone (%s)\n", snd_strerror(err));
      exit(EXIT_FAILURE);
    }
  }
  log_debug("final audio_channels: %d\n", audio_channels);
}

static int configure_audio_capture_device() {
  // ALSA
  int err;

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

  // use mmap
  err = snd_pcm_hw_params_set_access(capture_handle, alsa_hw_params,
      SND_PCM_ACCESS_MMAP_INTERLEAVED);
  if (err < 0) {
    log_fatal("error: cannot set access type (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // SND_PCM_FORMAT_S16_LE => PCM 16 bit signed little endian
  err = snd_pcm_hw_params_set_format(capture_handle, alsa_hw_params, SND_PCM_FORMAT_S16_LE);
  if (err < 0) {
    log_fatal("error: cannot set sample format (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // set the sample rate
  unsigned int rate = audio_sample_rate;
  err = snd_pcm_hw_params_set_rate_near(capture_handle, alsa_hw_params, &rate, 0);
  if (err < 0) {
    log_fatal("error: cannot set sample rate (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  unsigned int actual_rate;
  int actual_dir;
  err = snd_pcm_hw_params_get_rate(alsa_hw_params, &actual_rate, &actual_dir);
  if (err < 0) {
    log_fatal("error: failed to get sample rate from microphone (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }
  log_debug("actual sample rate=%u dir=%d\n", actual_rate, actual_dir);
  if (actual_rate != audio_sample_rate) {
    log_fatal("error: failed to set sample rate for microphone to %d (got %d)\n",
        audio_sample_rate, actual_rate);
    exit(EXIT_FAILURE);
  }

  // set the buffer size
  int alsa_buffer_multiply = ALSA_BUFFER_MULTIPLY;
  err = snd_pcm_hw_params_set_buffer_size(capture_handle, alsa_hw_params,
      buffer_size * alsa_buffer_multiply);
  while (err < 0) {
    log_debug("failed to set buffer size for microphone: buffer_size=%d multiply=%d\n", buffer_size, alsa_buffer_multiply);
    alsa_buffer_multiply /= 2;
    if (alsa_buffer_multiply == 0) {
      break;
    }
    log_debug("trying smaller buffer size for microphone: buffer_size=%d multiply=%d\n", buffer_size, alsa_buffer_multiply);
    err = snd_pcm_hw_params_set_buffer_size(capture_handle, alsa_hw_params,
        buffer_size * alsa_buffer_multiply);
  }
  if (err < 0) {
    log_fatal("error: failed to set buffer size for microphone: buffer_size=%d multiply=%d (%s)\n", buffer_size, alsa_buffer_multiply, snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // check the value of the buffer size
  err = snd_pcm_hw_params_get_buffer_size(alsa_hw_params, &real_buffer_size);
  if (err < 0) {
    log_fatal("error: failed to get buffer size from microphone (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }
  log_debug("microphone: buffer size: %d frames (channels=%d buffer_size=%d multiply=%d)\n", (int)real_buffer_size, audio_channels, buffer_size, alsa_buffer_multiply);

  audio_buffer_size = buffer_size;

  log_debug("microphone: setting period size to %d\n", period_size);
  dir = 0;
  // set the period size
  err = snd_pcm_hw_params_set_period_size_near(capture_handle, alsa_hw_params,
      (snd_pcm_uframes_t *)&period_size, &dir);
  if (err < 0) {
    log_fatal("error: failed to set period size for microphone (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  snd_pcm_uframes_t actual_period_size;
  err = snd_pcm_hw_params_get_period_size(alsa_hw_params, &actual_period_size, &dir);
  if (err < 0) {
    log_fatal("error: failed to get period size from microphone (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }
  log_debug("actual_period_size=%lu dir=%d\n", actual_period_size, dir);

  // apply the hardware configuration
  err = snd_pcm_hw_params (capture_handle, alsa_hw_params);
  if (err < 0) {
    log_fatal("error: cannot set PCM hardware parameters for microphone (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // end of configuration
  snd_pcm_hw_params_free(alsa_hw_params);

  err = snd_pcm_prepare(capture_handle);
  if (err < 0) {
    log_fatal("error: cannot prepare audio interface for use (%s)\n", snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  audio_fd_count = snd_pcm_poll_descriptors_count(capture_handle);
  if (audio_fd_count <= 0) {
    log_error("microphone error: invalid poll descriptors count\n");
    return audio_fd_count;
  }
  poll_fds = malloc(sizeof(struct pollfd) * audio_fd_count);
  if (poll_fds == NULL) {
    log_fatal("error: cannot allocate memory for poll_fds\n");
    exit(EXIT_FAILURE);
  }
  // get poll descriptors
  err = snd_pcm_poll_descriptors(capture_handle, poll_fds, audio_fd_count);
  if (err < 0) {
    log_error("microphone error: unable to obtain poll descriptors for capture: %s\n", snd_strerror(err));
    return err;
  }
  is_first_audio = 1;

  // dump the configuration of capture_handle
  if (log_get_level() <= LOG_LEVEL_DEBUG) {
    snd_output_t *output;
    err = snd_output_stdio_attach(&output, stdout, 0);
    if (err < 0) {
      log_error("snd_output_stdio_attach failed: %s\n", snd_strerror(err));
      return 0;
    }
    log_debug("audio capture device:\n");
    snd_pcm_dump(capture_handle, output);
  }

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
      av_strerror(ret, errbuf, sizeof(errbuf));
      log_error("error encoding frame: %s\n", errbuf);
      break;
    }
  }

  av_freep(&samples);
#if AUDIO_BUFFER_CHUNKS > 0
  for (i = 0; i < AUDIO_BUFFER_CHUNKS; i++) {
    av_freep(&audio_buffer[i]);
  }
#endif
  av_frame_free(&av_frame);
}

static void teardown_audio_capture_device() {
  snd_pcm_close (capture_handle);

  free(poll_fds);
}

static void teardown_audio_preview_device() {
  snd_pcm_close(audio_preview_handle);
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

  timestamp_shutdown();
  subtitle_shutdown();
  text_teardown();
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

static void set_gop_size(int gop_size) {
  OMX_VIDEO_CONFIG_AVCINTRAPERIOD avc_intra_period;
  OMX_ERRORTYPE error;

  memset(&avc_intra_period, 0, sizeof(OMX_VIDEO_CONFIG_AVCINTRAPERIOD));
  avc_intra_period.nSize = sizeof(OMX_VIDEO_CONFIG_AVCINTRAPERIOD);
  avc_intra_period.nVersion.nVersion = OMX_VERSION;
  avc_intra_period.nPortIndex = VIDEO_ENCODE_OUTPUT_PORT;

  // Distance between two IDR frames
  avc_intra_period.nIDRPeriod = gop_size;

  // It seems this value has no effect for the encoding.
  avc_intra_period.nPFrames = gop_size;

  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexConfigVideoAVCIntraPeriod, &avc_intra_period);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to set video_encode %d AVC intra period: 0x%x\n", VIDEO_ENCODE_OUTPUT_PORT, error);
    exit(EXIT_FAILURE);
  }
}

static void query_sensor_mode() {
  OMX_CONFIG_CAMERASENSORMODETYPE sensor_mode;
  OMX_ERRORTYPE error;
  int num_modes;
  int i;

  memset(&sensor_mode, 0, sizeof(OMX_CONFIG_CAMERASENSORMODETYPE));
  sensor_mode.nSize = sizeof(OMX_CONFIG_CAMERASENSORMODETYPE);
  sensor_mode.nVersion.nVersion = OMX_VERSION;
  sensor_mode.nPortIndex = OMX_ALL;
  sensor_mode.nModeIndex = 0;

  error = OMX_GetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigCameraSensorModes, &sensor_mode);
  if (error != OMX_ErrorNone) {
    log_error("error: failed to get camera sensor mode: 0x%x\n", error);
    return;
  }

  num_modes = sensor_mode.nNumModes;
  for (i = 0; i < num_modes; i++) {
    log_info("\n[camera sensor mode %d]\n", i);
    sensor_mode.nModeIndex = i;
    error = OMX_GetParameter(ILC_GET_HANDLE(camera_component),
        OMX_IndexConfigCameraSensorModes, &sensor_mode);
    if (error != OMX_ErrorNone) {
      log_error("error: failed to get camera sensor mode: 0x%x\n", error);
      return;
    }
    log_info("nWidth: %u\n", sensor_mode.nWidth);
    log_info("nHeight: %u\n", sensor_mode.nHeight);
    log_info("nPaddingRight: %u\n", sensor_mode.nPaddingRight);
    log_info("nPaddingDown: %u\n", sensor_mode.nPaddingDown);
    log_info("eColorFormat: %d\n", sensor_mode.eColorFormat);
    log_info("nFrameRateMax: %u (%.2f fps)\n",
        sensor_mode.nFrameRateMax, sensor_mode.nFrameRateMax / 256.0f);
    log_info("nFrameRateMin: %u (%.2f fps)\n",
        sensor_mode.nFrameRateMin, sensor_mode.nFrameRateMin / 256.0f);
  }
}

static void set_framerate_range(float min_fps, float max_fps) {
  OMX_PARAM_BRCMFRAMERATERANGETYPE framerate_range;
  OMX_ERRORTYPE error;

  if (min_fps == -1.0f && max_fps == -1.0f) {
    return;
  }

  memset(&framerate_range, 0, sizeof(OMX_PARAM_BRCMFRAMERATERANGETYPE));
  framerate_range.nSize = sizeof(OMX_PARAM_BRCMFRAMERATERANGETYPE);
  framerate_range.nVersion.nVersion = OMX_VERSION;
  framerate_range.nPortIndex = CAMERA_CAPTURE_PORT;

  error = OMX_GetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexParamBrcmFpsRange, &framerate_range);
  if (error != OMX_ErrorNone) {
    log_error("error: failed to get framerate range: 0x%x\n", error);
    return;
  }

  if (min_fps != -1.0f) {
    framerate_range.xFramerateLow = min_fps * 65536; // in Q16 format
  }
  if (max_fps != -1.0f) {
    framerate_range.xFramerateHigh = max_fps * 65536; // in Q16 format
  }

  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexParamBrcmFpsRange, &framerate_range);
  if (error != OMX_ErrorNone) {
    log_error("error: failed to set framerate range: 0x%x\n", error);
    return;
  }
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
    log_error("error: failed to set camera exposure to auto: 0x%x\n", error);
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
    log_error("error: failed to set camera exposure to night: 0x%x\n", error);
  }
  current_exposure_mode = EXPOSURE_NIGHT;
}

static void auto_select_exposure(int width, int height, uint8_t *data, float fps) {
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
  float average_y = (float)total_y / (float)count;

  // Approximate exposure time
  float msec_per_frame = 1000.0f / fps;
  float y_per_10msec = average_y * 10.0f / msec_per_frame;
  log_debug(" y=%.1f", y_per_10msec);
  if (y_per_10msec < auto_exposure_threshold) { // in the dark
    if (current_exposure_mode == EXPOSURE_AUTO) {
      log_debug(" ");
      set_exposure_to_night();
    }
  } else if (y_per_10msec >= auto_exposure_threshold) { // in the light
    if (current_exposure_mode == EXPOSURE_NIGHT) {
      log_debug(" ");
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
            send_video_start_time();
            send_audio_start_time();
            log_info("capturing started\n");
          }
        }

        if (is_audio_recording_started == 1) {
          if (video_pending_drop_frames > 0) {
            log_debug("dV");
            video_pending_drop_frames--;
          } else {
            log_debug(".");
            timestamp_update();
            subtitle_update();
            int is_text_changed = text_draw_all(last_video_buffer, video_width_32, video_height_16, 1); // is_video = 1
            if (is_text_changed && is_preview_enabled) {
              // the text has actually changed, redraw preview subtitle overlay
              dispmanx_update_text_overlay();
            }
            encode_and_send_image();
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

// Set red and blue gains used when AWB is off
static int camera_set_custom_awb_gains() {
  OMX_CONFIG_CUSTOMAWBGAINSTYPE custom_awb_gains;
  OMX_ERRORTYPE error;

// NOTE: OMX_IndexConfigCameraSettings is read-only

  memset(&custom_awb_gains, 0, sizeof(OMX_CONFIG_CUSTOMAWBGAINSTYPE));
  custom_awb_gains.nSize = sizeof(OMX_CONFIG_CUSTOMAWBGAINSTYPE);
  custom_awb_gains.nVersion.nVersion = OMX_VERSION;
  custom_awb_gains.xGainR = round(awb_red_gain * 65536); // Q16
  custom_awb_gains.xGainB = round(awb_blue_gain * 65536); // Q16

  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigCustomAwbGains, &custom_awb_gains);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to set camera custom awb gains: 0x%x\n", error);
    return -1;
  }

  return 0;
}

static int camera_set_exposure_value() {
  OMX_CONFIG_EXPOSUREVALUETYPE exposure_value;
  OMX_ERRORTYPE error;
  int i;

  memset(&exposure_value, 0, sizeof(OMX_CONFIG_EXPOSUREVALUETYPE));
  exposure_value.nSize = sizeof(OMX_CONFIG_EXPOSUREVALUETYPE);
  exposure_value.nVersion.nVersion = OMX_VERSION;
  exposure_value.nPortIndex = OMX_ALL;

  error = OMX_GetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigCommonExposureValue, &exposure_value);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to get camera exposure value: 0x%x\n", error);
    exit(EXIT_FAILURE);
  }

  OMX_METERINGTYPE metering = OMX_EVModeMax;
  for (i = 0; i < sizeof(exposure_metering_options) / sizeof(exposure_metering_option); i++) {
    if (strcmp(exposure_metering_options[i].name, exposure_metering) == 0) {
      metering = exposure_metering_options[i].metering;
      break;
    }
  }
  if (metering == OMX_EVModeMax) {
    log_error("error: invalid exposure metering value: %s\n", exposure_metering);
    return -1;
  }
  // default: OMX_MeteringModeAverage
  exposure_value.eMetering = metering;

  if (manual_exposure_compensation) {
    // OMX_S32 Q16; default: 0
    exposure_value.xEVCompensation = round(exposure_compensation * 65536 / 6.0f);
  }

  if (manual_exposure_aperture) {
    // Apparently this has no practical effect

    // OMX_U32 Q16; default: 0
    exposure_value.nApertureFNumber = round(exposure_aperture * 65536);
    // default: OMX_FALSE
    exposure_value.bAutoAperture = OMX_FALSE;
  }

  if (manual_exposure_shutter_speed) {
    // OMX_U32; default: 0
    exposure_value.nShutterSpeedMsec = exposure_shutter_speed;
    // default: OMX_TRUE
    exposure_value.bAutoShutterSpeed = OMX_FALSE;
  }

  if (manual_exposure_sensitivity) {
    // OMX_U32; default: 0
    exposure_value.nSensitivity = exposure_sensitivity;
    // default: OMX_TRUE
    exposure_value.bAutoSensitivity = OMX_FALSE;
  }

  log_debug("setting exposure:\n");
  log_debug("  eMetering: %d\n", exposure_value.eMetering);
  log_debug("  xEVCompensation: %d\n", exposure_value.xEVCompensation);
  log_debug("  nApertureFNumber: %u\n", exposure_value.nApertureFNumber);
  log_debug("  bAutoAperture: %u\n", exposure_value.bAutoAperture);
  log_debug("  nShutterSpeedMsec: %u\n", exposure_value.nShutterSpeedMsec);
  log_debug("  bAutoShutterSpeed: %u\n", exposure_value.bAutoShutterSpeed);
  log_debug("  nSensitivity: %u\n", exposure_value.nSensitivity);
  log_debug("  bAutoSensitivity: %u\n", exposure_value.bAutoSensitivity);

  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigCommonExposureValue, &exposure_value);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to set camera exposure value: 0x%x\n", error);
    return -1;
  }

  return 0;
}

static int camera_set_white_balance(char *wb) {
  OMX_CONFIG_WHITEBALCONTROLTYPE whitebal;
  OMX_ERRORTYPE error;
  int i;

  memset(&whitebal, 0, sizeof(OMX_CONFIG_WHITEBALCONTROLTYPE));
  whitebal.nSize = sizeof(OMX_CONFIG_WHITEBALCONTROLTYPE);
  whitebal.nVersion.nVersion = OMX_VERSION;
  whitebal.nPortIndex = OMX_ALL;

  OMX_WHITEBALCONTROLTYPE control = OMX_WhiteBalControlMax;
  for (i = 0; i < sizeof(white_balance_options) / sizeof(white_balance_option); i++) {
    if (strcmp(white_balance_options[i].name, wb) == 0) {
      control = white_balance_options[i].control;
      break;
    }
  }
  if (control == OMX_WhiteBalControlMax) {
    log_error("error: invalid white balance value: %s\n", wb);
    return -1;
  }
  whitebal.eWhiteBalControl = control;

  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigCommonWhiteBalance, &whitebal);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to set camera white balance: 0x%x\n", error);
    return -1;
  }

  return 0;
}

static int camera_set_exposure_control(char *ex) {
  OMX_CONFIG_EXPOSURECONTROLTYPE exposure_control_type;
  OMX_ERRORTYPE error;
  int i;

  memset(&exposure_control_type, 0, sizeof(OMX_CONFIG_EXPOSURECONTROLTYPE));
  exposure_control_type.nSize = sizeof(OMX_CONFIG_EXPOSURECONTROLTYPE);
  exposure_control_type.nVersion.nVersion = OMX_VERSION;
  exposure_control_type.nPortIndex = OMX_ALL;

  // Find out the value of eExposureControl
  OMX_EXPOSURECONTROLTYPE control = OMX_ExposureControlMax;
  for (i = 0; i < sizeof(exposure_control_options) / sizeof(exposure_control_option); i++) {
    if (strcmp(exposure_control_options[i].name, ex) == 0) {
      control = exposure_control_options[i].control;
      break;
    }
  }
  if (control == OMX_ExposureControlMax) {
    log_error("error: invalid exposure control value: %s\n", ex);
    return -1;
  }
  exposure_control_type.eExposureControl = control;

  log_debug("exposure control: %s\n", ex);
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigCommonExposure, &exposure_control_type);
  if (error != OMX_ErrorNone) {
    log_error("error: failed to set camera exposure control: 0x%x\n", error);
    return -1;
  }

  if (control == OMX_ExposureControlAuto) {
    current_exposure_mode = EXPOSURE_AUTO;
  } else if (control == OMX_ExposureControlNight) {
    current_exposure_mode = EXPOSURE_NIGHT;
  }

  return 0;
}

/* Set region of interest */
static int camera_set_input_crop(float left, float top, float width, float height) {
  OMX_CONFIG_INPUTCROPTYPE input_crop_type;
  OMX_ERRORTYPE error;

  memset(&input_crop_type, 0, sizeof(OMX_CONFIG_INPUTCROPTYPE));
  input_crop_type.nSize = sizeof(OMX_CONFIG_INPUTCROPTYPE);
  input_crop_type.nVersion.nVersion = OMX_VERSION;
  input_crop_type.nPortIndex = OMX_ALL;
  input_crop_type.xLeft = round(left * 0x10000);
  input_crop_type.xTop = round(top * 0x10000);
  input_crop_type.xWidth = round(width * 0x10000);
  input_crop_type.xHeight = round(height * 0x10000);

  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigInputCropPercentages, &input_crop_type);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to set camera input crop type: 0x%x\n", error);
    log_fatal("hint: maybe --roi value is not acceptable to camera\n");
    return -1;
  }

  return 0;
}

static int openmax_cam_open() {
  OMX_PARAM_PORTDEFINITIONTYPE cam_def;
  OMX_ERRORTYPE error;
  OMX_PARAM_PORTDEFINITIONTYPE portdef;
  OMX_PARAM_TIMESTAMPMODETYPE timestamp_mode;
  OMX_CONFIG_DISPLAYREGIONTYPE display_region;
  OMX_CONFIG_ROTATIONTYPE rotation;
  OMX_CONFIG_MIRRORTYPE mirror;
  int r;

  cam_client = ilclient_init();
  if (cam_client == NULL) {
    log_error("error: openmax_cam_open: ilclient_init failed\n");
    return -1;
  }

  ilclient_set_fill_buffer_done_callback(cam_client, cam_fill_buffer_done, 0);

  // create camera_component
  error = ilclient_create_component(cam_client, &camera_component, "camera",
      ILCLIENT_DISABLE_ALL_PORTS |
      ILCLIENT_ENABLE_OUTPUT_BUFFERS);
  if (error != 0) {
    log_fatal("error: failed to create camera component: 0x%x\n", error);
    log_fatal("Have you enabled camera via raspi-config or /boot/config.txt?\n");
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
    log_fatal("error: failed to get camera %d port definition: 0x%x\n", CAMERA_CAPTURE_PORT, error);
    exit(EXIT_FAILURE);
  }
  
  if(sensor_mode != sensor_mode_default) {
    OMX_PARAM_U32TYPE sensorMode;
    memset(&sensorMode, 0, sizeof(OMX_PARAM_U32TYPE));
    sensorMode.nSize = sizeof(OMX_PARAM_U32TYPE);
    sensorMode.nVersion.nVersion = OMX_VERSION;
    sensorMode.nPortIndex = OMX_ALL;
    sensorMode.nU32 = sensor_mode;
    error = OMX_SetParameter( ILC_GET_HANDLE(camera_component), 
      OMX_IndexParamCameraCustomSensorConfig, &sensorMode);
    
    if (error != OMX_ErrorNone) {
      log_fatal("error: failed to set camera sensor mode: 0x%x\n", error);
      exit(EXIT_FAILURE);
    }
  }

  // Configure port 71 (camera capture output)
  cam_def.format.video.nFrameWidth = video_width;
  cam_def.format.video.nFrameHeight = video_height;

  // nStride must be a multiple of 32 and equal to or larger than nFrameWidth.
  cam_def.format.video.nStride = (video_width+31)&~31;

  // nSliceHeight must be a multiple of 16.
  cam_def.format.video.nSliceHeight = (video_height+15)&~15;

  cam_def.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
  if (is_vfr_enabled) {
    log_debug("using variable frame rate\n");
    cam_def.format.video.xFramerate = 0x0; // variable frame rate
  } else {
    cam_def.format.video.xFramerate = fr_q16; // specify the frame rate in Q.16 (framerate * 2^16)
  }

  // This specifies the input pixel format.
  // See http://www.khronos.org/files/openmax_il_spec_1_0.pdf for details.
  cam_def.format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;
  cam_def.nBufferCountActual = N_BUFFER_COUNT_ACTUAL; // Affects to audio/video sync

  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexParamPortDefinition, &cam_def);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to set camera %d port definition: 0x%x\n", CAMERA_CAPTURE_PORT, error);
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
    log_fatal("error: failed to set camera timestamp mode: 0x%x\n", error);
    exit(EXIT_FAILURE);
  }

  // image rotation
  memset(&rotation, 0, sizeof(OMX_CONFIG_ROTATIONTYPE));
  rotation.nSize = sizeof(OMX_CONFIG_ROTATIONTYPE);
  rotation.nVersion.nVersion = OMX_VERSION;
  rotation.nPortIndex = CAMERA_CAPTURE_PORT;
  rotation.nRotation = video_rotation;
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigCommonRotate, &rotation);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to set camera capture %d rotation: 0x%x\n", CAMERA_CAPTURE_PORT, error);
    exit(EXIT_FAILURE);
  }

  // image mirroring (horizontal/vertical flip)
  memset(&mirror, 0, sizeof(OMX_CONFIG_MIRRORTYPE));
  mirror.nSize = sizeof(OMX_CONFIG_MIRRORTYPE);
  mirror.nVersion.nVersion = OMX_VERSION;
  mirror.nPortIndex = CAMERA_CAPTURE_PORT;
  if (video_hflip && video_vflip) {
    mirror.eMirror = OMX_MirrorBoth;
  } else if (video_hflip) {
    mirror.eMirror = OMX_MirrorHorizontal;
  } else if (video_vflip) {
    mirror.eMirror = OMX_MirrorVertical;
  } else {
    mirror.eMirror = OMX_MirrorNone;
  }
  error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
      OMX_IndexConfigCommonMirror, &mirror);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to set camera capture %d mirror (hflip/vflip): 0x%x\n", CAMERA_CAPTURE_PORT, error);
    exit(EXIT_FAILURE);
  }

  if (camera_set_exposure_control(exposure_control) != 0) {
    exit(EXIT_FAILURE);
  }

  if (camera_set_exposure_value() != 0) {
    exit(EXIT_FAILURE);
  }

  // Set region of interest (--roi)
  if (camera_set_input_crop(roi_left, roi_top, roi_width, roi_height) != 0) {
    exit(EXIT_FAILURE);
  }

  // Set camera component to idle state
  if (ilclient_change_component_state(camera_component, OMX_StateIdle) == -1) {
    log_fatal("error: failed to set camera to idle state\n");
    log_fatal("Perhaps another program is using camera, otherwise you need to reboot this pi\n");
    exit(EXIT_FAILURE);
  }

  if (is_clock_enabled) {
    // create clock component
    error = ilclient_create_component(cam_client, &clock_component, "clock",
        ILCLIENT_DISABLE_ALL_PORTS);
    if (error != 0) {
      log_fatal("error: failed to create clock component: 0x%x\n", error);
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
      log_error("error: failed to set clock state: 0x%x\n", error);
    }

    // Set up tunnel from clock to camera
    set_tunnel(tunnel+n_tunnel,
        clock_component, CLOCK_OUTPUT_1_PORT,
        camera_component, CAMERA_INPUT_PORT);

    if (ilclient_setup_tunnel(tunnel+(n_tunnel++), 0, 0) != 0) {
      log_fatal("error: failed to setup tunnel from clock to camera\n");
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
      log_fatal("error: failed to get camera preview %d port definition: 0x%x\n", CAMERA_PREVIEW_PORT, error);
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
      log_fatal("error: failed to set camera preview %d port definition: 0x%x\n", CAMERA_PREVIEW_PORT, error);
      exit(EXIT_FAILURE);
    }

    // image rotation
    memset(&rotation, 0, sizeof(OMX_CONFIG_ROTATIONTYPE));
    rotation.nSize = sizeof(OMX_CONFIG_ROTATIONTYPE);
    rotation.nVersion.nVersion = OMX_VERSION;
    rotation.nPortIndex = CAMERA_PREVIEW_PORT;
    rotation.nRotation = video_rotation;
    error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
        OMX_IndexConfigCommonRotate, &rotation);
    if (error != OMX_ErrorNone) {
      log_fatal("error: failed to set camera preview %d rotation: 0x%x\n", CAMERA_PREVIEW_PORT, error);
      exit(EXIT_FAILURE);
    }

    // image mirroring (horizontal/vertical flip)
    memset(&mirror, 0, sizeof(OMX_CONFIG_MIRRORTYPE));
    mirror.nSize = sizeof(OMX_CONFIG_MIRRORTYPE);
    mirror.nVersion.nVersion = OMX_VERSION;
    mirror.nPortIndex = CAMERA_PREVIEW_PORT;
    if (video_hflip && video_vflip) {
      mirror.eMirror = OMX_MirrorBoth;
    } else if (video_hflip) {
      mirror.eMirror = OMX_MirrorHorizontal;
    } else if (video_vflip) {
      mirror.eMirror = OMX_MirrorVertical;
    } else {
      mirror.eMirror = OMX_MirrorNone;
    }
    error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
        OMX_IndexConfigCommonMirror, &mirror);
    if (error != OMX_ErrorNone) {
      log_fatal("error: failed to set camera preview %d mirror (hflip/vflip): 0x%x\n", CAMERA_PREVIEW_PORT, error);
      exit(EXIT_FAILURE);
    }

    // create render_component
    r = ilclient_create_component(cam_client, &render_component, "video_render",
        ILCLIENT_DISABLE_ALL_PORTS);
    if (r != 0) {
      log_fatal("error: failed to create render component: 0x%x\n", r);
      exit(EXIT_FAILURE);
    }
    component_list[n_component_list++] = render_component;

    // Setup display region for preview window
    memset(&display_region, 0, sizeof(OMX_CONFIG_DISPLAYREGIONTYPE));
    display_region.nSize = sizeof(OMX_CONFIG_DISPLAYREGIONTYPE);
    display_region.nVersion.nVersion = OMX_VERSION;
    display_region.nPortIndex = VIDEO_RENDER_INPUT_PORT;

    // set default display
    display_region.num = DISP_DISPLAY_DEFAULT;

    if (is_previewrect_enabled) { // display preview window at specified position
      display_region.set = OMX_DISPLAY_SET_DEST_RECT | OMX_DISPLAY_SET_FULLSCREEN | OMX_DISPLAY_SET_NOASPECT | OMX_DISPLAY_SET_NUM;
      display_region.dest_rect.x_offset = preview_x;
      display_region.dest_rect.y_offset = preview_y;
      display_region.dest_rect.width = preview_width;
      display_region.dest_rect.height = preview_height;
      display_region.fullscreen = OMX_FALSE;
      display_region.noaspect = OMX_TRUE;
    } else { // fullscreen
      display_region.set = OMX_DISPLAY_SET_FULLSCREEN | OMX_DISPLAY_SET_NUM;
      display_region.fullscreen = OMX_TRUE;
    }

    error = OMX_SetParameter(ILC_GET_HANDLE(render_component),
        OMX_IndexConfigDisplayRegion, &display_region);
    if (error != OMX_ErrorNone) {
      log_fatal("error: failed to set render input %d display region: 0x%x\n", VIDEO_RENDER_INPUT_PORT, error);
      exit(EXIT_FAILURE);
    }

    // Set the opacity and layer of the preview window
    display_region.set = OMX_DISPLAY_SET_ALPHA | OMX_DISPLAY_SET_LAYER;
    display_region.alpha = (OMX_U32) preview_opacity;
    display_region.layer = DISP_LAYER_VIDEO_PREVIEW;
    error = OMX_SetParameter(ILC_GET_HANDLE(render_component),
        OMX_IndexConfigDisplayRegion, &display_region);
    if (error != OMX_ErrorNone) {
      log_fatal("error: failed to set render input %d alpha: 0x%x\n", VIDEO_RENDER_INPUT_PORT, error);
      exit(EXIT_FAILURE);
    }

    // Set up tunnel from camera to video_render
    set_tunnel(tunnel+n_tunnel,
        camera_component, CAMERA_PREVIEW_PORT,
        render_component, VIDEO_RENDER_INPUT_PORT);

    if (ilclient_setup_tunnel(tunnel+(n_tunnel++), 0, 0) != 0) {
      log_fatal("error: failed to setup tunnel from camera to render\n");
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
    log_error("error: cannot get output buffer from video_encode\n");
    return 0;
  }
  if (encbuf != NULL) {
    // merge the previous buffer
    concat_buf = realloc(encbuf, encbuf_size + out->nFilledLen);
    if (concat_buf == NULL) {
      log_fatal("error: cannot allocate memory for concat_buf (%d bytes)\n", encbuf_size + out->nFilledLen);
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
        log_fatal("error: cannot allocate memory for encbuf (%d bytes)\n", buf_len);
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
      log_debug("[NAL%d]", nal_unit_type);
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
        log_fatal("error: cannot allocate memory for codec config (%d bytes)\n", buf_len);
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
          if (divisor == 0.0f) { // This won't cause SIGFPE because of float, but just to be safe.
            fps = 99999.0f;
          } else {
            fps = 1.0f / divisor;
          }
          log_debug(" %5.2f fps k=%d", fps, keyframes_count);
          if (log_get_level() <= LOG_LEVEL_DEBUG) {
            print_audio_timing();
          }
          current_audio_frames = 0;
          frame_count = 0;

          if (is_auto_exposure_enabled) {
            auto_select_exposure(video_width, video_height, last_video_buffer, fps);
          }

          log_debug("\n");
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
  OMX_PARAM_U32TYPE u32;
  OMX_ERRORTYPE error;
  int r;
  int i;

  ilclient = ilclient_init();
  if (ilclient == NULL) {
    log_fatal("error: video_encode_startup: ilclient_init failed\n");
    return -1;
  }

  // create video_encode component
  r = ilclient_create_component(ilclient, &video_encode, "video_encode",
      ILCLIENT_DISABLE_ALL_PORTS |
      ILCLIENT_ENABLE_INPUT_BUFFERS |
      ILCLIENT_ENABLE_OUTPUT_BUFFERS);
  if (r != 0) {
    log_fatal("error: failed to create video_encode component: 0x%x\n", r);
    exit(EXIT_FAILURE);
  }
  component_list[n_component_list++] = video_encode;

  memset(&portdef, 0, sizeof(OMX_PARAM_PORTDEFINITIONTYPE));
  portdef.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
  portdef.nVersion.nVersion = OMX_VERSION;
  portdef.nPortIndex = VIDEO_ENCODE_INPUT_PORT;

  error = OMX_GetParameter(ILC_GET_HANDLE(video_encode), OMX_IndexParamPortDefinition, &portdef);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to get video_encode %d port definition: 0x%x\n", VIDEO_ENCODE_INPUT_PORT, error);
    exit(EXIT_FAILURE);
  }

  // Configure port 200 (video_encode input)
  portdef.format.video.nFrameWidth = video_width;
  portdef.format.video.nFrameHeight = video_height;

  // Do not set xFramerate to 0x0 even when we use VFR (variable frame rate),
  // since it will not work properly when video size is smaller than 1024x768.
  portdef.format.video.xFramerate = fr_q16; // specify the frame rate in Q.16 (framerate * 2^16)

  portdef.format.video.nBitrate = 0x0;
  portdef.format.video.nSliceHeight = (video_height+15)&~15;
  portdef.format.video.nStride = (video_width+31)&~31;
  portdef.nBufferCountActual = VIDEO_ENCODE_INPUT_BUFFER_COUNT;

  // This specifies the input pixel format.
  // See http://www.khronos.org/files/openmax_il_spec_1_0.pdf for details.
  portdef.format.video.eColorFormat = OMX_COLOR_FormatYUV420PackedPlanar;

  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamPortDefinition, &portdef);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to set video_encode %d port definition: 0x%x\n",
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
    log_fatal("error: failed to get video_encode %d port definition: 0x%x\n",
        VIDEO_ENCODE_OUTPUT_PORT, error);
    exit(EXIT_FAILURE);
  }

  // Configure port 201 (video_encode output)
  portdef_encode_output.nBufferCountActual = VIDEO_ENCODE_OUTPUT_BUFFER_COUNT;

  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamPortDefinition, &portdef_encode_output);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to set video_encode %d port definition: 0x%x\n",
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
    log_fatal("error: failed to set video_encode %d port format: 0x%x\n",
        VIDEO_ENCODE_OUTPUT_PORT, error);
    exit(EXIT_FAILURE);
  }

  memset(&avctype, 0, sizeof(OMX_VIDEO_PARAM_AVCTYPE));
  avctype.nSize = sizeof(OMX_VIDEO_PARAM_AVCTYPE);
  avctype.nVersion.nVersion = OMX_VERSION;
  avctype.nPortIndex = VIDEO_ENCODE_OUTPUT_PORT;

  error = OMX_GetParameter (ILC_GET_HANDLE(video_encode), OMX_IndexParamVideoAvc, &avctype);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to get video_encode %d AVC: 0x%x\n", VIDEO_ENCODE_OUTPUT_PORT, error);
    exit(EXIT_FAILURE);
  }

  // Number of P frames between I frames
  avctype.nPFrames = video_gop_size - 1;

  OMX_VIDEO_AVCPROFILETYPE profile = OMX_VIDEO_AVCProfileMax;
  for (i = 0; i < sizeof(video_avc_profile_options) / sizeof(video_avc_profile_option); i++) {
    if (strcmp(video_avc_profile_options[i].name, video_avc_profile) == 0) {
      profile = video_avc_profile_options[i].profile;
      break;
    }
  }
  if (profile == OMX_VIDEO_AVCProfileMax) {
    log_error("error: invalid AVC profile value: %s\n", video_avc_profile);
    return -1;
  }
  avctype.eProfile = profile;

  OMX_VIDEO_AVCLEVELTYPE level = OMX_VIDEO_AVCLevelMax;
  for (i = 0; i < sizeof(video_avc_level_options) / sizeof(video_avc_level_option); i++) {
    if (strcmp(video_avc_level_options[i].name, video_avc_level) == 0) {
      level = video_avc_level_options[i].level;
      break;
    }
  }
  if (level == OMX_VIDEO_AVCLevelMax) {
    log_error("error: invalid AVC level value: %s\n", video_avc_level);
    return -1;
  }
  avctype.eLevel = level;

  // Set AVC parameter
  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamVideoAvc, &avctype);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to set video_encode %d AVC: 0x%x\n", VIDEO_ENCODE_OUTPUT_PORT, error);
    log_fatal("Probably the combination of --avcprofile and --avclevel is not supported on Raspberry Pi\n");
    exit(EXIT_FAILURE);
  }

  // Set GOP size
  set_gop_size(video_gop_size);

  // Set bitrate
  memset(&bitrate_type, 0, sizeof(OMX_VIDEO_PARAM_BITRATETYPE));
  bitrate_type.nSize = sizeof(OMX_VIDEO_PARAM_BITRATETYPE);
  bitrate_type.nVersion.nVersion = OMX_VERSION;
  bitrate_type.nPortIndex = VIDEO_ENCODE_OUTPUT_PORT;
  if (video_bitrate == 0) { // disable rate control
    log_debug("rate control is disabled for video\n");
    bitrate_type.eControlRate = OMX_Video_ControlRateDisable;
    bitrate_type.nTargetBitrate = 0;
  } else {
    bitrate_type.eControlRate = OMX_Video_ControlRateVariable;
    bitrate_type.nTargetBitrate = video_bitrate; // in bits per second
  }

  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamVideoBitrate, &bitrate_type);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to set video_encode %d bitrate: 0x%x\n", VIDEO_ENCODE_OUTPUT_PORT, error);
    exit(EXIT_FAILURE);
  }

  // Minimum quantization level
  if (video_qp_min != -1) {
    memset(&u32, 0, sizeof(OMX_PARAM_U32TYPE));
    u32.nSize = sizeof(OMX_PARAM_U32TYPE);
    u32.nVersion.nVersion = OMX_VERSION;
    u32.nPortIndex = VIDEO_ENCODE_OUTPUT_PORT;
    u32.nU32 = video_qp_min;
    error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
        OMX_IndexParamBrcmVideoEncodeMinQuant, &u32);
    if (error != OMX_ErrorNone) {
      log_fatal("error: failed to set video_encode %d min quant: 0x%x\n", u32.nPortIndex, error);
      exit(EXIT_FAILURE);
    }
  }

  // Maximum quantization level
  if (video_qp_max != -1) {
    memset(&u32, 0, sizeof(OMX_PARAM_U32TYPE));
    u32.nSize = sizeof(OMX_PARAM_U32TYPE);
    u32.nVersion.nVersion = OMX_VERSION;
    u32.nPortIndex = VIDEO_ENCODE_OUTPUT_PORT;
    u32.nU32 = video_qp_max;
    error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
        OMX_IndexParamBrcmVideoEncodeMaxQuant, &u32);
    if (error != OMX_ErrorNone) {
      log_fatal("error: failed to set video_encode %d max quant: 0x%x\n", u32.nPortIndex, error);
      exit(EXIT_FAILURE);
    }
  }

  // Initial quantization level
  if (video_qp_initial != -1) {
    memset(&u32, 0, sizeof(OMX_PARAM_U32TYPE));
    u32.nSize = sizeof(OMX_PARAM_U32TYPE);
    u32.nVersion.nVersion = OMX_VERSION;
    u32.nPortIndex = VIDEO_ENCODE_OUTPUT_PORT;
    u32.nU32 = video_qp_initial;
    error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
        OMX_IndexParamBrcmVideoInitialQuant, &u32);
    if (error != OMX_ErrorNone) {
      log_fatal("error: failed to set video_encode %d initial quant: 0x%x\n", u32.nPortIndex, error);
      exit(EXIT_FAILURE);
    }
  }

  // Slice DQuant level
  if (video_slice_dquant != -1) {
    memset(&u32, 0, sizeof(OMX_PARAM_U32TYPE));
    u32.nSize = sizeof(OMX_PARAM_U32TYPE);
    u32.nVersion.nVersion = OMX_VERSION;
    u32.nPortIndex = VIDEO_ENCODE_OUTPUT_PORT;
    u32.nU32 = video_slice_dquant;
    error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
        OMX_IndexParamBrcmVideoRCSliceDQuant, &u32);
    if (error != OMX_ErrorNone) {
      log_fatal("error: failed to set video_encode %d slice dquant: 0x%x\n", u32.nPortIndex, error);
      exit(EXIT_FAILURE);
    }
  }

  // Separate buffers for each NAL unit
  memset(&boolean_type, 0, sizeof(OMX_CONFIG_BOOLEANTYPE));
  boolean_type.nSize = sizeof(OMX_CONFIG_BOOLEANTYPE);
  boolean_type.nVersion.nVersion = OMX_VERSION;
  boolean_type.bEnabled = OMX_TRUE;

  error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
      OMX_IndexParamBrcmNALSSeparate, &boolean_type);
  if (error != OMX_ErrorNone) {
    log_fatal("error: failed to set video_encode NAL separate: 0x%x\n", error);
    exit(EXIT_FAILURE);
  }

  // Set video_encode component to idle state
  log_debug("Set video_encode state to idle\n");
  if (ilclient_change_component_state(video_encode, OMX_StateIdle) == -1) {
    log_fatal("error: failed to set video_encode to idle state\n");
    exit(EXIT_FAILURE);
  }

  // Enable port buffers for port 71 (camera capture output)
  log_debug("Enable port buffers for camera %d\n", CAMERA_CAPTURE_PORT);
  if (ilclient_enable_port_buffers(camera_component, CAMERA_CAPTURE_PORT, NULL, NULL, NULL) != 0) {
    log_fatal("error: failed to enable port buffers for camera %d\n", CAMERA_CAPTURE_PORT);
    exit(EXIT_FAILURE);
  }

  // Enable port buffers for port 200 (video_encode input)
  log_debug("Enable port buffers for video_encode %d\n", VIDEO_ENCODE_INPUT_PORT);
  if (ilclient_enable_port_buffers(video_encode, VIDEO_ENCODE_INPUT_PORT, NULL, NULL, NULL) != 0) {
    log_fatal("error: failed to enable port buffers for video_encode %d\n", VIDEO_ENCODE_INPUT_PORT);
    exit(EXIT_FAILURE);
  }

  // Enable port buffers for port 201 (video_encode output)
  log_debug("Enable port buffers for video_encode %d\n", VIDEO_ENCODE_OUTPUT_PORT);
  if (ilclient_enable_port_buffers(video_encode, VIDEO_ENCODE_OUTPUT_PORT, NULL, NULL, NULL) != 0) {
    log_fatal("error: failed to enable port buffers for video_encode %d\n", VIDEO_ENCODE_OUTPUT_PORT);
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

  // Get a buffer that we feed raw camera image into
  buf = ilclient_get_input_buffer(video_encode, VIDEO_ENCODE_INPUT_PORT, 1);
  if (buf == NULL) {
    log_error("error: cannot get input buffer from video_encode\n");
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

  // Feed the raw camera image into video_encode
  // OMX_EmptyThisBuffer takes 22000-27000 usec at 1920x1080
  error = OMX_EmptyThisBuffer(ILC_GET_HANDLE(video_encode), buf);
  if (error != OMX_ErrorNone) {
    log_error("error emptying buffer: 0x%x\n", error);
  }

  while (1) {
    // Get H.264 encoded frame from video_encode
    out = ilclient_get_output_buffer(video_encode, VIDEO_ENCODE_OUTPUT_PORT, 1);
    int do_break = 0;
    if (out != NULL) {
      if (out->nFilledLen > 0) {
        video_encode_fill_buffer_done(out);
        out->nFilledLen = 0;
      } else {
        log_debug("e(0x%x)", out->nFlags);
        do_break = 1;
      }

      // If out->nFlags doesn't have ENDOFFRAME,
      // there is remaining buffer for this frame.
      if (out->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) {
        do_break = 1;
      }
    } else {
      do_break = 1;
    }

    // Return the buffer
    error = OMX_FillThisBuffer(ILC_GET_HANDLE(video_encode), out);
    if (error != OMX_ErrorNone) {
      log_error("error filling video_encode buffer: 0x%x\n", error);
    }

    if (do_break) {
      break;
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
    av_strerror(ret, errbuf, sizeof(errbuf));
    log_error("error encoding audio frame: %s\n", errbuf);
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

#if ENABLE_PTS_WRAP_AROUND
    pts = pts % PTS_MODULO;
#endif
    last_pts = pts;
    pkt.pts = pkt.dts = pts;

    if (is_vfr_enabled) {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      time_for_last_pts = ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
    }

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

      av_freep(&tcp_pkt.data);
      av_free_packet(&tcp_pkt);
    }

    if (is_hlsout_enabled) {
      pthread_mutex_lock(&mutex_writing);
      ret = hls_write_packet(hls, &pkt, 0);
      pthread_mutex_unlock(&mutex_writing);
      if (ret < 0) {
        av_strerror(ret, errbuf, sizeof(errbuf));
        log_error("audio frame write error (hls): %s\n", errbuf);
        log_error("please check if the disk is full\n");
      }
    }

    av_free_packet(&pkt);

    current_audio_frames++;
  } else {
    log_error("error: not getting audio output");
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
      log_fatal("microphone error: SUSPEND recovery failed: %s\n", snd_strerror(error));
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
        log_debug("[microphone started]");
        if ( (error = snd_pcm_start(capture_handle)) < 0) {
          log_fatal("error: cannot start microphone: %s\n", snd_strerror(error));
          exit(EXIT_FAILURE);
        }
        break;

      case 0:
        log_debug("not first audio");
        // wait for pcm to become ready
        if ( (error = snd_pcm_wait(capture_handle, -1)) < 0) {
          if ((error = xrun_recovery(capture_handle, error)) < 0) {
            log_fatal("microphone error: snd_pcm_wait: %s\n", snd_strerror(error));
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
        log_fatal("microphone error: mmap begin: %s\n", snd_strerror(error));
        exit(EXIT_FAILURE);
      }
      is_first_audio = 1;
    }
    size_t copy_size = frames * sizeof(short) * audio_channels;
    memcpy(this_samples + read_size, (my_areas[0].addr)+(offset*sizeof(short)*audio_channels), copy_size);
    read_size += copy_size;

    commitres = snd_pcm_mmap_commit(capture_handle, offset, frames);
    if (commitres < 0 || (snd_pcm_uframes_t)commitres != frames) {
      if ((error = xrun_recovery(capture_handle, commitres >= 0 ? commitres : -EPIPE)) < 0) {
        log_fatal("microphone error: mmap commit: %s\n", snd_strerror(error));
        exit(EXIT_FAILURE);
      }
      is_first_audio = 1;
    }
    size -= frames; // needed in the condition of the while loop to check if period is filled
  }

  if (is_audio_preview_enabled) {
    uint16_t *ptr;
    int cptr;
    int err;

    if (!is_audio_preview_device_opened) {
      open_audio_preview_device();
      is_audio_preview_device_opened = 1;
    }

    ptr = this_samples;
    cptr = period_size;
    while (cptr > 0) {
      err = snd_pcm_mmap_writei(audio_preview_handle, ptr, cptr);
      if (err == -EAGAIN) {
        continue;
      }
      if (err < 0) {
        if (xrun_recovery(audio_preview_handle, err) < 0) {
          log_fatal("audio preview error: %s\n", snd_strerror(err));
          exit(EXIT_FAILURE);
        }
        break; // skip one period
      }
      ptr += err * audio_preview_channels;
      cptr -= err;
    }
  }

  if (audio_volume_multiply != 1.0f) {
    int total_samples = period_size * audio_channels;
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
    log_fatal("error: failed to start clock: 0x%x\n", error);
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
    log_fatal("error: failed to stop clock: 0x%x\n", error);
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
    log_fatal("error: failed to start capturing video: 0x%x\n", error);
    exit(EXIT_FAILURE);
  }

  if (is_clock_enabled) {
    start_openmax_clock();
  }

  // set_framerate_range() is effective only after this point
  if (is_vfr_enabled) {
    set_framerate_range(min_fps, max_fps);
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
    log_fatal("error: failed to stop capturing video: 0x%x\n", error);
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

  // Changing the white balance only take effect after this point.
  if (camera_set_white_balance(white_balance) != 0) {
    exit(EXIT_FAILURE);
  }

  if (camera_set_custom_awb_gains() != 0) {
    exit(EXIT_FAILURE);
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
          log_error("microphone error: wait for poll failed\n");
        }
        continue;
      }
    }

    if (avail_flags & AVAIL_AUDIO) {
      read_audio_poll_mmap();
#if AUDIO_BUFFER_CHUNKS > 0
      if (AUDIO_BUFFER_CHUNKS == 0 || is_audio_buffer_filled) {
        memcpy(samples, audio_buffer[audio_buffer_index], period_size * sizeof(short) * audio_channels);
#else
      if (1) {
#endif
        if (is_audio_recording_started == 0) {
          is_audio_recording_started = 1;
          if (is_video_recording_started == 1) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            video_start_time = audio_start_time = ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
            send_video_start_time();
            send_audio_start_time();
            log_info("capturing started\n");
          }
        }
        if (is_video_recording_started == 1) {
          if (audio_pending_drop_frames > 0) {
            log_debug("dA");
            audio_pending_drop_frames--;
          } else {
            if (is_audio_muted) {
              memset(samples, 0, period_size * sizeof(short) * audio_channels);
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
  log_debug("teardown_tcp_output\n");
  mpegts_close_stream(tcp_ctx);
  mpegts_destroy_context(tcp_ctx);
  avformat_network_deinit();
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
      log_error("error: hls_output_dir (%s) is not a directory\n",
          hls_output_dir);
      exit(EXIT_FAILURE);
    }
  }

  if (access(hls_output_dir, R_OK) != 0) {
    log_error("error: cannot access hls_output_dir (%s): %s\n",
        hls_output_dir, strerror(errno));
    exit(EXIT_FAILURE);
  }
}

static void print_program_version() {
  log_info(PROGRAM_VERSION "\n");
}

static void print_usage() {
  log_info(PROGRAM_NAME " version " PROGRAM_VERSION "\n");
  log_info("Usage: " PROGRAM_NAME " [options]\n");
  log_info("\n");
  log_info("Options:\n");
  log_info(" [video]\n");
  log_info("  -w, --width <num>   Width in pixels (default: %d)\n", video_width_default);
  log_info("  -h, --height <num>  Height in pixels (default: %d)\n", video_height_default);
  log_info("  -v, --videobitrate <num>  Video bit rate (default: %ld)\n", video_bitrate_default);
  log_info("                      Set 0 to disable rate control\n");
  log_info("  -f, --fps <num>     Frame rate (default: %.1f)\n", video_fps_default);
  log_info("  -g, --gopsize <num>  GOP size (default: same value as fps)\n");
  log_info("  --vfr               Enable variable frame rate. GOP size will be\n");
  log_info("                      dynamically controlled.\n");
  log_info("  --minfps <num>      Minimum frames per second. Implies --vfr.\n");
  log_info("                      It might not work if width / height >= 1.45.\n");
  log_info("  --maxfps <num>      Maximum frames per second. Implies --vfr.\n");
  log_info("                      It might not work if width / height >= 1.45.\n");
  log_info("  --rotation <num>    Image rotation in clockwise degrees\n");
  log_info("                      (0, 90, 180, 270)\n");
  log_info("  --hflip             Flip image horizontally\n");
  log_info("  --vflip             Flip image vertically\n");
  log_info("  --avcprofile <str>  Set AVC/H.264 profile to one of:\n");
  log_info("                      constrained_baseline/baseline/main/high\n");
  log_info("                      (default: %s)\n", video_avc_profile_default);
  log_info("  --avclevel <value>  Set AVC/H.264 level (default: %s)\n", video_avc_level_default);
  log_info("  --qpmin <num>       Minimum quantization level (0..51)\n");
  log_info("  --qpmax <num>       Maximum quantization level (0..51)\n");
  log_info("  --qpinit <num>      Initial quantization level\n");
  log_info("  --dquant <num>      Slice DQuant level\n");
  log_info(" [audio]\n");
  log_info("  -c, --channels <num>  Audio channels (1=mono, 2=stereo)\n");
  log_info("                      Default is mono. If it fails, stereo is used.\n");
  log_info("  -r, --samplerate <num>  Audio sample rate (default: %d)\n", audio_sample_rate_default);
  log_info("  -a, --audiobitrate <num>  Audio bit rate (default: %ld)\n", audio_bitrate_default);
  log_info("  --alsadev <dev>     ALSA microphone device (default: %s)\n", alsa_dev_default);
  log_info("  --volume <num>      Amplify audio by multiplying the volume by <num>\n");
  log_info("                      (default: %.1f)\n", audio_volume_multiply_default);
  log_info("  --noaudio           Disable audio capturing\n");
  log_info("  --audiopreview      Enable audio preview\n");
  log_info("  --audiopreviewdev <dev>  Audio preview output device (default: %s)\n", audio_preview_dev_default);
  log_info(" [HTTP Live Streaming (HLS)]\n");
  log_info("  -o, --hlsdir <dir>  Generate HTTP Live Streaming files in <dir>\n");
  log_info("  --hlsnumberofsegments <num>  Set the number of segments in the m3u8 playlist (default: %d)\n", hls_number_of_segments);
  log_info("  --hlskeyframespersegment <num>  Set the number of keyframes per video segment (default: %d)\n", hls_keyframes_per_segment_default);
  log_info("  --hlsenc            Enable HLS encryption\n");
  log_info("  --hlsenckeyuri <uri>  Set HLS encryption key URI (default: %s)\n", hls_encryption_key_uri_default);
  log_info("  --hlsenckey <hex>   Set HLS encryption key in hex string\n");
  log_info("                      (default: ");
  log_hex(LOG_LEVEL_INFO, hls_encryption_key_default, sizeof(hls_encryption_key_default));
  log_info(")\n");
  log_info("  --hlsenciv <hex>    Set HLS encryption IV in hex string\n");
  log_info("                      (default: ");
  log_hex(LOG_LEVEL_INFO, hls_encryption_iv_default, sizeof(hls_encryption_iv_default));
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
  log_info("  --autoex            Enable automatic control of camera exposure between\n");
  log_info("                      daylight and night modes. This forces --vfr enabled.\n");
  log_info("  --autoexthreshold <num>  When average value of Y (brightness) for\n");
  log_info("                      10 milliseconds of captured image falls below <num>,\n");
  log_info("                      camera exposure will change to night mode. Otherwise\n");
  log_info("                      camera exposure is in daylight mode. Implies --autoex.\n");
  log_info("                      (default: %.1f)\n", auto_exposure_threshold_default);
  log_info("                      If --verbose option is enabled as well, average value of\n");
  log_info("                      Y is printed like y=28.0.\n");
  log_info("  --ex <value>        Set camera exposure. Implies --vfr. <value> is one of:\n");
  log_info("                        off auto night nightpreview backlight spotlight sports\n");
  log_info("                        snow beach verylong fixedfps antishake fireworks\n");
  log_info("                        largeaperture smallaperture\n");
  log_info("  --wb <value>        Set white balance. <value> is one of:\n");
  log_info("                        off: Disable white balance control\n");
  log_info("                        auto: Automatic white balance control (default)\n");
  log_info("                        sun: The sun provides the light source\n");
  log_info("                        cloudy: The sun provides the light source through clouds\n");
  log_info("                        shade: Light source is the sun and scene is in the shade\n");
  log_info("                        tungsten: Light source is tungsten\n");
  log_info("                        fluorescent: Light source is fluorescent\n");
  log_info("                        incandescent: Light source is incandescent\n");
  log_info("                        flash: Light source is a flash\n");
  log_info("                        horizon: Light source is the sun on the horizon\n");
  log_info("  --wbred <num>       Red gain. Implies \"--wb off\". (0.0 .. 8.0)\n");
  log_info("  --wbblue <num>      Blue gain. Implies \"--wb off\". (0.0 .. 8.0)\n");
  log_info("  --metering <value>  Set metering type. <value> is one of:\n");
  log_info("                        average: Center weight average metering (default)\n");
  log_info("                        spot: Spot (partial) metering\n");
  log_info("                        matrix: Matrix or evaluative metering\n");
  log_info("                        backlit: Assume a backlit image\n");
  log_info("  --evcomp <num>      Set Exposure Value compensation (-24..24) (default: 0)\n");
//  log_info("  --aperture <num>    Set aperture f-stop. Use 2 for f/2. (default: not set)\n");
//  log_info("                      * Not sure if this has practical effect.\n");
  log_info("  --shutter <num>     Set shutter speed in microseconds (default: auto).\n");
  log_info("                      Implies --vfr.\n");
  log_info("  --iso <num>         Set ISO sensitivity (100..800) (default: auto)\n");
  log_info("  --roi <x,y,w,h>     Set region of interest (crop rect) in ratio (0.0-1.0)\n");
  log_info("                      (default: %.0f,%.0f,%.0f,%.0f)\n",
      roi_left_default, roi_top_default, roi_width_default, roi_height_default);
  log_info("  -p, --preview       Display fullscreen preview\n");
  log_info("  --previewrect <x,y,width,height>\n");
  log_info("                      Display preview window at specified position\n");
  log_info("  --opacity           Preview window opacity\n");
  log_info("                      (0=transparent..255=opaque; default=%d)\n", preview_opacity_default);
  log_info("  --blank[=0xAARRGGBB]  Set the video background color to black (or optional ARGB value)\n");
  log_info("  --query             Query camera capabilities then exit\n");
  log_info("  --mode             Specify the camera sensor mode (values depend on the camera hardware)\n");
  log_info(" [timestamp] (may be a bit heavy on Raspberry Pi 1)\n");
  log_info("  --time              Enable timestamp\n");
  log_info("  --timeformat <spec>  Timestamp format (see \"man strftime\" for spec)\n");
  log_info("                       (default: \"%s\")\n", timestamp_format_default);
  log_info("  --timelayout <spec>  Timestamp position (relative mode)\n");
  log_info("                       layout is comma-separated list of:\n");
  log_info("                        top middle bottom  left center right\n");
  log_info("                       (default: bottom,right)\n");
  log_info("  --timehorizmargin <px>  Horizontal margin from edge (default: %d).\n", timestamp_horizontal_margin_default);
  log_info("                          Effective only if --timelayout is used.\n");
  log_info("  --timevertmargin <px>  Vertical margin from edge (default: %d).\n", timestamp_vertical_margin_default);
  log_info("                         Effective only if --timelayout is used.\n");
  log_info("  --timepos <x,y>     Timestamp position (absolute mode)\n");
//  log_info("  --timealign <spec>  Text alignment (left, center, right) (default: left)\n");
  log_info("  --timefontname <name>  Timestamp font name (default: %s)\n", timestamp_font_name_default);
  log_info("  --timefontfile <file>  Timestamp font file. This invalidates --timefontname.\n");
  log_info("  --timefontface <num>  Timestamp font face index (default: %d).\n", timestamp_font_face_index_default);
  log_info("                        Effective only if --timefontfile is used.\n");
  log_info("  --timept <pt>       Text size in points (default: %.1f)\n", timestamp_font_points_default);
  log_info("  --timedpi <num>     DPI for calculating text size (default: %d)\n", timestamp_font_dpi_default);
  log_info("  --timecolor <hex>   Text color (default: %06x)\n", timestamp_color_default);
  log_info("  --timestrokecolor <hex>  Text stroke color (default: %06x)\n", timestamp_stroke_color_default);
  log_info("                      Note that texts are rendered in grayscale.\n");
  log_info("  --timestrokewidth <pt>  Text stroke border radius (default: %.1f).\n",
      timestamp_stroke_width_default);
  log_info("                          To disable stroking borders, set this value to 0.\n");
  log_info("  --timespacing <px>  Additional letter spacing (default: %d)\n", timestamp_letter_spacing_default);
  log_info(" [misc]\n");
  log_info("  --recordbuf <num>   Start recording from <num> keyframes ago\n");
  log_info("                      (must be >= 1; default: %d)\n", record_buffer_keyframes_default);
  log_info("  --statedir <dir>    Set state dir (default: %s)\n", state_dir_default);
  log_info("  --hooksdir <dir>    Set hooks dir (default: %s)\n", hooks_dir_default);
  log_info("  -q, --quiet         Suppress all output except errors\n");
  log_info("  --verbose           Enable verbose output\n");
  log_info("  --version           Print program version\n");
  log_info("  --help              Print this help\n");
}

int main(int argc, char **argv) {
  int ret;

  static struct option long_options[] = {
    { "mode", required_argument, NULL, 0},
    { "width", required_argument, NULL, 'w' },
    { "height", required_argument, NULL, 'h' },
    { "fps", required_argument, NULL, 'f' },
    { "ptsstep", required_argument, NULL, 0 },
    { "videobitrate", required_argument, NULL, 'v' },
    { "gopsize", required_argument, NULL, 'g' },
    { "rotation", required_argument, NULL, 0 },
    { "hflip", no_argument, NULL, 0 },
    { "vflip", no_argument, NULL, 0 },
    { "avcprofile", required_argument, NULL, 0 },
    { "avclevel", required_argument, NULL, 0 },
    { "qpmin", required_argument, NULL, 0 },
    { "qpmax", required_argument, NULL, 0 },
    { "qpinit", required_argument, NULL, 0 },
    { "dquant", required_argument, NULL, 0 },
    { "alsadev", required_argument, NULL, 0 },
    { "audiobitrate", required_argument, NULL, 'a' },
    { "channels", required_argument, NULL, 'c' },
    { "samplerate", required_argument, NULL, 'r' },
    { "hlsdir", required_argument, NULL, 'o' },
    { "hlskeyframespersegment", required_argument, NULL, 0 },
    { "hlsnumberofsegments", required_argument, NULL, 0 },
    { "rtspout", no_argument, NULL, 0 },
    { "rtspvideocontrol", required_argument, NULL, 0 },
    { "rtspvideodata", required_argument, NULL, 0 },
    { "rtspaudiocontrol", required_argument, NULL, 0 },
    { "rtspaudiodata", required_argument, NULL, 0 },
    { "tcpout", required_argument, NULL, 0 },
    { "vfr", no_argument, NULL, 0 },
    { "minfps", required_argument, NULL, 0 },
    { "maxfps", required_argument, NULL, 0 },
    { "autoex", no_argument, NULL, 0 },
    { "autoexthreshold", required_argument, NULL, 0 },
    { "ex", required_argument, NULL, 0 },
    { "wb", required_argument, NULL, 0 },
    { "wbred", required_argument, NULL, 0 },
    { "wbblue", required_argument, NULL, 0 },
    { "metering", required_argument, NULL, 0 },
    { "evcomp", required_argument, NULL, 0 },
    { "aperture", required_argument, NULL, 0 },
    { "shutter", required_argument, NULL, 0 },
    { "iso", required_argument, NULL, 0 },
    { "roi", required_argument, NULL, 0 },
    { "query", no_argument, NULL, 0 },
    { "time", no_argument, NULL, 0 },
    { "timeformat", required_argument, NULL, 0 },
    { "timelayout", required_argument, NULL, 0 },
    { "timehorizmargin", required_argument, NULL, 0 },
    { "timevertmargin", required_argument, NULL, 0 },
    { "timepos", required_argument, NULL, 0 },
    { "timealign", required_argument, NULL, 0 },
    { "timefontname", required_argument, NULL, 0 },
    { "timefontfile", required_argument, NULL, 0 },
    { "timefontface", required_argument, NULL, 0 },
    { "timept", required_argument, NULL, 0 },
    { "timedpi", required_argument, NULL, 0 },
    { "timecolor", required_argument, NULL, 0 },
    { "timestrokecolor", required_argument, NULL, 0 },
    { "timestrokewidth", required_argument, NULL, 0 },
    { "timespacing", required_argument, NULL, 0 },
    { "statedir", required_argument, NULL, 0 },
    { "hooksdir", required_argument, NULL, 0 },
    { "volume", required_argument, NULL, 0 },
    { "noaudio", no_argument, NULL, 0 },
    { "audiopreview", no_argument, NULL, 0 },
    { "audiopreviewdev", required_argument, NULL, 0 },
    { "hlsenc", no_argument, NULL, 0 },
    { "hlsenckeyuri", required_argument, NULL, 0 },
    { "hlsenckey", required_argument, NULL, 0 },
    { "hlsenciv", required_argument, NULL, 0 },
    { "preview", no_argument, NULL, 'p' },
    { "previewrect", required_argument, NULL, 0 },
    { "blank", optional_argument, NULL, 0 },
    { "opacity", required_argument, NULL, 0 },
    { "quiet", no_argument, NULL, 'q' },
    { "recordbuf", required_argument, NULL, 0 },
    { "verbose", no_argument, NULL, 0 },
    { "version", no_argument, NULL, 0 },
    { "help", no_argument, NULL, 0 },
    { 0, 0, 0, 0 },
  };
  int option_index = 0;
  int opt;

  // Turn off buffering for stdout
  setvbuf(stdout, NULL, _IONBF, 0);

  log_set_level(log_level_default);
  log_set_stream(stdout);

  // Set default values of options
  sensor_mode = sensor_mode_default;
  video_width = video_width_default;
  video_height = video_height_default;
  video_fps = video_fps_default;
  video_pts_step = video_pts_step_default;
  video_gop_size = video_gop_size_default;
  video_rotation = video_rotation_default;
  video_hflip = video_hflip_default;
  video_vflip = video_vflip_default;
  video_bitrate = video_bitrate_default;
  strncpy(video_avc_profile, video_avc_profile_default, sizeof(video_avc_profile) - 1);
  video_avc_profile[sizeof(video_avc_profile) - 1] = '\0';
  strncpy(video_avc_level, video_avc_level_default, sizeof(video_avc_level) - 1);
  video_avc_level[sizeof(video_avc_level) - 1] = '\0';
  video_qp_min = video_qp_min_default;
  video_qp_max = video_qp_max_default;
  video_qp_initial = video_qp_initial_default;
  video_slice_dquant = video_slice_dquant_default;
  strncpy(alsa_dev, alsa_dev_default, sizeof(alsa_dev) - 1);
  alsa_dev[sizeof(alsa_dev) - 1] = '\0';
  audio_bitrate = audio_bitrate_default;
  audio_channels = audio_channels_default;
  audio_sample_rate = audio_sample_rate_default;
  is_audio_preview_enabled = is_audio_preview_enabled_default;
  strncpy(audio_preview_dev, audio_preview_dev_default, sizeof(audio_preview_dev) - 1);
  audio_preview_dev[sizeof(audio_preview_dev) - 1] = '\0';
  is_hlsout_enabled = is_hlsout_enabled_default;
  strncpy(hls_output_dir, hls_output_dir_default, sizeof(hls_output_dir) - 1);
  hls_output_dir[sizeof(hls_output_dir) - 1] = '\0';
  is_rtspout_enabled = is_rtspout_enabled_default;
  strncpy(rtsp_video_control_path, rtsp_video_control_path_default,
      sizeof(rtsp_video_control_path) - 1);
  rtsp_video_control_path[sizeof(rtsp_video_control_path) - 1] = '\0';
  strncpy(rtsp_audio_control_path, rtsp_audio_control_path_default,
      sizeof(rtsp_audio_control_path) - 1);
  rtsp_audio_control_path[sizeof(rtsp_audio_control_path) - 1] = '\0';
  strncpy(rtsp_video_data_path, rtsp_video_data_path_default,
      sizeof(rtsp_video_data_path) - 1);
  rtsp_video_data_path[sizeof(rtsp_video_data_path) - 1] = '\0';
  strncpy(rtsp_audio_data_path, rtsp_audio_data_path_default,
      sizeof(rtsp_audio_data_path) - 1);
  rtsp_audio_data_path[sizeof(rtsp_audio_data_path) - 1] = '\0';
  is_tcpout_enabled = is_tcpout_enabled_default;
  is_auto_exposure_enabled = is_auto_exposure_enabled_default;
  is_vfr_enabled = is_vfr_enabled_default;
  auto_exposure_threshold = auto_exposure_threshold_default;
  roi_left = roi_left_default;
  roi_top = roi_top_default;
  roi_width = roi_width_default;
  roi_height = roi_height_default;

  strncpy(white_balance, white_balance_default, sizeof(white_balance) - 1);
  white_balance[sizeof(white_balance) - 1] = '\0';

  strncpy(exposure_control, exposure_control_default, sizeof(exposure_control) - 1);
  exposure_control[sizeof(exposure_control) - 1] = '\0';

  awb_red_gain = awb_red_gain_default;
  awb_blue_gain = awb_blue_gain_default;
  strncpy(exposure_metering, exposure_metering_default, sizeof(exposure_metering) - 1);
  exposure_metering[sizeof(exposure_metering) - 1] = '\0';
  strncpy(state_dir, state_dir_default, sizeof(state_dir) - 1);
  state_dir[sizeof(state_dir) - 1] = '\0';
  strncpy(hooks_dir, hooks_dir_default, sizeof(hooks_dir) - 1);
  hooks_dir[sizeof(hooks_dir) - 1] = '\0';
  audio_volume_multiply = audio_volume_multiply_default;
  is_hls_encryption_enabled = is_hls_encryption_enabled_default;
  hls_keyframes_per_segment = hls_keyframes_per_segment_default;
  hls_number_of_segments = hls_number_of_segments_default;
  strncpy(hls_encryption_key_uri, hls_encryption_key_uri_default,
      sizeof(hls_encryption_key_uri) - 1);
  hls_encryption_key_uri[sizeof(hls_encryption_key_uri) - 1] = '\0';
  is_preview_enabled = is_preview_enabled_default;
  is_previewrect_enabled = is_previewrect_enabled_default;
  preview_opacity = preview_opacity_default;
  record_buffer_keyframes = record_buffer_keyframes_default;
  strncpy(timestamp_format, timestamp_format_default, sizeof(timestamp_format) - 1);
  timestamp_format[sizeof(timestamp_format) - 1] = '\0';
  timestamp_layout = timestamp_layout_default;
  timestamp_horizontal_margin = timestamp_horizontal_margin_default;
  timestamp_vertical_margin = timestamp_vertical_margin_default;
  timestamp_text_align = timestamp_text_align_default;
  strncpy(timestamp_font_name, timestamp_font_name_default, sizeof(timestamp_font_name) - 1);
  timestamp_font_name[sizeof(timestamp_font_name) - 1] = '\0';
  timestamp_font_face_index = timestamp_font_face_index_default;
  timestamp_font_points = timestamp_font_points_default;
  timestamp_font_dpi = timestamp_font_dpi_default;
  timestamp_color = timestamp_color_default;
  timestamp_stroke_color = timestamp_stroke_color_default;
  timestamp_stroke_width = timestamp_stroke_width_default;
  timestamp_letter_spacing = timestamp_letter_spacing_default;

  while ((opt = getopt_long(argc, argv, "w:h:v:f:g:c:r:a:o:pq", long_options, &option_index)) != -1) {
    switch (opt) {
      case 0:
        if (long_options[option_index].flag != 0) {
          break;
        }
        if (strcmp(long_options[option_index].name, "mode") == 0) {
          char* end;
          int value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid sensor mode: %s\n", optarg);
            return EXIT_FAILURE;
          }
          sensor_mode = value;
        } else if (strcmp(long_options[option_index].name, "ptsstep") == 0) {
          char *end;
          int value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid ptsstep: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid ptsstep: %d (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          video_pts_step = value;
          break;
        } else if (strcmp(long_options[option_index].name, "rotation") == 0) {
          char *end;
          int value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid rotation: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          video_rotation = value;
          break;
        } else if (strcmp(long_options[option_index].name, "hflip") == 0) {
          video_hflip = 1;
          break;
        } else if (strcmp(long_options[option_index].name, "vflip") == 0) {
          video_vflip = 1;
          break;
        } else if (strcmp(long_options[option_index].name, "avcprofile") == 0) {
          strncpy(video_avc_profile, optarg, sizeof(video_avc_profile) - 1);
          video_avc_profile[sizeof(video_avc_profile) - 1] = '\0';
          int matched = 0;
          int i;
          for (i = 0; i < sizeof(video_avc_profile_options) / sizeof(video_avc_profile_option); i++) {
            if (strcmp(video_avc_profile_options[i].name, video_avc_profile) == 0) {
              matched = 1;
              break;
            }
          }
          if (!matched) {
            log_fatal("error: invalid avcprofile: %s\n", optarg);
            return EXIT_FAILURE;
          }
        } else if (strcmp(long_options[option_index].name, "avclevel") == 0) {
          strncpy(video_avc_level, optarg, sizeof(video_avc_level) - 1);
          video_avc_level[sizeof(video_avc_level) - 1] = '\0';
          int matched = 0;
          int i;
          for (i = 0; i < sizeof(video_avc_level_options) / sizeof(video_avc_level_option); i++) {
            if (strcmp(video_avc_level_options[i].name, video_avc_level) == 0) {
              matched = 1;
              break;
            }
          }
          if (!matched) {
            log_fatal("error: invalid avclevel: %s\n", optarg);
            return EXIT_FAILURE;
          }
        } else if (strcmp(long_options[option_index].name, "qpmin") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid qpmin: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0 || value > 51) {
            log_fatal("error: invalid qpmin: %d (must be 0 <= qpmin <= 51)\n", value);
            return EXIT_FAILURE;
          }
          video_qp_min = value;
        } else if (strcmp(long_options[option_index].name, "qpmax") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid qpmax: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0 || value > 51) {
            log_fatal("error: invalid qpmax: %d (must be 0 <= qpmax <= 51)\n", value);
            return EXIT_FAILURE;
          }
          video_qp_max = value;
        } else if (strcmp(long_options[option_index].name, "qpinit") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid qpinit: %s\n", optarg);
            return EXIT_FAILURE;
          }
          video_qp_initial = value;
        } else if (strcmp(long_options[option_index].name, "dquant") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid dquant: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid dquant: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          video_slice_dquant = value;
        } else if (strcmp(long_options[option_index].name, "alsadev") == 0) {
          strncpy(alsa_dev, optarg, sizeof(alsa_dev) - 1);
          alsa_dev[sizeof(alsa_dev) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "rtspout") == 0) {
          is_rtspout_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "rtspvideocontrol") == 0) {
          strncpy(rtsp_video_control_path, optarg, sizeof(rtsp_video_control_path) - 1);
          rtsp_video_control_path[sizeof(rtsp_video_control_path) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "rtspaudiocontrol") == 0) {
          strncpy(rtsp_audio_control_path, optarg, sizeof(rtsp_audio_control_path) - 1);
          rtsp_audio_control_path[sizeof(rtsp_audio_control_path) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "rtspvideodata") == 0) {
          strncpy(rtsp_video_data_path, optarg, sizeof(rtsp_video_data_path) - 1);
          rtsp_video_data_path[sizeof(rtsp_video_data_path) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "rtspaudiodata") == 0) {
          strncpy(rtsp_audio_data_path, optarg, sizeof(rtsp_audio_data_path) - 1);
          rtsp_audio_data_path[sizeof(rtsp_audio_data_path) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "tcpout") == 0) {
          is_tcpout_enabled = 1;
          strncpy(tcp_output_dest, optarg, sizeof(tcp_output_dest) - 1);
          tcp_output_dest[sizeof(tcp_output_dest) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "vfr") == 0) {
          is_vfr_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "autoex") == 0) {
          is_auto_exposure_enabled = 1;
          is_vfr_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "autoexthreshold") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid autoexthreshold: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          auto_exposure_threshold = value;
          is_auto_exposure_enabled = 1;
          is_vfr_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "wb") == 0) {
          strncpy(white_balance, optarg, sizeof(white_balance) - 1);
          white_balance[sizeof(white_balance) - 1] = '\0';
          int matched = 0;
          int i;
          for (i = 0; i < sizeof(white_balance_options) / sizeof(white_balance_option); i++) {
            if (strcmp(white_balance_options[i].name, white_balance) == 0) {
              matched = 1;
              break;
            }
          }
          if (!matched) {
            log_fatal("error: invalid white balance: %s\n", optarg);
            return EXIT_FAILURE;
          }
        } else if (strcmp(long_options[option_index].name, "ex") == 0) {
          strncpy(exposure_control, optarg, sizeof(exposure_control) - 1);
          exposure_control[sizeof(exposure_control) - 1] = '\0';
          int matched = 0;
          int i;
          for (i = 0; i < sizeof(exposure_control_options) / sizeof(exposure_control_option); i++) {
            if (strcmp(exposure_control_options[i].name, exposure_control) == 0) {
              matched = 1;
              break;
            }
          }
          if (!matched) {
            log_fatal("error: invalid --ex: %s\n", optarg);
            return EXIT_FAILURE;
          }
          is_vfr_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "wbred") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid camrg: %s\n", optarg);
            return EXIT_FAILURE;
          }
          awb_red_gain = value;
          strcpy(white_balance, "off"); // Turns off AWB
        } else if (strcmp(long_options[option_index].name, "wbblue") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid cambg: %s\n", optarg);
            return EXIT_FAILURE;
          }
          awb_blue_gain = value;
          strcpy(white_balance, "off"); // Turns off AWB
        } else if (strcmp(long_options[option_index].name, "metering") == 0) {
          strncpy(exposure_metering, optarg, sizeof(exposure_metering) - 1);
          exposure_metering[sizeof(exposure_metering) - 1] = '\0';
          int matched = 0;
          int i;
          for (i = 0; i < sizeof(exposure_metering_options) / sizeof(exposure_metering_option); i++) {
            if (strcmp(exposure_metering_options[i].name, exposure_metering) == 0) {
              matched = 1;
              break;
            }
          }
          if (!matched) {
            log_fatal("error: invalid metering: %s\n", optarg);
            return EXIT_FAILURE;
          }
        } else if (strcmp(long_options[option_index].name, "evcomp") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid evcomp: %s\n", optarg);
            return EXIT_FAILURE;
          }
          manual_exposure_compensation = 1;
          exposure_compensation = value;
        } else if (strcmp(long_options[option_index].name, "aperture") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid aperture: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid aperture: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          manual_exposure_aperture = 1;
          exposure_aperture = value;
        } else if (strcmp(long_options[option_index].name, "shutter") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid shutter speed: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid shutter speed: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          manual_exposure_shutter_speed = 1;
          exposure_shutter_speed = value;
          is_vfr_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "iso") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid ISO sensitivity: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid ISO sensitivity: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          manual_exposure_sensitivity = 1;
          exposure_sensitivity = value;
        } else if (strcmp(long_options[option_index].name, "roi") == 0) {
          char *end;
          int i;
          float values[4];
          char *str_ptr = optarg;
          for (i = 0; i < 4; i++) {
            values[i] = strtof(str_ptr, &end);
            if (end == str_ptr || errno == ERANGE) { // parse error
              log_fatal("error: invalid --roi: value must be in x,y,width,height format\n");
              return EXIT_FAILURE;
            }
            if (values[i] < 0.0f || values[i] > 1.0f) {
              log_fatal("error: invalid --roi: %f (must be in the range of 0.0-1.0)\n", values[i]);
              return EXIT_FAILURE;
            }
            str_ptr = end + 1;
          }
          if (*end != '\0') {
            log_fatal("error: invalid --roi: value must be in x,y,width,height format\n");
            return EXIT_FAILURE;
          }
          roi_left = values[0];
          roi_top = values[1];
          roi_width = values[2];
          roi_height = values[3];
        } else if (strcmp(long_options[option_index].name, "minfps") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid minfps: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid minfps: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          min_fps = value;
          is_vfr_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "maxfps") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid maxfps: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid maxfps: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          max_fps = value;
          is_vfr_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "query") == 0) {
          query_and_exit = 1;
        } else if (strcmp(long_options[option_index].name, "time") == 0) {
          is_timestamp_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "timeformat") == 0) {
          strncpy(timestamp_format, optarg, sizeof(timestamp_format) - 1);
          timestamp_format[sizeof(timestamp_format) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "timelayout") == 0) {
          char *comma_p;
          char *search_p = optarg;
          size_t optarg_len = strlen(optarg);
          int param_len;
          LAYOUT_ALIGN layout_align = 0;
          while (1) {
            comma_p = strchr(search_p, ',');
            if (comma_p == NULL) {
              param_len = optarg + optarg_len - search_p;
            } else {
              param_len = comma_p - search_p;
            }
            if (strncmp(search_p, "top", param_len) == 0) {
              layout_align |= LAYOUT_ALIGN_TOP;
            } else if (strncmp(search_p, "middle", param_len) == 0) {
              layout_align |= LAYOUT_ALIGN_MIDDLE;
            } else if (strncmp(search_p, "bottom", param_len) == 0) {
              layout_align |= LAYOUT_ALIGN_BOTTOM;
            } else if (strncmp(search_p, "left", param_len) == 0) {
              layout_align |= LAYOUT_ALIGN_LEFT;
            } else if (strncmp(search_p, "center", param_len) == 0) {
              layout_align |= LAYOUT_ALIGN_CENTER;
            } else if (strncmp(search_p, "right", param_len) == 0) {
              layout_align |= LAYOUT_ALIGN_RIGHT;
            } else {
              log_fatal("error: invalid timelayout found at: %s\n", search_p);
              return EXIT_FAILURE;
            }
            if (comma_p == NULL || optarg + optarg_len - 1 - comma_p <= 0) { // no remaining chars
              break;
            }
            search_p = comma_p + 1;
          }
          timestamp_layout = layout_align;
        } else if (strcmp(long_options[option_index].name, "timehorizmargin") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timehorizmargin: %s\n", optarg);
            return EXIT_FAILURE;
          }
          timestamp_horizontal_margin = value;
        } else if (strcmp(long_options[option_index].name, "timevertmargin") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timevertmargin: %s\n", optarg);
            return EXIT_FAILURE;
          }
          timestamp_vertical_margin = value;
        } else if (strcmp(long_options[option_index].name, "timepos") == 0) {
          char *comma_p = strchr(optarg, ',');
          if (comma_p == NULL) {
            log_fatal("error: invalid timepos format: %s (should be <x>,<y>)\n", optarg);
            return EXIT_FAILURE;
          }

          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || end != comma_p || errno == ERANGE) { // parse error
            log_fatal("error: invalid timepos x: %s\n", optarg);
            return EXIT_FAILURE;
          }
          timestamp_pos_x = value;

          value = strtol(comma_p+1, &end, 10);
          if (end == comma_p+1 || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timepos y: %s\n", comma_p+1);
            return EXIT_FAILURE;
          }
          timestamp_pos_y = value;

          is_timestamp_abs_pos_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "timealign") == 0) {
          char *comma_p;
          char *search_p = optarg;
          size_t optarg_len = strlen(optarg);
          int param_len;
          TEXT_ALIGN text_align = 0;
          while (1) {
            comma_p = strchr(search_p, ',');
            if (comma_p == NULL) {
              param_len = optarg + optarg_len - search_p;
            } else {
              param_len = comma_p - search_p;
            }
            if (strncmp(search_p, "left", param_len) == 0) {
              text_align |= TEXT_ALIGN_LEFT;
            } else if (strncmp(search_p, "center", param_len) == 0) {
              text_align |= TEXT_ALIGN_CENTER;
            } else if (strncmp(search_p, "right", param_len) == 0) {
              text_align |= TEXT_ALIGN_RIGHT;
            } else {
              log_fatal("error: invalid timealign found at: %s\n", search_p);
              return EXIT_FAILURE;
            }
            if (comma_p == NULL || optarg + optarg_len - 1 - comma_p <= 0) { // no remaining chars
              break;
            }
            search_p = comma_p + 1;
          }
        } else if (strcmp(long_options[option_index].name, "timefontname") == 0) {
          strncpy(timestamp_font_name, optarg, sizeof(timestamp_font_name) - 1);
          timestamp_font_name[sizeof(timestamp_font_name) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "timefontfile") == 0) {
          strncpy(timestamp_font_file, optarg, sizeof(timestamp_font_file) - 1);
          timestamp_font_file[sizeof(timestamp_font_file) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "timefontface") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timefontface: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid timefontface: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          timestamp_font_face_index = value;
        } else if (strcmp(long_options[option_index].name, "timept") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timept: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value <= 0.0f) {
            log_fatal("error: invalid timept: %.1f (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          timestamp_font_points = value;
        } else if (strcmp(long_options[option_index].name, "timedpi") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timedpi: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid timedpi: %d (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          timestamp_font_dpi = value;
        } else if (strcmp(long_options[option_index].name, "timecolor") == 0) {
          char *end;
          long value = strtol(optarg, &end, 16);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timecolor: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid timecolor: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          timestamp_color = value;
        } else if (strcmp(long_options[option_index].name, "timestrokecolor") == 0) {
          char *end;
          long value = strtol(optarg, &end, 16);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timecolor: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid timecolor: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          timestamp_stroke_color = value;
        } else if (strcmp(long_options[option_index].name, "timestrokewidth") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timestrokewidth: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0.0f) {
            log_fatal("error: invalid timestrokewidth: %.1f (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          timestamp_stroke_width = value;
        } else if (strcmp(long_options[option_index].name, "timespacing") == 0) {
          char *end;
          long value = strtol(optarg, &end, 16);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timespacing: %s\n", optarg);
            return EXIT_FAILURE;
          }
          timestamp_letter_spacing = value;
        } else if (strcmp(long_options[option_index].name, "statedir") == 0) {
          strncpy(state_dir, optarg, sizeof(state_dir) - 1);
          state_dir[sizeof(state_dir) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "hooksdir") == 0) {
          strncpy(hooks_dir, optarg, sizeof(hooks_dir) - 1);
          hooks_dir[sizeof(hooks_dir) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "volume") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid volume: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value < 0.0) {
            log_fatal("error: invalid volume: %.1f (must be >= 0.0)\n", value);
            return EXIT_FAILURE;
          }
          audio_volume_multiply = value;
        } else if (strcmp(long_options[option_index].name, "noaudio") == 0) {
          disable_audio_capturing = 1;
        } else if (strcmp(long_options[option_index].name, "audiopreview") == 0) {
          is_audio_preview_enabled = 1;
          break;
        } else if (strcmp(long_options[option_index].name, "audiopreviewdev") == 0) {
          strncpy(audio_preview_dev, optarg, sizeof(audio_preview_dev) - 1);
          audio_preview_dev[sizeof(audio_preview_dev) - 1] = '\0';
          break;
        } else if (strcmp(long_options[option_index].name, "hlskeyframespersegment") == 0) { 
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid hlskeyframespersegment: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid hlskeyframespersegment: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          hls_keyframes_per_segment = (int) value;
        } else if (strcmp(long_options[option_index].name, "hlsnumberofsegments") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid hlsnumberofsegments: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid hlsnumberofsegments: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          hls_number_of_segments = (int) value;
        } else if (strcmp(long_options[option_index].name, "hlsenc") == 0) {
          is_hls_encryption_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "hlsenckeyuri") == 0) {
          strncpy(hls_encryption_key_uri, optarg, sizeof(hls_encryption_key_uri) - 1);
          hls_encryption_key_uri[sizeof(hls_encryption_key_uri) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "hlsenckey") == 0) {
          int i;
          for (i = 0; i < 16; i++) {
            int value;
            if (sscanf(optarg + i * 2, "%02x", &value) == 1) {
              hls_encryption_key[i] = value;
            } else {
              log_fatal("error: invalid hlsenckey: %s\n", optarg);
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
              log_fatal("error: invalid hlsenciv: %s\n", optarg);
              print_usage();
              return EXIT_FAILURE;
            }
          }
        } else if (strcmp(long_options[option_index].name, "previewrect") == 0) {
          char *token;
          char *saveptr = NULL;
          char *end;
          int i;
          long value;
          for (i = 1; ; i++, optarg = NULL) {
            token = strtok_r(optarg, ",", &saveptr);
            if (token == NULL) {
              break;
            }
            value = strtol(token, &end, 10);
            if (end == token || *end != '\0' || errno == ERANGE) { // parse error
              log_fatal("error: invalid previewrect number: %s\n", token);
              return EXIT_FAILURE;
            }
            switch (i) {
              case 1:
                preview_x = value;
                break;
              case 2:
                preview_y = value;
                break;
              case 3:
                preview_width = value;
                break;
              case 4:
                preview_height = value;
                break;
              default: // too many tokens
                log_fatal("error: invalid previewrect\n");
                return EXIT_FAILURE;
            }
          }
          if (i != 5) { // too few tokens
            log_fatal("error: invalid previewrect\n");
            return EXIT_FAILURE;
          }
          is_preview_enabled = 1;
          is_previewrect_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "blank") == 0) {
          blank_background_color = optarg ? strtoul(optarg, NULL, 0) : BLANK_BACKGROUND_DEFAULT;
          break;
        } else if (strcmp(long_options[option_index].name, "opacity") == 0) {
          char *end;
          int value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid opacity: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          preview_opacity = value;
          break;
        } else if (strcmp(long_options[option_index].name, "recordbuf") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid recordbuf: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value < 1) {
            log_fatal("error: invalid recordbuf: %ld (must be >= 1)\n", value);
            return EXIT_FAILURE;
          }
          record_buffer_keyframes = value;
        } else if (strcmp(long_options[option_index].name, "verbose") == 0) {
          log_set_level(LOG_LEVEL_DEBUG);
        } else if (strcmp(long_options[option_index].name, "version") == 0) {
          print_program_version();
          return EXIT_SUCCESS;
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
            log_fatal("error: invalid width: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid width: %ld (must be > 0)\n", value);
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
            log_fatal("error: invalid height: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid height: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          video_height = value;
          break;
        }
      case 'f':
        {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid fps: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0.0) {
            log_fatal("error: invalid fps: %.1f (must be > 0.0)\n", value);
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
            log_fatal("error: invalid gopsize: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid gopsize: %ld (must be > 0)\n", value);
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
            log_fatal("error: invalid videobitrate: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid videobitrate: %ld (must be >= 0)\n", value);
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
            log_fatal("error: invalid audiobitrate: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid audiobitrate: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          audio_bitrate = value;
          break;
        }
      case 'c':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid channels: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value != 1 && value != 2) {
            log_fatal("error: invalid channels: %ld (must be 1 or 2)\n", value);
            return EXIT_FAILURE;
          }
          audio_channels = value;
          is_audio_channels_specified = 1;
          break;
        }
      case 'r':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid samplerate: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid samplerate: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          audio_sample_rate = value;
          break;
        }
      case 'o':
        is_hlsout_enabled = 1;
        strncpy(hls_output_dir, optarg, sizeof(hls_output_dir) - 1);
        hls_output_dir[sizeof(hls_output_dir) - 1] = '\0';
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

  // Print a warning if the video size is larger than 1280x720
  if (video_width * video_height > 1280 * 720) {
    if (strcmp(video_avc_profile, "high") != 0 || strcmp(video_avc_level, "4") != 0) {
      log_info("using AVC High Profile Level 4\n");
      snprintf(video_avc_profile, sizeof(video_avc_profile), "high");
      snprintf(video_avc_level, sizeof(video_avc_level), "4");
    }
    if (!is_vfr_enabled && video_fps > 20.0f) {
      log_warn("warn: fps > 20 might not work properly when width and height is large.\n");
      log_warn("      Use lower --fps or use --vfr. If you still want to use this\n");
      log_warn("      configuration, see if picam keeps up with %.1f fps using --verbose.\n", video_fps);
    }
  }

  if (is_vfr_enabled &&
      (min_fps != -1.0f || max_fps != -1.0f) &&
      (float) video_width / (float) video_height >= 1.45) {
    log_warn("warning: --minfps and --maxfps might not work because width (%d) / height (%d) >= approx 1.45\n", video_width, video_height);
  }

  fr_q16 = video_fps * 65536;
  if (video_pts_step == video_pts_step_default) {
    video_pts_step = round(90000 / video_fps);

    // It appears that the minimum fps is 1.31
    if (video_pts_step > 68480) {
      video_pts_step = 68480;
    }
  }
  if (video_gop_size == video_gop_size_default) {
    video_gop_size = ceil(video_fps);
  }
  mpegts_set_config(video_bitrate, video_width, video_height);
  audio_min_value = (int) (-32768 / audio_volume_multiply);
  audio_max_value = (int) (32767 / audio_volume_multiply);
  keyframe_pointers = calloc(sizeof(int) * record_buffer_keyframes, 1);
  if (keyframe_pointers == NULL) {
    log_fatal("error: cannot allocate memory for keyframe_pointers\n");
    exit(EXIT_FAILURE);
  }

  log_debug("video_width=%d\n", video_width);
  log_debug("video_height=%d\n", video_height);
  log_debug("video_fps=%f\n", video_fps);
  log_debug("fr_q16=%d\n", fr_q16);
  log_debug("video_pts_step=%d\n", video_pts_step);
  log_debug("video_gop_size=%d\n", video_gop_size);
  log_debug("video_rotation=%d\n", video_rotation);
  log_debug("video_hflip=%d\n", video_hflip);
  log_debug("video_vflip=%d\n", video_vflip);
  log_debug("video_bitrate=%ld\n", video_bitrate);
  log_debug("video_avc_profile=%s\n", video_avc_profile);
  log_debug("video_avc_level=%s\n", video_avc_level);
  log_debug("video_qp_min=%d\n", video_qp_min);
  log_debug("video_qp_max=%d\n", video_qp_max);
  log_debug("video_qp_initial=%d\n", video_qp_initial);
  log_debug("video_slice_dquant=%d\n", video_slice_dquant);
  log_debug("alsa_dev=%s\n", alsa_dev);
  log_debug("audio_channels=%d\n", audio_channels);
  log_debug("audio_sample_rate=%d\n", audio_sample_rate);
  log_debug("audio_bitrate=%ld\n", audio_bitrate);
  log_debug("audio_volume_multiply=%f\n", audio_volume_multiply);
  log_debug("is_hlsout_enabled=%d\n", is_hlsout_enabled);
  log_debug("is_hls_encryption_enabled=%d\n", is_hls_encryption_enabled);
  log_debug("hls_keyframes_per_segment=%d\n", hls_keyframes_per_segment);
  log_debug("hls_number_of_segments=%d\n", hls_number_of_segments);
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
  log_debug("auto_exposure_threshold=%f\n", auto_exposure_threshold);
  log_debug("is_vfr_enabled=%d\n", is_vfr_enabled);
  log_debug("white_balance=%s\n", white_balance);
  log_debug("exposure_control=%s\n", exposure_control);
  log_debug("awb_red_gain=%f\n", awb_red_gain);
  log_debug("awb_blue_gain=%f\n", awb_blue_gain);
  log_debug("metering=%s\n", exposure_metering);
  log_debug("manual_exposure_compensation=%d\n", manual_exposure_compensation);
  log_debug("exposure_compensation=%f\n", exposure_compensation);
  log_debug("manual_exposure_aperture=%d\n", manual_exposure_aperture);
  log_debug("exposure_aperture=%u\n", exposure_aperture);
  log_debug("manual_exposure_shutter_speed=%d\n", manual_exposure_shutter_speed);
  log_debug("exposure_shutter_speed=%u\n", exposure_shutter_speed);
  log_debug("manual_exposure_sensitivity=%d\n", manual_exposure_sensitivity);
  log_debug("exposure_sensitivity=%u\n", exposure_sensitivity);
  log_debug("roi_left=%f\n", roi_left);
  log_debug("roi_top=%f\n", roi_top);
  log_debug("roi_width=%f\n", roi_width);
  log_debug("roi_height=%f\n", roi_height);
  log_debug("min_fps=%f\n", min_fps);
  log_debug("max_fps=%f\n", max_fps);
  log_debug("is_timestamp_enabled=%d\n", is_timestamp_enabled);
  log_debug("timestamp_format=%s\n", timestamp_format);
  log_debug("timestamp_layout=%d\n", timestamp_layout);
  log_debug("timestamp_horizontal_margin=%d\n", timestamp_horizontal_margin);
  log_debug("timestamp_vertical_margin=%d\n", timestamp_vertical_margin);
  log_debug("is_timestamp_abs_pos_enabled=%d\n", is_timestamp_abs_pos_enabled);
  log_debug("timestamp_pos_x=%d\n", timestamp_pos_x);
  log_debug("timestamp_pos_y=%d\n", timestamp_pos_y);
  log_debug("timestamp_text_align=%d\n", timestamp_text_align);
  log_debug("timestamp_font_name=%s\n", timestamp_font_name);
  log_debug("timestamp_font_file=%s\n", timestamp_font_file);
  log_debug("timestamp_font_face_index=%s\n", timestamp_font_face_index);
  log_debug("timestamp_font_points=%1f\n", timestamp_font_points);
  log_debug("timestamp_font_dpi=%d\n", timestamp_font_dpi);
  log_debug("timestamp_color=%06x\n", timestamp_color);
  log_debug("timestamp_stroke_color=%06x\n", timestamp_stroke_color);
  log_debug("timestamp_stroke_width=%.f\n", timestamp_stroke_width);
  log_debug("timestamp_letter_spacing=%d\n", timestamp_letter_spacing);
  log_debug("is_preview_enabled=%d\n", is_preview_enabled);
  log_debug("is_previewrect_enabled=%d\n", is_previewrect_enabled);
  log_debug("preview_x=%d\n", preview_x);
  log_debug("preview_y=%d\n", preview_y);
  log_debug("preview_width=%d\n", preview_width);
  log_debug("preview_height=%d\n", preview_height);
  log_debug("preview_opacity=%d\n", preview_opacity);
  log_debug("blank_background_color=0x%x\n", blank_background_color);
  log_debug("is_audio_preview_enabled=%d\n", is_audio_preview_enabled);
  log_debug("audio_preview_dev=%s\n", audio_preview_dev);
  log_debug("record_buffer_keyframes=%d\n", record_buffer_keyframes);
  log_debug("state_dir=%s\n", state_dir);
  log_debug("hooks_dir=%s\n", hooks_dir);

  video_width_32 = (video_width+31)&~31;
  video_height_16 = (video_height+15)&~15;

  if (!query_and_exit) {
    if (state_create_dir(state_dir) != 0) {
      return EXIT_FAILURE;
    }
    if (hooks_create_dir(hooks_dir) != 0) {
      return EXIT_FAILURE;
    }

    create_dir(rec_dir);
    create_dir(rec_tmp_dir);
    create_dir(rec_archive_dir);

    if (is_hlsout_enabled) {
      ensure_hls_dir_exists();
    }

    state_set(state_dir, "record", "false");

    if (clear_hooks(hooks_dir) != 0) {
      log_error("error: clear_hooks() failed\n");
    }
    start_watching_hooks(&hooks_thread, hooks_dir, on_file_create, 1);

    setup_socks();
  }

  if (is_preview_enabled || is_clock_enabled) {
    memset(tunnel, 0, sizeof(tunnel));
  }

  log_info("configuring devices\n");

  bcm_host_init();

  ret = OMX_Init();
  if (ret != OMX_ErrorNone) {
    log_fatal("error: OMX_Init failed: 0x%x\n", ret);
    ilclient_destroy(ilclient);
    return 1;
  }
  memset(component_list, 0, sizeof(component_list));

  if (is_preview_enabled) {
    // setup dispmanx preview (backgroud + subtitle overlay)
    dispmanx_init(blank_background_color, video_width, video_height);
  }

  ret = openmax_cam_open();
  if (ret != 0) {
    log_fatal("error: openmax_cam_open failed: %d\n", ret);
    return ret;
  }
  ret = video_encode_startup();
  if (ret != 0) {
    log_fatal("error: video_encode_startup failed: %d\n", ret);
    return ret;
  }

  av_log_set_level(AV_LOG_ERROR);

  if (!query_and_exit) {
    if (disable_audio_capturing) {
      log_debug("audio capturing is disabled\n");
    } else {
      ret = open_audio_capture_device();
      if (ret == -1) {
        log_warn("warning: audio capturing is disabled\n");
        disable_audio_capturing = 1;
      } else if (ret < 0) {
        log_fatal("error: init_audio failed: %d\n", ret);
        exit(EXIT_FAILURE);
      }
    }

    if (disable_audio_capturing) {
      // HLS will not work when video-only, so we add silent audio track.
      audio_channels = 1;
      codec_settings.audio_sample_rate = audio_sample_rate;
      codec_settings.audio_bit_rate = 1000;
      codec_settings.audio_channels = audio_channels;
      codec_settings.audio_profile = FF_PROFILE_AAC_LOW;
    } else {
      preconfigure_microphone();
      codec_settings.audio_sample_rate = audio_sample_rate;
      codec_settings.audio_bit_rate = audio_bitrate;
      codec_settings.audio_channels = audio_channels;
      codec_settings.audio_profile = FF_PROFILE_AAC_LOW;
    }

    if (is_tcpout_enabled) {
      setup_tcp_output();
    }

    // From http://tools.ietf.org/html/draft-pantos-http-live-streaming-12#section-6.2.1
    //
    // The server MUST NOT remove a media segment from the Playlist file if
    // the duration of the Playlist file minus the duration of the segment
    // is less than three times the target duration.
    //
    // So, num_recent_files should be 3 at the minimum.
#if AUDIO_ONLY
    hls = hls_create_audio_only(hls_number_of_segments, &codec_settings); // 2 == num_recent_files
#else
    hls = hls_create(hls_number_of_segments, &codec_settings); // 2 == num_recent_files
#endif

    if (is_hlsout_enabled) {
      hls->dir = hls_output_dir;
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
      memset(samples, 0, period_size * sizeof(short) * audio_channels);
      is_audio_recording_started = 1;
    } else {
      ret = configure_audio_capture_device();
      if (ret != 0) {
        log_fatal("error: configure_audio_capture_device: ret=%d\n", ret);
        exit(EXIT_FAILURE);
      }
    }

    prepare_encoded_packets();
  }

  struct sigaction int_handler = {.sa_handler = stopSignalHandler};
  sigaction(SIGINT, &int_handler, NULL);
  sigaction(SIGTERM, &int_handler, NULL);

  // initialize text library
  text_init();
  // setup timestamp
  if (is_timestamp_enabled) {
    if (timestamp_font_file[0] != 0) {
      timestamp_init(timestamp_font_file, timestamp_font_face_index,
          timestamp_font_points, timestamp_font_dpi);
    } else if (timestamp_font_name[0] != 0) {
      timestamp_init_with_font_name(timestamp_font_name,
          timestamp_font_points, timestamp_font_dpi);
    } else {
      timestamp_init_with_font_name(NULL,
          timestamp_font_points, timestamp_font_dpi);
    }
    timestamp_set_format(timestamp_format);
    if (is_timestamp_abs_pos_enabled) {
      timestamp_set_position(timestamp_pos_x, timestamp_pos_y);
    } else {
      timestamp_set_layout(timestamp_layout,
          timestamp_horizontal_margin, timestamp_vertical_margin);
    }
    timestamp_set_align(timestamp_text_align);
    timestamp_set_color(timestamp_color);
    timestamp_set_stroke_color(timestamp_stroke_color);
    timestamp_set_stroke_width(timestamp_stroke_width);
    timestamp_set_letter_spacing(timestamp_letter_spacing);
    timestamp_fix_position(video_width_32, video_height_16);
  }

  if (query_and_exit) {
    query_sensor_mode();
  } else {
    openmax_cam_loop();

    if (disable_audio_capturing) {
      pthread_create(&audio_nop_thread, NULL, audio_nop_loop, NULL);
      pthread_join(audio_nop_thread, NULL);
    } else {
      log_debug("start capturing audio\n");
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
      log_debug("hit Ctrl-\\ to force stop\n");
      pthread_cond_wait(&camera_finish_cond, &camera_finish_mutex);
    }
    pthread_mutex_unlock(&camera_finish_mutex);
  }

  stop_openmax_capturing();
  if (is_preview_enabled) {
    dispmanx_destroy();
  }
  shutdown_openmax();
  shutdown_video();

  if (!query_and_exit) {
    log_debug("teardown_audio_encode\n");
    teardown_audio_encode();

    if (!disable_audio_capturing) {
      log_debug("teardown_audio_capture_device\n");
      teardown_audio_capture_device();
      if (is_audio_preview_device_opened) {
        log_debug("teardown_audio_preview_device\n");
        teardown_audio_preview_device();
      }
    }

    log_debug("hls_destroy\n");
    hls_destroy(hls);
  }

  log_debug("pthread_mutex_destroy\n");
  pthread_mutex_destroy(&mutex_writing);
  pthread_mutex_destroy(&rec_mutex);
  pthread_mutex_destroy(&rec_write_mutex);
  pthread_mutex_destroy(&camera_finish_mutex);
  pthread_mutex_destroy(&tcp_mutex);
  pthread_cond_destroy(&rec_cond);
  pthread_cond_destroy(&camera_finish_cond);

  if (!query_and_exit) {
    if (is_tcpout_enabled) {
      teardown_tcp_output();
    }

    log_debug("teardown_socks\n");
    teardown_socks();

    log_debug("free_encoded_packets\n");
    free_encoded_packets();

    log_debug("stop_watching_hooks\n");
    stop_watching_hooks();
    log_debug("pthread_join hooks_thread\n");
    pthread_join(hooks_thread, NULL);
  }

  log_debug("free keyframe_pointers\n");
  free(keyframe_pointers);

  log_debug("shutdown successful\n");
  return 0;
}

#if defined(__cplusplus)
}
#endif
