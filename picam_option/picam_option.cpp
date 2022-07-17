#include <getopt.h>
#include <cstring>
#include <cmath>

#include "dispmanx/dispmanx.h"
#include "picam_option.hpp"

void PicamOption::print_program_version() {
  log_info(PROGRAM_VERSION "\n");
}

void PicamOption::print_usage() {
  PicamOption defaultOption;
  log_info(PROGRAM_NAME " version " PROGRAM_VERSION "\n");
  log_info("Usage: " PROGRAM_NAME " [options]\n");
  log_info("\n");
  log_info("Options:\n");
  log_info(" [video]\n");
  log_info("  -w, --width <num>   Width in pixels (default: %d)\n", defaultOption.video_width);
  log_info("  -h, --height <num>  Height in pixels (default: %d)\n", defaultOption.video_height);
  log_info("  -v, --videobitrate <num>  Video bit rate (default: %ld)\n", defaultOption.video_bitrate);
  log_info("                      Set 0 to disable rate control\n");
  log_info("  -f, --fps <num>     Frame rate (default: %.1f)\n", defaultOption.video_fps);
  log_info("  -g, --gopsize <num>  GOP size (default: same value as fps)\n");
  log_info("  --vfr               Enable variable frame rate. GOP size will be\n");
  log_info("                      dynamically controlled.\n");
  log_info("  --minfps <num>      Minimum frames per second. Implies --vfr.\n");
  // log_info("                      It might not work if width / height >= 1.45.\n");
  log_info("  --maxfps <num>      Maximum frames per second. Implies --vfr.\n");
  // log_info("                      It might not work if width / height >= 1.45.\n");
  // log_info("  --rotation <num>    Image rotation in clockwise degrees\n");
  // log_info("                      (0, 90, 180, 270)\n");
  log_info("  --hflip             Flip image horizontally\n");
  log_info("  --vflip             Flip image vertically\n");
  log_info("  --avcprofile <str>  Set AVC/H.264 profile to one of:\n");
  log_info("                      constrained_baseline/baseline/main/high\n");
  log_info("                      (default: %s)\n", defaultOption.video_avc_profile);
  log_info("  --avclevel <value>  Set AVC/H.264 level (default: %s)\n", defaultOption.video_avc_level);
  // log_info("  --qpmin <num>       Minimum quantization level (0..51)\n");
  // log_info("  --qpmax <num>       Maximum quantization level (0..51)\n");
  // log_info("  --qpinit <num>      Initial quantization level\n");
  // log_info("  --dquant <num>      Slice DQuant level\n");
  log_info(" [audio]\n");
  log_info("  -c, --channels <num>  Audio channels (1=mono, 2=stereo)\n");
  log_info("                      Default is mono. If it fails, stereo is used.\n");
  log_info("  -r, --samplerate <num>  Audio sample rate (default: %d)\n", defaultOption.audio_sample_rate);
  log_info("                      The sample rates supported by libfdk_aac encoder are:\n");
  log_info("                      8000, 11025, 12000, 16000, 22050, 24000,\n");
  log_info("                      32000, 44100, 48000, 64000, 88200, 96000\n");
  log_info("  -a, --audiobitrate <num>  Audio bit rate (default: %ld)\n", defaultOption.audio_bitrate);
  log_info("  --alsadev <dev>     ALSA microphone device (default: %s)\n", defaultOption.alsa_dev);
  log_info("  --volume <num>      Amplify audio by multiplying the volume by <num>\n");
  log_info("                      (default: %.1f)\n", defaultOption.audio_volume_multiply);
  log_info("  --noaudio           Disable audio capturing\n");
  log_info("  --audiopreview      Enable audio preview\n");
  log_info("  --audiopreviewdev <dev>  Audio preview output device (default: %s)\n", defaultOption.audio_preview_dev);
  log_info(" [HTTP Live Streaming (HLS)]\n");
  log_info("  -o, --hlsdir <dir>  Generate HTTP Live Streaming files in <dir>\n");
  log_info("  --hlsnumberofsegments <num>  Set the number of segments in the m3u8 playlist (default: %d)\n", defaultOption.hls_number_of_segments);
  log_info("  --hlskeyframespersegment <num>  Set the number of keyframes per video segment (default: %d)\n", defaultOption.hls_keyframes_per_segment);
  log_info("  --hlsenc            Enable HLS encryption\n");
  log_info("  --hlsenckeyuri <uri>  Set HLS encryption key URI (default: %s)\n", defaultOption.hls_encryption_key_uri);
  log_info("  --hlsenckey <hex>   Set HLS encryption key in hex string\n");
  log_info("                      (default: ");
  log_hex(LOG_LEVEL_INFO, defaultOption.hls_encryption_key, sizeof(defaultOption.hls_encryption_key));
  log_info(")\n");
  log_info("  --hlsenciv <hex>    Set HLS encryption IV in hex string\n");
  log_info("                      (default: ");
  log_hex(LOG_LEVEL_INFO, defaultOption.hls_encryption_iv, sizeof(defaultOption.hls_encryption_iv));
  log_info(")\n");
  log_info(" [output for node-rtsp-rtmp-server]\n");
  log_info("  --rtspout           Enable output for node-rtsp-rtmp-server\n");
  log_info("  --rtspvideocontrol <path>  Set video control socket path\n");
  log_info("                      (default: %s)\n", defaultOption.rtsp_video_control_path);
  log_info("  --rtspaudiocontrol <path>  Set audio control socket path\n");
  log_info("                      (default: %s)\n", defaultOption.rtsp_audio_control_path);
  log_info("  --rtspvideodata <path>  Set video data socket path\n");
  log_info("                      (default: %s)\n", defaultOption.rtsp_video_data_path);
  log_info("  --rtspaudiodata <path>  Set audio data socket path\n");
  log_info("                      (default: %s)\n", defaultOption.rtsp_audio_data_path);
  log_info(" [MPEG-TS output via TCP]\n");
  log_info("  --tcpout <url>      Enable TCP output to <url>\n");
  log_info("                      (e.g. --tcpout tcp://127.0.0.1:8181)\n");
  log_info(" [camera]\n");
  log_info("  --camera <num>      Choose the camera to use. Use --query to list the cameras.\n");
  log_info("  --autoex            Enable automatic control of camera exposure between\n");
  log_info("                      daylight and night modes. This forces --vfr enabled.\n");
  log_info("  --autoexthreshold <num>  When average value of Y (brightness) for\n");
  log_info("                      10 milliseconds of captured image falls below <num>,\n");
  log_info("                      camera exposure will change to night mode. Otherwise\n");
  log_info("                      camera exposure is in daylight mode. Implies --autoex.\n");
  log_info("                      (default: %.1f)\n", defaultOption.auto_exposure_threshold);
  log_info("                      If --verbose option is enabled as well, average value of\n");
  log_info("                      Y is printed like y=28.0.\n");
  log_info("  --ex <value>        Set camera exposure. Implies --vfr. <value> is one of:\n");
  log_info("                        normal short long custom\n");
  // TODO: Add --analoguegain <value>
  log_info("  --wb <value>        Set white balance. <value> is one of:\n");
  log_info("                        off: Disable auto white balance control\n");
  log_info("                        auto: Search over the whole colour temperature range (default)\n");
  log_info("                        incandescent: Incandescent AWB lamp mode\n");
  log_info("                        tungsten: Tungsten AWB lamp mode\n");
  log_info("                        fluorescent: Fluorescent AWB lamp mode\n");
  log_info("                        indoor: Indoor AWB lighting mode\n");
  log_info("                        daylight: Daylight AWB lighting mode\n");
  log_info("                        cloudy: Cloudy AWB lighting mode\n");
  log_info("                        custom: Custom AWB mode\n");
  log_info("  --wbred <num>       Red gain. Implies \"--wb off\". (0.0 .. 8.0)\n");
  log_info("  --wbblue <num>      Blue gain. Implies \"--wb off\". (0.0 .. 8.0)\n");
  log_info("  --metering <value>  Set metering type. <value> is one of:\n");
  log_info("                        center: Center-weighted metering mode (default)\n");
  log_info("                        spot: Spot metering mode\n");
  log_info("                        matrix: Matrix metering mode\n");
  log_info("                        custom: Custom metering mode\n");
  log_info("  --evcomp <num>      Set Exposure Value compensation (-8..8) (default: 0)\n");
//  log_info("  --aperture <num>    Set aperture f-stop. Use 2 for f/2. (default: not set)\n");
//  log_info("                      * Not sure if this has practical effect.\n");
  log_info("  --shutter <num>     Set shutter speed in microseconds (default: auto).\n");
  log_info("                      Implies --vfr.\n");
  // log_info("  --iso <num>         Set ISO sensitivity (100..800) (default: auto)\n");
  log_info("  --roi <x,y,w,h>     Set region of interest (crop rect) in ratio (0.0-1.0).\n");
  log_info("                      (default: %.0f,%.0f,%.0f,%.0f)\n",
      defaultOption.roi_left, defaultOption.roi_top, defaultOption.roi_width, defaultOption.roi_height);
  log_info("                      --roi affects performance and may reduce fps.\n");
  log_info("  -p, --preview       Display fullscreen preview\n");
  log_info("  --previewrect <x,y,width,height>\n");
  log_info("                      Display preview window at specified position\n");
  // log_info("  --opacity           Preview window opacity\n");
  // log_info("                      (0=transparent..255=opaque; default=%d)\n", defaultOption.preview_opacity);
  log_info("  --hdmi              Preview output HDMI port (0 or 1; default=%d)\n", defaultOption.preview_hdmi);
  log_info("                      HDMI port selection only works in console mode (when X is not running)\n");
  // log_info("  --blank[=0xAARRGGBB]  Set the video background color to black (or optional ARGB value)\n");
  log_info("  --query             Query camera capabilities then exit\n");
  // log_info("  --mode             Specify the camera sensor mode (values depend on the camera hardware)\n");
  log_info(" [timestamp] (may be a bit heavy on Raspberry Pi 1)\n");
  log_info("  --time              Enable timestamp\n");
  log_info("  --timeformat <spec>  Timestamp format (see \"man strftime\" for spec)\n");
  log_info("                       (default: \"%s\")\n", defaultOption.timestamp_format);
  log_info("  --timelayout <spec>  Timestamp position (relative mode)\n");
  log_info("                       layout is comma-separated list of:\n");
  log_info("                        top middle bottom  left center right\n");
  log_info("                       (default: bottom,right)\n");
  log_info("  --timehorizmargin <px>  Horizontal margin from edge (default: %d).\n", defaultOption.timestamp_horizontal_margin);
  log_info("                          Effective only if --timelayout is used.\n");
  log_info("  --timevertmargin <px>  Vertical margin from edge (default: %d).\n", defaultOption.timestamp_vertical_margin);
  log_info("                         Effective only if --timelayout is used.\n");
  log_info("  --timepos <x,y>     Timestamp position (absolute mode)\n");
//  log_info("  --timealign <spec>  Text alignment (left, center, right) (default: left)\n");
  log_info("  --timefontname <name>  Timestamp font name (default: %s)\n", defaultOption.timestamp_font_name);
  log_info("  --timefontfile <file>  Timestamp font file. This invalidates --timefontname.\n");
  log_info("  --timefontface <num>  Timestamp font face index (default: %d).\n", defaultOption.timestamp_font_face_index);
  log_info("                        Effective only if --timefontfile is used.\n");
  log_info("  --timept <pt>       Text size in points (default: %.1f)\n", defaultOption.timestamp_font_points);
  log_info("  --timedpi <num>     DPI for calculating text size (default: %d)\n", defaultOption.timestamp_font_dpi);
  log_info("  --timecolor <hex>   Text color (default: %06x)\n", defaultOption.timestamp_color);
  log_info("  --timestrokecolor <hex>  Text stroke color (default: %06x)\n", defaultOption.timestamp_stroke_color);
  log_info("                      Note that texts are rendered in grayscale.\n");
  log_info("  --timestrokewidth <pt>  Text stroke border radius (default: %.1f).\n",
      defaultOption.timestamp_stroke_width);
  log_info("                          To disable stroking borders, set this value to 0.\n");
  log_info("  --timespacing <px>  Additional letter spacing (default: %d)\n", defaultOption.timestamp_letter_spacing);
  log_info(" [misc]\n");
  log_info("  --recordbuf <num>   Start recording from <num> keyframes ago\n");
  log_info("                      (must be >= 1; default: %d)\n", defaultOption.record_buffer_keyframes);
  log_info("  --statedir <dir>    Set state dir (default: %s)\n", defaultOption.state_dir);
  log_info("  --hooksdir <dir>    Set hooks dir (default: %s)\n", defaultOption.hooks_dir);
  log_info("  -q, --quiet         Suppress all output except errors\n");
  log_info("  --verbose           Enable verbose output\n");
  log_info("  --version           Print program version\n");
  log_info("  --help              Print this help\n");
}

