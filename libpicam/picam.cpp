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
#include <linux/videodev2.h>
#include <libcamera/transform.h>

#include "core/libcamera_encoder.hpp"
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
#include "preview/preview.hpp"
#include "core/options.hpp"
#include "picam.hpp"

using namespace std::placeholders;

// Some keypress/signal handling.

#define DEBUG_SKIP_VIDEO 0

static int signal_received;
static std::thread audioThread;
static Audio *audio;
static Muxer *muxer;
static HTTPLiveStreaming *hls;
static float video_fps = 40.0f; // TODO: Make this an option
static int64_t video_timestamp_origin_us = -1;

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
	log_debug("Closing Libcamera application (frames displayed %d, dropped %d\n",
	preview_frames_displayed_, preview_frames_dropped_);
	StopCamera();
	Teardown();
	CloseCamera();
}

static void stop_audio_thread() {
	if (audioThread.joinable()) {
		std::cout << "joinable" << std::endl;
		audio->stop();
		audioThread.join();
		std::cout << "joined" << std::endl;
	}
	delete audio;
}

static void stop_rec_thread() {
	printf("stop_rec_thread\n");
	muxer->prepareForDestroy();
	muxer->waitForExit();
}

static void stop_all_threads() {
	printf("stop_all_threads begin\n");
	stop_audio_thread();
	stop_rec_thread();
	printf("stop_all_thread end\n");
}

