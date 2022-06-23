#pragma once

#include <libcamera/control_ids.h>
#include <linux/videodev2.h>

#include "log/log.h"
#include "text/text.h"

#define PROGRAM_NAME     "picam"
#define PROGRAM_VERSION  "2.0.0"

typedef struct white_balance_option {
  const char *name;
  libcamera::controls::AwbModeEnum control;
} white_balance_option;
const white_balance_option white_balance_options[] = {
  { "off",          libcamera::controls::AwbCustom },
  { "auto",         libcamera::controls::AwbAuto },
  // { "sun",          OMX_WhiteBalControlSunLight },
  { "cloudy",       libcamera::controls::AwbCloudy },
  // { "shade",        OMX_WhiteBalControlShade },
  { "tungsten",     libcamera::controls::AwbTungsten },
  { "fluorescent",  libcamera::controls::AwbFluorescent },
  { "incandescent", libcamera::controls::AwbIncandescent },
  // { "flash",        OMX_WhiteBalControlFlash },
  // { "horizon",      OMX_WhiteBalControlHorizon },
  // { "greyworld",    OMX_WhiteBalControlGreyWorld },
  { "indoor", libcamera::controls::AwbIndoor },
  { "daylight", libcamera::controls::AwbDaylight },
};

typedef struct exposure_control_option {
  const char *name;
  libcamera::controls::AeExposureModeEnum control;
} exposure_control_option;
const exposure_control_option exposure_control_options[] = {
  { "off",           libcamera::controls::ExposureCustom },
  { "normal",           libcamera::controls::ExposureNormal },
  { "short",           libcamera::controls::ExposureShort },
  { "long",           libcamera::controls::ExposureLong },
  // { "auto",          libcamera::controls::ExposureAuto },
  // { "night",         OMX_ExposureControlNight },
  // { "nightpreview",  OMX_ExposureControlNightWithPreview },
  // { "backlight",     OMX_ExposureControlBackLight },
  // { "spotlight",     OMX_ExposureControlSpotLight },
  // { "sports",        OMX_ExposureControlSports },
  // { "snow",          OMX_ExposureControlSnow },
  // { "beach",         OMX_ExposureControlBeach },
  // { "verylong",      OMX_ExposureControlVeryLong },
  // { "fixedfps",      OMX_ExposureControlFixedFps },
  // { "antishake",     OMX_ExposureControlAntishake },
  // { "fireworks",     OMX_ExposureControlFireworks },
  // { "largeaperture", OMX_ExposureControlLargeAperture },
  // { "smallaperture", OMX_ExposureControlSmallAperture },
};

typedef struct exposure_metering_option {
  const char *name;
  libcamera::controls::AeMeteringModeEnum metering;
} exposure_metering_option;
const exposure_metering_option exposure_metering_options[] = {
  { "centre_weighted", libcamera::controls::MeteringCentreWeighted },
  // { "average", OMX_MeteringModeAverage },
  { "spot",    libcamera::controls::MeteringSpot },
  { "matrix",  libcamera::controls::MeteringMatrix },
  { "custom",  libcamera::controls::MeteringCustom },
  // { "backlit", OMX_MeteringModeBacklit },
};

typedef struct video_avc_profile_option {
  const char *name;
  v4l2_mpeg_video_h264_profile profile;
} video_avc_profile_option;
const video_avc_profile_option video_avc_profile_options[] = {
  { "constrained_baseline", V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE },
  { "baseline",             V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE },
  { "main",                 V4L2_MPEG_VIDEO_H264_PROFILE_MAIN },
  { "high",                 V4L2_MPEG_VIDEO_H264_PROFILE_HIGH },
};

typedef struct video_avc_level_option {
  const char *name;
  v4l2_mpeg_video_h264_level level;
} video_avc_level_option;
const video_avc_level_option video_avc_level_options[] = {
  { "1",   V4L2_MPEG_VIDEO_H264_LEVEL_1_0 },
  { "1b",  V4L2_MPEG_VIDEO_H264_LEVEL_1B },
  { "1.1", V4L2_MPEG_VIDEO_H264_LEVEL_1_1 },
  { "1.2", V4L2_MPEG_VIDEO_H264_LEVEL_1_2 },
  { "1.3", V4L2_MPEG_VIDEO_H264_LEVEL_1_3 },
  { "2",   V4L2_MPEG_VIDEO_H264_LEVEL_2_0 },
  { "2.1", V4L2_MPEG_VIDEO_H264_LEVEL_2_1 },
  { "2.2", V4L2_MPEG_VIDEO_H264_LEVEL_2_2 },
  { "3",   V4L2_MPEG_VIDEO_H264_LEVEL_3_0 },
  { "3.1", V4L2_MPEG_VIDEO_H264_LEVEL_3_1 },
  { "3.2", V4L2_MPEG_VIDEO_H264_LEVEL_3_2 },
  { "4",   V4L2_MPEG_VIDEO_H264_LEVEL_4_0 },
  { "4.1", V4L2_MPEG_VIDEO_H264_LEVEL_4_1 },
  { "4.2", V4L2_MPEG_VIDEO_H264_LEVEL_4_2 },
  { "5",   V4L2_MPEG_VIDEO_H264_LEVEL_5_0 },
  { "5.1", V4L2_MPEG_VIDEO_H264_LEVEL_5_1 },
};

