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

Build and install these libraries for a Raspberry Pi:

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

Create the output directory for HTTP Live Streaming:

    $ mkdir /run/shm/video

Then run the process:

    $ cd picam
    $ ./stream.bin

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

### Recommended hardware

#### USB microphone

In combination with:
- [PLANEX PL-US35AP](http://www.planex.co.jp/product/usb/pl-us35ap/)
- [ELECOM MS-STM95](http://www2.elecom.co.jp/multimedia/microphone/ms-stm95/)

### License

LGPL v2.1, as some parts of FFmpeg source is included.
