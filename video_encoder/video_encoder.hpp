/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Based on h264_encoder.hpp - Copyright (C) 2020, Raspberry Pi (Trading) Ltd.
 */

#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <functional>

#include "picam_option/picam_option.hpp"
#include "core/stream_info.hpp"

// >>> encoder.hpp
typedef std::function<void(void *)> InputDoneCallback;
typedef std::function<void(void *, size_t, int64_t, bool)> OutputReadyCallback;
// <<< encoder.hpp

class VideoEncoder
{
public:
	VideoEncoder(PicamOption const *options, StreamInfo const &info);
	~VideoEncoder();
	void setGopSize(int gop_size);

	// >>> encoder.hpp
	// This is where the application sets the callback it gets whenever the encoder
	// has finished with an input buffer, so the application can re-use it.
	void SetInputDoneCallback(InputDoneCallback callback) { input_done_callback_ = callback; }
	// This callback is how the application is told that an encoded buffer is
	// available. The application may not hang on to the memory once it returns
	// (but the callback is already running in its own thread).
	void SetOutputReadyCallback(OutputReadyCallback callback) { output_ready_callback_ = callback; }
	// <<< encoder.hpp

	// >>> h264_encoder.hpp
	// Encode the given DMABUF.
	// Encode the given buffer. The buffer is specified both by an fd and size
	// describing a DMABUF, and by a mmapped userland pointer.
	void EncodeBuffer(int fd, size_t size, void *mem, StreamInfo const &info, int64_t timestamp_us);
	// <<< h264_encoder.hpp

protected:
	// >>> encoder.hpp
	InputDoneCallback input_done_callback_;
	OutputReadyCallback output_ready_callback_;
	PicamOption const *options_;
	// <<< encoder.hpp

private:
	// >>> h264_encoder.hpp
	// We want at least as many output buffers as there are in the camera queue
	// (we always want to be able to queue them when they arrive). Make loads
	// of capture buffers, as this is our buffering mechanism in case of delays
	// dealing with the output bitstream.
	static constexpr int NUM_OUTPUT_BUFFERS = 6;
	static constexpr int NUM_CAPTURE_BUFFERS = 12;

	// This thread just sits waiting for the encoder to finish stuff. It will either:
	// * receive "output" buffers (codec inputs), which we must return to the caller
	// * receive encoded buffers, which we pass to the application.
	void pollThread();

	// Handle the output buffers in another thread so as not to block the encoder. The
	// application can take its time, after which we return this buffer to the encoder for
	// re-use.
	void outputThread();

	bool abortPoll_;
	bool abortOutput_;
	int fd_;
	struct BufferDescription
	{
		void *mem;
		size_t size;
	};
	BufferDescription buffers_[NUM_CAPTURE_BUFFERS];
	int num_capture_buffers_;
	std::thread poll_thread_;
	std::mutex input_buffers_available_mutex_;
	std::queue<int> input_buffers_available_;
	struct OutputItem
	{
		void *mem;
		size_t bytes_used;
		size_t length;
		unsigned int index;
		bool keyframe;
		int64_t timestamp_us;
	};
	std::queue<OutputItem> output_queue_;
	std::mutex output_mutex_;
	std::condition_variable output_cond_var_;
	std::thread output_thread_;
	// std::ofstream my_fp;
	// <<< h264_encoder.hpp
};
