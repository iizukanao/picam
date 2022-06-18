#pragma once

#include <variant>
#include <libcamera/base/span.h>
#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/control_ids.h>
#include <libcamera/controls.h>
#include <libcamera/formats.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/property_ids.h>

#include "core/stream_info.hpp"
#include "core/completed_request.hpp"
#include "video_encoder/video_encoder.hpp"
#include "preview/preview.hpp"
#include "picam_option/picam_option.hpp"
#include "core/video_options.hpp"

typedef std::function<void(void *, size_t, int64_t, bool)> EncodeOutputReadyCallback;

class Picam {
public:
    Picam();
    ~Picam();
    int run(int argc, char *argv[]);
    void print_program_version();
    int parseOptions(int argc, char **argv);

    // >>> libcamera_app.hpp
	enum class MsgType
	{
		RequestComplete,
		Quit
	};
	typedef std::variant<CompletedRequestPtr> MsgPayload;
	struct Msg
	{
		Msg(MsgType const &t) : type(t) {}
		template <typename T>
		Msg(MsgType const &t, T p) : type(t), payload(std::forward<T>(p))
		{
		}
		MsgType type;
		MsgPayload payload;
	};

	static constexpr unsigned int FLAG_VIDEO_NONE = 0;
	static constexpr unsigned int FLAG_VIDEO_RAW = 1; // request raw image stream
	static constexpr unsigned int FLAG_VIDEO_JPEG_COLOURSPACE = 2; // force JPEG colour space

	void OpenCamera();
	void CloseCamera();

	void ConfigureVideo(unsigned int flags = FLAG_VIDEO_NONE);

	void Teardown();
	void StartCamera();
	void StopCamera();

	Msg Wait();
	void PostMessage(MsgType &t, MsgPayload &p);

	libcamera::Stream *GetStream(std::string const &name, StreamInfo *info = nullptr) const;
	libcamera::Stream *VideoStream(StreamInfo *info = nullptr) const;

	std::vector<libcamera::Span<uint8_t>> Mmap(libcamera::FrameBuffer *buffer) const;

	void ShowPreview(CompletedRequestPtr &completed_request, libcamera::Stream *stream);

	StreamInfo GetStreamInfo(libcamera::Stream const *stream) const;
    // <<< libcamera_app.hpp

    // >>> libcamera_encoder.hpp
	void StartEncoder()
	{
		createEncoder();
		encoder_->SetInputDoneCallback(std::bind(&Picam::encodeBufferDone, this, std::placeholders::_1));
		encoder_->SetOutputReadyCallback(encode_output_ready_callback_);
	}
	void SetEncodeOutputReadyCallback(EncodeOutputReadyCallback callback) { encode_output_ready_callback_ = callback; }
	void EncodeBuffer(CompletedRequestPtr &completed_request, libcamera::Stream *stream)
	{
		assert(encoder_);
		StreamInfo info = GetStreamInfo(stream);
		libcamera::FrameBuffer *buffer = completed_request->buffers[stream];
		libcamera::Span span = Mmap(buffer)[0];
		void *mem = span.data();
		if (!buffer || !mem)
			throw std::runtime_error("no buffer to encode");
		int64_t timestamp_ns = completed_request->metadata.contains(libcamera::controls::SensorTimestamp)
								? completed_request->metadata.get(libcamera::controls::SensorTimestamp)
								: buffer->metadata().timestamp;
		{
			std::lock_guard<std::mutex> lock(encode_buffer_queue_mutex_);
			encode_buffer_queue_.push(completed_request); // creates a new reference
		}
		encoder_->EncodeBuffer(buffer->planes()[0].fd.get(), span.size(), mem, info, timestamp_ns / 1000);
	}
	VideoOptions *GetOptions() const { return static_cast<VideoOptions *>(options_.get()); }
	void StopEncoder() { printf("StopEncoder\n"); encoder_.reset(); }
    // <<< libcamera_encoder.hpp

protected:
    // >>> libcamera_encoder.hpp
	void createEncoder()
	{
		std::cout << "createEncoder" << std::endl;
		StreamInfo info;
		VideoStream(&info);
		if (!info.width || !info.height || !info.stride) {
			throw std::runtime_error("video steam is not configured");
        }
		encoder_ = std::unique_ptr<VideoEncoder>(new VideoEncoder(this->option, info));
		std::cout << "encoder_ set" << std::endl;
	}
	std::unique_ptr<VideoEncoder> encoder_;
    // <<< libcamera_encoder.hpp

