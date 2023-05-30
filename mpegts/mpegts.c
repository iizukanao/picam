#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>

#include "mpegts.h"

static char errbuf[1024];

AVCodecContext *setup_video_stream(AVFormatContext *format_ctx, MpegTSCodecSettings *settings) {
  AVStream *video_stream;
  AVCodecContext *video_codec_ctx;

  video_stream = avformat_new_stream(format_ctx, 0);
  if (!video_stream) {
    fprintf(stderr, "avformat_new_stream failed\n");
    exit(EXIT_FAILURE);
  }
  video_stream->id = format_ctx->nb_streams - 1;

  const AVCodec *codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
  video_codec_ctx = avcodec_alloc_context3(codec);

  video_codec_ctx->codec_id      = AV_CODEC_ID_H264;
  video_codec_ctx->codec_type    = AVMEDIA_TYPE_VIDEO;
  video_codec_ctx->codec_tag     = 0;
  video_codec_ctx->bit_rate      = settings->video_bitrate;

  video_codec_ctx->profile       = settings->video_profile;
  video_codec_ctx->level         = settings->video_level;

  video_stream->time_base.num = 1;
  video_stream->time_base.den = 180000;
  video_codec_ctx->time_base.num = 1;
  video_codec_ctx->time_base.den = 180000;
  video_codec_ctx->framerate = av_mul_q(video_codec_ctx->time_base, (AVRational){2,1});
  video_codec_ctx->pix_fmt       = 0;
  video_codec_ctx->width         = settings->video_width;
  video_codec_ctx->height        = settings->video_height;
  video_codec_ctx->has_b_frames  = 0;
  video_codec_ctx->flags         |= AV_CODEC_FLAG_GLOBAL_HEADER;

  avcodec_parameters_from_context(video_stream->codecpar, video_codec_ctx);
  return video_codec_ctx;
}

static int is_sample_fmt_supported(const AVCodec *codec, enum AVSampleFormat sample_fmt) {
  const enum AVSampleFormat *p = codec->sample_fmts;

  while (*p != AV_SAMPLE_FMT_NONE) {
    if (*p == sample_fmt) {
      return 1;
    }
    p++;
  }

  return 0;
}

AVCodecContext *setup_audio_stream(AVFormatContext *format_ctx, MpegTSCodecSettings *settings) {
  const AVCodec *aac_codec;
  AVCodecContext *audio_codec_ctx = NULL;
  AVStream *audio_stream;
  int ret;

  aac_codec = avcodec_find_encoder_by_name("libfdk_aac");
  if (!aac_codec) {
    fprintf(stderr, "codec libfdk_aac is not available. Install ffmpeg with libfdk_aac support.\n");
    exit(EXIT_FAILURE);
  }

  audio_stream = avformat_new_stream(format_ctx, aac_codec);
  if (!audio_stream) {
    fprintf(stderr, "avformat_new_stream for audio error\n");
    exit(EXIT_FAILURE);
  }
  audio_stream->id = format_ctx->nb_streams - 1;
  audio_codec_ctx = avcodec_alloc_context3(aac_codec);

  audio_codec_ctx->sample_fmt = AV_SAMPLE_FMT_S16;
  if ( ! is_sample_fmt_supported(aac_codec, audio_codec_ctx->sample_fmt) ) {
    fprintf(stderr, "Sample format %s is not supported\n",
        av_get_sample_fmt_name(audio_codec_ctx->sample_fmt));
    exit(EXIT_FAILURE);
  }

  audio_stream->time_base.num = 1;
  audio_stream->time_base.den = settings->audio_sample_rate;
  audio_codec_ctx->time_base.num = 1;
  audio_codec_ctx->time_base.den = settings->audio_sample_rate;
  audio_codec_ctx->framerate = audio_codec_ctx->time_base;
  audio_codec_ctx->bit_rate = settings->audio_bit_rate;
  audio_codec_ctx->codec_type = AVMEDIA_TYPE_AUDIO;
  audio_codec_ctx->profile = settings->audio_profile;
  audio_codec_ctx->sample_rate = settings->audio_sample_rate;
  if (settings->audio_channels == 2) {
    AVChannelLayout ch_layout = AV_CHANNEL_LAYOUT_STEREO;
    audio_codec_ctx->ch_layout = ch_layout;
  } else {
    AVChannelLayout ch_layout = AV_CHANNEL_LAYOUT_MONO;
    audio_codec_ctx->ch_layout = ch_layout;
  }

  ret = avcodec_open2(audio_codec_ctx, aac_codec, NULL);
  if (ret < 0) {
    av_strerror(ret, errbuf, sizeof(errbuf));
    fprintf(stderr, "avcodec_open2 failed: %s\n", errbuf);
    exit(EXIT_FAILURE);
  }

  // This must be called after avcodec_open2()
  avcodec_parameters_from_context(audio_stream->codecpar, audio_codec_ctx);

  return audio_codec_ctx;
}

void mpegts_destroy_context(AVFormatContext *format_ctx) {
  // unsigned int i;
  // for (i = 0; i < format_ctx->nb_streams; i++) {
  //   avcodec_close(format_ctx->streams[i]->codec);
  // }
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

MpegTSContext _mpegts_create_context(int use_video, int use_audio, MpegTSCodecSettings *settings) {
  AVFormatContext *format_ctx;
  AVOutputFormat *out_fmt;
  const AVOutputFormat *guessed_fmt;
  AVCodecContext *codec_context_video = NULL;
  AVCodecContext *codec_context_audio = NULL;

  guessed_fmt = av_guess_format("mpegts", NULL, NULL);
  if (!guessed_fmt) {
    fprintf(stderr, "av_guess_format failed\n");
    exit(EXIT_FAILURE);
  }
  out_fmt = malloc(sizeof(AVOutputFormat));
  if (out_fmt == NULL) {
    fprintf(stderr, "out_fmt malloc failed\n");
    exit(EXIT_FAILURE);
  }
  memcpy(out_fmt, guessed_fmt, sizeof(AVOutputFormat));

  out_fmt->flags |= ~AVFMT_GLOBALHEADER;

  format_ctx = avformat_alloc_context();
  if (!format_ctx) {
    fprintf(stderr, "avformat_alloc_context failed\n");
    exit(EXIT_FAILURE);
  }
  format_ctx->oformat = out_fmt;

#if !(AUDIO_ONLY)
  if (use_video) {
    codec_context_video = setup_video_stream(format_ctx, settings);
  }
#endif
  if (use_audio) {
    codec_context_audio = setup_audio_stream(format_ctx, settings);
  }

  return (MpegTSContext){
    format_ctx,
    codec_context_video,
    codec_context_audio,
  };
}

MpegTSContext mpegts_create_context(MpegTSCodecSettings *settings) {
  return _mpegts_create_context(1, 1, settings);
}

MpegTSContext mpegts_create_context_video_only(MpegTSCodecSettings *settings) {
  return _mpegts_create_context(1, 0, settings);
}

MpegTSContext mpegts_create_context_audio_only(MpegTSCodecSettings *settings) {
  return _mpegts_create_context(0, 1, settings);
}