class PicamOption {
public:
    PicamOption();
    ~PicamOption();
    void print_usage();
    int parse(int argc, char **argv);

    // Directory to put recorded MPEG-TS files
    char *rec_dir = (char *)"rec";
    char *rec_tmp_dir = (char *)"rec/tmp";
    char *rec_archive_dir = (char *)"rec/archive";

    // If true, query camera capabilities and exit
    int query_and_exit = 0;

    // If this value is 1, audio capturing is disabled.
    int disable_audio_capturing = 0;

    int log_level = LOG_LEVEL_INFO;
    int sensor_mode = -1;
    int video_width = 1280;
    int video_height = 720;
    float video_fps = 30.0f;
    float min_fps = -1.0f;
    float max_fps = -1.0f;
    int fr_q16;
    int video_pts_step = 0;
    int is_video_pts_step_specified = 0;
    int video_gop_size = 0;
    int is_video_gop_size_specified = 0;
    int video_rotation = 0;
    int video_hflip = 0;
    int video_vflip = 0;
    long video_bitrate = 2000 * 1000; // 2 Mbps
    char video_avc_profile[21] = "constrained_baseline";
    char video_avc_level[4] = "3.1";
    int video_qp_min = -1;
    int video_qp_max = -1;
    int video_qp_initial = -1;
    int video_slice_dquant = -1;
    char alsa_dev[256] = "hw:0,0";
    int is_audio_preview_enabled = 0;
    char audio_preview_dev[256] = "plughw:0,0";
    long audio_bitrate = 40000; // 40 Kbps
    int is_audio_channels_specified = 0;
    int audio_channels = 1;
    int audio_preview_channels;
    int audio_sample_rate = 48000;
    int audio_period_size;
    int is_hlsout_enabled = 0;
    char hls_output_dir[256] = "/run/shm/video";
    int hls_keyframes_per_segment = 1;
    int hls_number_of_segments = 3;
    int is_rtspout_enabled = 0;
    char rtsp_video_control_path[256] = "/tmp/node_rtsp_rtmp_videoControl";
    char rtsp_audio_control_path[256] = "/tmp/node_rtsp_rtmp_audioControl";
    char rtsp_video_data_path[256] = "/tmp/node_rtsp_rtmp_videoData";
    char rtsp_audio_data_path[256] = "/tmp/node_rtsp_rtmp_audioData";
    int is_tcpout_enabled = 0;
    char tcp_output_dest[256];
    int is_auto_exposure_enabled = 0;
    int is_vfr_enabled = 0;
    float auto_exposure_threshold = 5.0f;

    float roi_left = 0.0f;
    float roi_top = 0.0f;
    float roi_width = 1.0f;
    float roi_height = 1.0f;

    char white_balance[13] = "auto";
    char exposure_control[14] = "auto";

    // Red gain used when AWB is off
    float awb_red_gain = 0.0f;

    // Blue gain used when AWB is off
    float awb_blue_gain = 0.0f;

    char exposure_metering[8] = "average";

    int manual_exposure_compensation = 0; // EV compensation
    float exposure_compensation;
    int manual_exposure_aperture = 0; // f-number
    float exposure_aperture;
    int manual_exposure_shutter_speed = 0; // in microseconds
    unsigned int exposure_shutter_speed;
    int manual_exposure_sensitivity = 0; // ISO
    unsigned int exposure_sensitivity;

    char state_dir[256] = "state";
    char hooks_dir[256] = "hooks";
    float audio_volume_multiply = 1.0f;
    int is_hls_encryption_enabled = 0;
    char hls_encryption_key_uri[256] = "stream.key";
    uint8_t hls_encryption_key[16] = {
        0x75, 0xb0, 0xa8, 0x1d, 0xe1, 0x74, 0x87, 0xc8,
        0x8a, 0x47, 0x50, 0x7a, 0x7e, 0x1f, 0xdf, 0x73,
    };
    uint8_t hls_encryption_iv[16] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    int is_preview_enabled = 0;
    int is_previewrect_enabled = 0;
    int preview_x;
    int preview_y;
    int preview_width;
    int preview_height;
    int preview_opacity = 255;
    uint32_t blank_background_color;
    int record_buffer_keyframes = 5;

    int is_timestamp_enabled = 0;
    char timestamp_format[128] = "%a %b %d %l:%M:%S %p";
    LAYOUT_ALIGN timestamp_layout = (LAYOUT_ALIGN) (LAYOUT_ALIGN_BOTTOM | LAYOUT_ALIGN_RIGHT);
    int timestamp_horizontal_margin = 10;
    int timestamp_vertical_margin = 10;
    int timestamp_pos_x;
    int timestamp_pos_y;
    int is_timestamp_abs_pos_enabled = 0;
    TEXT_ALIGN timestamp_text_align = TEXT_ALIGN_LEFT;
    char timestamp_font_name[128] = "FreeMono:style=Bold";
    char timestamp_font_file[1024];
    int timestamp_font_face_index = 0;
    float timestamp_font_points = 14.0f;
    int timestamp_font_dpi = 96;
    int timestamp_color = 0xffffff;
    int timestamp_stroke_color = 0x000000;
    float timestamp_stroke_width = 1.3f;
    int timestamp_letter_spacing = 0;

private:
    void calculate();
};