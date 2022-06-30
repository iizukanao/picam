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

// ALSA buffer size for capture will be multiplied by this number
#define ALSA_BUFFER_MULTIPLY 50

// static HTTPLiveStreaming *hls;

// sound
static snd_pcm_t *capture_handle;
static snd_pcm_t *audio_preview_handle;
static snd_pcm_hw_params_t *alsa_hw_params;
static AVFrame *av_frame;
static int audio_fd_count;
static struct pollfd *poll_fds; // file descriptors for polling audio
static int is_first_audio;
// static int period_size;
static int audio_buffer_size;
// static int is_audio_preview_enabled;
// static const int is_audio_preview_enabled_default = 0;
// static int is_audio_preview_device_opened = 0;

// static MpegTSCodecSettings codec_settings;

// TODO: Make these values injectable from options
// static unsigned int audio_sample_rate = 48000;
// static int audio_channels = 1;
// static const char *alsa_dev = "hw:2,0";
// static int hls_number_of_segments = 3;
// static long audio_bitrate = 40000;

static char errbuf[1024];

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

  printf("step 0\n");
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

  printf("nb_channels=%d frame_size=%d format=%d\n",
    ctx->ch_layout.nb_channels,
    ctx->frame_size,
    ctx->format
  );
  buffer_size = av_samples_get_buffer_size(NULL, ctx->ch_layout.nb_channels,
      ctx->frame_size, (AVSampleFormat) ctx->format, 0);
  printf("buffer_size: %d\n", buffer_size);

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
  if (!this->option->disable_audio_capturing) {
    this->preconfigure_microphone();
  }
}

void Audio::setup(HTTPLiveStreaming *hls) {
  log_debug("audio setup\n");
  this->hls = hls;

	// codec_settings.audio_sample_rate = audio_sample_rate;
	// codec_settings.audio_bit_rate = audio_bitrate;
	// codec_settings.audio_channels = audio_channels;
	// codec_settings.audio_profile = FF_PROFILE_AAC_LOW;

// #if AUDIO_ONLY
//   hls = hls_create_audio_only(hls_number_of_segments, &codec_settings); // 2 == num_recent_files
// #else
// 	printf("hls_create\n");
//   hls = hls_create(hls_number_of_segments, &codec_settings); // 2 == num_recent_files
// #endif

//   this->setup_av_frame(hls->format_ctx);
  this->setup_av_frame(hls->format_ctx);

  if (this->option->disable_audio_capturing) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    audio_start_time = ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
  } else {
    printf("configuring audio capture device\n");
    int ret = this->configure_audio_capture_device();
    if (ret != 0) {
      log_fatal("error: configure_audio_capture_device: ret=%d\n", ret);
      exit(EXIT_FAILURE);
    }
  }
	printf("audio device configured\n");
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

// int64_t Audio::get_next_audio_pts() {
//   int64_t pts;
//   this->audio_frame_count++;

//   // We use audio timing as the base clock,
//   // so we do not modify PTS here.
//   pts = this->audio_current_pts + this->audio_pts_step_base;

//   this->audio_current_pts = pts;

//   return pts;
// }

// void Audio::send_audio_frame(uint8_t *databuf, int databuflen, int64_t pts) {
//   if (this->is_rtspout_enabled) {
//     int payload_size = databuflen + 7;  // +1(packet type) +6(pts)
//     int total_size = payload_size + 3;  // more 3 bytes for payload length
//     uint8_t *sendbuf = (uint8_t *)malloc(total_size);
//     if (sendbuf == NULL) {
//       log_error("error: cannot allocate memory for audio sendbuf: size=%d", total_size);
//       return;
//     }
//     // payload header
//     sendbuf[0] = (payload_size >> 16) & 0xff;
//     sendbuf[1] = (payload_size >> 8) & 0xff;
//     sendbuf[2] = payload_size & 0xff;
//     // payload
//     sendbuf[3] = 0x03;  // packet type (0x03 == audio data)
//     sendbuf[4] = (pts >> 40) & 0xff;
//     sendbuf[5] = (pts >> 32) & 0xff;
//     sendbuf[6] = (pts >> 24) & 0xff;
//     sendbuf[7] = (pts >> 16) & 0xff;
//     sendbuf[8] = (pts >> 8) & 0xff;
//     sendbuf[9] = pts & 0xff;
//     memcpy(sendbuf + 10, databuf, databuflen);
//     printf("[debug] send is disabled\n");
//     // if (send(sockfd_audio, sendbuf, total_size, 0) == -1) {
//     //   perror("send audio data");
//     // }
//     free(sendbuf);
//   } // if (is_rtspout_enabled)
// }

