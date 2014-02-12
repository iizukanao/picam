#ifndef PICAM_CONFIG_H
#define PICAM_CONFIG_H

#if defined(__cplusplus)
extern "C" {
#endif

// 1600x900 is the upper limit if you don't use tunnel from camera to video_encode.
// 720p (1280x720) is good enough for most cases.
#define WIDTH 1280
#define HEIGHT 720

// Even if we specify 30 FPS, Raspberry Pi Camera provides slighly lower FPS.
#define TARGET_FPS 30

// Distance between two key frames
#define GOP_SIZE TARGET_FPS

#define H264_BIT_RATE 2000 * 1000  // 2 Mbps

// Choose the value that results in non-repeating decimal when divided by 90000.
// e.g. 48000 and 32000 are fine, but 44100 and 22050 are bad.
#define AUDIO_SAMPLE_RATE 48000 

#define AAC_BIT_RATE 40000  // 40 Kbps

// Set this value to 1 if you want to produce audio-only stream for debugging.
#define AUDIO_ONLY 0

#if defined(__cplusplus)
}
#endif

#endif
