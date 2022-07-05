#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "muxer/muxer.hpp"
#include "mpegts/mpegts.h"
#include "libstate/state.h"
#include "rtsp/rtsp.h"
#include "log/log.h"

// Number of packets to chase recording for each cycle
#define REC_CHASE_PACKETS 10

static pthread_mutex_t rec_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rec_write_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t rec_cond = PTHREAD_COND_INITIALIZER;
// static pthread_mutex_t encoded_packet_mutex = PTHREAD_MUTEX_INITIALIZER;

static char errbuf[1024];

Muxer::Muxer(PicamOption *option) {
  this->option = option;
}

Muxer::~Muxer() {
  this->free_encoded_packets();
  if (this->keyframe_pointers != NULL) {
    free(this->keyframe_pointers);
    this->keyframe_pointers = NULL;
  }
}

void Muxer::setup(MpegTSCodecSettings *codec_settings, HTTPLiveStreaming *hls) {
  this->codec_settings = codec_settings;
  this->hls = hls;

	// MpegTSContext rec_ctx = mpegts_create_context(codec_settings);
	// char recording_tmp_filepath[256];
  //   time_t rawtime;
	// struct tm *timeinfo;
	// time(&rawtime);
	// timeinfo = localtime(&rawtime);
	// char recording_basename[256];
	// char *rec_tmp_dir = "rec/tmp";
  // strftime(recording_basename, sizeof(recording_basename), "%Y-%m-%d_%H-%M-%S", timeinfo);
	// snprintf(recording_tmp_filepath, sizeof(recording_tmp_filepath),
	// 	"%s/%s", rec_tmp_dir, recording_basename);
	// mpegts_open_stream(rec_ctx.format_context, recording_tmp_filepath, 0);
}

// void Muxer::waitForExit() {
//   if (this->recThread.joinable()) {
//     printf("joining recThread\n");
//     this->recThread.join();
//     printf("joined recThread\n");
//   }
// }

void *Muxer::rec_thread_stop(int skip_cleanup) {
  FILE *fsrc, *fdest;
  int read_len;
  uint8_t *copy_buf;

  log_info("stop rec\n");
  if (!skip_cleanup) {
    copy_buf = (uint8_t *)malloc(BUFSIZ);
    if (copy_buf == NULL) {
      perror("malloc for copy_buf");
      pthread_exit(0);
    }

    pthread_mutex_lock(&rec_write_mutex);
    mpegts_close_stream(rec_format_ctx);
    mpegts_destroy_context(rec_format_ctx);
    pthread_mutex_unlock(&rec_write_mutex);

    log_debug("copy ");
    fsrc = fopen(recording_tmp_filepath, "r");
    if (fsrc == NULL) {
      log_error("error: failed to open %s: %s\n",
          recording_tmp_filepath, strerror(errno));
    }
    fdest = fopen(recording_archive_filepath, "a");
    if (fdest == NULL) {
      log_error("error: failed to open %s: %s\n",
          recording_archive_filepath, strerror(errno));
      fclose(fsrc);
    }
    while (1) {
      read_len = fread(copy_buf, 1, BUFSIZ, fsrc);
      if (read_len > 0) {
        fwrite(copy_buf, 1, read_len, fdest);
      }
      if (read_len != BUFSIZ) {
        break;
      }
    }
    if (feof(fsrc)) {
      fclose(fsrc);
      fclose(fdest);
    } else {
      log_error("error: rec_thread_stop: not an EOF?: %s\n", strerror(errno));
    }

    // Create a symlink
    char symlink_dest_path[2048];
    size_t rec_dir_len = strlen(this->rec_settings.rec_dir);
    struct stat file_stat;

    // If recording_archive_filepath starts with "rec/", then remove it
    if (strncmp(recording_archive_filepath, this->rec_settings.rec_dir, rec_dir_len) == 0 &&
        recording_archive_filepath[rec_dir_len] == '/') {
      snprintf(symlink_dest_path, sizeof(symlink_dest_path),
          recording_archive_filepath + rec_dir_len + 1);
    } else if (recording_archive_filepath[0] == '/') { // absolute path
      snprintf(symlink_dest_path, sizeof(symlink_dest_path),
          recording_archive_filepath);
    } else { // relative path
      char cwd[1024];
      if (getcwd(cwd, sizeof(cwd)) == NULL) {
        log_error("error: failed to get current working directory: %s\n",
            strerror(errno));
        cwd[0] = '.';
        cwd[1] = '.';
        cwd[2] = '\0';
      }
      snprintf(symlink_dest_path, sizeof(symlink_dest_path),
          "%s/%s", cwd, recording_archive_filepath);
    }

    log_debug("symlink(%s, %s)\n", symlink_dest_path, recording_filepath);
    if (lstat(recording_filepath, &file_stat) == 0) { // file (symlink) exists
      log_info("replacing existing symlink: %s\n", recording_filepath);
      unlink(recording_filepath);
    }
    if (symlink(symlink_dest_path, recording_filepath) != 0) {
      log_error("error: cannot create symlink from %s to %s: %s\n",
          symlink_dest_path, recording_filepath, strerror(errno));
    }

    // unlink tmp file
    log_debug("unlink");
    unlink(recording_tmp_filepath);

    state_set(NULL, "last_rec", recording_filepath);

    free(copy_buf);
  }

  is_recording = 0;
  state_set(NULL, "record", "false");

  pthread_exit(0);
}