	// >>> libcamera_app.hpp
	std::unique_ptr<Options> options_;
	// <<< libcamera_app.hpp

private:
    int audio_min_value;
    int audio_max_value;
    PicamOption *option;

    void event_loop();
    void setOption(PicamOption *option);
    void setupEncoder();
    void modifyBuffer(CompletedRequestPtr &completed_request);

    // >>> libcamera_app.hpp
	template <typename T>
	class MessageQueue
	{
	public:
		template <typename U>
		void Post(U &&msg)
		{
			std::unique_lock<std::mutex> lock(mutex_);
			queue_.push(std::forward<U>(msg));
			cond_.notify_one();
		}
		T Wait()
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cond_.wait(lock, [this] { return !queue_.empty(); });
			T msg = std::move(queue_.front());
			queue_.pop();
			return msg;
		}
		void Clear()
		{
			std::unique_lock<std::mutex> lock(mutex_);
			queue_ = {};
		}

	private:
		std::queue<T> queue_;
		std::mutex mutex_;
		std::condition_variable cond_;
	};
	struct PreviewItem
	{
		PreviewItem() : stream(nullptr) {}
		PreviewItem(CompletedRequestPtr &b, libcamera::Stream *s) : completed_request(b), stream(s) {}
		PreviewItem &operator=(PreviewItem &&other)
		{
			completed_request = std::move(other.completed_request);
			stream = other.stream;
			other.stream = nullptr;
			return *this;
		}
		CompletedRequestPtr completed_request;
		libcamera::Stream *stream;
	};

	void setupCapture();
	void makeRequests();
	void queueRequest(CompletedRequest *completed_request);
	void requestComplete(libcamera::Request *request);
	void previewDoneCallback(int fd);
	void startPreview();
	void stopPreview();
	void previewThread();
	void configureDenoise(const std::string &denoise_mode);

	std::unique_ptr<libcamera::CameraManager> camera_manager_;
	std::shared_ptr<libcamera::Camera> camera_;
	bool camera_acquired_ = false;
	std::unique_ptr<libcamera::CameraConfiguration> configuration_;
	std::map<libcamera::FrameBuffer *, std::vector<libcamera::Span<uint8_t>>> mapped_buffers_;
	std::map<std::string, libcamera::Stream *> streams_;
	libcamera::FrameBufferAllocator *allocator_ = nullptr;
	std::map<libcamera::Stream *, std::queue<libcamera::FrameBuffer *>> frame_buffers_;
	std::vector<std::unique_ptr<libcamera::Request>> requests_;
	std::mutex completed_requests_mutex_;
	std::set<CompletedRequest *> completed_requests_;
	bool camera_started_ = false;
	std::mutex camera_stop_mutex_;
	MessageQueue<Msg> msg_queue_;
	// Related to the preview window.
	std::unique_ptr<Preview> preview_;
	std::map<int, CompletedRequestPtr> preview_completed_requests_;
	std::mutex preview_mutex_;
	std::mutex preview_item_mutex_;
	PreviewItem preview_item_;
	std::condition_variable preview_cond_var_;
	bool preview_abort_ = false;
	uint32_t preview_frames_displayed_ = 0;
	uint32_t preview_frames_dropped_ = 0;
	std::thread preview_thread_;
	// For setting camera controls.
	std::mutex control_mutex_;
	libcamera::ControlList controls_;
	// Other:
	uint64_t last_timestamp_;
	uint64_t sequence_ = 0;
	// PostProcessor post_processor_;
    // <<< libcamera_app.hpp

    // >>> libcamera_encoder.hpp
	void encodeBufferDone(void *mem)
	{
		// mem is nullptr
		// std::cout << "encodeBufferDone: mem=" << mem << std::endl;

		// If non-NULL, mem would indicate which buffer has been completed, but
		// currently we're just assuming everything is done in order. (We could
		// handle this by replacing the queue with a vector of <mem, completed_request>
		// pairs.)
		assert(mem == nullptr);
		{
			std::lock_guard<std::mutex> lock(encode_buffer_queue_mutex_);
			if (encode_buffer_queue_.empty())
				throw std::runtime_error("no buffer available to return");
			encode_buffer_queue_.pop(); // drop shared_ptr reference
		}
	}

	std::queue<CompletedRequestPtr> encode_buffer_queue_;
	std::mutex encode_buffer_queue_mutex_;
	EncodeOutputReadyCallback encode_output_ready_callback_;
    // <<< libcamera_encoder.hpp
};
