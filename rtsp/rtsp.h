#ifdef __cplusplus
extern "C" {
#endif

#ifndef _PICAM_RTSP_H_
#define _PICAM_RTSP_H_

typedef struct RtspConfig {
  char *rtsp_video_control_path;
  char *rtsp_audio_control_path;
  char *rtsp_video_data_path;
  char *rtsp_audio_data_path;
} RtspConfig;

void rtsp_setup_socks(RtspConfig config);
void rtsp_teardown_socks();
void rtsp_send_video_start_time();
void rtsp_send_audio_start_time(int64_t audio_start_time);
void rtsp_send_video_frame(uint8_t *databuf, int databuflen, int64_t pts);
void rtsp_send_audio_frame(uint8_t *databuf, int databuflen, int64_t pts);

#endif

#ifdef __cplusplus
}
#endif