void Muxer::flush_record() {
  this->rec_thread_needs_flush = 1;
}

void Muxer::stop_record() {
  this->rec_thread_needs_exit = 1;
}

void Muxer::prepareForDestroy() {
  printf("prepareForDestroy: is_recording=%d\n", this->is_recording);
  if (this->is_recording) {
    this->rec_thread_needs_write = 1;
    pthread_cond_signal(&rec_cond);

    this->stop_record();
    pthread_join(this->rec_thread, NULL);
  }
}

void Muxer::check_record_duration() {
  time_t now;

  if (this->is_recording) {
    now = time(NULL);
    if (now - this->rec_start_time > this->flush_recording_seconds) {
      this->flush_record();
    }
  }
}

void Muxer::prepare_encoded_packets(float video_fps, float audio_fps) {
  encoded_packets_size = (video_fps + 1) * record_buffer_keyframes * 2 +
    (audio_fps + 1) * record_buffer_keyframes * 2 + 100;

  int malloc_size = sizeof(EncodedPacket *) * encoded_packets_size;
  encoded_packets = (EncodedPacket **)malloc(malloc_size);
  if (encoded_packets == NULL) {
    log_error("error: cannot allocate memory for encoded_packets\n");
    exit(EXIT_FAILURE);
  }
  memset(encoded_packets, 0, malloc_size);

  this->keyframe_pointers = (int *)calloc(sizeof(int) * this->record_buffer_keyframes, 1);
  if (this->keyframe_pointers == NULL) {
    log_fatal("error: cannot allocate memory for keyframe_pointers\n");
    exit(EXIT_FAILURE);
  }
}

// Check if disk usage is >= 95%
int Muxer::is_disk_almost_full() {
  struct statvfs stat;
  statvfs("/", &stat);
  int used_percent = ceil( (stat.f_blocks - stat.f_bfree) * 100.0f / stat.f_blocks);
  log_info("disk_usage=%d%% ", used_percent);
  if (used_percent >= 95) {
    return 1;
  } else {
    return 0;
  }
}

// void start_rec_thread(Muxer *muxer, RecSettings rec_settings) {
//   muxer->rec_start();
// }

void *rec_thread_start(void *self) {
  Muxer *muxer = reinterpret_cast<Muxer *>(self);
  return muxer->rec_start();
}

