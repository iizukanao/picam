### picam

#### Features

- Capture video from Raspberry Pi Camera (up to 720p)
- Encode the video to H.264 using hardware encoder
- Capture audio from USB microphone via ALSA
- Encode the audio to AAC-LC using libavcodec
- Mux video and audio stream into MPEG-TS format
- Segment MPEG-TS and produce the files for HTTP Live Streaming
- Optional encryption for HTTP Live Streaming
- PTS/DTS alignment
- Audio/video sync within 30 milliseconds on normal condition
- Start/stop recording and MPEG-TS file output via file hooks
- Start recording from 5 seconds before
- Mute/unmute via file hooks
- Supports audio+video and video-only recording
- Auto exposure adjustment
- Send H.264/AAC data with timestamps via UNIX domain socket to another process


### Required hardware

- Raspberry Pi
- Raspberry Pi Camera Board
- USB microphone (see [Recommended hardware](#recommended-hardware))


### Installation

#### Install libraries

Build and install these libraries on a Raspberry Pi:

- [FFmpeg](http://www.ffmpeg.org/) ([compilation guide for Raspberry Pi](http://trac.ffmpeg.org/wiki/CompilationGuide/RaspberryPi))
- [alsa-lib](http://www.alsa-project.org/main/index.php/Main_Page)
- [fdk-aac](http://sourceforge.net/projects/opencore-amr/)

#### Compile libilclient

On a Raspberry Pi, issue the following command:

    $ cd /opt/vc/src/hello_pi/libs/ilclient
    $ make

#### Compile picam

    $ git clone https://github.com/iizukanao/picam.git
    $ cd picam
    $ make


### Usage

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


### Recording

#### Start recording

    $ cd picam
    $ touch hooks/start_record

#### Stop recording

    $ cd picam
    $ touch hooks/stop_record

Recorded files are generated in rec/ directory.


### Mute/Unmute

To mute microphone temporarily:

    $ touch hooks/mute

To unmute microphone:

    $ touch hooks/unmute


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


### HTTP Live Streaming

HTTP Live Streaming is disabled by default. To enable HTTP Live Streaming and generate files in /run/shm/hls, run:

    $ ./picam -o /run/shm/hls

#### Encryption

Optionally you can enable encryption for HTTP Live Streaming. We will use the following settings as an example.

- **HTTP Live Streaming output directory**: /run/shm/hls/
- **Encryption key**: 0xf0f1f2f3f4f5f6f7f8f9fafbfcfdfeff
- **Encryption IV**:  0x000102030405060708090a0b0c0d0e0f

First you have to create a file named "enc.key" which contains 16-byte encryption key. To create such file, run:

    $ echo -n $'\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff' > enc.key

Put enc.key in /run/shm/hls/ directory. Then, run picam with the following options:

    $ ./picam -o /run/shm/hls --hlsenc --hlsenckeyuri enc.key \
      --hlsenckey f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff \
      --hlsenciv 000102030405060708090a0b0c0d0e0f

You can watch the HTTP Live Streaming by accessing /run/shm/hls/index.m3u8 via HTTP or HTTPS with QuickTime Player.


### Recommended hardware

#### USB microphone

Any cheap USB microphone should work as long as it is supported by ALSA. I have tested this program with the combination of:

- USB to 3.5mm audio adapter: [PLANEX PL-US35AP](http://www.planex.co.jp/product/usb/pl-us35ap/)
- Microphone: [ELECOM MS-STM95](http://www2.elecom.co.jp/multimedia/microphone/ms-stm95/)

### License

LGPL v2.1, as some parts of FFmpeg source is included.
