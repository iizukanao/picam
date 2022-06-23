/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 *
 * libcamera_vid.cpp - libcamera video record app.
 */

#include <chrono>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <libcamera/transform.h>
#include <libcamera/control_ids.h>

// #include "core/libcamera_encoder.hpp"
#include "core/frame_info.hpp"
#include "output/output.hpp"
#include "timestamp/timestamp.h"
#include "subtitle/subtitle.h"
#include "text/text.h"
#include "log/log.h"
#include "audio/audio.hpp"
#include "httplivestreaming/httplivestreaming.h"
#include "muxer/muxer.hpp"
#include "libstate/state.h"
#include "libhook/hook.h"
#include "preview/preview.hpp"
#include "core/options.hpp"
#include "rtsp/rtsp.h"
#include "picam.hpp"

// How much PTS difference between audio and video is
// considered to be too large
#define PTS_DIFF_TOO_LARGE 45000  // 90000 == 1 second

using namespace std::placeholders;

// Some keypress/signal handling.

#define DEBUG_SKIP_VIDEO 0

// static int signal_received;
static std::thread audioThread;
static float video_fps = 40.0f; // TODO: Make this an option
// static int64_t video_timestamp_origin_us = -1;

// hooks
static pthread_t hooks_thread;

// NAL unit type 9
static const uint8_t access_unit_delimiter[] = {
	0x00, 0x00, 0x00, 0x01, 0x09, 0xf0,
};
static const int access_unit_delimiter_length = 6;

static void check_camera_stack()
{
	int fd = open("/dev/video0", O_RDWR, 0);
	if (fd < 0)
		return;

	v4l2_capability caps;
	int ret = ioctl(fd, VIDIOC_QUERYCAP, &caps);
	close(fd);

	if (ret < 0 || strcmp((char *)caps.driver, "bm2835 mmal"))
		return;

	fprintf(stderr, "ERROR: the system appears to be configured for the legacy camera stack\n");
	exit(-1);
}

Picam::Picam() {
	check_camera_stack();

	// if (!options_)
	// 	options_ = std::make_unique<Options>();
}

Picam::~Picam() {
	log_debug("Closing Libcamera application (preview frames displayed %d, dropped %d\n",
	preview_frames_displayed_, preview_frames_dropped_);
	StopCamera();
	Teardown();
	CloseCamera();
}

void Picam::stopAudioThread() {
	printf("stopAudioThread begin\n");
	if (audioThread.joinable()) {
		std::cout << "joinable" << std::endl;
		audio->stop();
		audioThread.join();
		std::cout << "joined" << std::endl;
	}
	delete audio;
	printf("stopAudioThread end\n");
}

void Picam::stopRecThread() {
	printf("stopRecThread begin\n");
	this->muxer->prepareForDestroy();
	this->muxer->waitForExit();
	printf("stopRecThread end\n");
}

void Picam::stopAllThreads() {
	this->stopAudioThread();
	this->stopRecThread();
}

// static void default_signal_handler(int signal_number)
// {
// 	signal_received = signal_number;
// 	std::cout << "Received signal " << signal_number << std::endl;
// 	Picam *picam;
// 	picam->getInstance().stopAllThreads();
// 	std::cout << "end of signal handler" << std::endl;
// }

#if !DEBUG_SKIP_VIDEO
static int get_key_or_signal(PicamOption const *options, pollfd p[1])
{
	int key = 0;
	// if (options->keypress)
	// {
	// 	poll(p, 1, 0);
	// 	if (p[0].revents & POLLIN)
	// 	{
	// 		char *user_string = nullptr;
	// 		size_t len;
	// 		[[maybe_unused]] size_t r = getline(&user_string, &len, stdin);
	// 		key = user_string[0];
	// 	}
	// }
	// if (options->signal)
	// {
	// 	if (signal_received == SIGUSR1)
	// 		key = '\n';
	// 	else if (signal_received == SIGUSR2)
	// 		key = 'x';
	// 	signal_received = 0;
	// }
	return key;
}
#endif

static int get_colourspace_flags(std::string const &codec)
{
	if (codec == "mjpeg" || codec == "yuv420")
		return Picam::FLAG_VIDEO_JPEG_COLOURSPACE;
	else
		return Picam::FLAG_VIDEO_NONE;
}


#if !DEBUG_SKIP_VIDEO
void Picam::modifyBuffer(CompletedRequestPtr &completed_request)
{
	libcamera::Stream *stream = this->VideoStream();
	StreamInfo info = this->GetStreamInfo(stream);
	libcamera::FrameBuffer *buffer = completed_request->buffers[stream];
	libcamera::Span span = this->Mmap(buffer)[0];
	void *mem = span.data();
	if (!buffer || !mem)
		throw std::runtime_error("no buffer to encode");
	// int64_t timestamp_ns = completed_request->metadata.contains(libcamera::controls::SensorTimestamp)
	// 						? completed_request->metadata.get(libcamera::controls::SensorTimestamp)
	// 						: buffer->metadata().timestamp;
	const uint32_t FOURCC_YU12 = 0x32315559; // YU12
	if (info.pixel_format.fourcc() == FOURCC_YU12) {

		// subtitle test
		// static bool isTextInited = false;
		// if (!isTextInited) {
		// 	isTextInited = true;
		// 	float font_points = 28.0f;
		//     int font_dpi = 96;
		//     int color = 0xffffff;
		// 	int stroke_color = 0x000000;
		// 	float stroke_width = 1.0f;
		// 	int in_preview = 1;
		// 	int in_video = 1;
		// 	int letter_spacing = 0;
		// 	float line_height_multiply = 1.0f;
		// 	float tab_scale = 1.0f;
		// 	LAYOUT_ALIGN layout_align = (LAYOUT_ALIGN) (LAYOUT_ALIGN_BOTTOM | LAYOUT_ALIGN_CENTER);
		//     TEXT_ALIGN text_align = TEXT_ALIGN_CENTER;
		// 	int horizontal_margin = 0;
		// 	int vertical_margin = 35;
		//     float duration = 2.0f;
		// 	const char *replaced_text = "Hello C+_+!";
		// 	int text_len = strlen(replaced_text);
	  //       subtitle_init_with_font_name(NULL, font_points, font_dpi);
		// 	subtitle_set_color(color);
		// 	subtitle_set_stroke_color(stroke_color);
		// 	subtitle_set_stroke_width(stroke_width);
		// 	subtitle_set_visibility(in_preview, in_video);
		// 	subtitle_set_letter_spacing(letter_spacing);
		// 	subtitle_set_line_height_multiply(line_height_multiply);
		// 	subtitle_set_tab_scale(tab_scale);
		// 	subtitle_set_layout(layout_align,
		// 		horizontal_margin, vertical_margin);
		// 	subtitle_set_align(text_align);

		// 	// show subtitle for 7 seconds
		// 	subtitle_show(replaced_text, text_len, duration);
		// }

		// 640x480 -> max 100 fps
		// 1920x1080 -> max 40 fps
		// 1280x720 -> max 47.5 fps
		timestamp_update();
		subtitle_update();
		text_draw_all((uint8_t *)mem, info.width, info.height, 1); // is_video = 1
		// std::cout << "is_text_changed: " << is_text_changed << std::endl;
	}
}
#endif

// The main event loop for the application.