void Muxer::start_record(RecSettings settings) {
  if (this->is_recording) {
    log_warn("recording is already started\n");
    return;
  }

  if (this->is_disk_almost_full()) {
    log_error("error: disk is almost full, recording not started\n");
    return;
  }

  this->rec_thread_needs_exit = 0;
  this->rec_settings = settings;
  pthread_create(&this->rec_thread, NULL, rec_thread_start, this);
  // if (this->recThread.joinable()) {
  //   log_debug("recThread is joinable\n");
  //   this->recThread.join();
  // }
  // log_debug("creating recThread\n");
	// this->recThread = std::thread(start_rec_thread, this, settings);
}

void Muxer::setup_tcp_output()
{
  // avformat_network_init();
  MpegTSContext ts_ctx = mpegts_create_context(this->codec_settings);
  tcp_ctx = ts_ctx.format_context;
  mpegts_open_stream(tcp_ctx, this->option->tcp_output_dest, 0);
}

void Muxer::teardown_tcp_output() {
  log_debug("teardown_tcp_output\n");
  mpegts_close_stream(tcp_ctx);
  mpegts_destroy_context(tcp_ctx);
  // avformat_network_deinit();
}

// Receives both video and audio frames.
void Muxer::onFrameArrive(EncodedPacket *encoded_packet) {
  bool isVideo = encoded_packet->stream_index == 0;
  bool isVideoKeyframe = isVideo &&
    encoded_packet->flags & AV_PKT_FLAG_KEY; // keyframe

  if (this->is_recording) {
    pthread_mutex_lock(&rec_mutex);
    this->rec_thread_needs_write = 1;
    pthread_cond_signal(&rec_cond);
    pthread_mutex_unlock(&rec_mutex);
  }

  // av_write_frame() may change the internal data of AVPacket.
  // av_write_frame() may fail with the following error if
  // internal data is in invalid state.
  // "AAC bitstream not in ADTS format and extradata missing".

  AVPacket *pkt = av_packet_alloc();
  encoded_packet_to_avpacket(encoded_packet, pkt);

  if (this->option->is_tcpout_enabled) {
    pthread_mutex_lock(&tcp_mutex);
    av_write_frame(tcp_ctx, pkt);
    pthread_mutex_unlock(&tcp_mutex);
  }

  if (this->option->is_rtspout_enabled) {
    if (isVideo) {
      rtsp_send_video_frame(encoded_packet->data, encoded_packet->size, encoded_packet->pts);
    } else { // audio
      rtsp_send_audio_frame(encoded_packet->data, encoded_packet->size, encoded_packet->pts);
    }
  }

  if (this->option->is_hlsout_enabled) {
    pthread_mutex_lock(&mutex_writing);

    int split; // Whether we should split .ts file here
    if (isVideoKeyframe) {
      if (video_send_keyframe_count % this->option->hls_keyframes_per_segment == 0 && video_frame_count != 1) {
        split = 1;
      } else {
        split = 0;
      }

      video_send_keyframe_count = video_send_keyframe_count % this->option->hls_keyframes_per_segment;

      // Update counter
      video_send_keyframe_count++;
    } else {
      split = 0;
    }

    int ret = hls_write_packet(hls, pkt, split);
    pthread_mutex_unlock(&mutex_writing);
    if (ret < 0) {
      av_strerror(ret, errbuf, sizeof(errbuf));
      log_error("keyframe write error (hls): %s\n", errbuf);
      log_error("please check if the disk is full\n");
    }
  }

  av_packet_free(&pkt);
}

