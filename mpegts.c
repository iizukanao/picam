#include <libavutil/avutil.h>

#include "mpegts.h"

static long video_bitrate;
static int video_width;
static int video_height;
static char errbuf[1024];

void mpegts_set_config(long bitrate, int width, int height) {
  video_bitrate = bitrate;
  video_width = width;
  video_height = height;
}

void setup_video_stream(AVFormatContext *format_ctx) {
  AVStream *video_stream;
  AVCodecContext *video_codec_ctx;

  video_stream = avformat_new_stream(format_ctx, 0);
  if (!video_stream) {
    fprintf(stderr, "avformat_new_stream failed\n");
    exit(EXIT_FAILURE);
  }
  video_stream->id = format_ctx->nb_streams - 1;

  video_codec_ctx                = video_stream->codec;
  video_codec_ctx->codec_id      = AV_CODEC_ID_H264;
  video_codec_ctx->codec_type    = AVMEDIA_TYPE_VIDEO;
  video_codec_ctx->codec_tag     = 0;
  video_codec_ctx->bit_rate      = video_bitrate;

  // Main profile is not playable on Android
  video_codec_ctx->profile       = FF_PROFILE_H264_CONSTRAINED_BASELINE;
  video_codec_ctx->level         = 31;  // Level 3.1

  video_stream->time_base.num = 1;
  video_stream->time_base.den = 180000;
  video_codec_ctx->time_base.num = 1;
  video_codec_ctx->time_base.den = 180000;
  video_codec_ctx->ticks_per_frame = 2;
  video_codec_ctx->pix_fmt       = 0;
  video_codec_ctx->width         = video_width;
  video_codec_ctx->height        = video_height;
  video_codec_ctx->has_b_frames  = 0;
  video_codec_ctx->flags         |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

static int is_sample_fmt_supported(AVCodec *codec, enum AVSampleFormat sample_fmt) {
  const enum AVSampleFormat *p = codec->sample_fmts;

  while (*p != AV_SAMPLE_FMT_NONE) {
    if (*p == sample_fmt) {
      return 1;
    }
    p++;
  }

  return 0;
}

void setup_audio_stream(AVFormatContext *format_ctx, MpegTSCodecSettings *settings) {
  AVCodec *aac_codec;
  AVCodecContext *audio_codec_ctx = NULL;
  AVStream *audio_stream;
  int ret;

  aac_codec = avcodec_find_encoder_by_name("libfdk_aac");
  if (!aac_codec) {
    fprintf(stderr, "codec not found\n");
    exit(EXIT_FAILURE);
  }

  audio_stream = avformat_new_stream(format_ctx, aac_codec);
  if (!audio_stream) {
    fprintf(stderr, "avformat_new_stream for audio error\n");
    exit(EXIT_FAILURE);
  }
  audio_stream->id = format_ctx->nb_streams - 1;
  audio_codec_ctx = audio_stream->codec;

  audio_codec_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
  if ( ! is_sample_fmt_supported(aac_codec, audio_codec_ctx->sample_fmt) ) {
    fprintf(stderr, "Sample format %s is not supported",
        av_get_sample_fmt_name(audio_codec_ctx->sample_fmt));
    exit(EXIT_FAILURE);
  }

  audio_stream->time_base.num = 1;
  audio_stream->time_base.den = settings->audio_sample_rate;
  audio_codec_ctx->time_base.num = 1;
  audio_codec_ctx->time_base.den = settings->audio_sample_rate;
  audio_codec_ctx->ticks_per_frame = 1;
  audio_codec_ctx->bit_rate = settings->audio_bit_rate;
  audio_codec_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
  audio_codec_ctx->profile = settings->audio_profile;
  audio_codec_ctx->sample_rate = settings->audio_sample_rate;
  if (settings->audio_channels == 2) {
    audio_codec_ctx->channel_layout = AV_CH_LAYOUT_STEREO;
  } else {
    audio_codec_ctx->channel_layout = AV_CH_LAYOUT_MONO;
  }
  audio_codec_ctx->channels = av_get_channel_layout_nb_channels(audio_codec_ctx->channel_layout);

  ret = avcodec_open2(audio_codec_ctx, aac_codec, NULL);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    fprintf(stderr, "avcodec_open2 failed: %s\n", errbuf);
    exit(EXIT_FAILURE);
  }
}

void mpegts_destroy_context(AVFormatContext *format_ctx) {
  int i;
  for (i = 0; i < format_ctx->nb_streams; i++) {
    avcodec_close(format_ctx->streams[i]->codec);
  }
  avformat_free_context(format_ctx);
}

void mpegts_close_stream(AVFormatContext *format_ctx) {
  av_write_trailer(format_ctx);
  avio_close(format_ctx->pb);
}

void mpegts_close_stream_without_trailer(AVFormatContext *format_ctx) {
  avio_close(format_ctx->pb);
}

void mpegts_open_stream(AVFormatContext *format_ctx, char *outputfilename, int dump_format) {
  int ret;

  if (dump_format) {
    av_dump_format(format_ctx, 0, outputfilename, 1);
  }

  if (strcmp(outputfilename, "-") == 0) {
    outputfilename = "pipe:1";
  }

  ret = avio_open(&format_ctx->pb, outputfilename, AVIO_FLAG_WRITE);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    fprintf(stderr, "avio_open for %s failed: %s\n", outputfilename, errbuf);
    exit(EXIT_FAILURE);
  }

  if (avformat_write_header(format_ctx, NULL)) {
    fprintf(stderr, "avformat_write_header failed\n");
    exit(EXIT_FAILURE);
  }
}

void mpegts_open_stream_without_header(AVFormatContext *format_ctx, char *outputfilename, int dump_format) {
  int ret;

  if (dump_format) {
    av_dump_format(format_ctx, 0, outputfilename, 1);
  }

  if (strcmp(outputfilename, "-") == 0) {
    outputfilename = "pipe:1";
  }

  ret = avio_open(&format_ctx->pb, outputfilename, AVIO_FLAG_WRITE);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    fprintf(stderr, "avio_open for %s failed: %s\n", outputfilename, errbuf);
    exit(EXIT_FAILURE);
  }
}

AVFormatContext *_mpegts_create_context(int use_video, int use_audio, MpegTSCodecSettings *settings) {
  AVFormatContext *format_ctx;
  AVOutputFormat *out_fmt;

  av_register_all();

  out_fmt = av_guess_format("mpegts", NULL, NULL);
  out_fmt->flags |= ~AVFMT_GLOBALHEADER;
  if (!out_fmt) {
    fprintf(stderr, "av_guess_format failed\n");
    exit(EXIT_FAILURE);
  }

  format_ctx = avformat_alloc_context();
  if (!format_ctx) {
    fprintf(stderr, "avformat_alloc_context failed\n");
    exit(EXIT_FAILURE);
  }
  format_ctx->oformat = out_fmt;

#if !(AUDIO_ONLY)
  if (use_video) {
    setup_video_stream(format_ctx);
  }
#endif
  if (use_audio) {
    setup_audio_stream(format_ctx, settings);
  }

  return format_ctx;
}

AVFormatContext *mpegts_create_context(MpegTSCodecSettings *settings) {
  return _mpegts_create_context(1, 1, settings);
}

AVFormatContext *mpegts_create_context_video_only(MpegTSCodecSettings *settings) {
  return _mpegts_create_context(1, 0, settings);
}

AVFormatContext *mpegts_create_context_audio_only(MpegTSCodecSettings *settings) {
  return _mpegts_create_context(0, 1, settings);
}