PicamOption::PicamOption() {
}

PicamOption::~PicamOption() {

}

void PicamOption::calculate() {

}

int PicamOption::parse(int argc, char **argv) {
  static struct option long_options[] = {
    // { "mode", required_argument, NULL, 0},
    { "width", required_argument, NULL, 'w' },
    { "height", required_argument, NULL, 'h' },
    { "fps", required_argument, NULL, 'f' },
    { "ptsstep", required_argument, NULL, 0 },
    { "videobitrate", required_argument, NULL, 'v' },
    { "gopsize", required_argument, NULL, 'g' },
    // { "rotation", required_argument, NULL, 0 },
    { "hflip", no_argument, NULL, 0 },
    { "vflip", no_argument, NULL, 0 },
    { "avcprofile", required_argument, NULL, 0 },
    { "avclevel", required_argument, NULL, 0 },
    { "qpmin", required_argument, NULL, 0 },
    { "qpmax", required_argument, NULL, 0 },
    { "qpinit", required_argument, NULL, 0 },
    { "dquant", required_argument, NULL, 0 },
    { "alsadev", required_argument, NULL, 0 },
    { "audiobitrate", required_argument, NULL, 'a' },
    { "channels", required_argument, NULL, 'c' },
    { "samplerate", required_argument, NULL, 'r' },
    { "hlsdir", required_argument, NULL, 'o' },
    { "hlskeyframespersegment", required_argument, NULL, 0 },
    { "hlsnumberofsegments", required_argument, NULL, 0 },
    { "rtspout", no_argument, NULL, 0 },
    { "rtspvideocontrol", required_argument, NULL, 0 },
    { "rtspvideodata", required_argument, NULL, 0 },
    { "rtspaudiocontrol", required_argument, NULL, 0 },
    { "rtspaudiodata", required_argument, NULL, 0 },
    { "tcpout", required_argument, NULL, 0 },
    { "vfr", no_argument, NULL, 0 },
    { "minfps", required_argument, NULL, 0 },
    { "maxfps", required_argument, NULL, 0 },
    { "camera", required_argument, NULL, 0 },
    { "autoex", no_argument, NULL, 0 },
    { "autoexthreshold", required_argument, NULL, 0 },
    { "ex", required_argument, NULL, 0 },
    { "wb", required_argument, NULL, 0 },
    { "wbred", required_argument, NULL, 0 },
    { "wbblue", required_argument, NULL, 0 },
    { "metering", required_argument, NULL, 0 },
    { "evcomp", required_argument, NULL, 0 },
    { "aperture", required_argument, NULL, 0 },
    { "shutter", required_argument, NULL, 0 },
    { "iso", required_argument, NULL, 0 },
    { "roi", required_argument, NULL, 0 },
    { "query", no_argument, NULL, 0 },
    { "time", no_argument, NULL, 0 },
    { "timeformat", required_argument, NULL, 0 },
    { "timelayout", required_argument, NULL, 0 },
    { "timehorizmargin", required_argument, NULL, 0 },
    { "timevertmargin", required_argument, NULL, 0 },
    { "timepos", required_argument, NULL, 0 },
    { "timealign", required_argument, NULL, 0 },
    { "timefontname", required_argument, NULL, 0 },
    { "timefontfile", required_argument, NULL, 0 },
    { "timefontface", required_argument, NULL, 0 },
    { "timept", required_argument, NULL, 0 },
    { "timedpi", required_argument, NULL, 0 },
    { "timecolor", required_argument, NULL, 0 },
    { "timestrokecolor", required_argument, NULL, 0 },
    { "timestrokewidth", required_argument, NULL, 0 },
    { "timespacing", required_argument, NULL, 0 },
    { "statedir", required_argument, NULL, 0 },
    { "hooksdir", required_argument, NULL, 0 },
    { "volume", required_argument, NULL, 0 },
    { "noaudio", no_argument, NULL, 0 },
    { "audiopreview", no_argument, NULL, 0 },
    { "audiopreviewdev", required_argument, NULL, 0 },
    { "hlsenc", no_argument, NULL, 0 },
    { "hlsenckeyuri", required_argument, NULL, 0 },
    { "hlsenckey", required_argument, NULL, 0 },
    { "hlsenciv", required_argument, NULL, 0 },
    { "preview", no_argument, NULL, 'p' },
    { "previewrect", required_argument, NULL, 0 },
    // { "blank", optional_argument, NULL, 0 },
    // { "opacity", required_argument, NULL, 0 },
    { "hdmi", required_argument, NULL, 0 },
    { "quiet", no_argument, NULL, 'q' },
    { "recordbuf", required_argument, NULL, 0 },
    { "verbose", no_argument, NULL, 0 },
    { "version", no_argument, NULL, 0 },
    { "help", no_argument, NULL, 0 },
    { 0, 0, 0, 0 },
  };
  int option_index = 0;
  int opt;

  // // Turn off buffering for stdout
  // setvbuf(stdout, NULL, _IONBF, 0);

  // log_set_level(log_level_default);
  // log_set_stream(stdout);

  while ((opt = getopt_long(argc, argv, "w:h:v:f:g:c:r:a:o:pq", long_options, &option_index)) != -1) {
    switch (opt) {
      case 0:
        if (long_options[option_index].flag != 0) {
          break;
        }
        // if (strcmp(long_options[option_index].name, "mode") == 0) {
        //   char* end;
        //   int value = strtol(optarg, &end, 10);
        //   if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
        //     log_fatal("error: invalid sensor mode: %s\n", optarg);
        //     return EXIT_FAILURE;
        //   }
        //   sensor_mode = value;
        if (strcmp(long_options[option_index].name, "ptsstep") == 0) {
          char *end;
          int value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid ptsstep: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid ptsstep: %d (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          video_pts_step = value;
          is_video_pts_step_specified = 1;
        // } else if (strcmp(long_options[option_index].name, "rotation") == 0) {
        //   char *end;
        //   int value = strtol(optarg, &end, 10);
        //   if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
        //     log_fatal("error: invalid rotation: %s\n", optarg);
        //     print_usage();
        //     return EXIT_FAILURE;
        //   }
        //   video_rotation = value;
        } else if (strcmp(long_options[option_index].name, "hflip") == 0) {
          video_hflip = 1;
        } else if (strcmp(long_options[option_index].name, "vflip") == 0) {
          video_vflip = 1;
        } else if (strcmp(long_options[option_index].name, "avcprofile") == 0) {
          strncpy(video_avc_profile, optarg, sizeof(video_avc_profile) - 1);
          video_avc_profile[sizeof(video_avc_profile) - 1] = '\0';
          int matched = 0;
          unsigned int i;
          for (i = 0; i < sizeof(video_avc_profile_options) / sizeof(video_avc_profile_option); i++) {
            if (strcmp(video_avc_profile_options[i].name, video_avc_profile) == 0) {
              matched = 1;
              break;
            }
          }
          if (!matched) {
            log_fatal("error: invalid avcprofile: %s\n", optarg);
            return EXIT_FAILURE;
          }
        } else if (strcmp(long_options[option_index].name, "avclevel") == 0) {
          strncpy(video_avc_level, optarg, sizeof(video_avc_level) - 1);
          video_avc_level[sizeof(video_avc_level) - 1] = '\0';
          int matched = 0;
          unsigned int i;
          for (i = 0; i < sizeof(video_avc_level_options) / sizeof(video_avc_level_option); i++) {
            if (strcmp(video_avc_level_options[i].name, video_avc_level) == 0) {
              matched = 1;
              break;
            }
          }
          if (!matched) {
            log_fatal("error: invalid avclevel: %s\n", optarg);
            return EXIT_FAILURE;
          }
        } else if (strcmp(long_options[option_index].name, "qpmin") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid qpmin: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0 || value > 51) {
            log_fatal("error: invalid qpmin: %d (must be 0 <= qpmin <= 51)\n", value);
            return EXIT_FAILURE;
          }
          video_qp_min = value;
        } else if (strcmp(long_options[option_index].name, "qpmax") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid qpmax: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0 || value > 51) {
            log_fatal("error: invalid qpmax: %d (must be 0 <= qpmax <= 51)\n", value);
            return EXIT_FAILURE;
          }
          video_qp_max = value;
        } else if (strcmp(long_options[option_index].name, "qpinit") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid qpinit: %s\n", optarg);
            return EXIT_FAILURE;
          }
          video_qp_initial = value;
        } else if (strcmp(long_options[option_index].name, "dquant") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid dquant: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid dquant: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          video_slice_dquant = value;
        } else if (strcmp(long_options[option_index].name, "alsadev") == 0) {
          strncpy(alsa_dev, optarg, sizeof(alsa_dev) - 1);
          alsa_dev[sizeof(alsa_dev) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "rtspout") == 0) {
          is_rtspout_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "rtspvideocontrol") == 0) {
          strncpy(rtsp_video_control_path, optarg, sizeof(rtsp_video_control_path) - 1);
          rtsp_video_control_path[sizeof(rtsp_video_control_path) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "rtspaudiocontrol") == 0) {
          strncpy(rtsp_audio_control_path, optarg, sizeof(rtsp_audio_control_path) - 1);
          rtsp_audio_control_path[sizeof(rtsp_audio_control_path) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "rtspvideodata") == 0) {
          strncpy(rtsp_video_data_path, optarg, sizeof(rtsp_video_data_path) - 1);
          rtsp_video_data_path[sizeof(rtsp_video_data_path) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "rtspaudiodata") == 0) {
          strncpy(rtsp_audio_data_path, optarg, sizeof(rtsp_audio_data_path) - 1);
          rtsp_audio_data_path[sizeof(rtsp_audio_data_path) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "tcpout") == 0) {
          is_tcpout_enabled = 1;
          strncpy(tcp_output_dest, optarg, sizeof(tcp_output_dest) - 1);
          tcp_output_dest[sizeof(tcp_output_dest) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "vfr") == 0) {
          is_vfr_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "camera") == 0) {
          char *end;
          // todo
          int value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid --camera: %s\n", optarg);
            return EXIT_FAILURE;
          }
          // We allow only >= 0
          if (value < 0) {
            log_fatal("error: invalid --camera: %d (must be 0 or greater)\n", value);
            return EXIT_FAILURE;
          }
          camera_id = value;
        } else if (strcmp(long_options[option_index].name, "autoex") == 0) {
          is_auto_exposure_enabled = 1;
          is_vfr_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "autoexthreshold") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid autoexthreshold: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          auto_exposure_threshold = value;
          is_auto_exposure_enabled = 1;
          is_vfr_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "wb") == 0) {
          strncpy(white_balance, optarg, sizeof(white_balance) - 1);
          white_balance[sizeof(white_balance) - 1] = '\0';
          int matched = 0;
          unsigned int i;
          for (i = 0; i < sizeof(white_balance_options) / sizeof(white_balance_option); i++) {
            if (strcmp(white_balance_options[i].name, white_balance) == 0) {
              matched = 1;
              break;
            }
          }
          if (!matched) {
            log_fatal("error: invalid white balance: %s\n", optarg);
            return EXIT_FAILURE;
          }
        } else if (strcmp(long_options[option_index].name, "ex") == 0) {
          strncpy(exposure_control, optarg, sizeof(exposure_control) - 1);
          exposure_control[sizeof(exposure_control) - 1] = '\0';
          int matched = 0;
          unsigned int i;
          for (i = 0; i < sizeof(exposure_control_options) / sizeof(exposure_control_option); i++) {
            if (strcmp(exposure_control_options[i].name, exposure_control) == 0) {
              matched = 1;
              break;
            }
          }
          if (!matched) {
            log_fatal("error: invalid --ex: %s\n", optarg);
            return EXIT_FAILURE;
          }
          is_vfr_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "wbred") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid --wbred: %s\n", optarg);
            return EXIT_FAILURE;
          }
          awb_red_gain = value;
          strcpy(white_balance, "off"); // Turns off AWB
        } else if (strcmp(long_options[option_index].name, "wbblue") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid --wbblue: %s\n", optarg);
            return EXIT_FAILURE;
          }
          awb_blue_gain = value;
          strcpy(white_balance, "off"); // Turns off AWB
        } else if (strcmp(long_options[option_index].name, "metering") == 0) {
          strncpy(exposure_metering, optarg, sizeof(exposure_metering) - 1);
          exposure_metering[sizeof(exposure_metering) - 1] = '\0';
          int matched = 0;
          unsigned int i;
          for (i = 0; i < sizeof(exposure_metering_options) / sizeof(exposure_metering_option); i++) {
            if (strcmp(exposure_metering_options[i].name, exposure_metering) == 0) {
              matched = 1;
              break;
            }
          }
          if (!matched) {
            log_fatal("error: invalid metering: %s\n", optarg);
            return EXIT_FAILURE;
          }
        } else if (strcmp(long_options[option_index].name, "evcomp") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid evcomp: %s\n", optarg);
            return EXIT_FAILURE;
          }
          manual_exposure_compensation = 1;
          exposure_compensation = value;
        } else if (strcmp(long_options[option_index].name, "aperture") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid aperture: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid aperture: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          manual_exposure_aperture = 1;
          exposure_aperture = value;
        } else if (strcmp(long_options[option_index].name, "shutter") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid shutter speed: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid shutter speed: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          manual_exposure_shutter_speed = 1;
          exposure_shutter_speed = value;
          is_vfr_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "iso") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid ISO sensitivity: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid ISO sensitivity: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          manual_exposure_sensitivity = 1;
          exposure_sensitivity = value;
        } else if (strcmp(long_options[option_index].name, "roi") == 0) {
          char *end;
          int i;
          float values[4];
          char *str_ptr = optarg;
          for (i = 0; i < 4; i++) {
            values[i] = strtof(str_ptr, &end);
            if (end == str_ptr || errno == ERANGE) { // parse error
              log_fatal("error: invalid --roi: value must be in x,y,width,height format\n");
              return EXIT_FAILURE;
            }
            if (values[i] < 0.0f || values[i] > 1.0f) {
              log_fatal("error: invalid --roi: %f (must be in the range of 0.0-1.0)\n", values[i]);
              return EXIT_FAILURE;
            }
            str_ptr = end + 1;
          }
          if (*end != '\0') {
            log_fatal("error: invalid --roi: value must be in x,y,width,height format\n");
            return EXIT_FAILURE;
          }
          roi_left = values[0];
          roi_top = values[1];
          roi_width = values[2];
          roi_height = values[3];
        } else if (strcmp(long_options[option_index].name, "minfps") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid minfps: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid minfps: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          min_fps = value;
          is_vfr_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "maxfps") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid maxfps: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid maxfps: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          max_fps = value;
          is_vfr_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "query") == 0) {
          query_and_exit = 1;
        } else if (strcmp(long_options[option_index].name, "time") == 0) {
          is_timestamp_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "timeformat") == 0) {
          strncpy(timestamp_format, optarg, sizeof(timestamp_format) - 1);
          timestamp_format[sizeof(timestamp_format) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "timelayout") == 0) {
          char *comma_p;
          char *search_p = optarg;
          size_t optarg_len = strlen(optarg);
          int param_len;
          int layout_align = 0;
          while (1) {
            comma_p = strchr(search_p, ',');
            if (comma_p == NULL) {
              param_len = optarg + optarg_len - search_p;
            } else {
              param_len = comma_p - search_p;
            }
            if (strncmp(search_p, "top", param_len) == 0) {
              layout_align |= LAYOUT_ALIGN_TOP;
            } else if (strncmp(search_p, "middle", param_len) == 0) {
              layout_align |= LAYOUT_ALIGN_MIDDLE;
            } else if (strncmp(search_p, "bottom", param_len) == 0) {
              layout_align |= LAYOUT_ALIGN_BOTTOM;
            } else if (strncmp(search_p, "left", param_len) == 0) {
              layout_align |= LAYOUT_ALIGN_LEFT;
            } else if (strncmp(search_p, "center", param_len) == 0) {
              layout_align |= LAYOUT_ALIGN_CENTER;
            } else if (strncmp(search_p, "right", param_len) == 0) {
              layout_align |= LAYOUT_ALIGN_RIGHT;
            } else {
              log_fatal("error: invalid timelayout found at: %s\n", search_p);
              return EXIT_FAILURE;
            }
            if (comma_p == NULL || optarg + optarg_len - 1 - comma_p <= 0) { // no remaining chars
              break;
            }
            search_p = comma_p + 1;
          }
          timestamp_layout = (LAYOUT_ALIGN) layout_align;
        } else if (strcmp(long_options[option_index].name, "timehorizmargin") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timehorizmargin: %s\n", optarg);
            return EXIT_FAILURE;
          }
          timestamp_horizontal_margin = value;
        } else if (strcmp(long_options[option_index].name, "timevertmargin") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timevertmargin: %s\n", optarg);
            return EXIT_FAILURE;
          }
          timestamp_vertical_margin = value;
        } else if (strcmp(long_options[option_index].name, "timepos") == 0) {
          char *comma_p = strchr(optarg, ',');
          if (comma_p == NULL) {
            log_fatal("error: invalid timepos format: %s (should be <x>,<y>)\n", optarg);
            return EXIT_FAILURE;
          }

          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || end != comma_p || errno == ERANGE) { // parse error
            log_fatal("error: invalid timepos x: %s\n", optarg);
            return EXIT_FAILURE;
          }
          timestamp_pos_x = value;

          value = strtol(comma_p+1, &end, 10);
          if (end == comma_p+1 || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timepos y: %s\n", comma_p+1);
            return EXIT_FAILURE;
          }
          timestamp_pos_y = value;

          is_timestamp_abs_pos_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "timealign") == 0) {
          char *comma_p;
          char *search_p = optarg;
          size_t optarg_len = strlen(optarg);
          int param_len;
          int text_align = 0;
          while (1) {
            comma_p = strchr(search_p, ',');
            if (comma_p == NULL) {
              param_len = optarg + optarg_len - search_p;
            } else {
              param_len = comma_p - search_p;
            }
            if (strncmp(search_p, "left", param_len) == 0) {
              text_align |= TEXT_ALIGN_LEFT;
            } else if (strncmp(search_p, "center", param_len) == 0) {
              text_align |= TEXT_ALIGN_CENTER;
            } else if (strncmp(search_p, "right", param_len) == 0) {
              text_align |= TEXT_ALIGN_RIGHT;
            } else {
              log_fatal("error: invalid timealign found at: %s\n", search_p);
              return EXIT_FAILURE;
            }
            if (comma_p == NULL || optarg + optarg_len - 1 - comma_p <= 0) { // no remaining chars
              break;
            }
            search_p = comma_p + 1;
          }
          this->timestamp_text_align = (TEXT_ALIGN) text_align;
        } else if (strcmp(long_options[option_index].name, "timefontname") == 0) {
          strncpy(timestamp_font_name, optarg, sizeof(timestamp_font_name) - 1);
          timestamp_font_name[sizeof(timestamp_font_name) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "timefontfile") == 0) {
          strncpy(timestamp_font_file, optarg, sizeof(timestamp_font_file) - 1);
          timestamp_font_file[sizeof(timestamp_font_file) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "timefontface") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timefontface: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid timefontface: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          timestamp_font_face_index = value;
        } else if (strcmp(long_options[option_index].name, "timept") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timept: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value <= 0.0f) {
            log_fatal("error: invalid timept: %.1f (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          timestamp_font_points = value;
        } else if (strcmp(long_options[option_index].name, "timedpi") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timedpi: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid timedpi: %d (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          timestamp_font_dpi = value;
        } else if (strcmp(long_options[option_index].name, "timecolor") == 0) {
          char *end;
          long value = strtol(optarg, &end, 16);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timecolor: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid timecolor: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          timestamp_color = value;
        } else if (strcmp(long_options[option_index].name, "timestrokecolor") == 0) {
          char *end;
          long value = strtol(optarg, &end, 16);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timecolor: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid timecolor: %d (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          timestamp_stroke_color = value;
        } else if (strcmp(long_options[option_index].name, "timestrokewidth") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timestrokewidth: %s\n", optarg);
            return EXIT_FAILURE;
          }
          if (value < 0.0f) {
            log_fatal("error: invalid timestrokewidth: %.1f (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          timestamp_stroke_width = value;
        } else if (strcmp(long_options[option_index].name, "timespacing") == 0) {
          char *end;
          long value = strtol(optarg, &end, 16);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid timespacing: %s\n", optarg);
            return EXIT_FAILURE;
          }
          timestamp_letter_spacing = value;
        } else if (strcmp(long_options[option_index].name, "statedir") == 0) {
          strncpy(state_dir, optarg, sizeof(state_dir) - 1);
          state_dir[sizeof(state_dir) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "hooksdir") == 0) {
          strncpy(hooks_dir, optarg, sizeof(hooks_dir) - 1);
          hooks_dir[sizeof(hooks_dir) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "volume") == 0) {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid volume: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value < 0.0) {
            log_fatal("error: invalid volume: %.1f (must be >= 0.0)\n", value);
            return EXIT_FAILURE;
          }
          audio_volume_multiply = value;
        } else if (strcmp(long_options[option_index].name, "noaudio") == 0) {
          disable_audio_capturing = 1;
        } else if (strcmp(long_options[option_index].name, "audiopreview") == 0) {
          is_audio_preview_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "audiopreviewdev") == 0) {
          strncpy(audio_preview_dev, optarg, sizeof(audio_preview_dev) - 1);
          audio_preview_dev[sizeof(audio_preview_dev) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "hlskeyframespersegment") == 0) { 
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid hlskeyframespersegment: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid hlskeyframespersegment: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          hls_keyframes_per_segment = (int) value;
        } else if (strcmp(long_options[option_index].name, "hlsnumberofsegments") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid hlsnumberofsegments: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid hlsnumberofsegments: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          hls_number_of_segments = (int) value;
        } else if (strcmp(long_options[option_index].name, "hlsenc") == 0) {
          is_hls_encryption_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "hlsenckeyuri") == 0) {
          strncpy(hls_encryption_key_uri, optarg, sizeof(hls_encryption_key_uri) - 1);
          hls_encryption_key_uri[sizeof(hls_encryption_key_uri) - 1] = '\0';
        } else if (strcmp(long_options[option_index].name, "hlsenckey") == 0) {
          int i;
          for (i = 0; i < 16; i++) {
            unsigned int value;
            if (sscanf(optarg + i * 2, "%02x", &value) == 1) {
              hls_encryption_key[i] = value;
            } else {
              log_fatal("error: invalid hlsenckey: %s\n", optarg);
              print_usage();
              return EXIT_FAILURE;
            }
          }
        } else if (strcmp(long_options[option_index].name, "hlsenciv") == 0) {
          int i;
          for (i = 0; i < 16; i++) {
            unsigned int value;
            if (sscanf(optarg + i * 2, "%02x", &value) == 1) {
              hls_encryption_iv[i] = value;
            } else {
              log_fatal("error: invalid hlsenciv: %s\n", optarg);
              print_usage();
              return EXIT_FAILURE;
            }
          }
        } else if (strcmp(long_options[option_index].name, "previewrect") == 0) {
          char *token;
          char *saveptr = NULL;
          char *end;
          int i;
          long value;
          for (i = 1; ; i++, optarg = NULL) {
            token = strtok_r(optarg, ",", &saveptr);
            if (token == NULL) {
              break;
            }
            value = strtol(token, &end, 10);
            if (end == token || *end != '\0' || errno == ERANGE) { // parse error
              log_fatal("error: invalid previewrect number: %s\n", token);
              return EXIT_FAILURE;
            }
            switch (i) {
              case 1:
                preview_x = value;
                break;
              case 2:
                preview_y = value;
                break;
              case 3:
                preview_width = value;
                break;
              case 4:
                preview_height = value;
                break;
              default: // too many tokens
                log_fatal("error: invalid previewrect\n");
                return EXIT_FAILURE;
            }
          }
          if (i != 5) { // too few tokens
            log_fatal("error: invalid previewrect\n");
            return EXIT_FAILURE;
          }
          is_preview_enabled = 1;
          is_previewrect_enabled = 1;
        } else if (strcmp(long_options[option_index].name, "hdmi") == 0) {
          char *end;
          int value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid hdmi: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          // We allow only HDMI 0 or 1
          if (value != 0 && value != 1) {
            log_fatal("error: invalid hdmi: %d (must be 0 or 1)\n", value);
            return EXIT_FAILURE;
          }
          preview_hdmi = value;
        // } else if (strcmp(long_options[option_index].name, "blank") == 0) {
        //   blank_background_color = optarg ? strtoul(optarg, NULL, 0) : BLANK_BACKGROUND_DEFAULT;
        // } else if (strcmp(long_options[option_index].name, "opacity") == 0) {
        //   char *end;
        //   int value = strtol(optarg, &end, 10);
        //   if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
        //     log_fatal("error: invalid opacity: %s\n", optarg);
        //     print_usage();
        //     return EXIT_FAILURE;
        //   }
        //   preview_opacity = value;
        } else if (strcmp(long_options[option_index].name, "recordbuf") == 0) {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid recordbuf: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value < 1) {
            log_fatal("error: invalid recordbuf: %ld (must be >= 1)\n", value);
            return EXIT_FAILURE;
          }
          record_buffer_keyframes = value;
        } else if (strcmp(long_options[option_index].name, "verbose") == 0) {
          log_set_level(LOG_LEVEL_DEBUG);
        } else if (strcmp(long_options[option_index].name, "version") == 0) {
          this->show_version = true;
          return 0;
        } else if (strcmp(long_options[option_index].name, "help") == 0) {
          this->show_help = true;
          return 0;
        }
        break;
      case 'w':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid width: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid width: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          video_width = value;
          break;
        }
      case 'h':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid height: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid height: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          video_height = value;
          break;
        }
      case 'f':
        {
          char *end;
          double value = strtod(optarg, &end);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid fps: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0.0) {
            log_fatal("error: invalid fps: %.1f (must be > 0.0)\n", value);
            return EXIT_FAILURE;
          }
          video_fps = value;
          break;
        }
      case 'g':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid gopsize: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid gopsize: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          video_gop_size = value;
          is_video_gop_size_specified = 1;
          break;
        }
      case 'v':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid videobitrate: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value < 0) {
            log_fatal("error: invalid videobitrate: %ld (must be >= 0)\n", value);
            return EXIT_FAILURE;
          }
          video_bitrate = value;
          break;
        }
      case 'a':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid audiobitrate: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid audiobitrate: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          audio_bitrate = value;
          break;
        }
      case 'c':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid channels: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value != 1 && value != 2) {
            log_fatal("error: invalid channels: %ld (must be 1 or 2)\n", value);
            return EXIT_FAILURE;
          }
          audio_channels = value;
          is_audio_channels_specified = 1;
          break;
        }
      case 'r':
        {
          char *end;
          long value = strtol(optarg, &end, 10);
          if (end == optarg || *end != '\0' || errno == ERANGE) { // parse error
            log_fatal("error: invalid samplerate: %s\n", optarg);
            print_usage();
            return EXIT_FAILURE;
          }
          if (value <= 0) {
            log_fatal("error: invalid samplerate: %ld (must be > 0)\n", value);
            return EXIT_FAILURE;
          }
          audio_sample_rate = value;
          break;
        }
      case 'o':
        is_hlsout_enabled = 1;
        strncpy(hls_output_dir, optarg, sizeof(hls_output_dir) - 1);
        hls_output_dir[sizeof(hls_output_dir) - 1] = '\0';
        break;
      case 'p':
        is_preview_enabled = 1;
        break;
      case 'q':
        log_set_level(LOG_LEVEL_ERROR);
        break;
      default:
        print_usage();
        return EXIT_FAILURE;
    }
  }

  if (is_vfr_enabled &&
      (min_fps != -1.0f || max_fps != -1.0f) &&
      (float) video_width / (float) video_height >= 1.45) {
    log_warn("warning: --minfps and --maxfps might not work because width (%d) / height (%d) >= approx 1.45\n", video_width, video_height);
  }

  if (!is_video_pts_step_specified) {
    video_pts_step = round(90000 / video_fps);

    // It appears that the minimum fps is 1.31
    if (video_pts_step > 68480) {
      video_pts_step = 68480;
    }
  }
  if (!is_video_gop_size_specified) {
    video_gop_size = ceil(video_fps);
  }

  audio_min_value = (int) (-32768 / audio_volume_multiply);
  audio_max_value = (int) (32767 / audio_volume_multiply);

  log_debug("video_width=%d\n", video_width);
  log_debug("video_height=%d\n", video_height);
  log_debug("video_fps=%f\n", video_fps);
  log_debug("video_pts_step=%d\n", video_pts_step);
  log_debug("video_gop_size=%d\n", video_gop_size);
  // log_debug("video_rotation=%d\n", video_rotation);
  log_debug("video_hflip=%d\n", video_hflip);
  log_debug("video_vflip=%d\n", video_vflip);
  log_debug("video_bitrate=%ld\n", video_bitrate);
  log_debug("video_avc_profile=%s\n", video_avc_profile);
  log_debug("video_avc_level=%s\n", video_avc_level);
  log_debug("video_qp_min=%d\n", video_qp_min);
  log_debug("video_qp_max=%d\n", video_qp_max);
  log_debug("video_qp_initial=%d\n", video_qp_initial);
  log_debug("video_slice_dquant=%d\n", video_slice_dquant);
  log_debug("alsa_dev=%s\n", alsa_dev);
  log_debug("audio_channels=%d\n", audio_channels);
  log_debug("audio_sample_rate=%d\n", audio_sample_rate);
  log_debug("audio_bitrate=%ld\n", audio_bitrate);
  log_debug("audio_volume_multiply=%f\n", audio_volume_multiply);
  log_debug("is_hlsout_enabled=%d\n", is_hlsout_enabled);
  log_debug("is_hls_encryption_enabled=%d\n", is_hls_encryption_enabled);
  log_debug("hls_keyframes_per_segment=%d\n", hls_keyframes_per_segment);
  log_debug("hls_number_of_segments=%d\n", hls_number_of_segments);
  log_debug("hls_encryption_key_uri=%s\n", hls_encryption_key_uri);
  log_debug("hls_encryption_key=0x");
  log_hex(LOG_LEVEL_DEBUG, hls_encryption_key, sizeof(hls_encryption_key));
  log_debug("\n");
  log_debug("hls_encryption_iv=0x");
  log_hex(LOG_LEVEL_DEBUG, hls_encryption_iv, sizeof(hls_encryption_iv));
  log_debug("\n");
  log_debug("hls_output_dir=%s\n", hls_output_dir);
  log_debug("rtsp_enabled=%d\n", is_rtspout_enabled);
  log_debug("rtsp_video_control_path=%s\n", rtsp_video_control_path);
  log_debug("rtsp_audio_control_path=%s\n", rtsp_audio_control_path);
  log_debug("rtsp_video_data_path=%s\n", rtsp_video_data_path);
  log_debug("rtsp_audio_data_path=%s\n", rtsp_audio_data_path);
  log_debug("tcp_enabled=%d\n", is_tcpout_enabled);
  log_debug("tcp_output_dest=%s\n", tcp_output_dest);
  log_debug("auto_exposure_enabled=%d\n", is_auto_exposure_enabled);
  log_debug("auto_exposure_threshold=%f\n", auto_exposure_threshold);
  log_debug("is_vfr_enabled=%d\n", is_vfr_enabled);
  log_debug("white_balance=%s\n", white_balance);
  log_debug("exposure_control=%s\n", exposure_control);
  log_debug("awb_red_gain=%f\n", awb_red_gain);
  log_debug("awb_blue_gain=%f\n", awb_blue_gain);
  log_debug("metering=%s\n", exposure_metering);
  log_debug("manual_exposure_compensation=%d\n", manual_exposure_compensation);
  log_debug("exposure_compensation=%f\n", exposure_compensation);
  log_debug("manual_exposure_aperture=%d\n", manual_exposure_aperture);
  log_debug("exposure_aperture=%u\n", exposure_aperture);
  log_debug("manual_exposure_shutter_speed=%d\n", manual_exposure_shutter_speed);
  log_debug("exposure_shutter_speed=%u\n", exposure_shutter_speed);
  log_debug("manual_exposure_sensitivity=%d\n", manual_exposure_sensitivity);
  log_debug("exposure_sensitivity=%u\n", exposure_sensitivity);
  log_debug("roi_left=%f\n", roi_left);
  log_debug("roi_top=%f\n", roi_top);
  log_debug("roi_width=%f\n", roi_width);
  log_debug("roi_height=%f\n", roi_height);
  log_debug("min_fps=%f\n", min_fps);
  log_debug("max_fps=%f\n", max_fps);
  log_debug("is_timestamp_enabled=%d\n", is_timestamp_enabled);
  log_debug("timestamp_format=%s\n", timestamp_format);
  log_debug("timestamp_layout=%d\n", timestamp_layout);
  log_debug("timestamp_horizontal_margin=%d\n", timestamp_horizontal_margin);
  log_debug("timestamp_vertical_margin=%d\n", timestamp_vertical_margin);
  log_debug("is_timestamp_abs_pos_enabled=%d\n", is_timestamp_abs_pos_enabled);
  log_debug("timestamp_pos_x=%d\n", timestamp_pos_x);
  log_debug("timestamp_pos_y=%d\n", timestamp_pos_y);
  log_debug("timestamp_text_align=%d\n", timestamp_text_align);
  log_debug("timestamp_font_name=%s\n", timestamp_font_name);
  log_debug("timestamp_font_file=%s\n", timestamp_font_file);
  log_debug("timestamp_font_face_index=%s\n", timestamp_font_face_index);
  log_debug("timestamp_font_points=%1f\n", timestamp_font_points);
  log_debug("timestamp_font_dpi=%d\n", timestamp_font_dpi);
  log_debug("timestamp_color=%06x\n", timestamp_color);
  log_debug("timestamp_stroke_color=%06x\n", timestamp_stroke_color);
  log_debug("timestamp_stroke_width=%.f\n", timestamp_stroke_width);
  log_debug("timestamp_letter_spacing=%d\n", timestamp_letter_spacing);
  log_debug("is_preview_enabled=%d\n", is_preview_enabled);
  log_debug("is_previewrect_enabled=%d\n", is_previewrect_enabled);
  log_debug("preview_x=%d\n", preview_x);
  log_debug("preview_y=%d\n", preview_y);
  log_debug("preview_width=%d\n", preview_width);
  log_debug("preview_height=%d\n", preview_height);
  // log_debug("preview_opacity=%d\n", preview_opacity);
  log_debug("preview_hdmi=%d\n", preview_hdmi);
  // log_debug("blank_background_color=0x%x\n", blank_background_color);
  log_debug("is_audio_preview_enabled=%d\n", is_audio_preview_enabled);
  log_debug("audio_preview_dev=%s\n", audio_preview_dev);
  log_debug("record_buffer_keyframes=%d\n", record_buffer_keyframes);
  log_debug("state_dir=%s\n", state_dir);
  log_debug("hooks_dir=%s\n", hooks_dir);

  return 0;
}