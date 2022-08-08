extern "C" {
#include <libavcodec/avcodec.h>
}

#include <alsa/asoundlib.h>
#include "log/log.h"
#include "httplivestreaming/httplivestreaming.h"
#include "audio/audio.hpp"

// Internal flag indicates that audio is available for read
#define AVAIL_AUDIO 2

// ALSA buffer size for playback will be multiplied by this number (max: 16)
#define ALSA_PLAYBACK_BUFFER_MULTIPLY 10

// ALSA buffer size for capture will be multiplied by this number
#define ALSA_BUFFER_MULTIPLY 50

// sound
static snd_pcm_t *capture_handle;
static snd_pcm_t *audio_preview_handle;
static snd_pcm_hw_params_t *alsa_hw_params;
static AVFrame *av_frame;
static int audio_fd_count;
static struct pollfd *poll_fds; // file descriptors for polling audio
static int is_first_audio;
static int audio_buffer_size;

static char errbuf[1024];

Audio::Audio(PicamOption *option)
	: option(option)
{
}

Audio::~Audio()
{
}

void Audio::teardown()
{
  if (!this->option->disable_audio_capturing) {
    log_debug("teardown_audio_capture_device\n");
    this->teardown_audio_capture_device();
    if (this->is_audio_preview_device_opened) {
      log_debug("teardown_audio_preview_device\n");
      this->teardown_audio_preview_device();
    }
  }
}

void Audio::teardown_audio_capture_device() {
  snd_pcm_close (capture_handle);

  free(poll_fds);
}

void Audio::teardown_audio_preview_device() {
  snd_pcm_close(audio_preview_handle);
}

int64_t Audio::get_next_audio_write_time() {
  if (this->audio_frame_count == 0) {
    return this->audio_start_time;
  }
  return this->audio_start_time + this->audio_frame_count * 1000000000.0f / ((float)this->option->audio_sample_rate / (float)this->option->audio_period_size);
}

int Audio::open_audio_capture_device() {
  int err;

  log_debug("opening ALSA device for capture: %s\n", this->option->alsa_dev);
  err = snd_pcm_open(&capture_handle, this->option->alsa_dev, SND_PCM_STREAM_CAPTURE, 0);
  if (err < 0) {
    log_error("error: cannot open audio capture device '%s': %s\n",
        this->option->alsa_dev, snd_strerror(err));
    log_error("hint: specify correct ALSA device with '--alsadev <dev>'\n");
    return -1;
  }

  return 0;
}