static void default_signal_handler(int signal_number)
{
	signal_received = signal_number;
	std::cout << "Received signal " << signal_number << std::endl;
	stop_all_threads();
	std::cout << "end of signal handler" << std::endl;
}

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
	// int64_t timestamp_ns = completed_request->metadata.contains(controls::SensorTimestamp)
	// 						? completed_request->metadata.get(controls::SensorTimestamp)
	// 						: buffer->metadata().timestamp;
	const uint32_t FOURCC_YU12 = 0x32315559; // YU12
	if (info.pixel_format.fourcc() == FOURCC_YU12) {
		static bool isTextInited = false;
		if (!isTextInited) {
			isTextInited = true;
			float font_points = 28.0f;
		    int font_dpi = 96;
		    int color = 0xffffff;
			int stroke_color = 0x000000;
			float stroke_width = 1.0f;
			int in_preview = 1;
			int in_video = 1;
			int letter_spacing = 0;
			float line_height_multiply = 1.0f;
			float tab_scale = 1.0f;
			LAYOUT_ALIGN layout_align = (LAYOUT_ALIGN) (LAYOUT_ALIGN_BOTTOM | LAYOUT_ALIGN_CENTER);
		    TEXT_ALIGN text_align = TEXT_ALIGN_CENTER;
			int horizontal_margin = 0;
			int vertical_margin = 35;
		    float duration = 2.0f;
			const char *replaced_text = "Hello C+_+!";
			int text_len = strlen(replaced_text);
	        subtitle_init_with_font_name(NULL, font_points, font_dpi);
			subtitle_set_color(color);
			subtitle_set_stroke_color(stroke_color);
			subtitle_set_stroke_width(stroke_width);
			subtitle_set_visibility(in_preview, in_video);
			subtitle_set_letter_spacing(letter_spacing);
			subtitle_set_line_height_multiply(line_height_multiply);
			subtitle_set_tab_scale(tab_scale);
			subtitle_set_layout(layout_align,
				horizontal_margin, vertical_margin);
			subtitle_set_align(text_align);

			// show subtitle for 7 seconds
			subtitle_show(replaced_text, text_len, duration);
		}

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

void outputReady(void *mem, size_t size, int64_t timestamp_us, bool keyframe)
{
	// printf("outputReady size=%d timestamp=%lld keyframe=%d\n", size, timestamp_us, keyframe);
	if (video_timestamp_origin_us == -1) {
		video_timestamp_origin_us = timestamp_us;
		printf("xxx set video_timestamp_origin_us=%lld\n", video_timestamp_origin_us);

		// Without the casting (int64_t), we will going to get the wrong result
		video_timestamp_origin_us -= (int64_t)std::round(1000000 / video_fps);

		printf("1000000 / video_fps = %f (%lld)\n", 1000000 / video_fps, (int64_t)(1000000 / video_fps));
		printf("xxx adjusted video_timestamp_origin_us=%lld\n", video_timestamp_origin_us);
	}
	int flags = 0;
	if (keyframe) {
		flags |= AV_PKT_FLAG_KEY;
	}
	muxer->add_encoded_packet(std::round((timestamp_us - video_timestamp_origin_us) / (1000000 / 90000)), (uint8_t *)mem, size, hls->format_ctx->streams[0]->index, flags);

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
	// std::cout << "### SetEncodeOutputReadyCallback ###" << std::endl;
	this->SetEncodeOutputReadyCallback(outputReady);
	// this->SetEncodeOutputReadyCallback(std::bind(&Output::OutputReady, output.get(), _1, _2, _3, _4));

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

	muxer = new Muxer();
	muxer->setup(&codec_settings);
	audio->set_encode_callback([=](int64_t pts, uint8_t *data, int size, int stream_index, int flags) -> void {
		muxer->add_encoded_packet(pts, data, size, stream_index, flags);
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
	long video_bitrate = 2000 * 1000; // 2 Mbps // TODO: Make this an option

	mpegts_set_config(option->video_bitrate, option->video_width, option->video_height);
	float audio_volume_multiply = 1.0f; // TODO: Make this an option
	int audio_min_value = (int) (-32768 / audio_volume_multiply);
	int audio_max_value = (int) (32767 / audio_volume_multiply);
	printf("fr_q16=%d video_bitrate=%ld audio_min_value=%d audio_max_value=%d\n",
		fr_q16, video_bitrate, audio_min_value, audio_max_value);

	muxer->prepare_encoded_packets(video_fps, audio->get_fps());

	// Monitoring for keypresses and signals.
	signal(SIGUSR1, default_signal_handler);
	signal(SIGUSR2, default_signal_handler);
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

	muxer->start_record();

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
    uint64_t timeout_ms = (6 * 60 + 15) * 1000;
    uint32_t frames = 0;
		bool timeout = !frames && timeout_ms &&
					   (now - start_time > std::chrono::milliseconds(timeout_ms));
		bool frameout = frames && count >= frames;
		if (timeout || frameout || key == 'x' || key == 'X')
		{
			if (timeout)
				std::cerr << "Halting: reached timeout of " << timeout_ms << " milliseconds.\n";
			this->StopCamera(); // stop complains if encoder very slow to close
			this->StopEncoder();
			stop_all_threads();
			return;
		}

		CompletedRequestPtr &completed_request = std::get<CompletedRequestPtr>(msg.payload);

		std::cout << " sequence=" << completed_request->sequence << "\tframerate=" << completed_request->framerate << std::endl;
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

		this->modifyBuffer(completed_request);
		this->EncodeBuffer(completed_request, this->VideoStream());

		// TODO: Transfer the encoded H.264 frame to MEPG-TS

		this->ShowPreview(completed_request, this->VideoStream());
	}
#endif

	// TODO: Audio thread event loop {
	//   TODO: Capture PCM from microphone
	//   TODO: Feed the PCM into MPEG-TS
	// }

	audioThread.join();

	std::cout << "delete audio" << std::endl;
	delete audio;
}

void Picam::setOption(PicamOption *option)
{
  this->option = option;
}

int Picam::run(int argc, char *argv[])
{
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
    // this->setupEncoder();
    this->event_loop();

		// LibcameraEncoder app;
		// VideoOptions *options = this->GetOptions();
		// if (options->Parse(argc, argv))
		// {
		// 	if (options->verbose)
		// 		options->Print();

		// 	event_loop(app);
		// }
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
	// Make a preview window.
	Options previewOptions;
	previewOptions.width = this->option->video_width;
	previewOptions.height = this->option->video_height;
	previewOptions.framerate = this->option->video_fps;
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
	stopPreview();

	// post_processor_.Teardown();

	log_debug("Tearing down requests, buffers and configuration\n");

	for (auto &iter : mapped_buffers_)
	{
		// assert(iter.first->planes().size() == iter.second.size());
		// for (unsigned i = 0; i < iter.first->planes().size(); i++)
		for (auto &span : iter.second)
			munmap(span.data(), span.size());
	}
	mapped_buffers_.clear();

	delete allocator_;
	allocator_ = nullptr;

	configuration_.reset();

	frame_buffers_.clear();

	streams_.clear();
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
	if (!controls_.contains(controls::ScalerCrop) && roi_width != 0 && roi_height != 0)
	{
		libcamera::Rectangle sensor_area = camera_->properties().get(properties::ScalerCropMaximum);
		int x = roi_x * sensor_area.width;
		int y = roi_y * sensor_area.height;
		int w = roi_width * sensor_area.width;
		int h = roi_height * sensor_area.height;
		libcamera::Rectangle crop(x, y, w, h);
		crop.translateBy(sensor_area.topLeft());
		log_debug("Using crop %s\n", crop.toString());
		controls_.set(controls::ScalerCrop, crop);
	}

	// controls_.set(controls::FrameDuration, INT64_C(16666));

	// Framerate is a bit weird. If it was set programmatically, we go with that, but
	// otherwise it applies only to preview/video modes. For stills capture we set it
	// as long as possible so that we get whatever the exposure profile wants.
	if (!controls_.contains(controls::FrameDurationLimits))
	{
		// if (StillStream())
		// 	controls_.set(controls::FrameDurationLimits, { INT64_C(100), INT64_C(1000000000) });
		// else if (options_->framerate > 0)
		if (this->option->video_fps > 0)
		{
			int64_t frame_time = 1000000 / this->option->video_fps; // in us
			std::cout << "frame_time=" << frame_time << std::endl; // frame_time == 33333
			controls_.set(controls::FrameDurationLimits, { frame_time, frame_time });
			// controls_.set(controls::FrameDurationLimits, { frame_time, INT64_C(50000) });
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

	if (!controls_.contains(controls::ExposureTime) && shutter)
		controls_.set(controls::ExposureTime, shutter);
	if (!controls_.contains(controls::AnalogueGain) && gain)
		controls_.set(controls::AnalogueGain, gain);
	if (!controls_.contains(controls::AeMeteringMode))
		controls_.set(controls::AeMeteringMode, metering_index);
	if (!controls_.contains(controls::AeExposureMode))
		controls_.set(controls::AeExposureMode, exposure_index);
	if (!controls_.contains(controls::ExposureValue))
		controls_.set(controls::ExposureValue, ev);
	if (!controls_.contains(controls::AwbMode))
		controls_.set(controls::AwbMode, awb_index);
	if (!controls_.contains(controls::ColourGains) && awb_gain_r && awb_gain_b)
		controls_.set(controls::ColourGains, { awb_gain_r, awb_gain_b });
	if (!controls_.contains(controls::Brightness))
		controls_.set(controls::Brightness, brightness);
	if (!controls_.contains(controls::Contrast))
		controls_.set(controls::Contrast, contrast);
	if (!controls_.contains(controls::Saturation))
		controls_.set(controls::Saturation, saturation);
	if (!controls_.contains(controls::Sharpness))
		controls_.set(controls::Sharpness, sharpness);

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
	std::lock_guard<std::mutex> lock(preview_item_mutex_);
	if (!preview_item_.stream)
		preview_item_ = PreviewItem(completed_request, stream); // copy the shared_ptr here
	else
		preview_frames_dropped_++;
	preview_cond_var_.notify_one();
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
	uint64_t timestamp = payload->metadata.contains(controls::SensorTimestamp)
							? payload->metadata.get(controls::SensorTimestamp)
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
	std::lock_guard<std::mutex> lock(preview_mutex_);
	auto it = preview_completed_requests_.find(fd);
	if (it == preview_completed_requests_.end())
		throw std::runtime_error("previewDoneCallback: missing fd " + std::to_string(fd));
	preview_completed_requests_.erase(it); // drop shared_ptr reference
}

void Picam::startPreview()
{
	preview_abort_ = false;
	preview_thread_ = std::thread(&Picam::previewThread, this);
}

void Picam::stopPreview()
{
	if (!preview_thread_.joinable()) // in case never started
		return;

	{
		std::lock_guard<std::mutex> lock(preview_item_mutex_);
		preview_abort_ = true;
		preview_cond_var_.notify_one();
	}
	preview_thread_.join();
	preview_item_ = PreviewItem();
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