void audioLoop(Audio *audio)
{
	audio->loop();
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

// Set red and blue gains used when AWB is off
static int camera_set_custom_awb_gains() {
	// TODO: Implement this using libcamera

//   OMX_CONFIG_CUSTOMAWBGAINSTYPE custom_awb_gains;
//   OMX_ERRORTYPE error;

// // NOTE: OMX_IndexConfigCameraSettings is read-only

//   memset(&custom_awb_gains, 0, sizeof(OMX_CONFIG_CUSTOMAWBGAINSTYPE));
//   custom_awb_gains.nSize = sizeof(OMX_CONFIG_CUSTOMAWBGAINSTYPE);
//   custom_awb_gains.nVersion.nVersion = OMX_VERSION;
//   custom_awb_gains.xGainR = round(awb_red_gain * 65536); // Q16
//   custom_awb_gains.xGainB = round(awb_blue_gain * 65536); // Q16

//   error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
//       OMX_IndexConfigCustomAwbGains, &custom_awb_gains);
//   if (error != OMX_ErrorNone) {
//     log_fatal("error: failed to set camera custom awb gains: 0x%x\n", error);
//     return -1;
//   }

  return 0;
}

[[maybe_unused]] static int camera_set_exposure_value() {
	// TODO: Implement this using libcamera

  // OMX_CONFIG_EXPOSUREVALUETYPE exposure_value;
  // OMX_ERRORTYPE error;
  // int i;

  // memset(&exposure_value, 0, sizeof(OMX_CONFIG_EXPOSUREVALUETYPE));
  // exposure_value.nSize = sizeof(OMX_CONFIG_EXPOSUREVALUETYPE);
  // exposure_value.nVersion.nVersion = OMX_VERSION;
  // exposure_value.nPortIndex = OMX_ALL;

  // error = OMX_GetParameter(ILC_GET_HANDLE(camera_component),
  //     OMX_IndexConfigCommonExposureValue, &exposure_value);
  // if (error != OMX_ErrorNone) {
  //   log_fatal("error: failed to get camera exposure value: 0x%x\n", error);
  //   exit(EXIT_FAILURE);
  // }

  // OMX_METERINGTYPE metering = OMX_EVModeMax;
  // for (i = 0; i < sizeof(exposure_metering_options) / sizeof(exposure_metering_option); i++) {
  //   if (strcmp(exposure_metering_options[i].name, exposure_metering) == 0) {
  //     metering = exposure_metering_options[i].metering;
  //     break;
  //   }
  // }
  // if (metering == OMX_EVModeMax) {
  //   log_error("error: invalid exposure metering value: %s\n", exposure_metering);
  //   return -1;
  // }
  // // default: OMX_MeteringModeAverage
  // exposure_value.eMetering = metering;

  // if (manual_exposure_compensation) {
  //   // OMX_S32 Q16; default: 0
  //   exposure_value.xEVCompensation = round(exposure_compensation * 65536 / 6.0f);
  // }

  // if (manual_exposure_aperture) {
  //   // Apparently this has no practical effect

  //   // OMX_U32 Q16; default: 0
  //   exposure_value.nApertureFNumber = round(exposure_aperture * 65536);
  //   // default: OMX_FALSE
  //   exposure_value.bAutoAperture = OMX_FALSE;
  // }

  // if (manual_exposure_shutter_speed) {
  //   // OMX_U32; default: 0
  //   exposure_value.nShutterSpeedMsec = exposure_shutter_speed;
  //   // default: OMX_TRUE
  //   exposure_value.bAutoShutterSpeed = OMX_FALSE;
  // }

  // if (manual_exposure_sensitivity) {
  //   // OMX_U32; default: 0
  //   exposure_value.nSensitivity = exposure_sensitivity;
  //   // default: OMX_TRUE
  //   exposure_value.bAutoSensitivity = OMX_FALSE;
  // }

  // log_debug("setting exposure:\n");
  // log_debug("  eMetering: %d\n", exposure_value.eMetering);
  // log_debug("  xEVCompensation: %d\n", exposure_value.xEVCompensation);
  // log_debug("  nApertureFNumber: %u\n", exposure_value.nApertureFNumber);
  // log_debug("  bAutoAperture: %u\n", exposure_value.bAutoAperture);
  // log_debug("  nShutterSpeedMsec: %u\n", exposure_value.nShutterSpeedMsec);
  // log_debug("  bAutoShutterSpeed: %u\n", exposure_value.bAutoShutterSpeed);
  // log_debug("  nSensitivity: %u\n", exposure_value.nSensitivity);
  // log_debug("  bAutoSensitivity: %u\n", exposure_value.bAutoSensitivity);

  // error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
  //     OMX_IndexConfigCommonExposureValue, &exposure_value);
  // if (error != OMX_ErrorNone) {
  //   log_fatal("error: failed to set camera exposure value: 0x%x\n", error);
  //   return -1;
  // }

  return 0;
}

static int camera_set_white_balance(char *wb) {
	// TODO: Implement this using libcamera

  // OMX_CONFIG_WHITEBALCONTROLTYPE whitebal;
  // OMX_ERRORTYPE error;
  // int i;

  // memset(&whitebal, 0, sizeof(OMX_CONFIG_WHITEBALCONTROLTYPE));
  // whitebal.nSize = sizeof(OMX_CONFIG_WHITEBALCONTROLTYPE);
  // whitebal.nVersion.nVersion = OMX_VERSION;
  // whitebal.nPortIndex = OMX_ALL;

  // OMX_WHITEBALCONTROLTYPE control = OMX_WhiteBalControlMax;
  // for (i = 0; i < sizeof(white_balance_options) / sizeof(white_balance_option); i++) {
  //   if (strcmp(white_balance_options[i].name, wb) == 0) {
  //     control = white_balance_options[i].control;
  //     break;
  //   }
  // }
  // if (control == OMX_WhiteBalControlMax) {
  //   log_error("error: invalid white balance value: %s\n", wb);
  //   return -1;
  // }
  // whitebal.eWhiteBalControl = control;

  // error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
  //     OMX_IndexConfigCommonWhiteBalance, &whitebal);
  // if (error != OMX_ErrorNone) {
  //   log_fatal("error: failed to set camera white balance: 0x%x\n", error);
  //   return -1;
  // }

  return 0;
}

static int camera_set_exposure_control(char *ex) {
	// TODO: Implement this using libcamera

  // OMX_CONFIG_EXPOSURECONTROLTYPE exposure_control_type;
  // OMX_ERRORTYPE error;
  // int i;

  // memset(&exposure_control_type, 0, sizeof(OMX_CONFIG_EXPOSURECONTROLTYPE));
  // exposure_control_type.nSize = sizeof(OMX_CONFIG_EXPOSURECONTROLTYPE);
  // exposure_control_type.nVersion.nVersion = OMX_VERSION;
  // exposure_control_type.nPortIndex = OMX_ALL;

  // // Find out the value of eExposureControl
  // OMX_EXPOSURECONTROLTYPE control = OMX_ExposureControlMax;
  // for (i = 0; i < sizeof(exposure_control_options) / sizeof(exposure_control_option); i++) {
  //   if (strcmp(exposure_control_options[i].name, ex) == 0) {
  //     control = exposure_control_options[i].control;
  //     break;
  //   }
  // }
  // if (control == OMX_ExposureControlMax) {
  //   log_error("error: invalid exposure control value: %s\n", ex);
  //   return -1;
  // }
  // exposure_control_type.eExposureControl = control;

  // log_debug("exposure control: %s\n", ex);
  // error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
  //     OMX_IndexConfigCommonExposure, &exposure_control_type);
  // if (error != OMX_ErrorNone) {
  //   log_error("error: failed to set camera exposure control: 0x%x\n", error);
  //   return -1;
  // }

  // if (control == OMX_ExposureControlAuto) {
  //   current_exposure_mode = EXPOSURE_AUTO;
  // } else if (control == OMX_ExposureControlNight) {
  //   current_exposure_mode = EXPOSURE_NIGHT;
  // }

  return 0;
}

/* Set region of interest */
[[maybe_unused]] static int camera_set_input_crop(float left, float top, float width, float height) {
	// TODO: Implement this using libcamera

  // OMX_CONFIG_INPUTCROPTYPE input_crop_type;
  // OMX_ERRORTYPE error;

  // memset(&input_crop_type, 0, sizeof(OMX_CONFIG_INPUTCROPTYPE));
  // input_crop_type.nSize = sizeof(OMX_CONFIG_INPUTCROPTYPE);
  // input_crop_type.nVersion.nVersion = OMX_VERSION;
  // input_crop_type.nPortIndex = OMX_ALL;
  // input_crop_type.xLeft = round(left * 0x10000);
  // input_crop_type.xTop = round(top * 0x10000);
  // input_crop_type.xWidth = round(width * 0x10000);
  // input_crop_type.xHeight = round(height * 0x10000);

  // error = OMX_SetParameter(ILC_GET_HANDLE(camera_component),
  //     OMX_IndexConfigInputCropPercentages, &input_crop_type);
  // if (error != OMX_ErrorNone) {
  //   log_fatal("error: failed to set camera input crop type: 0x%x\n", error);
  //   log_fatal("hint: maybe --roi value is not acceptable to camera\n");
  //   return -1;
  // }

  return 0;
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

  buf = (char *)malloc(filesize + 1);
  if (buf == NULL) {
    log_error("read_file: error reading %s: failed to allocate memory (%d bytes)", filepath, filesize + 1);
    fclose(fp);
    return -1;
  }

  result = fread(buf, 1, filesize, fp);
  if (result != (size_t) filesize) {
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

// Check if hls_output_dir is accessible.
// Also create HLS output directory if it doesn't exist.
void Picam::ensure_hls_dir_exists() {
  struct stat st;
  int err;

  err = stat(this->option->hls_output_dir, &st);
  if (err == -1) {
    if (errno == ENOENT) {
      // create directory
      if (mkdir(this->option->hls_output_dir, 0755) == 0) { // success
        log_info("created HLS output directory: %s\n", this->option->hls_output_dir);
      } else { // error
        log_error("error creating hls_output_dir (%s): %s\n",
            this->option->hls_output_dir, strerror(errno));
        exit(EXIT_FAILURE);
      }
    } else {
      perror("stat hls_output_dir");
      exit(EXIT_FAILURE);
    }
  } else {
    if (!S_ISDIR(st.st_mode)) {
      log_error("error: hls_output_dir (%s) is not a directory\n",
          this->option->hls_output_dir);
      exit(EXIT_FAILURE);
    }
  }

  if (access(this->option->hls_output_dir, R_OK) != 0) {
    log_error("error: cannot access hls_output_dir (%s): %s\n",
        this->option->hls_output_dir, strerror(errno));
    exit(EXIT_FAILURE);
  }
}

// parse the contents of hooks/start_record
void Picam::parse_start_record_file(char *full_filename) {
  char buf[1024];

  this->rec_settings.recording_basename = (char *)""; // empties the basename used for this recording
  this->rec_settings.recording_dest_dir = (char *)""; // empties the directory the result file will be put in
  this->muxer->recording_look_back_keyframes = -1;

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
        if (value > this->muxer->record_buffer_keyframes) {
          log_error("error: per-recording recordbuf (%d) cannot be greater than "
              "global recordbuf (%d); using %d\n"
              "hint: try increasing global recordbuf with \"--recordbuf %d\" or "
              "\"echo %d > hooks/set_recordbuf\"\n",
              value, this->muxer->record_buffer_keyframes, this->muxer->record_buffer_keyframes,
              value, value);
          continue;
        }
        this->muxer->recording_look_back_keyframes = value;
        log_info("using recordbuf=%d for this recording\n", this->muxer->recording_look_back_keyframes);
      } else if (strncmp(buf, "dir", sep_p - buf) == 0) { // directory
        size_t len = strcspn(sep_p + 1, "\r\n");
        if (len > sizeof(this->rec_settings.recording_dest_dir) - 1) {
          len = sizeof(this->rec_settings.recording_dest_dir) - 1;
        }
        strncpy(this->rec_settings.recording_dest_dir, sep_p + 1, len);
        this->rec_settings.recording_dest_dir[len] = '\0';
        // Create the directory if it does not exist
        create_dir(this->rec_settings.recording_dest_dir);
      } else if (strncmp(buf, "filename", sep_p - buf) == 0) { // basename
        size_t len = strcspn(sep_p + 1, "\r\n");
        if (len > sizeof(this->rec_settings.recording_basename) - 1) {
          len = sizeof(this->rec_settings.recording_basename) - 1;
        }
        strncpy(this->rec_settings.recording_basename, sep_p + 1, len);
        this->rec_settings.recording_basename[len] = '\0';
      } else {
        log_error("failed to parse line in %s: %s\n",
            full_filename, buf);
      }
    }
    fclose(fp);
  }
}

void on_file_create(char *filename, char *content) {
	Picam *picam;
	picam->getInstance().handleHook(filename, content);
}

