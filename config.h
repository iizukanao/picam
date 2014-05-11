#ifndef PICAM_CONFIG_H
#define PICAM_CONFIG_H

#if defined(__cplusplus)
extern "C" {
#endif

// Video frame size
// 1600x900 is the upper limit due to the speed of coping a video frame.
// 720p (1280x720) is good enough for most cases.
#define WIDTH 1280
#define HEIGHT 720

// Target FPS (frames per second)
// The actual FPS is slightly lower than this value due to
// the problem on Raspberry Pi Camera's side.
#define TARGET_FPS 30
// Distance between two key frames
#define GOP_SIZE TARGET_FPS

// Video (H.264) bitrate in bits per second
#define H264_BIT_RATE 2000 * 1000  // 2 Mbps

// Audio sample rate
// Choose the value that results in non-repeating decimal when divided by 90000.
// e.g. 48000 and 32000 are good, but 44100 and 22050 are bad.
#define AUDIO_SAMPLE_RATE 48000 

// Audio (AAC LC) bitrate in bits per second
#define AAC_BIT_RATE 40000  // 40 Kbps

// Enable HTTP Live Streaming output or not
#define ENABLE_HLS_OUTPUT  1  // default: 1

// Output directory for HTTP Live Streaming files
#define HLS_OUTPUT_DIR  "/run/shm/video"

// Enable output to RTSP server via UNIX domain sockets or not
// Meant to be used with https://github.com/iizukanao/node-rtsp-rtmp-server
#define ENABLE_UNIX_SOCKETS_OUTPUT 1  // default: 1
// UNIX domain sockets provided by node-rtsp-rtmp-server
#define SOCK_PATH_VIDEO_CONTROL "/tmp/node_rtsp_rtmp_videoControl"
#define SOCK_PATH_AUDIO_CONTROL "/tmp/node_rtsp_rtmp_audioControl"
#define SOCK_PATH_VIDEO_DATA    "/tmp/node_rtsp_rtmp_videoData"
#define SOCK_PATH_AUDIO_DATA    "/tmp/node_rtsp_rtmp_audioData"

// Enable MPEG-TS output via TCP socket or not
#define ENABLE_TCP_OUTPUT 0  // default: 0
// Where to send MPEG-TS stream (available if ENABLE_TCP_OUTPUT is 1)
#define TCP_OUTPUT_DEST "tcp://127.0.0.1:8181"

// Enable audio amplification or not
#define ENABLE_AUDIO_AMPLIFICATION 0
// Audio volume is multiplied by this number
#define AUDIO_VOLUME_MULTIPLY 2.0
// Minimum possible value of audio data from microphone
#define AUDIO_MIN_VALUE -16384
// Maximum possible value of audio data from microphone
#define AUDIO_MAX_VALUE 16383

// Whether or not to enable auto exposure (day/night) mode
#define ENABLE_AUTO_EXPOSURE 0
// When the average value of Y in a frame goes below this number,
// the camera will go into night mode
#define EXPOSURE_NIGHT_Y_THRESHOLD 40
// When the average value of Y in a frame goes above this number,
// the camera will go into day (auto) mode
#define EXPOSURE_AUTO_Y_THRESHOLD 50

// Enable verbose output or not
#define ENABLE_VERBOSE_LOG 1


////////////////////////////
// Advanced configurations
////////////////////////////

// Enable video preview on the screen or not
#define ENABLE_PREVIEW 0

// Enable encryption in HTTP Live Streaming or not
// Key and IV are defined in stream.c
#define ENABLE_HLS_ENCRYPTION 0

// Number of keyframes to be kept in recording buffer
#define RECORD_BUFFER_KEYFRAMES 5

// Which color (YUV) is used to fill blank borders
#define FILL_COLOR_Y 0
#define FILL_COLOR_U 128
#define FILL_COLOR_V 128

// Directory that holds state files
#define STATE_DIR "state"

// Directory that accept hooks files
#define HOOKS_DIR "hooks"

#if defined(__cplusplus)
}
#endif

#endif
