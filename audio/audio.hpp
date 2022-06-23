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
  int read_audio_poll_mmap();
  int wait_for_poll(snd_pcm_t *device, struct pollfd *target_fds, unsigned int audio_fd_count);
  void set_encode_callback(std::function<void(int64_t pts, uint8_t *data, int size, int stream_index, int flags)> callback);
  float get_fps();
  int get_audio_pts_step_base();
  void mute();
  void unmute();

protected:
  PicamOption *option;

private:
  std::function<void(int64_t pts, uint8_t *data, int size, int stream_index, int flags)> encode_callback;
  bool keepRunning = true;
  int is_audio_recording_started = 0;
  int64_t video_current_pts = 0;
  int64_t audio_current_pts = 0;
  int64_t last_pts = 0;
  int audio_pts_step_base;
  int open_audio_capture_device();
  void encode_and_send_audio();
  int64_t get_next_audio_pts();
  void send_audio_frame(uint8_t *databuf, int databuflen, int64_t pts);
  void preconfigure_microphone();
  int get_audio_channels();
  int get_audio_sample_rate();
  int configure_audio_capture_device();
  int is_rtspout_enabled = 0;
  long long audio_frame_count = 0;
  int is_vfr_enabled = 0;
  int current_audio_frames = 0;
  int is_audio_preview_enabled = 0;
  int is_audio_preview_device_opened = 0;
  float audio_volume_multiply = 1.0f;
  int audio_min_value = (int) (-32768 / this->audio_volume_multiply);
  int audio_max_value = (int) (32767 / this->audio_volume_multiply);
  uint16_t *samples;
  void setup_av_frame(AVFormatContext *format_ctx);
  std::ofstream my_fp;
  HTTPLiveStreaming *hls;
  // int period_size;
  bool is_muted = false;
};
