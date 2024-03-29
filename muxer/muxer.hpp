#pragma once

#include <pthread.h>
#include <thread>
#include "httplivestreaming/httplivestreaming.h"
#include "picam_option/picam_option.hpp"

#define sizeof_member(type, member) sizeof(((type *)0)->member)

typedef struct EncodedPacket {
  int64_t pts; // AVPacket.pts (presentation timestamp)
  uint8_t *data; // AVPacket.data (payload)
  int size; // AVPacket.size (size of payload in bytes)
  int stream_index; // AVPacket.stream_index
  int flags; // AVPacket.flags (keyframe, etc.)
} EncodedPacket;

// Recording settings
typedef struct RecSettings {
  char recording_dest_dir[1024];
  char recording_basename[256];
  // Directory to put recorded MPEG-TS files
  char rec_dir[256];
  char rec_tmp_dir[256];
  char rec_archive_dir[1024];
} RecSettings;

class Muxer
{
  public:
    Muxer(PicamOption *option);
    ~Muxer();
    void setup(MpegTSCodecSettings *codec_settings, HTTPLiveStreaming *hls);
    int write_encoded_packets(int max_packets, int origin_pts);
    void start_record(RecSettings *rec_settings);
    void *rec_start();
    void write_frame(AVPacket *pkt);
    void add_encoded_packet(int64_t pts, uint8_t *data, int size, int stream_index, int flags);
    void prepare_encoded_packets(float video_fps, float audio_fps);
    void stop_record();
    void onFrameArrive(EncodedPacket *encoded_packet);
    void prepareForDestroy();
    void mark_keyframe_packet();
    int set_record_buffer_keyframes(int newsize);
    void setup_tcp_output();
    void teardown_tcp_output();

    // how many keyframes should we look back for the next recording
    int recording_look_back_keyframes;

    int record_buffer_keyframes = 5;

  private:
    PicamOption *option;
    pthread_t rec_thread;
    int is_disk_almost_full();
    void free_encoded_packets();
    void check_record_duration();
    void *rec_thread_stop(int skip_cleanup);
    void flush_record();
    HTTPLiveStreaming *hls;
    RecSettings *rec_settings;
    EncodedPacket **encoded_packets; // circular buffer that stores encoded audio and video
    int encoded_packets_size; // the number of EncodedPacket that can be stored in encoded_packets
    int current_encoded_packet = -1; // write pointer of encoded_packets array that holds latest encoded audio or video
    int *keyframe_pointers = NULL; // circular buffer that stores where keyframe occurs within encoded_packets
    int current_keyframe_pointer = -1; // write pointer of keyframe_pointers array
    int is_keyframe_pointers_filled = 0; // will be changed to 1 once encoded_packets is fully filled
    int rec_thread_frame = 0;
    AVFormatContext *rec_format_ctx;
    time_t rec_start_time;

    char recording_basename[sizeof_member(RecSettings, recording_basename)] = {};
    char recording_filepath[
      sizeof_member(RecSettings, rec_dir) - 1 +
      sizeof(recording_basename) - 1 +
      10 + // %d
      5 + // "/-.ts"
      1 // '\0'
      ] = {}; // 526
    char recording_tmp_filepath[
      sizeof_member(RecSettings, rec_tmp_dir) - 1 +
      sizeof(recording_basename) - 1 +
      1 + // "/"
      1 // '\0'
      ] = {}; // 512
    char recording_archive_filepath[
      sizeof_member(RecSettings, recording_dest_dir) - 1 +
      sizeof(recording_basename) - 1 +
      1 + // "/"
      1 // '\0'
      ] = {}; // 1280
    int is_recording = 0;

    MpegTSCodecSettings *codec_settings;
    MpegTSContext mpegts_ctx;

    int rec_thread_needs_exit = 0;
    int rec_thread_needs_flush = 0;
    int rec_thread_needs_write = 0;
    int flush_recording_seconds = 5; // Flush recording data every 5 seconds

    int video_send_keyframe_count = 0;
    int64_t video_frame_count = 0;

    // tcp output
    AVFormatContext *tcp_ctx;
    pthread_mutex_t tcp_mutex = PTHREAD_MUTEX_INITIALIZER;

    // hls output
    pthread_mutex_t mutex_writing = PTHREAD_MUTEX_INITIALIZER;
};

extern "C" {
  void encoded_packet_to_avpacket(EncodedPacket *encoded_packet, AVPacket *av_packet);
}
