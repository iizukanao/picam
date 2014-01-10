### picam

#### Features

- Capture video from Raspberry Pi Camera
- Capture audio from USB microphone
- Mux video and audio stream into MPEG-TS format

### Required hardware

- Raspberry Pi
- Raspberry Pi Camera Board
- USB microphone (see [Recommended hardware](#recommended-hardware))

### Installation

#### Install libraries

Build and install these libraries for a Raspberry Pi:

- ffmpeg
- libasound
- libfdk-aac

#### Compile libilclient

On a Raspberry Pi, issue the following command:

    $ cd /opt/vc/src/hello_pi/libs/ilclient
    $ make

#### Compile picam

    $ git clone https://github.com/iizukanao/picam.git
    $ cd picam
    $ make
    $ ./stream.bin

### Recommended hardware

#### USB microphone

In combination with:
- [PLANEX PL-US35AP](http://www.planex.co.jp/product/usb/pl-us35ap/)
- [ELECOM MS-STM95](http://www2.elecom.co.jp/multimedia/microphone/ms-stm95/)

### License

LGPL v2.1, as some parts of FFmpeg source is included.
