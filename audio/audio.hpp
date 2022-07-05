#pragma once

extern "C" {
#include <libavformat/avformat.h>
}
#include <alsa/asoundlib.h>
#include "httplivestreaming/httplivestreaming.h"
#include "core/video_options.hpp"
#include "muxer/muxer.hpp"
#include "picam_option/picam_option.hpp"

class Audio
{
public:
	Audio(PicamOption *option);
  ~Audio();
  void setup(HTTPLiveStreaming *hls);
  void loop();
  void stop();
  void teardown();
  int read_audio_poll_mmap();
  int wait_for_poll(snd_pcm_t *device, struct pollfd *target_fds, unsigned int audio_fd_count);
  void set_encode_callback(std::function<void(int64_t pts, uint8_t *data, int size, int stream_index, int flags)> callback);
  float get_fps();
  void mute();
  void unmute();
  void set_audio_start_time(int64_t audio_start_time);
  void preconfigure();

protected:
  PicamOption *option;

private:
  std::function<void(int64_t pts, uint8_t *data, int size, int stream_index, int flags)> encode_callback;
  bool keepRunning = true;
  int64_t audio_start_time = LLONG_MIN;
  int open_audio_capture_device();
  int open_audio_preview_device();
  void teardown_audio_capture_device();
  void teardown_audio_preview_device();
  void encode_and_send_audio();
  void preconfigure_microphone();
  int microphone_channels = -1;
  int get_audio_channels();
  int get_audio_sample_rate();
  int configure_audio_capture_device();
  int64_t get_next_audio_write_time();
  long long audio_frame_count = 0;
  int is_audio_preview_device_opened = 0;
  uint16_t *samples;
  void setup_av_frame(AVFormatContext *format_ctx);
  HTTPLiveStreaming *hls;
  bool is_muted = false;
};