void Audio::encode_and_send_audio() {
  int ret;
  // int64_t pts;

#if AUDIO_ONLY
  AVCodecContext *ctx = this->hls->format_ctx->streams[0]->codec;
#else
  // AVCodecParameters *ctx = this->hls->format_ctx->streams[1]->codecpar;
#endif

  this->audio_frame_count++;

  AVPacket *pkt = av_packet_alloc();
  if (pkt == NULL) {
    log_error("av_packet_alloc failed: %s\n", errbuf);
    exit(EXIT_FAILURE);
  }
  pkt->data = NULL; // packet data will be allocated by the encoder
  pkt->size = 0;

  // const AVCodec *codec = avcodec_find_encoder_by_name("libfdk_aac");
  // if (!codec) {
  //   fprintf(stderr, "codec not found\n");
  //   exit(EXIT_FAILURE);
  // }

  // AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
  // if (codec_ctx == NULL) {
  //   log_error("avcodec_alloc_context3 failed\n");
  //   exit(EXIT_FAILURE);
  // }
  // avcodec_parameters_to_context(codec_ctx, ctx);
  // ret = avcodec_open2(codec_ctx, codec, NULL);
  // if (ret < 0) {
  //   av_strerror(ret, errbuf, sizeof(errbuf));
  //   log_error("avcodec_open2 failed: %s\n", errbuf);
  //   exit(EXIT_FAILURE);
  // }
  // printf("codec_id=%d buf=%p samples=%p\n", ctx->codec_id, (void *)av_frame->buf, (void *)this->samples);
  // printf("sample_rate=%d bit_rate=%lld nb_channels=%d\n", ctx->sample_rate, ctx->bit_rate, ctx->ch_layout.nb_channels);
  // av_frame->pts = 1000;
  // printf("av_frame->nb_samples=%d ctx->frame_size=%d av_frame->ch=%d linesize=%d pts=%lld frameformat=%d ctxformat=%d\n", av_frame->nb_samples, ctx->frame_size, av_frame->ch_layout.nb_channels, av_frame->linesize[0], av_frame->pts, av_frame->format, ctx->format);

  // uint8_t *audiobuf = (uint8_t *)av_malloc(2048);
  // if (audiobuf == NULL) {
  //   log_error("av_malloc failed\n");
  //   exit(EXIT_FAILURE);
  // }
  // memset(audiobuf, 0, 2048);
  // printf("avcodec_fill_audio_frame\n");
  // ret = avcodec_fill_audio_frame(av_frame, ctx->ch_layout.nb_channels, (AVSampleFormat)ctx->format, audiobuf,
  //                         2048, 0);
  // if (ret < 0) {
  //   av_strerror(ret, errbuf, sizeof(errbuf));
  //   log_error("avcodec_fill_audio_frame failed: %s\n", errbuf);
  //   exit(EXIT_FAILURE);
  // }

  // av_frame_make_writable does not work
//   printf("av_frame_make_writable\n");
// #if LIBAVCODEC_VERSION_MAJOR >= 55
//   ret = av_frame_make_writable(av_frame);
//   if (ret < 0) {
//     av_strerror(ret, errbuf, sizeof(errbuf));
//     log_error("av_frame_make_writable failed: %s\n", errbuf);
//     exit(EXIT_FAILURE);
//   }
// #endif

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
  // avcodec_free_context(&codec_ctx);
  if (ret == 0) {
#if AUDIO_ONLY
    pkt.stream_index = hls->format_ctx->streams[0]->index; // This must be done after avcodec_encode_audio2
#else
    pkt->stream_index = hls->format_ctx->streams[1]->index; // This must be done after avcodec_encode_audio2
#endif // AUDIO_ONLY

    // pts = get_next_audio_pts();

    // send_audio_frame(pkt->data, pkt->size, pts);

#if ENABLE_PTS_WRAP_AROUND
    pts = pts % PTS_MODULO;
#endif
    // last_pts = pts;
    // pkt->pts = pkt->dts = pts;

    if (this->encode_callback) {
      this->encode_callback(
          0, // pts (will not be used)
          pkt->data,
          pkt->size,
          pkt->stream_index,
          pkt->flags);
    }

    // We have to copy AVPacket before av_write_frame()
    // because it changes internal data of the AVPacket.
    // Otherwise av_write_frame() will fail with the error:
    // "AAC bitstream not in ADTS format and extradata missing".

    // if (is_recording) {
    //   pthread_mutex_lock(&rec_mutex);
    //   rec_thread_needs_write = 1;
    //   pthread_cond_signal(&rec_cond);
    //   pthread_mutex_unlock(&rec_mutex);
    // }

    // if (is_tcpout_enabled) {
    //   // Setup AVPacket
    //   AVPacket tcp_pkt;
    //   av_init_packet(&tcp_pkt);
    //   tcp_pkt.size = pkt.size;
    //   tcp_pkt.data = av_malloc(pkt.size);
    //   memcpy(tcp_pkt.data, pkt.data, pkt.size);
    //   tcp_pkt.stream_index = pkt.stream_index;
    //   tcp_pkt.pts = tcp_pkt.dts = pkt.pts;

    //   // Send the AVPacket
    //   pthread_mutex_lock(&tcp_mutex);
    //   av_write_frame(tcp_ctx, &tcp_pkt);
    //   pthread_mutex_unlock(&tcp_mutex);

    //   av_freep(&tcp_pkt.data);
    //   av_free_packet(&tcp_pkt);
    // }

    // if (is_hlsout_enabled) {
    //   pthread_mutex_lock(&mutex_writing);
    //   ret = hls_write_packet(hls, &pkt, 0);
    //   pthread_mutex_unlock(&mutex_writing);
    //   if (ret < 0) {
    //     av_strerror(ret, errbuf, sizeof(errbuf));
    //     log_error("audio frame write error (hls): %s\n", errbuf);
    //     log_error("please check if the disk is full\n");
    //   }
    // }

    av_packet_unref(pkt);

    // this->current_audio_frames++;
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
    // log_debug("snd_pcm_mmap_begin\n");
    if ((error = snd_pcm_mmap_begin (capture_handle, &my_areas, &offset, &frames)) < 0) {
      if ((error = xrun_recovery(capture_handle, error)) < 0) {
        log_fatal("microphone error: mmap begin: %s\n", snd_strerror(error));
        exit(EXIT_FAILURE);
      }
      is_first_audio = 1;
    }
    size_t copy_size = frames * sizeof(short) * this->get_audio_channels();
    // printf("audio memcpy begin\n");
    memcpy(this_samples + read_size / sizeof(short),
      ((uint16_t *)my_areas[0].addr)+(offset*this->get_audio_channels()), // we don't need to multiply offset by sizeof(short) because of (uint16_t *) type
      copy_size);
    // printf("audio memcpy end\n");
    read_size += copy_size;

    // log_debug("snd_pcm_mmap_commit\n");
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

  std::cout << "Audio::loop" << std::endl;

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