int Audio::open_audio_preview_device() {
  int err;
  snd_pcm_hw_params_t *audio_preview_params;

  log_debug("opening ALSA device for playback (preview): %s\n", this->option->audio_preview_dev);
  err = snd_pcm_open(&audio_preview_handle, this->option->audio_preview_dev, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
  if (err < 0) {
    log_error("error: cannot open audio playback (preview) device '%s': %s\n",
        this->option->audio_preview_dev, snd_strerror(err));
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

  this->option->audio_preview_channels = this->get_audio_channels();
  err = snd_pcm_hw_params_set_channels(audio_preview_handle, audio_preview_params, this->option->audio_preview_channels);
  if (err < 0) {
    log_fatal("error: cannot set channel count for audio preview: %s\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // set the sample rate
  unsigned int rate = this->get_audio_sample_rate();
  err = snd_pcm_hw_params_set_rate_near(audio_preview_handle, audio_preview_params, &rate, 0);
  if (err < 0) {
    log_fatal("error: cannot set sample rate for audio preview: %s\n",
        snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // set the buffer size
  log_debug("setting audio preview buffer size to %d (audio_buffer_size=%d ALSA_PLAYBACK_BUFFER_MULTIPLY=%d)\n", audio_buffer_size * ALSA_PLAYBACK_BUFFER_MULTIPLY,
    audio_buffer_size, ALSA_PLAYBACK_BUFFER_MULTIPLY);
  err = snd_pcm_hw_params_set_buffer_size(audio_preview_handle, audio_preview_params,
      audio_buffer_size * ALSA_PLAYBACK_BUFFER_MULTIPLY);
  if (err < 0) {
    log_fatal("error: failed to set buffer size for audio preview: audio_buffer_size=%d error=%s\n",
        audio_buffer_size, snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  int period_size = this->option->audio_period_size;
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

void Audio::set_encode_callback(std::function<void(int64_t pts, uint8_t *data, int size, int stream_index, int flags)> callback = nullptr) {
  this->encode_callback = callback;
}

int Audio::get_audio_sample_rate() {
  return this->hls->audio_ctx->sample_rate;
}

inline int Audio::get_audio_channels() {
  return this->hls->audio_ctx->ch_layout.nb_channels;
}

// int Audio::get_audio_pts_step_base() {
//   return this->audio_pts_step_base;
// }

// Configure the microphone before main setup
void Audio::preconfigure_microphone() {
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
  err = snd_pcm_hw_params_set_channels(capture_handle, alsa_hw_params, this->option->audio_channels);
  if (err < 0) {
    if (microphone_channels == 1) {
      if (this->option->is_audio_channels_specified) {
        log_info("cannot use mono audio; trying stereo\n");
      } else {
        log_debug("cannot use mono audio; trying stereo\n");
      }
      microphone_channels = 2;
    } else {
      if (this->option->is_audio_channels_specified) {
        log_info("cannot use stereo audio; trying mono\n");
      } else {
        log_debug("cannot use stereo audio; trying mono\n");
      }
      microphone_channels = 1;
    }
    err = snd_pcm_hw_params_set_channels(capture_handle, alsa_hw_params, microphone_channels);
    if (err < 0) {
      log_fatal("error: cannot set channel count for microphone (%s)\n", snd_strerror(err));
      exit(EXIT_FAILURE);
    }
  }
  log_debug("final microphone channels: %d\n", microphone_channels);
  this->option->audio_channels = microphone_channels;
}

void Audio::setup_av_frame(AVFormatContext *format_ctx) {
  AVCodecParameters *audio_codec_ctx;
  int ret;
  int buffer_size;

#if AUDIO_ONLY
  audio_codec_ctx = format_ctx->streams[0]->codec;
#else
  audio_codec_ctx = format_ctx->streams[1]->codecpar;
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
  av_frame->format = audio_codec_ctx->format;
  log_debug("format: %d\n", audio_codec_ctx->format);
  av_frame->ch_layout = audio_codec_ctx->ch_layout;
  log_debug("audio_codec_ctx->ch_layout: %" PRIu64 "\n", audio_codec_ctx->ch_layout);
  log_debug("av_frame->channel_layout: %" PRIu64 "\n", av_frame->ch_layout);
//   av_frame->channels = audio_codec_ctx->channels;
//   log_debug("audio_codec_ctx->channels: %d\n", audio_codec_ctx->channels);
//   log_debug("av_frame->channels: %d\n", av_frame->channels);

  buffer_size = av_samples_get_buffer_size(NULL, audio_codec_ctx->ch_layout.nb_channels,
      audio_codec_ctx->frame_size, (AVSampleFormat) audio_codec_ctx->format, 0);
  log_debug("buffer_size=%d\n", buffer_size);
  this->samples = (uint16_t *)av_malloc(buffer_size);
  if (!this->samples) {
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

  this->option->audio_period_size = buffer_size / this->get_audio_channels() / sizeof(short);
  this->option->audio_pts_step = 90000.0f * this->option->audio_period_size / this->get_audio_sample_rate();
  log_debug("audio_pts_step: %d\n", this->option->audio_pts_step);

  if (this->option->disable_audio_capturing) {
    memset(this->samples, 0, this->option->audio_period_size * sizeof(short) * this->option->audio_channels);
  }

  ret = avcodec_fill_audio_frame(av_frame, audio_codec_ctx->ch_layout.nb_channels, (AVSampleFormat) audio_codec_ctx->format,
      (const uint8_t*)this->samples, buffer_size, 0);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    log_error("error: avcodec_fill_audio_frame failed: %s\n", errbuf);
    exit(EXIT_FAILURE);
  }
}

int Audio::configure_audio_capture_device() {
  // ALSA
  int err;

  // libavcodec
#if AUDIO_ONLY
  AVCodecContext *ctx = this->hls->format_ctx->streams[0]->codec;
#else
  AVCodecParameters *ctx = this->hls->format_ctx->streams[1]->codecpar;
#endif
  int buffer_size;

  // ALSA poll mmap
  snd_pcm_uframes_t real_buffer_size; // real buffer size in frames
  int dir;

  buffer_size = av_samples_get_buffer_size(NULL, ctx->ch_layout.nb_channels,
      ctx->frame_size, (AVSampleFormat) ctx->format, 0);

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

  int audio_sample_rate = this->get_audio_sample_rate();
  log_debug("audio_sample_rate: %d (%u)\n", audio_sample_rate, (unsigned int)audio_sample_rate);
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
  if (actual_rate != (unsigned int)audio_sample_rate) {
    log_fatal("error: failed to set sample rate for microphone to %u (got %d): %s\n",
        audio_sample_rate, actual_rate, snd_strerror(err));
    exit(EXIT_FAILURE);
  }

  // set the buffer size
  int alsa_buffer_multiply = ALSA_BUFFER_MULTIPLY;
  log_debug("setting microphone buffer size to %d (buffer_size=%d alsa_buffer_multiply=%d)\n", buffer_size * alsa_buffer_multiply, buffer_size, alsa_buffer_multiply);
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
  log_debug("microphone: buffer size: %d frames (channels=%d buffer_size=%d multiply=%d)\n", (int)real_buffer_size, this->get_audio_channels(), buffer_size, alsa_buffer_multiply);

  audio_buffer_size = buffer_size;

  log_debug("microphone: setting period size to %d\n", this->option->audio_period_size);
  dir = 0;
  // set the period size
  err = snd_pcm_hw_params_set_period_size_near(capture_handle, alsa_hw_params,
      (snd_pcm_uframes_t *)&this->option->audio_period_size, &dir);
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
  poll_fds = (pollfd *) malloc(sizeof(struct pollfd) * audio_fd_count);
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

void Audio::preconfigure() {
  if (!this->option->disable_audio_capturing) {
    int ret = this->open_audio_capture_device();
    if (ret == -1) {
      log_warn("warning: audio capturing is disabled\n");
      this->option->disable_audio_capturing = 1;
    } else if (ret < 0) {
      log_fatal("error: init_audio failed: %d\n", ret);
      exit(EXIT_FAILURE);
    }
  }
  log_debug("disable_audio_capturing: %d\n", this->option->disable_audio_capturing);
  if (!this->option->disable_audio_capturing) {
    this->preconfigure_microphone();
  }
}

void Audio::setup(HTTPLiveStreaming *hls) {
  log_debug("audio setup\n");
  this->hls = hls;

  this->setup_av_frame(hls->format_ctx);

  if (this->option->disable_audio_capturing) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    audio_start_time = ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
  } else {
    log_debug("configuring audio capture device\n");
    int ret = this->configure_audio_capture_device();
    if (ret != 0) {
      log_fatal("error: configure_audio_capture_device: ret=%d\n", ret);
      exit(EXIT_FAILURE);
    }
  }
	log_debug("audio device configured\n");
}

void Audio::stop() {
  this->keepRunning = false;
}

// Callback function that is called when an error has occurred
static int xrun_recovery(snd_pcm_t *handle, int error) {
  switch(error) {
    case -EPIPE: // Buffer underrun
      log_error("microphone error: buffer underrun (data rate from microphone is too slow)\n");
      if ((error = snd_pcm_prepare(handle)) < 0) {
        log_error("microphone error: unable to recover from underrun, "
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
          log_error("microphone error: unable to recover from suspend, "
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
int Audio::wait_for_poll(snd_pcm_t *device, struct pollfd *target_fds, unsigned int audio_fd_count) {
  unsigned short revents;
  int avail_flags = 0;
  int ret;

  while (1) {
    ret = poll(target_fds, audio_fd_count, -1); // -1 means block
    if (ret < 0) {
      if (this->keepRunning) {
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

void Audio::encode_and_send_audio() {
  int ret;

  this->audio_frame_count++;

  AVPacket *pkt = av_packet_alloc();
  if (pkt == NULL) {
    log_error("av_packet_alloc failed: %s\n", errbuf);
    exit(EXIT_FAILURE);
  }
  pkt->data = NULL; // packet data will be allocated by the encoder
  pkt->size = 0;

  // encode the samples
  ret = avcodec_send_frame(this->hls->audio_ctx, av_frame);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    log_error("avcodec_send_frame failed: %s (%d)\n", errbuf, ret);
    exit(EXIT_FAILURE);
  }
  ret = avcodec_receive_packet(this->hls->audio_ctx, pkt);
  if (ret != EAGAIN && ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    log_error("avcodec_receive_packet failed: %s\n", errbuf);
    exit(EXIT_FAILURE);
  }
  if (ret == 0) {
    pkt->stream_index = hls->format_ctx->streams[1]->index; // This must be done after encoding an audio

#if ENABLE_PTS_WRAP_AROUND
    pts = pts % PTS_MODULO;
#endif

    if (this->encode_callback) {
      this->encode_callback(
          0, // pts (will not be used)
          pkt->data,
          pkt->size,
          pkt->stream_index,
          pkt->flags);
    }

    av_packet_unref(pkt);
  } else {
    log_error("error: not getting audio output");
  }
}

float Audio::get_fps() {
  return this->get_audio_sample_rate() / this->get_audio_channels() / this->option->audio_period_size;
}

int Audio::read_audio_poll_mmap() {
  const snd_pcm_channel_area_t *my_areas; // mapped memory area info
  snd_pcm_sframes_t avail, commitres; // aux for frames count
  snd_pcm_uframes_t offset, frames, size; //aux for frames count
  int error;
  uint16_t *this_samples;

#if AUDIO_BUFFER_CHUNKS > 0
  this_samples = audio_buffer[audio_buffer_index];
#else
  this_samples = this->samples;
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
  if (avail < this->option->audio_period_size) { // check if one period is ready to process
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
  size = this->option->audio_period_size;
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
    size_t copy_size = frames * sizeof(short) * this->get_audio_channels();
    memcpy(this_samples + read_size / sizeof(short),
      ((uint16_t *)my_areas[0].addr)+(offset*this->get_audio_channels()), // we don't need to multiply offset by sizeof(short) because of (uint16_t *) type
      copy_size);
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

  if (this->option->is_audio_preview_enabled) {
    uint16_t *ptr;
    int cptr;
    int err;

    if (!this->is_audio_preview_device_opened) {
      this->open_audio_preview_device();
      is_audio_preview_device_opened = 1;
    }

    ptr = this_samples;
    cptr = this->option->audio_period_size;
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
      ptr += err * this->option->audio_preview_channels;
      cptr -= err;
    }
  }

  if (this->option->audio_volume_multiply != 1.0f) {
    int total_samples = this->option->audio_period_size * this->get_audio_channels();
    int i;
    for (i = 0; i < total_samples; i++) {
      int16_t value = (int16_t)this_samples[i];
      if (value < this->option->audio_min_value) { // negative overflow of audio volume
        log_info("o-");
        value = -32768;
      } else if (value > this->option->audio_max_value) { // positive overflow audio volume
        log_info("o+");
        value = 32767;
      } else {
        value = (int16_t)(value * this->option->audio_volume_multiply);
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

void Audio::loop() {
  int avail_flags;

  while (this->keepRunning) {
    if (this->option->disable_audio_capturing) {
      this->encode_and_send_audio();
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      int64_t diff_time = this->get_next_audio_write_time() - (ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec);
      if (diff_time > 0) {
        ts.tv_sec = diff_time / 1000000000;
        diff_time -= ts.tv_sec * 1000000000;
        ts.tv_nsec = diff_time;
        int ret = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
        if (ret != 0) {
          log_error("nanosleep error:%d\n", ret);
        }
      }
      continue;
    }

    if (is_first_audio) {
      this->read_audio_poll_mmap();
      // We ignore the first audio frame.
      // Because there is always a big delay after
      // the first and the second frame.
    }

    avail_flags = this->wait_for_poll(capture_handle, poll_fds, audio_fd_count);
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
      this->read_audio_poll_mmap();
#if AUDIO_BUFFER_CHUNKS > 0
      if (AUDIO_BUFFER_CHUNKS == 0 || is_audio_buffer_filled) {
        memcpy(this->samples, audio_buffer[audio_buffer_index], this->option->audio_period_size * sizeof(short) * audio_channels);
#else
      if (1) {
#endif
        this->encode_and_send_audio();
      } // end of if(1)
    } // end of if (avail_flags & AVAIL_AUDIO)
  } // end of while loop (keepRunning)
}

void Audio::mute() {
  this->is_muted = true;
}

void Audio::unmute() {
  this->is_muted = false;
}

void Audio::set_audio_start_time(int64_t audio_start_time)
{
  this->audio_start_time = audio_start_time;
  this->audio_frame_count = 0;
}
