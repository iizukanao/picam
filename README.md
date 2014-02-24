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

(Documentation is in progress)

#### Install libraries

Build and install these libraries on a Raspberry Pi:

- [FFmpeg](http://www.ffmpeg.org/)
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

### Starting the process

    $ cd picam
    $ ./stream.bin

Make sure that [node-rtsp-rtmp-server](https://github.com/iizukanao/node-rtsp-rtmp-server) is running before starting stream.bin. Alternatively you can [use nginx-rtmp-module](#using-picam-in-combination-with-nginx-rtmp-module), or [use HTTP Live Streaming only](#using-http-live-streaming-only).

### Recording

#### Start recording

    $ cd picam
    $ touch hooks/start_record

#### Stop recording

    $ cd picam
    $ touch hooks/stop_record

### Mute/Unmute

To mute:

    $ touch hooks/mute

To unmute:

    $ touch hooks/unmute

Recorded MPEG-TS file will go in `archive` directory.

### Using picam in combination with nginx-rtmp-module

To use picam with [nginx-rtmp-module](https://github.com/arut/nginx-rtmp-module), make changes to the two constants in config.h like this:

    #define ENABLE_UNIX_SOCKETS_OUTPUT 0
    #define ENABLE_TCP_OUTPUT 1

Run `make` to build stream.bin. Add the following lines to nginx.conf:

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

Start nginx server, then run stream.bin. You can access the RTMP stream at `rtmp://YOUR_RASPBERRYPI_IP/webcam/mystream`.

### HTTP Live Streaming

HTTP Live Streaming files are generated in `/run/shm/video`. You can change the output directory by changing `HLS_OUTPUT_DIR` in config.h.

If you want to turn off HTTP Live Streaming, set `ENABLE_HLS_OUTPUT` to 0 in config.h, then run `make`.

### Using HTTP Live Streaming only

To enable HTTP Live Streaming only and disable other output, change the constants in config.h as follows:

    #define ENABLE_HLS_OUTPUT  1
    #define ENABLE_UNIX_SOCKETS_OUTPUT  0
    #define ENABLE_TCP_OUTPUT  0

Then run `make`.

### Recommended hardware

#### USB microphone

I have tested this program with the combination of:

- USB to 3.5mm audio adapter: [PLANEX PL-US35AP](http://www.planex.co.jp/product/usb/pl-us35ap/)
- Microphone: [ELECOM MS-STM95](http://www2.elecom.co.jp/multimedia/microphone/ms-stm95/)

### License

LGPL v2.1, as some parts of FFmpeg source is included.
