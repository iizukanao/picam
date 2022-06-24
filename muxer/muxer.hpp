#pragma once

#include <pthread.h>
#include <thread>
#include "httplivestreaming/httplivestreaming.h"
#include "picam_option/picam_option.hpp"

typedef struct EncodedPacket {
  int64_t pts; // AVPacket.pts (presentation timestamp)
  uint8_t *data; // AVPacket.data (payload)
  int size; // AVPacket.size (size of payload in bytes)
  int stream_index; // AVPacket.stream_index
  int flags; // AVPacket.flags (keyframe, etc.)
} EncodedPacket;

// Recording settings
typedef struct RecSettings {
  char *recording_dest_dir;
  char *recording_basename;
  // Directory to put recorded MPEG-TS files
  char *rec_dir;
  char *rec_tmp_dir;
  char *rec_archive_dir;
} RecSettings;

class Muxer
{
  public:
    Muxer(PicamOption *option);
    ~Muxer();
    void setup(MpegTSCodecSettings *codec_settings);
    int write_encoded_packets(int max_packets, int origin_pts);
    void start_record(RecSettings rec_settings);
    void *rec_start();
    void write_frame(AVPacket *pkt);
    void add_encoded_packet(int64_t pts, uint8_t *data, int size, int stream_index, int flags);
    void prepare_encoded_packets(float video_fps, float audio_fps);
    // void waitForExit();
    void stop_record();
    void onFrameArrive();
    void prepareForDestroy();
    void mark_keyframe_packet();
    int set_record_buffer_keyframes(int newsize);

    // how many keyframes should we look back for the next recording
    int recording_look_back_keyframes;

    int record_buffer_keyframes = 5;

  private:
    PicamOption *option;
    // std::thread recThread;
    pthread_t rec_thread;
    int is_disk_almost_full();
    void free_encoded_packets();
    void check_record_duration();
    void *rec_thread_stop(int skip_cleanup);
    void flush_record();
    HTTPLiveStreaming *hls;
    RecSettings rec_settings;
    EncodedPacket **encoded_packets; // circular buffer that stores encoded audio and video
    int encoded_packets_size; // the number of EncodedPacket that can be stored in encoded_packets
    int current_encoded_packet = -1; // write pointer of encoded_packets array that holds latest encoded audio or video
    int *keyframe_pointers; // circular buffer that stores where keyframe occurs within encoded_packets
    int current_keyframe_pointer = -1; // write pointer of keyframe_pointers array
    int is_keyframe_pointers_filled = 0; // will be changed to 1 once encoded_packets is fully filled
    int rec_thread_frame = 0;
    AVFormatContext *rec_format_ctx;
    time_t rec_start_time;

    char recording_filepath[270];
    char recording_tmp_filepath[259];
    char recording_archive_filepath[1024];
    char recording_basename[256];
    char recording_dest_dir[1024];
    int is_recording = 0;

    MpegTSCodecSettings *codec_settings;
    MpegTSContext mpegts_ctx;

    int rec_thread_needs_exit = 0;
    int rec_thread_needs_flush = 0;
    int rec_thread_needs_write = 0;
    int flush_recording_seconds = 5; // Flush recording data every 5 seconds
};