// Called from thread
void *Muxer::rec_start() {
  time_t rawtime;
  struct tm *timeinfo;
  AVPacket *av_pkt;
  int wrote_packets;
  int is_caught_up = 0;
  int unique_number = 1;
  int64_t rec_start_pts, rec_end_pts;
  char state_buf[256];
  EncodedPacket *enc_pkt;
  int filename_decided = 0;
  uint8_t *copy_buf;
  FILE *fsrc, *fdest;
  int read_len;
  const char *dest_dir;
  int has_error;

  this->rec_thread_needs_exit = 0;
  has_error = 0;
  copy_buf = (uint8_t *)malloc(BUFSIZ);
  if (copy_buf == NULL) {
    perror("malloc for copy_buf");
    pthread_exit(0);
  }

  time(&rawtime);
  timeinfo = localtime(&rawtime);

  this->rec_start_time = time(NULL);
  rec_start_pts = -1;

  if (this->rec_settings.recording_dest_dir[0] != 0) {
    dest_dir = this->rec_settings.recording_dest_dir;
  } else {
    dest_dir = this->rec_settings.rec_archive_dir;
  }

  if (this->rec_settings.recording_basename[0] != 0) { // basename is already decided
    strncpy(this->recording_basename, this->rec_settings.recording_basename, sizeof(this->recording_basename)-1);
    this->recording_basename[sizeof(this->recording_basename)-1] = '\0';

    snprintf(this->recording_filepath, sizeof(this->recording_filepath),
        "%s/%s", this->rec_settings.rec_dir, this->recording_basename);
    snprintf(recording_archive_filepath, sizeof(recording_archive_filepath),
        "%s/%s", dest_dir, this->recording_basename);
    snprintf(recording_tmp_filepath, sizeof(recording_tmp_filepath),
        "%s/%s", this->rec_settings.rec_tmp_dir, this->recording_basename);
    filename_decided = 1;
  } else {
    strftime(this->recording_basename, sizeof(this->recording_basename), "%Y-%m-%d_%H-%M-%S", timeinfo);
    snprintf(recording_filepath, sizeof(recording_filepath),
        "%s/%s.ts", this->rec_settings.rec_dir, this->recording_basename);
    if (access(recording_filepath, F_OK) != 0) { // filename is decided
      sprintf(this->recording_basename + strlen(recording_basename), ".ts"); // add ".ts"
      snprintf(recording_archive_filepath, sizeof(recording_archive_filepath),
          "%s/%s", dest_dir, this->recording_basename);
      snprintf(recording_tmp_filepath, sizeof(recording_tmp_filepath),
          "%s/%s", this->rec_settings.rec_tmp_dir, this->recording_basename);
      filename_decided = 1;
    }
    while (!filename_decided) {
      unique_number++;
      snprintf(recording_filepath, sizeof(recording_filepath),
          "%s/%s-%d.ts", this->rec_settings.rec_dir, recording_basename, unique_number);
      if (access(recording_filepath, F_OK) != 0) { // filename is decided
        sprintf(recording_basename + strlen(recording_basename), "-%d.ts", unique_number);
        snprintf(recording_archive_filepath, sizeof(recording_archive_filepath),
            "%s/%s", dest_dir, recording_basename);
        snprintf(recording_tmp_filepath, sizeof(recording_tmp_filepath),
            "%s/%s", this->rec_settings.rec_tmp_dir, recording_basename);
        filename_decided = 1;
      }
    }
  }

  // Remove existing file
  if (unlink(recording_archive_filepath) == 0) {
    log_info("removed existing file: %s\n", recording_archive_filepath);
  }

  pthread_mutex_lock(&rec_write_mutex);
  this->mpegts_ctx = mpegts_create_context(this->codec_settings);
  this->rec_format_ctx = this->mpegts_ctx.format_context;
  mpegts_open_stream(this->rec_format_ctx, recording_tmp_filepath, 0);
  is_recording = 1;
  log_info("start rec to %s\n", recording_archive_filepath);
  state_set(NULL, "record", "true");
  pthread_mutex_unlock(&rec_write_mutex);

  int look_back_keyframes;
  if (this->recording_look_back_keyframes != -1) {
    look_back_keyframes = this->recording_look_back_keyframes;
  } else {
    look_back_keyframes = record_buffer_keyframes;
  }

  int start_keyframe_pointer;
  if (!is_keyframe_pointers_filled) { // first cycle has not been finished
    if (look_back_keyframes - 1 > current_keyframe_pointer) { // not enough pre-start buffer
      start_keyframe_pointer = 0;
    } else {
      start_keyframe_pointer = current_keyframe_pointer - look_back_keyframes + 1;
    }
  } else { // at least one cycle has been passed
    start_keyframe_pointer = current_keyframe_pointer - look_back_keyframes + 1;
  }

  // turn negative into positive
  while (start_keyframe_pointer < 0) {
    start_keyframe_pointer += record_buffer_keyframes;
  }

  rec_thread_frame = keyframe_pointers[start_keyframe_pointer];
  printf("start_keyframe_pointer=%d rec_thread_frame=%d\n", start_keyframe_pointer, rec_thread_frame);
  enc_pkt = encoded_packets[rec_thread_frame];
  printf("enc_pkt: %p\n", (void *)enc_pkt);
  if (enc_pkt) {
    rec_start_pts = enc_pkt->pts;
    write_encoded_packets(REC_CHASE_PACKETS, rec_start_pts);
  } else {
    rec_start_pts = 0;
  }

  av_pkt = av_packet_alloc();
  while (!this->rec_thread_needs_exit) {
    pthread_mutex_lock(&rec_mutex);
    while (!this->rec_thread_needs_write) {
      // printf("rec_cond wait\n");
      pthread_cond_wait(&rec_cond, &rec_mutex);
      // printf("rec_cond waited\n");
    }
    pthread_mutex_unlock(&rec_mutex);

    if (this->rec_thread_frame != this->current_encoded_packet) {
      wrote_packets = write_encoded_packets(REC_CHASE_PACKETS, rec_start_pts);
      if (wrote_packets <= 2) {
        if (!is_caught_up) {
          log_debug("caught up");
          is_caught_up = 1;
        }
      }
    }
    this->check_record_duration();
    if (this->rec_thread_needs_flush) {
      log_debug("F");
      mpegts_close_stream_without_trailer(rec_format_ctx);

      fsrc = fopen(recording_tmp_filepath, "r");
      if (fsrc == NULL) {
        log_error("error: failed to open %s: %s\n",
            recording_tmp_filepath, strerror(errno));
        has_error = 1;
        break;
      }
      fdest = fopen(recording_archive_filepath, "a");
      if (fdest == NULL) {
        log_error("error: failed to open %s: %s\n",
            recording_archive_filepath, strerror(errno));
        has_error = 1;
        break;
      }
      while (1) {
        read_len = fread(copy_buf, 1, BUFSIZ, fsrc);

        if (read_len > 0) {
          fwrite(copy_buf, 1, read_len, fdest);
        }
        if (read_len != BUFSIZ) {
          break;
        }
      }
      if (feof(fsrc)) {
        fclose(fsrc);
        fclose(fdest);
      } else {
        log_error("error: rec_thread_start: not an EOF?: %s\n", strerror(errno));
      }

      mpegts_open_stream_without_header(rec_format_ctx, recording_tmp_filepath, 0);
      rec_thread_needs_flush = 0;
      rec_start_time = time(NULL);
    }
    rec_thread_needs_write = 0;
  }
  free(copy_buf);
  av_packet_free(&av_pkt);
  int prev_frame = rec_thread_frame - 1;
  if (prev_frame == -1) {
    prev_frame = encoded_packets_size - 1;
  }
  enc_pkt = encoded_packets[prev_frame];
  rec_end_pts = enc_pkt->pts;
  snprintf(state_buf, sizeof(state_buf), "duration_pts=%" PRId64 "\nduration_sec=%f\n",
      rec_end_pts - rec_start_pts,
      (rec_end_pts - rec_start_pts) / 90000.0f);
  state_set(NULL, this->recording_basename, state_buf);

  return this->rec_thread_stop(has_error);
}