static void set_gop_size(int gop_size) {
	// TODO: Implement this using libcamera

  // OMX_VIDEO_CONFIG_AVCINTRAPERIOD avc_intra_period;
  // OMX_ERRORTYPE error;

  // memset(&avc_intra_period, 0, sizeof(OMX_VIDEO_CONFIG_AVCINTRAPERIOD));
  // avc_intra_period.nSize = sizeof(OMX_VIDEO_CONFIG_AVCINTRAPERIOD);
  // avc_intra_period.nVersion.nVersion = OMX_VERSION;
  // avc_intra_period.nPortIndex = VIDEO_ENCODE_OUTPUT_PORT;

  // // Distance between two IDR frames
  // avc_intra_period.nIDRPeriod = gop_size;

  // // It seems this value has no effect for the encoding.
  // avc_intra_period.nPFrames = gop_size;

  // error = OMX_SetParameter(ILC_GET_HANDLE(video_encode),
  //     OMX_IndexConfigVideoAVCIntraPeriod, &avc_intra_period);
  // if (error != OMX_ErrorNone) {
  //   log_fatal("error: failed to set video_encode %d AVC intra period: 0x%x\n", VIDEO_ENCODE_OUTPUT_PORT, error);
  //   exit(EXIT_FAILURE);
  // }
}

