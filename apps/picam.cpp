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

#include "core/libcamera_encoder.hpp"
#include "output/output.hpp"
#include "timestamp/timestamp.h"

using namespace std::placeholders;

// Some keypress/signal handling.

static int signal_received;
static void default_signal_handler(int signal_number)
{
	signal_received = signal_number;
	std::cerr << "Received signal " << signal_number << std::endl;
}

static int get_key_or_signal(VideoOptions const *options, pollfd p[1])
{
	int key = 0;
	if (options->keypress)
	{
		poll(p, 1, 0);
		if (p[0].revents & POLLIN)
		{
			char *user_string = nullptr;
			size_t len;
			[[maybe_unused]] size_t r = getline(&user_string, &len, stdin);
			key = user_string[0];
		}
	}
	if (options->signal)
	{
		if (signal_received == SIGUSR1)
			key = '\n';
		else if (signal_received == SIGUSR2)
			key = 'x';
		signal_received = 0;
	}
	return key;
}

static int get_colourspace_flags(std::string const &codec)
{
	if (codec == "mjpeg" || codec == "yuv420")
		return LibcameraEncoder::FLAG_VIDEO_JPEG_COLOURSPACE;
	else
		return LibcameraEncoder::FLAG_VIDEO_NONE;
}

// The main event loop for the application.

// Based on libcamera_vid.cpp
static void event_loop(LibcameraEncoder &app)
{
	VideoOptions const *options = app.GetOptions();
	std::unique_ptr<Output> output = std::unique_ptr<Output>(Output::Create(options));
	std::cout << "### SetEncodeOutputReadyCallback ###" << std::endl;
	app.SetEncodeOutputReadyCallback(std::bind(&Output::OutputReady, output.get(), _1, _2, _3, _4));

	std::cout << "### OpenCamera" << std::endl;
	app.OpenCamera();
	std::cout << "### ConfigureVideo" << std::endl;
	app.ConfigureVideo(get_colourspace_flags(options->codec));
	std::cout << "### StartEncoder" << std::endl;
	app.StartEncoder();
	std::cout << "### StartCamera" << std::endl;
	app.StartCamera();
	auto start_time = std::chrono::high_resolution_clock::now();

	// Monitoring for keypresses and signals.
	signal(SIGUSR1, default_signal_handler);
	signal(SIGUSR2, default_signal_handler);
	pollfd p[1] = { { STDIN_FILENO, POLLIN, 0 } };
	uint64_t lastTimestamp = 0;

	for (unsigned int count = 0; ; count++)
	{
		// std::cout << "Wait" << std::endl;
		LibcameraEncoder::Msg msg = app.Wait();
		// std::cout << "OK" << std::endl;
		if (msg.type == LibcameraEncoder::MsgType::RequestComplete) {
			// std::cout << "Msg RequestComplete" << std::endl;
		} else if (msg.type == LibcameraEncoder::MsgType::Quit) {
			std::cout << "Msg Quit" << std::endl;
		}
		if (msg.type == LibcameraEncoder::MsgType::Quit)
			return;
		else if (msg.type != LibcameraEncoder::MsgType::RequestComplete)
			throw std::runtime_error("unrecognised message!");

		// Check if we received a signal
		int key = get_key_or_signal(options, p);
		if (key == '\n')
			output->Signal();

		// if (options->verbose)
		// 	std::cerr << "Viewfinder frame " << count << std::endl;
		auto now = std::chrono::high_resolution_clock::now();
		bool timeout = !options->frames && options->timeout &&
					   (now - start_time > std::chrono::milliseconds(options->timeout));
		bool frameout = options->frames && count >= options->frames;
		if (timeout || frameout || key == 'x' || key == 'X')
		{
			if (timeout)
				std::cerr << "Halting: reached timeout of " << options->timeout << " milliseconds.\n";
			app.StopCamera(); // stop complains if encoder very slow to close
			app.StopEncoder();
			return;
		}

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
		int video_width_32 = (options->width+31)&~31;
		int video_height_16 = (options->height+15)&~15;
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

		CompletedRequestPtr &completed_request = std::get<CompletedRequestPtr>(msg.payload);
		std::cout << " sequence=" << completed_request->sequence << " framerate=" << completed_request->framerate << std::endl;
		// std::cout << " buffers:" << std::endl;
		for (const auto& elem : completed_request->buffers)
		{
		// 	auto config = elem.first->configuration();
		// 	std::cout << "  pixelformat=" << config.pixelFormat.toString()
		// 		<< " width=" << config.size.width << " height=" << config.size.height
		// 		<< " frameSize=" << config.frameSize << " bufferCount=" << config.bufferCount
		// 		<< " second=" << elem.second << std::endl;
			auto frameBuffer = elem.second;
		// 	auto planes = frameBuffer->planes();
		// 	for (const libcamera::FrameBuffer::Plane plane : elem.second->planes()) {
		// 		std::cout << "  plane length=" << plane.length << std::endl;
		// 	}
			auto metadata = frameBuffer->metadata();
			// std::cout << " timestamp=" << metadata.timestamp << std::endl;
			if (lastTimestamp != 0) {
				std::cout << " diff=" << ((metadata.timestamp - lastTimestamp) / 1000000.0f) << std::endl;
			}
			lastTimestamp = metadata.timestamp;
		}
		app.EncodeBuffer(completed_request, app.VideoStream());
		app.ShowPreview(completed_request, app.VideoStream());
		// std::cout << "showPreview" << std::endl;
	}
}

int main(int argc, char *argv[])
{
	try
	{
		LibcameraEncoder app;
		VideoOptions *options = app.GetOptions();
		if (options->Parse(argc, argv))
		{
			if (options->verbose)
				options->Print();

			event_loop(app);
		}
	}
	catch (std::exception const &e)
	{
		std::cerr << "ERROR: *** " << e.what() << " ***" << std::endl;
		return -1;
	}
	return 0;
}