int Muxer::write_encoded_packets(int max_packets, int origin_pts) {
  int ret;
  AVPacket *avpkt;
  EncodedPacket *enc_pkt;
  char errbuf[1024];

  avpkt = av_packet_alloc();
  int wrote_packets = 0;

  pthread_mutex_lock(&rec_write_mutex);
  while (1) {
    wrote_packets++;
    enc_pkt = this->encoded_packets[this->rec_thread_frame];
    avpkt->pts = avpkt->dts = enc_pkt->pts - origin_pts;
    avpkt->data = enc_pkt->data;
    avpkt->size = enc_pkt->size;
    avpkt->stream_index = enc_pkt->stream_index;
    avpkt->flags = enc_pkt->flags;
    ret = av_write_frame(this->rec_format_ctx, avpkt);
    if (ret < 0) {
      av_strerror(ret, errbuf, sizeof(errbuf));
      log_error("error: write_encoded_packets: av_write_frame: %s\n", errbuf);
    }
    if (++this->rec_thread_frame == this->encoded_packets_size) {
      this->rec_thread_frame = 0;
    }
    if (this->rec_thread_frame == this->current_encoded_packet) {
      break;
    }
    if (wrote_packets == max_packets) {
      break;
    }
  }
  pthread_mutex_unlock(&rec_write_mutex);
  av_packet_free(&avpkt);

  return wrote_packets;
}

