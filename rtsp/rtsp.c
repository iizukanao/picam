#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <stdio.h>

#include "log/log.h"
#include "rtsp.h"

// UNIX domain sockets
static int sockfd_video;
static int sockfd_video_control;
static int sockfd_audio;
static int sockfd_audio_control;

void rtsp_setup_socks(RtspConfig config) {
  struct sockaddr_un remote_video;
  struct sockaddr_un remote_audio;

  int len;
  struct sockaddr_un remote_video_control;
  struct sockaddr_un remote_audio_control;

  log_debug("connecting to UNIX domain sockets\n");

  // Setup sockfd_video
  if ((sockfd_video = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("socket video");
    exit(EXIT_FAILURE);
  }
  remote_video.sun_family = AF_UNIX;
  strcpy(remote_video.sun_path, config.rtsp_video_data_path);
  len = strlen(remote_video.sun_path) + sizeof(remote_video.sun_family);
  if (connect(sockfd_video, (struct sockaddr *)&remote_video, len) == -1) {
    log_error("error: failed to connect to video data socket (%s): %s\n"
        "perhaps RTSP server (https://github.com/iizukanao/node-rtsp-rtmp-server) is not running?\n",
        config.rtsp_video_data_path, strerror(errno));
    exit(EXIT_FAILURE);
  }

  // Setup sockfd_video_control
  if ((sockfd_video_control = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("socket video_control");
    exit(EXIT_FAILURE);
  }
  remote_video_control.sun_family = AF_UNIX;
  strcpy(remote_video_control.sun_path, config.rtsp_video_control_path);
  len = strlen(remote_video_control.sun_path) + sizeof(remote_video_control.sun_family);
  if (connect(sockfd_video_control, (struct sockaddr *)&remote_video_control, len) == -1) {
    log_error("error: failed to connect to video control socket (%s): %s\n"
        "perhaps RTSP server (https://github.com/iizukanao/node-rtsp-rtmp-server) is not running?\n",
        config.rtsp_video_control_path, strerror(errno));
    exit(EXIT_FAILURE);
  }

  // Setup sockfd_audio
  if ((sockfd_audio = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("socket audio");
    exit(EXIT_FAILURE);
  }
  remote_audio.sun_family = AF_UNIX;
  strcpy(remote_audio.sun_path, config.rtsp_audio_data_path);
  len = strlen(remote_audio.sun_path) + sizeof(remote_audio.sun_family);
  if (connect(sockfd_audio, (struct sockaddr *)&remote_audio, len) == -1) {
    log_error("error: failed to connect to audio data socket (%s): %s\n"
        "perhaps RTSP server (https://github.com/iizukanao/node-rtsp-rtmp-server) is not running?\n",
        config.rtsp_audio_data_path, strerror(errno));
    exit(EXIT_FAILURE);
  }

  // Setup sockfd_audio_control
  if ((sockfd_audio_control = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("socket audio_control");
    exit(EXIT_FAILURE);
  }
  remote_audio_control.sun_family = AF_UNIX;
  strcpy(remote_audio_control.sun_path, config.rtsp_audio_control_path);
  len = strlen(remote_audio_control.sun_path) + sizeof(remote_audio_control.sun_family);
  if (connect(sockfd_audio_control, (struct sockaddr *)&remote_audio_control, len) == -1) {
    log_error("error: failed to connect to audio control socket (%s): %s\n"
        "perhaps RTSP server (https://github.com/iizukanao/node-rtsp-rtmp-server) is not running?\n",
        config.rtsp_audio_control_path, strerror(errno));
    exit(EXIT_FAILURE);
  }
}

// Send video packet to node-rtsp-rtmp-server
void send_video_start_time() {
  int payload_size = 11;
  uint8_t sendbuf[14] = {
    // payload size
    (payload_size >> 16) & 0xff,
    (payload_size >> 8) & 0xff,
    payload_size & 0xff,

    // payload
    // packet type
    0x00,
    // stream name
    'l', 'i', 'v', 'e', '/', 'p', 'i', 'c', 'a', 'm',
  };
  if (send(sockfd_video_control, sendbuf, sizeof(sendbuf), 0) == -1) {
    perror("send video start time");
    exit(EXIT_FAILURE);
  }
}

// Send audio packet to node-rtsp-rtmp-server
void send_audio_start_time(int64_t audio_start_time) {
  int payload_size = 9;
  int64_t logical_start_time = audio_start_time;
  uint8_t sendbuf[12] = {
    // payload size
    (payload_size >> 16) & 0xff,
    (payload_size >> 8) & 0xff,
    payload_size & 0xff,
    // packet type (0x01 == audio start time)
    0x01,
    // payload
    logical_start_time >> 56,
    (logical_start_time >> 48) & 0xff,
    (logical_start_time >> 40) & 0xff,
    (logical_start_time >> 32) & 0xff,
    (logical_start_time >> 24) & 0xff,
    (logical_start_time >> 16) & 0xff,
    (logical_start_time >> 8) & 0xff,
    logical_start_time & 0xff,
  };
  if (send(sockfd_audio_control, sendbuf, 12, 0) == -1) {
    perror("send audio start time");
    exit(EXIT_FAILURE);
  }
}

void rtsp_send_video_frame(uint8_t *databuf, int databuflen, int64_t pts) {
  int payload_size = databuflen + 7;  // +1(packet type) +6(pts)
  int total_size = payload_size + 3;  // more 3 bytes for payload length
  uint8_t *sendbuf = malloc(total_size);
  if (sendbuf == NULL) {
    log_error("error: cannot allocate memory for video sendbuf: size=%d", total_size);
    return;
  }
  // payload header
  sendbuf[0] = (payload_size >> 16) & 0xff;
  sendbuf[1] = (payload_size >> 8) & 0xff;
  sendbuf[2] = payload_size & 0xff;
  // payload
  sendbuf[3] = 0x02;  // packet type (0x02 == video data)
  sendbuf[4] = (pts >> 40) & 0xff;
  sendbuf[5] = (pts >> 32) & 0xff;
  sendbuf[6] = (pts >> 24) & 0xff;
  sendbuf[7] = (pts >> 16) & 0xff;
  sendbuf[8] = (pts >> 8) & 0xff;
  sendbuf[9] = pts & 0xff;
  memcpy(sendbuf + 10, databuf, databuflen);
  if (send(sockfd_video, sendbuf, total_size, 0) == -1) {
    perror("send video data");
  }
  free(sendbuf);
}