void Picam::handleHook(char *filename, char *content) {
	printf("handleHook: filename=%s content=%s\n", filename, content);
  if (strcmp(filename, "start_record") == 0) {
    char buf[269]; // sizeof(hooks_dir) + strlen("/start_record")

    // parse the contents of hooks/start_record
    snprintf(buf, sizeof(buf), "%s/%s", this->option->hooks_dir, filename);
    this->parse_start_record_file(buf);

    this->muxer->start_record(this->rec_settings);
  } else if (strcmp(filename, "stop_record") == 0) {
    this->muxer->stop_record();
  } else if (strcmp(filename, "mute") == 0) {
		this->audio->mute();
  } else if (strcmp(filename, "unmute") == 0) {
		this->audio->unmute();
  } else if (strcmp(filename, "wbred") == 0) {
    char buf[262]; // sizeof(hooks_dir) + strlen("/wbred")
    snprintf(buf, sizeof(buf), "%s/%s", this->option->hooks_dir, filename);
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
          this->option->awb_red_gain = value;
          if (camera_set_custom_awb_gains() == 0) {
            log_info("changed red gain to %.2f\n", this->option->awb_red_gain);
          } else {
            log_error("error: failed to set wbred\n");
          }
        }
        free(file_buf);
      }
    }
  } else if (strcmp(filename, "wbblue") == 0) {
    char buf[263]; // sizeof(hooks_dir) + strlen("/wbblue")
    snprintf(buf, sizeof(buf), "%s/%s", this->option->hooks_dir, filename);
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
          this->option->awb_blue_gain = value;
          if (camera_set_custom_awb_gains() == 0) {
            log_info("changed blue gain to %.2f\n", this->option->awb_blue_gain);
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
    unsigned int i;
    for (i = 0; i < sizeof(white_balance_options) / sizeof(white_balance_option); i++) {
      if (strcmp(white_balance_options[i].name, wb_mode) == 0) {
        strncpy(this->option->white_balance, wb_mode, sizeof(this->option->white_balance) - 1);
        this->option->white_balance[sizeof(this->option->white_balance) - 1] = '\0';
        matched = 1;
        break;
      }
    }
    if (matched) {
      if (camera_set_white_balance(this->option->white_balance) == 0) {
        log_info("changed the white balance to %s\n", this->option->white_balance);
      } else {
        log_error("error: failed to set the white balance to %s\n", this->option->white_balance);
      }
    } else {
      log_error("hook error: invalid white balance: %s\n", wb_mode);
      log_error("(valid values: ");
      unsigned int size = sizeof(white_balance_options) / sizeof(white_balance_option);
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
    unsigned int i;

    if (!this->option->is_vfr_enabled) {
      log_warn("warn: Use --vfr or --ex in order to ex_* hook to properly take effect\n");
    }

    for (i = 0; i < sizeof(exposure_control_options) / sizeof(exposure_control_option); i++) {
      if (strcmp(exposure_control_options[i].name, ex_mode) == 0) {
        strncpy(this->option->exposure_control, ex_mode, sizeof(this->option->exposure_control) - 1);
        this->option->exposure_control[sizeof(this->option->exposure_control) - 1] = '\0';
        matched = 1;
        break;
      }
    }
    if (matched) {
      if (camera_set_exposure_control(this->option->exposure_control) == 0) {
        log_info("changed the exposure control to %s\n", this->option->exposure_control);
      } else {
        log_error("error: failed to set the exposure control to %s\n", this->option->exposure_control);
      }
    } else {
      log_error("hook error: invalid exposure control: %s\n", ex_mode);
      log_error("(valid values: ");
      unsigned int size = sizeof(exposure_control_options) / sizeof(exposure_control_option);
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
    char buf[270]; // sizeof(hooks_dir) + strlen("/set_recordbuf")
    snprintf(buf, sizeof(buf), "%s/%s", this->option->hooks_dir, filename);
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
          if (this->muxer->set_record_buffer_keyframes(value) == 0) {
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
    int layout_align = LAYOUT_ALIGN_BOTTOM | LAYOUT_ALIGN_CENTER;
    int text_align = TEXT_ALIGN_CENTER;
    int horizontal_margin = 0;
    int vertical_margin = 35;
    int in_preview = 1;
    int in_video = 1;

    char filepath[265]; // sizeof(hooks_dir) + strlen("/subtitle")
    snprintf(filepath, sizeof(filepath), "%s/%s", this->option->hooks_dir, filename);
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
        unsigned int i;
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
          subtitle_set_layout((LAYOUT_ALIGN)layout_align,
              horizontal_margin, vertical_margin);
        }
        subtitle_set_align((TEXT_ALIGN)text_align);

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

// Return next video PTS for variable frame rate
int64_t Picam::get_next_video_pts_vfr() {
  video_frame_count++;

  if (time_for_last_pts == 0) {
    video_current_pts = audio_current_pts;
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
int64_t Picam::get_next_video_pts_cfr() {
  int64_t pts;
  video_frame_count++;

  if (video_current_pts == 0) {
	video_current_pts = audio_current_pts - this->option->video_pts_step;
	log_debug("initial video_current_pts set to %" PRId64 "\n", video_current_pts);
  }

  int pts_diff = audio_current_pts - video_current_pts - this->option->video_pts_step;
  int tolerance = (this->option->video_pts_step + audio->get_audio_pts_step_base()) * 2;
  if (pts_diff >= PTS_DIFF_TOO_LARGE) {
    // video PTS is too slow
    log_debug("vR%d", pts_diff);
    pts = audio_current_pts;
  } else if (pts_diff >= tolerance) {
    if (pts_mode != PTS_SPEED_UP) {
      // speed up video PTS
      speed_up_count++;
      pts_mode = PTS_SPEED_UP;
      log_debug("vSPEED_UP(video_pts_step=%d audio_pts_base=%d pts_diff=%d)", this->option->video_pts_step, audio->get_audio_pts_step_base(), pts_diff);
    }
    // Catch up with audio PTS if the delay is too large.
    pts = video_current_pts + this->option->video_pts_step + 150;
  } else if (pts_diff <= -tolerance) {
    if (pts_mode != PTS_SPEED_DOWN) {
      // speed down video PTS
      pts_mode = PTS_SPEED_DOWN;
      speed_down_count++;
      log_debug("vSPEED_DOWN(%d)", pts_diff);
    }
    pts = video_current_pts + this->option->video_pts_step - 150;
  } else {
    pts = video_current_pts + this->option->video_pts_step;
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
int64_t Picam::get_next_video_pts() {
  if (this->option->is_vfr_enabled) { // variable frame rate
    return get_next_video_pts_vfr();
  } else { // constant frame rate
    return get_next_video_pts_cfr();
  }
}

int64_t Picam::get_next_audio_pts() {
  int64_t pts;
  audio_frame_count++;

  // We use audio timing as the base clock,
  // so we do not modify PTS here.
  pts = audio_current_pts + audio->get_audio_pts_step_base();

  audio_current_pts = pts;

  return pts;
}

// // This function is called after the video encoder produces each frame
// static int video_encode_fill_buffer_done(OMX_BUFFERHEADERTYPE *out) {
//   int nal_unit_type;
//   uint8_t *buf;
//   int buf_len;
//   int is_endofnal = 1;
//   uint8_t *concat_buf = NULL;
//   struct timespec tsEnd, tsDiff;

//   // out->nTimeStamp is useless as the value is always zero

//   if (out == NULL) {
//     log_error("error: cannot get output buffer from video_encode\n");
//     return 0;
//   }
//   if (encbuf != NULL) {
//     // merge the previous buffer
//     concat_buf = realloc(encbuf, encbuf_size + out->nFilledLen);
//     if (concat_buf == NULL) {
//       log_fatal("error: cannot allocate memory for concat_buf (%d bytes)\n", encbuf_size + out->nFilledLen);
//       free(encbuf);
//       exit(EXIT_FAILURE);
//     }
//     memcpy(concat_buf + encbuf_size, out->pBuffer, out->nFilledLen);
//     buf = concat_buf;
//     buf_len = encbuf_size + out->nFilledLen;
//   } else {
//     buf = out->pBuffer;
//     buf_len = out->nFilledLen;
//   }
//   if (!(out->nFlags & OMX_BUFFERFLAG_ENDOFFRAME) &&
//       !(out->nFlags & OMX_BUFFERFLAG_CODECCONFIG)) {
//     // There is remaining buffer for the current frame
//     nal_unit_type = buf[4] & 0x1f;
//     encbuf_size = buf_len;
//     if (concat_buf != NULL) {
//       encbuf = concat_buf;
//       concat_buf = NULL;
//     } else {
//       encbuf = malloc(buf_len);
//       if (encbuf == NULL) {
//         log_fatal("error: cannot allocate memory for encbuf (%d bytes)\n", buf_len);
//         exit(EXIT_FAILURE);
//       }
//       memcpy(encbuf, buf, buf_len);
//     }
//     is_endofnal = 0;
//   } else {
//     encbuf = NULL;
//     encbuf_size = -1;

//     nal_unit_type = buf[4] & 0x1f;
//     if (nal_unit_type != 1 && nal_unit_type != 5) {
//       log_debug("[NAL%d]", nal_unit_type);
//     }
//     if (out->nFlags != 0x480 && out->nFlags != 0x490 &&
//         out->nFlags != 0x430 && out->nFlags != 0x410 &&
//         out->nFlags != 0x400 && out->nFlags != 0x510 &&
//         out->nFlags != 0x530) {
//       log_warn("\nnew flag (%d,nal=%d)\n", out->nFlags, nal_unit_type);
//     }
//     if (out->nFlags & OMX_BUFFERFLAG_DATACORRUPT) {
//       log_warn("\n=== OMX_BUFFERFLAG_DATACORRUPT ===\n");
//     }
//     if (out->nFlags & OMX_BUFFERFLAG_EXTRADATA) {
//       log_warn("\n=== OMX_BUFFERFLAG_EXTRADATA ===\n");
//     }
//     if (out->nFlags & OMX_BUFFERFLAG_FRAGMENTLIST) {
//       log_warn("\n=== OMX_BUFFERFLAG_FRAGMENTLIST ===\n");
//     }
//     if (out->nFlags & OMX_BUFFERFLAG_DISCONTINUITY) {
//       log_warn("\n=== OMX_BUFFERFLAG_DISCONTINUITY ===\n");
//     }
//     if (out->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
//       // Insert timing info to nal_unit_type 7
//       codec_configs[n_codec_configs] = malloc(buf_len);
//       if (codec_configs[n_codec_configs] == NULL) {
//         log_fatal("error: cannot allocate memory for codec config (%d bytes)\n", buf_len);
//         exit(EXIT_FAILURE);
//       }
//       codec_config_sizes[n_codec_configs] = buf_len;
//       memcpy(codec_configs[n_codec_configs], buf, buf_len);

//       codec_config_total_size += codec_config_sizes[n_codec_configs];
//       n_codec_configs++;

//       send_video_frame(buf, buf_len, 0);
//     } else { // video frame
//       frame_count++; // will be used for printing stats about FPS, etc.

//       if (out->nFlags & OMX_BUFFERFLAG_SYNCFRAME) { // keyframe
//         if (nal_unit_type != 5) {
//           log_debug("SYNCFRAME nal_unit_type=%d len=%d\n", nal_unit_type, buf_len);
//         }
//         int consume_time = 0;
//         if (nal_unit_type == 1 || nal_unit_type == 2 ||
//             nal_unit_type == 3 || nal_unit_type == 4 ||
//             nal_unit_type == 5) {
//           consume_time = 1;
//         } else {
//           log_debug("(nosl)");
//         }
// #if !(AUDIO_ONLY)
//         send_keyframe(buf, buf_len, consume_time);
// #endif

//         // calculate FPS and display it
//         if (tsBegin.tv_sec != 0 && tsBegin.tv_nsec != 0) {
//           keyframes_count++;
//           clock_gettime(CLOCK_MONOTONIC, &tsEnd);
//           timespec_subtract(&tsDiff, &tsEnd, &tsBegin);
//           unsigned long long wait_nsec = tsDiff.tv_sec * INT64_C(1000000000) + tsDiff.tv_nsec;
//           float divisor = (float)wait_nsec / (float)frame_count / 1000000000;
//           float fps;
//           if (divisor == 0.0f) { // This won't cause SIGFPE because of float, but just to be safe.
//             fps = 99999.0f;
//           } else {
//             fps = 1.0f / divisor;
//           }
//           log_debug(" %5.2f fps k=%d", fps, keyframes_count);
//           if (log_get_level() <= LOG_LEVEL_DEBUG) {
//             print_audio_timing();
//           }
//           current_audio_frames = 0;
//           frame_count = 0;

//           if (is_auto_exposure_enabled) {
//             auto_select_exposure(video_width, video_height, last_video_buffer, fps);
//           }

//           log_debug("\n");
//         }
//         clock_gettime(CLOCK_MONOTONIC, &tsBegin);
//       } else { // inter frame
//         if (nal_unit_type != 9) { // Exclude nal_unit_type 9 (Access unit delimiter)
//           int consume_time = 0;
//           if (nal_unit_type == 1 || nal_unit_type == 2 ||
//               nal_unit_type == 3 || nal_unit_type == 4 ||
//               nal_unit_type == 5) {
//             consume_time = 1;
//           } else {
//             log_debug("(nosl)");
//           }
// #if !(AUDIO_ONLY)
//           send_pframe(buf, buf_len, consume_time);
// #endif
//         }
//       }
//     }
//   }
//   if (concat_buf != NULL) {
//     free(concat_buf);
//   }

//   return is_endofnal;
// }

void Picam::print_audio_timing() {
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

/* Return 1 if the difference is negative, otherwise 0.  */
static int timespec_subtract(struct timespec *result, struct timespec *t2, struct timespec *t1) {
  long long diff = (t2->tv_nsec + INT64_C(1000000000) * t2->tv_sec) - (t1->tv_nsec + INT64_C(1000000000) * t1->tv_sec);
  result->tv_sec = diff / 1000000000;
  result->tv_nsec = diff % 1000000000;

  return (diff<0);
}

void Picam::check_video_and_audio_started() {
	if (this->is_audio_started && this->is_video_started) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		this->video_start_time = this->audio_start_time = ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
		if (this->option->is_rtspout_enabled) {
			send_video_start_time();
			send_audio_start_time(this->audio_start_time);
		}
		log_info("capturing started\n");
	}
}

// Called when encoded (H.264) video buffer is ready
void Picam::videoEncodeDoneCallback(void *mem, size_t size, int64_t timestamp_us, bool keyframe)
{
	uint8_t *bytes = (uint8_t *)mem;
	// The structure of bytes is 00 00 00 01 <..NAL unit..>:
	//   zero_byte <00> -> always present in Raspberry Pi's H.264 encoder
	//   start_code_prefix_one_3bytes <00 00 01>
	//   nal_unit <..remaining bytes..>
	// The first byte of nal_unit consists of:
	//   forbidden_zero_bit (1 bit)
	//   nal_ref_idc (2 bits)
	//   nal_unit_type (5 bits)
	uint8_t *complete_mem = bytes;
	size_t complete_mem_size = size;
	uint8_t nalUnitType = bytes[4] & 0b11111; // Lower 5 bits of the 5th byte
	if (nalUnitType == 7) {
		// The structure of the very first frame always comes like this:
		//   00 00 00 01 27 <..SPS payload..> 00 00 00 01 28 <..PPS payload..> 00 00 00 01 25 <..I-frame payload..>
		// So, instead of finding each start code prefix (00 00 01) and NAL unit type,
		// we take a shortcut and just soak up until just before 00 00 00 01 25 and copy it into this->sps_pps
		uint8_t const start_code_keyframe[] = { 0x00, 0x00, 0x00, 0x01, 0x25 };
		uint8_t *keyframe_start = (uint8_t *)memmem(bytes + 5, size - 5, start_code_keyframe, sizeof(start_code_keyframe));
		if (keyframe_start == NULL) {
			log_error("SPS/PPS was not found in the encoded frame\n");
		} else {
			this->sps_pps_size = keyframe_start - bytes;
			printf("set_sps_pps: size=%u\n", this->sps_pps_size);
			if (this->sps_pps != NULL) {
				free(this->sps_pps);
			}
			this->sps_pps = (uint8_t *)malloc(this->sps_pps_size);
			if (this->sps_pps == NULL) {
				log_fatal("malloc for sps_pps failed\n");
				exit(EXIT_FAILURE);
			}
			memcpy(this->sps_pps, bytes, this->sps_pps_size);
			if (this->option->is_rtspout_enabled) {
				rtsp_send_video_frame(this->sps_pps, this->sps_pps_size, 0);
			}
		}
	} else if (nalUnitType == 5) {
		// Append an Access Unit Delimiter, SPS, and PPS in front of this NAL unit
		complete_mem_size = access_unit_delimiter_length + this->sps_pps_size + size;
		complete_mem = (uint8_t *)malloc(complete_mem_size);
		if (complete_mem == NULL) {
			log_fatal("malloc for complete_mem failed\n");
			exit(EXIT_FAILURE);
		}
		memcpy(complete_mem, access_unit_delimiter, access_unit_delimiter_length);
		memcpy(complete_mem + access_unit_delimiter_length, this->sps_pps, this->sps_pps_size);
		memcpy(complete_mem + access_unit_delimiter_length + this->sps_pps_size, bytes, size);
	}

	// timestamp_us is not correct

	// printf("outputReady size=%d timestamp=%lld keyframe=%d\n", size, timestamp_us, keyframe);
	// if (video_timestamp_origin_us == -1) {
	// 	video_timestamp_origin_us = timestamp_us;
	// 	printf("xxx set video_timestamp_origin_us=%lld\n", video_timestamp_origin_us);

	// 	// Without the casting (int64_t), we will going to get the wrong result
	// 	video_timestamp_origin_us -= (int64_t)std::round(1000000 / video_fps);

	// 	printf("1000000 / video_fps = %f (%lld)\n", 1000000 / video_fps, (int64_t)(1000000 / video_fps));
	// 	printf("xxx adjusted video_timestamp_origin_us=%lld\n", video_timestamp_origin_us);
	// }
	int flags = 0;
	if (keyframe) {
		flags |= AV_PKT_FLAG_KEY;
	}
	if (!this->is_video_started) {
		this->is_video_started = true;
		this->check_video_and_audio_started();
	}
	// muxer->add_encoded_packet(std::round((timestamp_us - video_timestamp_origin_us) / (1000000 / 90000)), (uint8_t *)mem, size, hls->format_ctx->streams[0]->index, flags);
	int64_t pts = this->get_next_video_pts();

#if ENABLE_AUTO_GOP_SIZE_CONTROL_FOR_VFR
  if (this->option->is_vfr_enabled) {
		printf("controlling vfr\n");
    int64_t pts_between_keyframes = pts - last_keyframe_pts;
    if (pts_between_keyframes < 80000) { // < .89 seconds
      // Frame rate is running faster than we thought
      int ideal_video_gop_size = (frames_since_last_keyframe + 1)
        * 90000.0f / pts_between_keyframes;
      if (ideal_video_gop_size > this->option->video_gop_size) {
        this->option->video_gop_size = ideal_video_gop_size;
        log_debug("increase gop_size to %d ", ideal_video_gop_size);
        set_gop_size(this->option->video_gop_size);
      }
    }
    last_keyframe_pts = pts;
    frames_since_last_keyframe = 0;
  }
#endif

	this->last_pts = pts;
	this->muxer->add_encoded_packet(pts, complete_mem, complete_mem_size, hls->format_ctx->streams[0]->index, flags);

	this->frame_count_since_keyframe++;

	log_debug(".");
	if (keyframe) {
		this->muxer->mark_keyframe_packet();
		// calculate FPS and display it
		if (tsBegin.tv_sec != 0 && tsBegin.tv_nsec != 0) {
			struct timespec tsEnd, tsDiff;
			keyframes_count++;
			clock_gettime(CLOCK_MONOTONIC, &tsEnd);
			timespec_subtract(&tsDiff, &tsEnd, &tsBegin);
			uint64_t wait_nsec = tsDiff.tv_sec * INT64_C(1000000000) + tsDiff.tv_nsec;
			float divisor = (float)wait_nsec / (float)frame_count_since_keyframe / 1000000000;
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
			frame_count_since_keyframe = 0;

			// if (is_auto_exposure_enabled) {
			// 	auto_select_exposure(video_width, video_height, last_video_buffer, fps);
			// }

			log_debug("\n");
		}
		clock_gettime(CLOCK_MONOTONIC, &tsBegin);
	}

	if (complete_mem != bytes) {
		printf("freeing complete_mem\n");
		free(complete_mem);
	}

	// // std::cout << "### OutputReady" << std::endl;
	// // When output is enabled, we may have to wait for the next keyframe.
	// uint32_t flags = keyframe ? FLAG_KEYFRAME : FLAG_NONE;
	// if (!enable_)
	// 	state_ = DISABLED;
	// else if (state_ == DISABLED)
	// 	state_ = WAITING_KEYFRAME;
	// if (state_ == WAITING_KEYFRAME && keyframe)
	// 	state_ = RUNNING, flags |= FLAG_RESTART;
	// if (state_ != RUNNING)
	// 	return;

	// // Frig the timestamps to be continuous after a pause.
	// if (flags & FLAG_RESTART)
	// 	time_offset_ = timestamp_us - last_timestamp_;
	// last_timestamp_ = timestamp_us - time_offset_;

	// outputBuffer(mem, size, last_timestamp_, flags);

	// // Save timestamps to a file, if that was requested.
	// if (fp_timestamps_)
	// 	fprintf(fp_timestamps_, "%" PRId64 ".%03" PRId64 "\n", last_timestamp_ / 1000, last_timestamp_ % 1000);
}

// Based on libcamera_vid.cpp
void Picam::event_loop()
{
	// VideoOptions const *options = this->GetOptions();
	// std::unique_ptr<Output> output = std::unique_ptr<Output>(Output::Create(options));
	// this->SetEncodeOutputReadyCallback(std::bind(&Output::OutputReady, output.get(), _1, _2, _3, _4));

	// std::cout << "### SetEncodeOutputReadyCallback ###" << std::endl;
	this->SetEncodeOutputReadyCallback(std::bind(&Picam::videoEncodeDoneCallback, this, _1, _2, _3, _4));

	int hls_number_of_segments = 3;
	MpegTSCodecSettings codec_settings;
	unsigned int audio_sample_rate = 48000;
	long audio_bitrate = 40000;
	int audio_channels = 1;
	codec_settings.audio_sample_rate = audio_sample_rate;
	codec_settings.audio_bit_rate = audio_bitrate;
	codec_settings.audio_channels = audio_channels;
	codec_settings.audio_profile = FF_PROFILE_AAC_LOW;

#if AUDIO_ONLY
  hls = hls_create_audio_only(hls_number_of_segments, &codec_settings); // 2 == num_recent_files
#else
	printf("hls_create\n");
	hls = hls_create(hls_number_of_segments, &codec_settings); // 2 == num_recent_files
#endif

	std::cout << "### OpenCamera" << std::endl;
	this->OpenCamera();
	std::cout << "### ConfigureVideo" << std::endl;
  const char *codec = "h264";
	this->ConfigureVideo(get_colourspace_flags(codec));
	std::cout << "### StartEncoder" << std::endl;
	this->StartEncoder();
	std::cout << "### StartCamera" << std::endl;
	this->StartCamera();
#if !DEBUG_SKIP_VIDEO
	auto start_time = std::chrono::high_resolution_clock::now();
#endif
	
	// TODO: Configure microphone
	// TODO: Set up MPEG-TS (Configure) libavformat and libavcodec)
	// TODO: Create a thread for audio capture and encoding

	state_default_dir("state");

	audio = new Audio(this->option);
	// std::unique_ptr<Audio> audio(new Audio(options));
	// audio->setup() をスレッド内でやる必要がある?
	audio->setup(hls);

	this->muxer = new Muxer(this->option);
	this->muxer->setup(&codec_settings);
	audio->set_encode_callback([=](int64_t pts, uint8_t *data, int size, int stream_index, int flags) -> void {
		if (!this->is_audio_started) {
			this->is_audio_started = true;
			this->check_video_and_audio_started();
		}
		int64_t audio_pts = this->get_next_audio_pts();
		this->muxer->add_encoded_packet(audio_pts, data, size, stream_index, flags);
	});

	int fr_q16 = video_fps * 65536;
	int video_pts_step_default = 0;
	int video_pts_step = video_pts_step_default; // Make this an option
	if (video_pts_step == video_pts_step_default) {
		video_pts_step = round(90000 / video_fps);

		// It appears that the minimum fps is 1.31
		if (video_pts_step > 68480) {
		video_pts_step = 68480;
		}
	}
	int video_gop_size_default = 0;
	int video_gop_size = video_gop_size_default; // TODO: Make this an option
	if (video_gop_size == video_gop_size_default) {
		video_gop_size = ceil(video_fps);
	}

	mpegts_set_config(option->video_bitrate, option->video_width, option->video_height);
	float audio_volume_multiply = option->audio_volume_multiply;
	int audio_min_value = (int) (-32768 / audio_volume_multiply);
	int audio_max_value = (int) (32767 / audio_volume_multiply);
	printf("fr_q16=%d video_bitrate=%ld audio_min_value=%d audio_max_value=%d\n",
		fr_q16, option->video_bitrate, audio_min_value, audio_max_value);

	this->muxer->prepare_encoded_packets(video_fps, audio->get_fps());

	// Monitoring for keypresses and signals.
	// signal(SIGUSR1, default_signal_handler);
	// signal(SIGUSR2, default_signal_handler);

#if !DEBUG_SKIP_VIDEO
	pollfd p[1] = { { STDIN_FILENO, POLLIN, 0 } };
#endif
	// uint64_t lastTimestamp = 0;

	float timestamp_font_points = 14.0f;
	int timestamp_font_dpi = 96;
	const char *timestamp_format = "%a %b %d %l:%M:%S %p";
	const LAYOUT_ALIGN timestamp_layout = (LAYOUT_ALIGN) (LAYOUT_ALIGN_BOTTOM | LAYOUT_ALIGN_RIGHT);
	int timestamp_horizontal_margin = 10;
	int timestamp_vertical_margin = 10;
	const TEXT_ALIGN timestamp_text_align = TEXT_ALIGN_LEFT;
	const int timestamp_color = 0xffffff;
	const int timestamp_stroke_color = 0x000000;
	const float timestamp_stroke_width = 1.3f;
	const int timestamp_letter_spacing = 0;
	// std::cout << "width=" << options->width << " height=" << options->height << std::endl;
	int video_width_32 = (option->video_width+31)&~31;
	int video_height_16 = (option->video_height+15)&~15;

	// std::cout << "width_32=" << video_width_32 << " height_16=" << video_height_16 << std::endl;
	timestamp_init_with_font_name(NULL,
		timestamp_font_points, timestamp_font_dpi);
	timestamp_set_format(timestamp_format);
	timestamp_set_layout(timestamp_layout,
		timestamp_horizontal_margin, timestamp_vertical_margin);
	timestamp_set_align(timestamp_text_align);
	timestamp_set_color(timestamp_color);
	timestamp_set_stroke_color(timestamp_stroke_color);
	timestamp_set_stroke_width(timestamp_stroke_width);
	timestamp_set_letter_spacing(timestamp_letter_spacing);
	timestamp_fix_position(video_width_32, video_height_16);

	audioThread = std::thread(audioLoop, audio);

	// muxer->start_record();

#if !DEBUG_SKIP_VIDEO
	for (unsigned int count = 0; ; count++)
	{
		Picam::Msg msg = this->Wait();
		if (msg.type == Picam::MsgType::RequestComplete) {
			// std::cout << "Msg RequestComplete" << std::endl;
		} else if (msg.type == Picam::MsgType::Quit) {
			std::cout << "Msg Quit" << std::endl;
		}
		if (msg.type == Picam::MsgType::Quit)
			return;
		else if (msg.type != Picam::MsgType::RequestComplete)
			throw std::runtime_error("unrecognised message!");

		// Check if we received a signal
		int key = get_key_or_signal(option, p);
		// if (key == '\n')
		// 	output->Signal();

		// log_debug("Viewfinder frame %d\n", count);
		auto now = std::chrono::high_resolution_clock::now();
		uint64_t timeout_ms = 5 * 60 * 1000;
		// uint64_t timeout_ms = 30 * 1000;
		uint32_t frames = 0;
		bool timeout = !frames && timeout_ms &&
					   (now - start_time > std::chrono::milliseconds(timeout_ms));
		bool frameout = frames && count >= frames;
		if (!this->keepRunning || timeout || frameout || key == 'x' || key == 'X')
		{
			// if (timeout)
			// 	std::cerr << "Halting: reached timeout of " << timeout_ms << " milliseconds.\n";
			std::cerr << "Halting" << std::endl;
			this->StopCamera(); // stop complains if encoder very slow to close
			this->StopEncoder();
			this->stopAllThreads();
			return;
		}

		// Got a video frame from camera
		CompletedRequestPtr &completed_request = std::get<CompletedRequestPtr>(msg.payload);

		// std::cout << " sequence=" << completed_request->sequence << "\tframerate=" << completed_request->framerate << std::endl;
		// std::cout << " buffers:" << std::endl;
		// for (const auto& elem : completed_request->buffers)
		// {
		// // 	auto config = elem.first->configuration();
		// // 	std::cout << "  pixelformat=" << config.pixelFormat.toString()
		// // 		<< " width=" << config.size.width << " height=" << config.size.height
		// // 		<< " frameSize=" << config.frameSize << " bufferCount=" << config.bufferCount
		// // 		<< " second=" << elem.second << std::endl;
		// 	auto frameBuffer = elem.second;
		// // 	auto planes = frameBuffer->planes();
		// // 	for (const libcamera::FrameBuffer::Plane plane : elem.second->planes()) {
		// // 		std::cout << "  plane length=" << plane.length << std::endl;
		// // 	}
		// 	auto metadata = frameBuffer->metadata();
		// 	// std::cout << " timestamp=" << metadata.timestamp << std::endl;
		// 	if (lastTimestamp != 0) {
		// 		std::cout << " diff=" << ((metadata.timestamp - lastTimestamp) / 1000000.0f) << std::endl;
		// 	}
		// 	lastTimestamp = metadata.timestamp;
		// }

		if (this->option->is_vfr_enabled) {
			// video
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			time_for_last_pts = ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
		}

		this->modifyBuffer(completed_request);

		// NOTE: If Raspberry Pi is connected to a monitor, EncodeBuffer() will
		// take some time and fps will drop
		// auto start = std::chrono::high_resolution_clock::now();
		this->EncodeBuffer(completed_request, this->VideoStream());
    // auto stop = std::chrono::high_resolution_clock::now();
		// auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
		// std::cout << "time " << duration.count() << std::endl;

		this->ShowPreview(completed_request, this->VideoStream());
	}
#endif

	audioThread.join();

	std::cout << "delete audio" << std::endl;
	delete audio;
}

void Picam::setOption(PicamOption *option)
{
  this->option = option;
}

void stopSignalHandler(int signo) {
  log_debug("stop requested (signal=%d)\n", signo);
	Picam *picam;
	picam->getInstance().stop();
}

void Picam::stop() {
	this->keepRunning = false;
}

int Picam::run(int argc, char *argv[])
{
  // Turn off buffering for stdout
  setvbuf(stdout, NULL, _IONBF, 0);

  log_set_level(LOG_LEVEL_DEBUG); // Log level will be changed later by PicamOption::parse()
  log_set_stream(stdout);

	try
	{
    PicamOption option;
    int ret = option.parse(argc, argv);
    if (ret != 0) {
      exit(ret);
    }
    mpegts_set_config(option.video_bitrate, option.video_width, option.video_height);
    audio_min_value = (int) (-32768 / option.audio_volume_multiply);
    audio_max_value = (int) (32767 / option.audio_volume_multiply);

    this->setOption(&option);

		struct sigaction int_handler;
		int_handler.sa_handler = stopSignalHandler;
		sigaction(SIGINT, &int_handler, NULL);
		sigaction(SIGTERM, &int_handler, NULL);

		if (!this->option->query_and_exit) {
			if (state_create_dir(this->option->state_dir) != 0) {
				return EXIT_FAILURE;
			}
			if (hooks_create_dir(this->option->hooks_dir) != 0) {
				return EXIT_FAILURE;
			}

			create_dir(this->option->rec_dir);
			create_dir(this->option->rec_tmp_dir);
			create_dir(this->option->rec_archive_dir);

			if (this->option->is_hlsout_enabled) {
				this->ensure_hls_dir_exists();
			}

			state_set(this->option->state_dir, "record", "false");

			if (clear_hooks(this->option->hooks_dir) != 0) {
				log_error("error: clear_hooks() failed\n");
			}
			start_watching_hooks(&hooks_thread, this->option->hooks_dir, on_file_create, 1);

			if (this->option->is_rtspout_enabled) {
				rtsp_setup_socks({
					this->option->rtsp_video_control_path,
					this->option->rtsp_audio_control_path,
					this->option->rtsp_video_data_path,
					this->option->rtsp_audio_data_path,
				});
			}
		}

		this->rec_settings = {
			(char *)"", // recording_dest_dir
			(char *)"", // recording_basename
			this->option->rec_dir, // rec_dir
			this->option->rec_tmp_dir, // rec_tmp_dir
			this->option->rec_archive_dir, // rec_archive_dir
		};

    this->event_loop();
	}
	catch (std::exception const &e)
	{
		std::cerr << "ERROR: *** " << e.what() << " ***" << std::endl;
		return -1;
	}
	return 0;
}

// >>> libcamera_app.cpp
void Picam::OpenCamera()
{
	char previewRect[128];
	// Make a preview window.
	Options previewOptions;
	previewOptions.width = this->option->video_width;
	previewOptions.height = this->option->video_height;
	previewOptions.framerate = this->option->video_fps;
	previewOptions.nopreview = !this->option->is_preview_enabled;
	previewOptions.qt_preview = false; // This is always false as QT preview works only if X is running
	previewOptions.verbose = log_get_level() >= LOG_LEVEL_DEBUG;
	if (this->option->is_previewrect_enabled) {
		previewOptions.fullscreen = false;
		previewOptions.preview_x = this->option->preview_x;
		previewOptions.preview_y = this->option->preview_y;
		previewOptions.preview_width = this->option->preview_width;
		previewOptions.preview_height = this->option->preview_height;
		snprintf(
			previewRect,
			sizeof(previewRect)-1,
			"%d,%d,%d,%d",
			this->option->preview_x,
			this->option->preview_y,
			this->option->preview_width,
			this->option->preview_height);
		previewOptions.preview = previewRect;
	} else {
		previewOptions.fullscreen = true;
		previewOptions.preview_x = 0;
		previewOptions.preview_y = 0;
		previewOptions.preview_width = 0;
		previewOptions.preview_height = 0;
	}
	// fullscreen=false, x=100 y=200 width=300 height=400 にすると画面が暗くなる
	preview_ = std::unique_ptr<Preview>(make_preview(&previewOptions));
	preview_->SetDoneCallback(std::bind(&Picam::previewDoneCallback, this, std::placeholders::_1));

	log_debug("Opening camera...\n");

	camera_manager_ = std::make_unique<libcamera::CameraManager>();
	int ret = camera_manager_->start();
	if (ret)
		throw std::runtime_error("camera manager failed to start, code " + std::to_string(-ret));

	std::vector<std::shared_ptr<libcamera::Camera>> cameras = camera_manager_->cameras();
	// Do not show USB webcams as these are not supported in libcamera-apps!
	auto rem = std::remove_if(cameras.begin(), cameras.end(),
							  [](auto &cam) { return cam->id().find("/usb") != std::string::npos; });
	cameras.erase(rem, cameras.end());

	unsigned int cameraId = 0;

	if (cameras.size() == 0)
		throw std::runtime_error("no cameras available");
	if (cameraId >= cameras.size())
		throw std::runtime_error("selected camera is not available");

	std::string const &cam_id = cameras[cameraId]->id();
	camera_ = camera_manager_->get(cam_id);
	if (!camera_)
		throw std::runtime_error("failed to find camera " + cam_id);

	if (camera_->acquire())
		throw std::runtime_error("failed to acquire camera " + cam_id);
	camera_acquired_ = true;

	log_debug("Acquired camera %d\n", cam_id);

	// if (!options_->post_process_file.empty())
	// 	post_processor_.Read(options_->post_process_file);

	// The queue takes over ownership from the post-processor.
	// post_processor_.SetCallback(
	// 	[this](CompletedRequestPtr &r) { this->msg_queue_.Post(Msg(MsgType::RequestComplete, std::move(r))); });
}

void Picam::CloseCamera()
{
	preview_.reset();

	if (camera_acquired_)
		camera_->release();
	camera_acquired_ = false;

	camera_.reset();

	camera_manager_.reset();

	log_debug("Camera closed\n");
}

static libcamera::PixelFormat mode_to_pixel_format(Mode const &mode)
{
	// The saving grace here is that we can ignore the Bayer order and return anything -
	// our pipeline handler will give us back the order that works, whilst respecting the
	// bit depth and packing. We may get a "stream adjusted" message, which we can ignore.

	static std::vector<std::pair<Mode, libcamera::PixelFormat>> table = {
		{ Mode(0, 0, 8, false), libcamera::formats::SBGGR8 },
		{ Mode(0, 0, 8, true), libcamera::formats::SBGGR8 },
		{ Mode(0, 0, 10, false), libcamera::formats::SBGGR10 },
		{ Mode(0, 0, 10, true), libcamera::formats::SBGGR10_CSI2P },
		{ Mode(0, 0, 12, false), libcamera::formats::SBGGR12 },
		{ Mode(0, 0, 12, true), libcamera::formats::SBGGR12_CSI2P },
	};

	auto it = std::find_if(table.begin(), table.end(), [&mode] (auto &m) { return mode.bit_depth == m.first.bit_depth && mode.packed == m.first.packed; });
	if (it != table.end())
		return it->second;

	return libcamera::formats::SBGGR12_CSI2P;
}

void Picam::ConfigureVideo(unsigned int flags)
{
	log_debug("Configuring video...\n");

	// Previously option
	bool have_raw_stream = false;
	bool have_lores_stream = false;

	// bool have_raw_stream = (flags & FLAG_VIDEO_RAW) || options_->mode.bit_depth;
	// bool have_lores_stream = options_->lores_width && options_->lores_height;

	libcamera::StreamRoles stream_roles = { libcamera::StreamRole::VideoRecording };
	int lores_index = 1;
	if (have_raw_stream)
	{
		stream_roles.push_back(libcamera::StreamRole::Raw);
		lores_index = 2;
	}
	if (have_lores_stream)
		stream_roles.push_back(libcamera::StreamRole::Viewfinder);
	configuration_ = camera_->generateConfiguration(stream_roles);
	if (!configuration_)
		throw std::runtime_error("failed to generate video configuration");

	// Now we get to override any of the default settings from the options_->
	libcamera::StreamConfiguration &cfg = configuration_->at(0);
	cfg.pixelFormat = libcamera::formats::YUV420;
	cfg.bufferCount = 6; // 6 buffers is better than 4
	cfg.size.width = this->option->video_width;
	cfg.size.height = this->option->video_height;
	if (flags & FLAG_VIDEO_JPEG_COLOURSPACE)
		cfg.colorSpace = libcamera::ColorSpace::Jpeg;
	else if (cfg.size.width >= 1280 || cfg.size.height >= 720)
		cfg.colorSpace = libcamera::ColorSpace::Rec709;
	else
		cfg.colorSpace = libcamera::ColorSpace::Smpte170m;

	// TODO

	// configuration_->transform = options_->transform;

	// post_processor_.AdjustConfig("video", &configuration_->at(0));

	// Previously option
	Mode mode;
	bool rawfull = false;
	libcamera::Transform transform = libcamera::Transform::Identity;
	if (this->option->video_hflip) {
		transform = libcamera::Transform::HFlip * transform;
	}
	if (this->option->video_vflip) {
		transform = libcamera::Transform::VFlip * transform;
	}
	bool ok;
	libcamera::Transform rot = libcamera::transformFromRotation(this->option->video_rotation, &ok);
	if (!ok)
		throw std::runtime_error("illegal rotation value");
	transform = rot * transform;
	if (!!(transform & libcamera::Transform::Transpose))
		throw std::runtime_error("transforms requiring transpose not supported");
	std::string denoise = "auto";

	if (have_raw_stream)
	{
		if (mode.bit_depth)
		{
			configuration_->at(1).size = mode.Size();
			configuration_->at(1).pixelFormat = mode_to_pixel_format(mode);
		}
		else if (!rawfull)
			configuration_->at(1).size = configuration_->at(0).size;
		configuration_->at(1).bufferCount = configuration_->at(0).bufferCount;
	}
	if (have_lores_stream)
	{
		libcamera::Size lores_size(this->option->video_width, this->option->video_height);
		lores_size.alignDownTo(2, 2);
		if (lores_size.width > configuration_->at(0).size.width ||
			lores_size.height > configuration_->at(0).size.height)
			throw std::runtime_error("Low res image larger than video");
		configuration_->at(lores_index).pixelFormat = libcamera::formats::YUV420;
		configuration_->at(lores_index).size = lores_size;
		configuration_->at(lores_index).bufferCount = configuration_->at(0).bufferCount;
	}
	configuration_->transform = transform;

	configureDenoise(denoise == "auto" ? "cdn_fast" : denoise);
	setupCapture();

	streams_["video"] = configuration_->at(0).stream();
	if (have_raw_stream)
		streams_["raw"] = configuration_->at(1).stream();
	if (have_lores_stream)
		streams_["lores"] = configuration_->at(lores_index).stream();

	// post_processor_.Configure();

	log_debug("Video setup complete\n");
}

void Picam::Teardown()
{
	printf("stopPreview begin\n");
	stopPreview();
	printf("stopPreview end\n");

	// post_processor_.Teardown();

	log_debug("Tearing down requests, buffers and configuration\n");

	printf("clearing mapped_buffers_\n");
	for (auto &iter : mapped_buffers_)
	{
		// assert(iter.first->planes().size() == iter.second.size());
		// for (unsigned i = 0; i < iter.first->planes().size(); i++)
		for (auto &span : iter.second)
			munmap(span.data(), span.size());
	}
	mapped_buffers_.clear();
	printf("cleared mapped_buffers_\n");

	delete allocator_;
	allocator_ = nullptr;

	printf("configuration reset\n");
	configuration_.reset();

	printf("frame buffers clear\n");
	frame_buffers_.clear();

	printf("streams clear\n");
	streams_.clear();
	printf("Teardown end\n");
}

void Picam::StartCamera()
{
	// This makes all the Request objects that we shall need.
	makeRequests();

	// Previously option
	float roi_x = 0;
	float roi_y = 0;
	float roi_width = 0;
	float roi_height = 0;

	// Build a list of initial controls that we must set in the camera before starting it.
	// We don't overwrite anything the application may have set before calling us.
	if (!controls_.contains(libcamera::controls::ScalerCrop) && roi_width != 0 && roi_height != 0)
	{
		libcamera::Rectangle sensor_area = camera_->properties().get(libcamera::properties::ScalerCropMaximum);
		int x = roi_x * sensor_area.width;
		int y = roi_y * sensor_area.height;
		int w = roi_width * sensor_area.width;
		int h = roi_height * sensor_area.height;
		libcamera::Rectangle crop(x, y, w, h);
		crop.translateBy(sensor_area.topLeft());
		log_debug("Using crop %s\n", crop.toString());
		controls_.set(libcamera::controls::ScalerCrop, crop);
	}

	// controls_.set(libcamera::controls::FrameDuration, INT64_C(16666));

	// Framerate is a bit weird. If it was set programmatically, we go with that, but
	// otherwise it applies only to preview/video modes. For stills capture we set it
	// as long as possible so that we get whatever the exposure profile wants.
	if (!controls_.contains(libcamera::controls::FrameDurationLimits))
	{
		// if (StillStream())
		// 	controls_.set(libcamera::controls::FrameDurationLimits, { INT64_C(100), INT64_C(1000000000) });
		// else if (options_->framerate > 0)
		if (this->option->is_vfr_enabled) {
			// Set framerate range to min=1 and max=100
			int64_t frame_time_fps1 = 1000000 / 1; // in us
			int64_t frame_time_fps100 = 1000000 / 100; // in us
			std::cout << "vfr frame_time=" << frame_time_fps100 << ".." << frame_time_fps1 << std::endl; // frame_time == 33333
			controls_.set(libcamera::controls::FrameDurationLimits, { frame_time_fps100, frame_time_fps1 });
		} else if (this->option->video_fps > 0) {
			int64_t frame_time = 1000000 / this->option->video_fps; // in us
			std::cout << "frame_time=" << frame_time << std::endl; // frame_time == 33333
			controls_.set(libcamera::controls::FrameDurationLimits, { frame_time, frame_time });
			// controls_.set(libcamera::controls::FrameDurationLimits, { frame_time, INT64_C(50000) });
		}
	}

	// Previously option
	float shutter = 0;
	float gain = 0;
	int metering_index = libcamera::controls::MeteringCentreWeighted;
	int exposure_index = libcamera::controls::ExposureNormal;
	float ev = 0;
	int awb_index = libcamera::controls::AwbAuto;
	float awb_gain_r = 0;
	float awb_gain_b = 0;
	float brightness = 0;
	float contrast = 1.0;
	float saturation = 1.0;
	float sharpness = 1.0;

	if (!controls_.contains(libcamera::controls::ExposureTime) && shutter)
		controls_.set(libcamera::controls::ExposureTime, shutter);
	if (!controls_.contains(libcamera::controls::AnalogueGain) && gain)
		controls_.set(libcamera::controls::AnalogueGain, gain);
	if (!controls_.contains(libcamera::controls::AeMeteringMode))
		controls_.set(libcamera::controls::AeMeteringMode, metering_index);
	if (!controls_.contains(libcamera::controls::AeExposureMode))
		controls_.set(libcamera::controls::AeExposureMode, exposure_index);
	if (!controls_.contains(libcamera::controls::ExposureValue))
		controls_.set(libcamera::controls::ExposureValue, ev);
	if (!controls_.contains(libcamera::controls::AwbMode))
		controls_.set(libcamera::controls::AwbMode, awb_index);
	if (!controls_.contains(libcamera::controls::ColourGains) && awb_gain_r && awb_gain_b)
		controls_.set(libcamera::controls::ColourGains, { awb_gain_r, awb_gain_b });
	if (!controls_.contains(libcamera::controls::Brightness))
		controls_.set(libcamera::controls::Brightness, brightness);
	if (!controls_.contains(libcamera::controls::Contrast))
		controls_.set(libcamera::controls::Contrast, contrast);
	if (!controls_.contains(libcamera::controls::Saturation))
		controls_.set(libcamera::controls::Saturation, saturation);
	if (!controls_.contains(libcamera::controls::Sharpness))
		controls_.set(libcamera::controls::Sharpness, sharpness);

	if (camera_->start(&controls_))
		throw std::runtime_error("failed to start camera");
	controls_.clear();
	camera_started_ = true;
	last_timestamp_ = 0;

	// post_processor_.Start();

	camera_->requestCompleted.connect(this, &Picam::requestComplete);

	for (std::unique_ptr<libcamera::Request> &request : requests_)
	{
		if (camera_->queueRequest(request.get()) < 0)
			throw std::runtime_error("Failed to queue request");
	}

	log_debug("Camera started!\n");
}

void Picam::StopCamera()
{
	printf("StopCamera\n");
	{
		// We don't want QueueRequest to run asynchronously while we stop the camera.
		std::lock_guard<std::mutex> lock(camera_stop_mutex_);
		if (camera_started_)
		{
			if (camera_->stop())
				throw std::runtime_error("failed to stop camera");

			// post_processor_.Stop();

			camera_started_ = false;
		}
	}

	if (camera_)
		camera_->requestCompleted.disconnect(this, &Picam::requestComplete);

	// An application might be holding a CompletedRequest, so queueRequest will get
	// called to delete it later, but we need to know not to try and re-queue it.
	completed_requests_.clear();

	msg_queue_.Clear();

	requests_.clear();

	controls_.clear(); // no need for mutex here

	log_debug("Camera stopped!\n");
}

Picam::Msg Picam::Wait()
{
	return msg_queue_.Wait();
}

void Picam::queueRequest(CompletedRequest *completed_request)
{
	libcamera::Request::BufferMap buffers(std::move(completed_request->buffers));

	libcamera::Request *request = completed_request->request;
	delete completed_request;
	assert(request);

	// This function may run asynchronously so needs protection from the
	// camera stopping at the same time.
	std::lock_guard<std::mutex> stop_lock(camera_stop_mutex_);
	if (!camera_started_)
		return;

	// An application could be holding a CompletedRequest while it stops and re-starts
	// the camera, after which we don't want to queue another request now.
	{
		std::lock_guard<std::mutex> lock(completed_requests_mutex_);
		auto it = completed_requests_.find(completed_request);
		if (it == completed_requests_.end())
			return;
		completed_requests_.erase(it);
	}

	for (auto const &p : buffers)
	{
		if (request->addBuffer(p.first, p.second) < 0)
			throw std::runtime_error("failed to add buffer to request in QueueRequest");
	}

	{
		std::lock_guard<std::mutex> lock(control_mutex_);
		request->controls() = std::move(controls_);
	}

	if (camera_->queueRequest(request) < 0)
		throw std::runtime_error("failed to queue request");
}

void Picam::PostMessage(MsgType &t, MsgPayload &p)
{
	msg_queue_.Post(Msg(t, std::move(p)));
}

libcamera::Stream *Picam::GetStream(std::string const &name, StreamInfo *info) const
{
	auto it = streams_.find(name);
	if (it == streams_.end())
		return nullptr;
	if (info)
		*info = GetStreamInfo(it->second);
	return it->second;
}

libcamera::Stream *Picam::VideoStream(StreamInfo *info) const
{
	return GetStream("video", info);
}

std::vector<libcamera::Span<uint8_t>> Picam::Mmap(libcamera::FrameBuffer *buffer) const
{
	auto item = mapped_buffers_.find(buffer);
	if (item == mapped_buffers_.end())
		return {};
	return item->second;
}

void Picam::ShowPreview(CompletedRequestPtr &completed_request, libcamera::Stream *stream)
{
	// printf("preview: ShowPreview begin\n");
	std::lock_guard<std::mutex> lock(preview_item_mutex_);
	if (!preview_item_.stream)
		preview_item_ = PreviewItem(completed_request, stream); // copy the shared_ptr here
	else
		preview_frames_dropped_++;
	preview_cond_var_.notify_one();
	// printf("preview: ShowPreview end\n");
}

StreamInfo Picam::GetStreamInfo(libcamera::Stream const *stream) const
{
	libcamera::StreamConfiguration const &cfg = stream->configuration();
	StreamInfo info;
	info.width = cfg.size.width;
	info.height = cfg.size.height;
	info.stride = cfg.stride;
	info.pixel_format = stream->configuration().pixelFormat;
	info.colour_space = stream->configuration().colorSpace;
	return info;
}

void Picam::setupCapture()
{
	// First finish setting up the configuration.

	libcamera::CameraConfiguration::Status validation = configuration_->validate();
	if (validation == libcamera::CameraConfiguration::Invalid)
		throw std::runtime_error("failed to valid stream configurations");
	else if (validation == libcamera::CameraConfiguration::Adjusted)
		std::cerr << "Stream configuration adjusted" << std::endl;

	if (camera_->configure(configuration_.get()) < 0)
		throw std::runtime_error("failed to configure streams");
	log_debug("Camera streams configured\n");

	// Next allocate all the buffers we need, mmap them and store them on a free list.

	allocator_ = new libcamera::FrameBufferAllocator(camera_);
	for (libcamera::StreamConfiguration &config : *configuration_)
	{
		libcamera::Stream *stream = config.stream();

		if (allocator_->allocate(stream) < 0)
			throw std::runtime_error("failed to allocate capture buffers");

		for (const std::unique_ptr<libcamera::FrameBuffer> &buffer : allocator_->buffers(stream))
		{
			// "Single plane" buffers appear as multi-plane here, but we can spot them because then
			// planes all share the same fd. We accumulate them so as to mmap the buffer only once.
			size_t buffer_size = 0;
			for (unsigned i = 0; i < buffer->planes().size(); i++)
			{
				const libcamera::FrameBuffer::Plane &plane = buffer->planes()[i];
				buffer_size += plane.length;
				if (i == buffer->planes().size() - 1 || plane.fd.get() != buffer->planes()[i + 1].fd.get())
				{
					void *memory = mmap(NULL, buffer_size, PROT_READ | PROT_WRITE, MAP_SHARED, plane.fd.get(), 0);
					mapped_buffers_[buffer.get()].push_back(
						libcamera::Span<uint8_t>(static_cast<uint8_t *>(memory), buffer_size));
					buffer_size = 0;
				}
			}
			frame_buffers_[stream].push(buffer.get());
		}
	}
	log_debug("Buffers allocated and mapped\n");

	startPreview();

	// The requests will be made when StartCamera() is called.
}

void Picam::makeRequests()
{
	auto free_buffers(frame_buffers_);
	while (true)
	{
		for (libcamera::StreamConfiguration &config : *configuration_)
		{
			libcamera::Stream *stream = config.stream();
			if (stream == configuration_->at(0).stream())
			{
				if (free_buffers[stream].empty())
				{
					log_debug("Requests created\n");
					return;
				}
				std::unique_ptr<libcamera::Request> request = camera_->createRequest();
				if (!request)
					throw std::runtime_error("failed to make request");
				requests_.push_back(std::move(request));
			}
			else if (free_buffers[stream].empty())
				throw std::runtime_error("concurrent streams need matching numbers of buffers");

			libcamera::FrameBuffer *buffer = free_buffers[stream].front();
			free_buffers[stream].pop();
			if (requests_.back()->addBuffer(stream, buffer) < 0)
				throw std::runtime_error("failed to add buffer to request");
		}
	}
}

void Picam::requestComplete(libcamera::Request *request)
{
	if (request->status() == libcamera::Request::RequestCancelled)
		return;

	CompletedRequest *r = new CompletedRequest(sequence_++, request);
	CompletedRequestPtr payload(r, [this](CompletedRequest *cr) { this->queueRequest(cr); });
	{
		std::lock_guard<std::mutex> lock(completed_requests_mutex_);
		completed_requests_.insert(r);
	}

	// We calculate the instantaneous framerate in case anyone wants it.
	// Use the sensor timestamp if possible as it ought to be less glitchy than
	// the buffer timestamps.
	uint64_t timestamp = payload->metadata.contains(libcamera::controls::SensorTimestamp)
							? payload->metadata.get(libcamera::controls::SensorTimestamp)
							: payload->buffers.begin()->second->metadata().timestamp;
	if (last_timestamp_ == 0 || last_timestamp_ == timestamp)
		payload->framerate = 0;
	else
		payload->framerate = 1e9 / (timestamp - last_timestamp_);
	last_timestamp_ = timestamp;

	// post_processor_.Process(payload); // post-processor can re-use our shared_ptr

	// My test:
	// [this](CompletedRequestPtr &r) { this->msg_queue_.Post(Msg(MsgType::RequestComplete, std::move(r))); }(payload);

	this->msg_queue_.Post(Msg(MsgType::RequestComplete, std::move(payload)));
}

void Picam::previewDoneCallback(int fd)
{
	// printf("preview: previewDoneCallback begin\n");
	std::lock_guard<std::mutex> lock(preview_mutex_);
	auto it = preview_completed_requests_.find(fd);
	if (it == preview_completed_requests_.end())
		throw std::runtime_error("previewDoneCallback: missing fd " + std::to_string(fd));
	preview_completed_requests_.erase(it); // drop shared_ptr reference
	// printf("preview: previewDoneCallback end\n");
}

void Picam::startPreview()
{
	printf("preview: startPreview begin\n");
	preview_abort_ = false;
	preview_thread_ = std::thread(&Picam::previewThread, this);
	printf("preview: startPreview end\n");
}

void Picam::stopPreview()
{
	printf("preview: stopPreview begin\n");
	if (!preview_thread_.joinable()) {
		printf("preview: preview_thread is not joinable\n");
		// in case never started
		return;
	}

	{
		std::lock_guard<std::mutex> lock(preview_item_mutex_);
		preview_abort_ = true;
		preview_cond_var_.notify_one();
	}
	preview_thread_.join();
	preview_item_ = PreviewItem();
	printf("preview: stopPreview end\n");
}

void Picam::previewThread()
{
	while (true)
	{
		PreviewItem item;
		while (!item.stream)
		{
			std::unique_lock<std::mutex> lock(preview_item_mutex_);
			if (preview_abort_)
			{
				preview_->Reset();
				return;
			}
			else if (preview_item_.stream)
				item = std::move(preview_item_); // re-use existing shared_ptr reference
			else
				preview_cond_var_.wait(lock);
		}

		if (item.stream->configuration().pixelFormat != libcamera::formats::YUV420)
			throw std::runtime_error("Preview windows only support YUV420");

		StreamInfo info = GetStreamInfo(item.stream);
		libcamera::FrameBuffer *buffer = item.completed_request->buffers[item.stream];
		libcamera::Span span = Mmap(buffer)[0];

		// Fill the frame info with the ControlList items and ancillary bits.
		FrameInfo frame_info(item.completed_request->metadata);
		frame_info.fps = item.completed_request->framerate;
		frame_info.sequence = item.completed_request->sequence;

		int fd = buffer->planes()[0].fd.get();
		{
			std::lock_guard<std::mutex> lock(preview_mutex_);
			// the reference to the shared_ptr moves to the map here
			preview_completed_requests_[fd] = std::move(item.completed_request);
		}
		if (preview_->Quit())
		{
			log_debug("Preview window has quit\n");
			msg_queue_.Post(Msg(MsgType::Quit));
		}
		preview_frames_displayed_++;
		preview_->Show(fd, span, info);
		// if (!options_->info_text.empty())
		// {
		// 	std::string s = frame_info.ToString(options_->info_text);
		// 	preview_->SetInfoText(s);
		// }
	}
}

void Picam::configureDenoise(const std::string &denoise_mode)
{
	using namespace libcamera::controls::draft;

	static const std::map<std::string, NoiseReductionModeEnum> denoise_table = {
		{ "off", NoiseReductionModeOff },
		{ "cdn_off", NoiseReductionModeMinimal },
		{ "cdn_fast", NoiseReductionModeFast },
		{ "cdn_hq", NoiseReductionModeHighQuality }
	};
	NoiseReductionModeEnum denoise;

	auto const mode = denoise_table.find(denoise_mode);
	if (mode == denoise_table.end())
		throw std::runtime_error("Invalid denoise mode " + denoise_mode);
	denoise = mode->second;

	controls_.set(NoiseReductionMode, denoise);
}
// <<< libcamera_app.cpp