// Remember the point where keyframe occurs within encoded_packets
void Muxer::mark_keyframe_packet() {
  pthread_mutex_lock(&rec_write_mutex);
  // keyframe_pointers is a circular buffer and
  // current_keyframe_pointer holds the index of last written item.
  current_keyframe_pointer++;
  if (current_keyframe_pointer >= record_buffer_keyframes) {
    current_keyframe_pointer = 0;
    if (!is_keyframe_pointers_filled) {
      is_keyframe_pointers_filled = 1;
    }
  }
  keyframe_pointers[current_keyframe_pointer] = current_encoded_packet;
  pthread_mutex_unlock(&rec_write_mutex);
}

void Muxer::add_encoded_packet(int64_t pts, uint8_t *data, int size, int stream_index, int flags) {
  EncodedPacket *packet;

  // printf("add stream=%d pts=%lld size=%d flags=%d", stream_index, pts, size, flags);
  // if (stream_index == 0 && flags) {
  //   printf(" (keyframe)");
  // }
  // printf("\n");

  pthread_mutex_lock(&rec_write_mutex);
  // pthread_mutex_lock(&encoded_packet_mutex);
  if (++current_encoded_packet == encoded_packets_size) {
    current_encoded_packet = 0;
  }
  packet = encoded_packets[current_encoded_packet];
  if (packet != NULL) {
    int next_keyframe_pointer = current_keyframe_pointer + 1;
    if (next_keyframe_pointer >= record_buffer_keyframes) {
      next_keyframe_pointer = 0;
    }
    // log_debug("current_keyframe_pointer=%d current_encoded_packet=%d next_keyframe_pointer=%d keyframe_pointers[next]=%d\n",
      // current_keyframe_pointer,
      // current_encoded_packet, next_keyframe_pointer, keyframe_pointers[next_keyframe_pointer]);
    if (current_encoded_packet == keyframe_pointers[next_keyframe_pointer]) {
      log_warn("warning: Record buffer is starving. Recorded file may not start from keyframe. Try reducing the value of --gopsize.\n");
    }

    av_freep(&packet->data);
  } else {
    packet = (EncodedPacket *)malloc(sizeof(EncodedPacket));
    if (packet == NULL) {
      perror("malloc for EncodedPacket");
      return;
    }
    encoded_packets[current_encoded_packet] = packet;
  }

  uint8_t *copied_data = (uint8_t *)av_malloc(size);
  if (copied_data == NULL) {
    perror("av_malloc for copied_data");
    exit(EXIT_FAILURE);
  }

  // If this part is not guarded by mutex, segmentation fault will happen
  // printf("add_encoded_packet memcpy begin\n");
  memcpy(copied_data, data, size);
  // printf("add_encoded_packet memcpy end\n");
  packet->pts = pts;
  packet->data = copied_data;
  packet->size = size;
  packet->stream_index = stream_index;
  packet->flags = flags;
  // pthread_mutex_unlock(&encoded_packet_mutex);
  pthread_mutex_unlock(&rec_write_mutex);

  this->onFrameArrive(packet);
}

