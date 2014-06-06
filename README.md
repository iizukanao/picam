### picam

#### Features

- Capture video from Raspberry Pi Camera (up to 720p) and encode it to H.264 using hardware encoder
- Capture audio from USB microphone via ALSA and encode it to AAC-LC using libavcodec (FFmpeg)
- Record to an MPEG-TS file
- Generate files for HTTP Live Streaming with optional encryption
- If USB microphone is not available, it only records the video


### Required hardware

- Raspberry Pi
- Raspberry Pi Camera Board
- USB microphone (see [Recommended hardware](#recommended-hardware))


### Installation

#### Install libraries

Build and install these libraries on a Raspberry Pi (Raspbian):

- [alsa-lib](http://www.alsa-project.org/main/index.php/Main_Page)
- [fdk-aac](http://sourceforge.net/projects/opencore-amr/)
- [FFmpeg](http://www.ffmpeg.org/) with "--enable-libfdk-aac" configure option

The output of `ffmpeg -codecs | grep aac` should contain the following line:

     DEA.L. aac                  AAC (Advanced Audio Coding) (decoders: aac libfdk_aac ) (encoders: aac libfdk_aac )

#### Compile libilclient

On a Raspberry Pi, issue the following command:

    $ cd /opt/vc/src/hello_pi/libs/ilclient
    $ make

#### Compile picam

    $ git clone https://github.com/iizukanao/picam.git
    $ cd picam
    $ make

If you want to save some disk space, run:

    $ strip picam


### Usage

picam command continuously reads video from Raspberry Pi Camera and audio from ALSA.

    $ ./picam
    video_width=1280
    video_height=720
    video_fps=30.0
    gop_size=30
    video_bitrate=2000000
    alsa_dev=hw:0,0
    audio_sample_rate=48000
    audio_bitrate=40000
    audio_volume_multiply=1.000000
    is_hlsout_enabled=0
    is_hls_encryption_enabled=0
    hls_encryption_key_uri=stream.key
    hls_encryption_key=0x75b0a81de17487c88a47507a7e1fdf73
    hls_encryption_iv=0x000102030405060708090a0b0c0d0e0f
    hls_output_dir=/run/shm/video
    rtsp_enabled=0
    rtsp_video_control_path=/tmp/node_rtsp_rtmp_videoControl
    rtsp_audio_control_path=/tmp/node_rtsp_rtmp_audioControl
    rtsp_video_data_path=/tmp/node_rtsp_rtmp_videoData
    rtsp_audio_data_path=/tmp/node_rtsp_rtmp_audioData
    tcp_enabled=0
    tcp_output_dest=
    auto_exposure_enabled=0
    exposure_night_y_threshold=40
    exposure_auto_y_threshold=50
    is_preview_enabled=0
    record_buffer_keyframes=5
    state_dir=state
    hooks_dir=hooks
    exposure mode: auto
    ................................ 31.46 fps k=1
    .............................. 30.49 fps k=2
    .............................. 30.55 fps k=3

#### Recording

To start recording, create a file named `hooks/start_record` while picam command is running.

    $ touch hooks/start_record

You will see `start rec` in the picam command output.

To stop recording, create a file named `hooks/stop_record`.

    $ touch hooks/stop_record

The recorded MPEG-TS file is in rec/archive/ directory.

To convert MPEG-TS to MP4, run:

    $ ffmpeg -i test.ts -c:v copy -c:a copy -bsf:a aac_adtstoasc test.mp4

#### Mute/Unmute

To mute microphone temporarily, create a file named `hooks/mute`.

    $ touch hooks/mute

To unmute microphone, create a file named `hooks/unmute`.

    $ touch hooks/unmute

#### Command options

    $ ./picam --help
    picam version 1.0.0
    Usage: picam [options]

    Options:
     [video]
      -w, --width         Width in pixels (default: 1280)
      -h, --height        Height in pixels (default: 720)
      -v, --videobitrate  Video bit rate (default: 2000000)
      -g, --gopsize       GOP size (default: 30)
     [audio]
      -r, --samplerate    Audio sample rate (default: 48000)
      -a, --audiobitrate  Audio bit rate (default: 40000)
      --alsadev <dev>     ALSA microphone device (default: hw:0,0)
      --volume <num>      Amplify audio by multiplying the volume by <num>
                          (default: 1.0)
     [HTTP Live Streaming (HLS)]
      -o, --hlsdir <dir>  Generate HTTP Live Streaming files in <dir>
      --hlsenc            Enable HLS encryption
      --hlsenckeyuri <uri>  Set HLS encryption key URI (default: stream.key)
      --hlsenckey <hex>   Set HLS encryption key in hex string
                          (default: 75b0a81de17487c88a47507a7e1fdf73)
      --hlsenciv <hex>    Set HLS encryption IV in hex string
                          (default: 000102030405060708090a0b0c0d0e0f)
     [output for node-rtsp-rtmp-server]
      --rtspout           Enable output for node-rtsp-rtmp-server
      --rtspvideocontrol <path>  Set video control socket path
                          (default: /tmp/node_rtsp_rtmp_videoControl)
      --rtspaudiocontrol <path>  Set audio control socket path
                          (default: /tmp/node_rtsp_rtmp_audioControl)
      --rtspvideodata <path>  Set video data socket path
                          (default: /tmp/node_rtsp_rtmp_videoData)
      --rtspaudiodata <path>  Set audio data socket path
                          (default: /tmp/node_rtsp_rtmp_audioData)
     [MPEG-TS output via TCP]
      --tcpout <url>      Enable TCP output to <url>
                          (e.g. --tcpout tcp://127.0.0.1:8181)
     [camera]
      --autoexposure      Enable automatic changing of exposure
      --expnight <num>    Change the exposure to night mode if the average
                          value of Y (brightness) is <= <num> while in
                          daylight mode (default: 40)
      --expday <num>      Change the exposure to daylight mode if the average
                          value of Y (brightness) is >= <num> while in
                          night mode (default: 50)
      -p, --preview       Display a preview window for video
     [misc]
      --recordbuf <num>   Start recording from <num> keyframes ago
                          (default: 5)
      --statedir <dir>    Set state dir (default: state)
      --hooksdir <dir>    Set hooks dir (default: hooks)
      -q, --quiet         Turn off most of the log messages
      --help              Print this help


### HTTP Live Streaming

HTTP Live Streaming is disabled by default. To enable HTTP Live Streaming and generate files in /run/shm/hls, run:

    $ ./picam -o /run/shm/hls

#### Encryption

Optionally you can enable encryption for HTTP Live Streaming. We will use the following settings as an example.

- **HTTP Live Streaming output directory**: /run/shm/hls/
- **Encryption key**: 0xf0f1f2f3f4f5f6f7f8f9fafbfcfdfeff
- **Encryption IV**:  0x000102030405060708090a0b0c0d0e0f
- **Encryption key file**: enc.key

First you have to create a file named "enc.key" which contains 16-byte encryption key. To create such file, run:

    $ echo -n $'\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff' > enc.key

Put enc.key in /run/shm/hls/ directory. Then, run picam with the following options:

    $ ./picam -o /run/shm/hls --hlsenc --hlsenckeyuri enc.key \
      --hlsenckey f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff \
      --hlsenciv 000102030405060708090a0b0c0d0e0f

You can watch the HTTP Live Streaming by accessing /run/shm/hls/index.m3u8 via HTTP or HTTPS with QuickTime Player.


### Using picam in combination with nginx-rtmp-module

To use picam with [nginx-rtmp-module](https://github.com/arut/nginx-rtmp-module), add the following lines to nginx.conf:

    rtmp {
        server {
            listen 1935;
            chunk_size 4000;
            application webcam {
                live on;

                exec_static /path/to/ffmpeg -i tcp://127.0.0.1:8181?listen
                                            -c:v copy -ar 44100 -ab 40000
                                            -f flv rtmp://localhost:1935/webcam/mystream;
            }
        }
    }

Note that `/path/to/ffmpeg` should be replaced with the actual absolute path to ffmpeg command.

Start nginx server, then run:

    $ ./picam --tcpout tcp://127.0.0.1:8181

You can access your live stream at `rtmp://YOUR_RASPBERRYPI_IP/webcam/mystream`.


### Recommended hardware

#### USB microphone

Any cheap USB microphone should work as long as it is supported by ALSA. I have tested this program with the combination of:

- USB to 3.5mm audio adapter: [PLANEX PL-US35AP](http://www.planex.co.jp/product/usb/pl-us35ap/)
- Microphone: [ELECOM MS-STM95](http://www2.elecom.co.jp/multimedia/microphone/ms-stm95/)

### License

LGPL v2.1, as some parts of FFmpeg source is included.