void Muxer::free_encoded_packets() {
  int i;
  EncodedPacket *packet;

  for (i = 0; i < this->encoded_packets_size; i++) {
    packet = this->encoded_packets[i];
    if (packet != NULL) {
      av_freep(&packet->data);
      free(packet);
    }
  }
}

// set record_buffer_keyframes to newsize
int Muxer::set_record_buffer_keyframes(int newsize) {
  int i;
  void *result;
  int malloc_size;

  if (is_recording) {
    log_error("error: recordbuf cannot be changed while recording\n");
    return -1;
  }

  if (newsize < 1) {
    log_error("error changing recordbuf to %d (must be >= 1)\n", newsize);
    return -1;
  }

  if (newsize == record_buffer_keyframes) { // no change
    log_debug("recordbuf does not change: current=%d new=%d\n",
        record_buffer_keyframes, newsize);
    return -1;
  }

  for (i = 0; i < encoded_packets_size; i++) {
    EncodedPacket *packet = encoded_packets[i];
    if (packet != NULL) {
      av_freep(&packet->data);
      free(packet);
    }
  }

  // reset encoded_packets
  int audio_fps = this->option->audio_sample_rate / 1 / this->option->audio_period_size;
  int new_encoded_packets_size = (this->option->video_fps + 1) * newsize * 2 +
    (audio_fps + 1) * newsize * 2 + 100;
  malloc_size = sizeof(EncodedPacket *) * new_encoded_packets_size;
  result = realloc(encoded_packets, malloc_size);
  int success = 0;
  if (result == NULL) {
    log_error("error: failed to set encoded_packets to %d while trying to allocate "
        "%d bytes of memory\n", newsize, malloc_size);
    // fallback to old size
    malloc_size = sizeof(EncodedPacket *) * encoded_packets_size;
  } else {
    encoded_packets = (EncodedPacket **)result;
    encoded_packets_size = new_encoded_packets_size;
    success = 1;
  }
  memset(encoded_packets, 0, malloc_size);

  if (success) {
    // reset keyframe_pointers
    malloc_size = sizeof(int) * newsize;
    result = realloc(keyframe_pointers, malloc_size);
    if (result == NULL) {
      log_error("error: failed to set keyframe_pointers to %d while trying to allocate "
          "%d bytes of memory\n", newsize, malloc_size);
      // fallback to old size
      malloc_size = sizeof(int) * record_buffer_keyframes;
    } else {
      keyframe_pointers = (int *)result;
      record_buffer_keyframes = newsize;
    }
  } else {
    malloc_size = sizeof(int) * record_buffer_keyframes;
  }
  memset(keyframe_pointers, 0, malloc_size);

  current_encoded_packet = -1;
  current_keyframe_pointer = -1;
  is_keyframe_pointers_filled = 0;

  return 0;
}

void encoded_packet_to_avpacket(EncodedPacket *enc_pkt, AVPacket *avpkt)
{
  avpkt->pts = avpkt->dts = enc_pkt->pts;
  avpkt->data = enc_pkt->data;
  avpkt->size = enc_pkt->size;
  avpkt->stream_index = enc_pkt->stream_index;
  avpkt->flags = enc_pkt->flags;
}
