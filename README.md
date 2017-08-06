### picam

### Features

- Generate H.264/AAC encoded MPEG-TS file from Raspberry Pi Camera (v1/v2) and optionally USB microphone or Wolfson Audio Card
- Generate HTTP Live Streaming files with optional encryption
- Add timestamp
- Display Unicode text with arbitrary font

### Performance (Latency)

Time from real motion to playback on Strobe Media Player over RTMP:

| Video bitrate | Minimum latency |
| ------------: | --------------: |
| 300 Kbps      |         0.3 sec |
| 500 Kbps      |         0.6 sec |
|   1 Mbps      |         0.8 sec |
|   2 Mbps      |         1.0 sec |
|   3 Mbps      |         1.3 sec |

**In HTTP Live Streaming (HLS), the latency will never go below 3-4 seconds.** This limitation stems from the design of HLS.

The above results were tested with:

- Video: 1280x720, 30 fps, GOP size 30
- Audio: 48 Khz mono, 40 Kbps
- RTMP Server: [node-rtsp-rtmp-server](https://github.com/iizukanao/node-rtsp-rtmp-server)
- Client: Flash Player 14,0,0,145 on Firefox 31.0 for Mac, using [test/strobe_media_playback.html](https://github.com/iizukanao/picam/blob/master/test/strobe_media_playback.html)
- Network: Wi-Fi network created by a USB dongle attached to Raspberry Pi


### Required hardware

- Raspberry Pi
- Raspberry Pi Camera Board v1 or v2
- (optionally) USB microphone or Wolfson Audio Card


### Supported operating systems

- Raspbian
- Arch Linux

### Installation

Binary release is available at https://github.com/iizukanao/picam/releases/latest

Also, out-of-the-box SD card image for live streaming (picam + Raspbian + live streaming server) is available at https://github.com/iizukanao/picam-streamer

If you want to build picam yourself, see [INSTALL.md](INSTALL.md).


### Using a binary release

The fastest way to use picam is to use a binary release. To set up and use it, run the following commands on your Raspberry Pi (Raspbian). It will set up picam in `~/picam/`.

```bash
# If you have not enabled camera, enable it with raspi-config then reboot
sudo raspi-config

# Install dependencies
sudo apt-get update
sudo apt-get install libharfbuzz0b libfontconfig1

# Create directories and symbolic links
cat > make_dirs.sh <<'EOF'
#!/bin/bash
DEST_DIR=~/picam
SHM_DIR=/run/shm

mkdir -p $SHM_DIR/rec
mkdir -p $SHM_DIR/hooks
mkdir -p $SHM_DIR/state
mkdir -p $DEST_DIR/archive

ln -sfn $DEST_DIR/archive $SHM_DIR/rec/archive
ln -sfn $SHM_DIR/rec $DEST_DIR/rec
ln -sfn $SHM_DIR/hooks $DEST_DIR/hooks
ln -sfn $SHM_DIR/state $DEST_DIR/state
EOF

chmod +x make_dirs.sh
./make_dirs.sh

# Optionally, increase microphone volume with alsamixer
alsamixer

# Install picam binary
wget https://github.com/iizukanao/picam/releases/download/v1.4.6/picam-1.4.6-binary.tar.xz
tar xvf picam-1.4.6-binary.tar.xz
cp picam-1.4.6-binary/picam ~/picam/

# Run picam
cd ~/picam
./picam --alsadev hw:1,0
```


### Usage

#### Create symbolic links (optional, but strongly recommended)

You can take advantage of RAM drive (`/run/shm/`) and reduce access to SD card. It also provides better quality of recording.

First, stop picam if it is running. Create **rec**, **hooks**, and **state** directories in `/run/shm/`, then change directories with the same name in picam to symbolic links. Create another symbolic link from `/run/shm/rec/archive` to somewhere on SD card.

Result:

    picam
    | ...
    |-- archive
    |-- hooks -> /run/shm/hooks
    |-- rec -> /run/shm/rec
    `-- state -> /run/shm/state

    /run/shm/
    |-- hooks
    |-- rec
    |   |-- archive -> /home/pi/picam/archive
    |   `-- tmp (automatically created by picam)
    `-- state


#### Finding ALSA device name

First, find ALSA device name of your microphone.

    $ arecord -l
    **** List of CAPTURE Hardware Devices ****
    card 1: Device [USB PnP Sound Device], device 0: USB Audio [USB Audio]
      Subdevices: 1/1
      Subdevice #0: subdevice #0

ALSA device name is consisted of `hw:<card>,<device>`. In the above example, the ALSA device name is `hw:1,0`.

If you got `no soundcards found` error, try `sudo arecord -l`. If that output looks good, you might want to add your user to `audio` group.

    $ sudo usermod -a -G audio $USER
    (once logout, then login)
    $ groups
    wheel audio pi  <-- (make sure that 'audio' is in the list)
    $ arecord -l
    **** List of CAPTURE Hardware Devices ****
    card 1: Device [USB PnP Sound Device], device 0: USB Audio [USB Audio]
      Subdevices: 1/1
      Subdevice #0: subdevice #0

#### Starting picam

Run picam with your ALSA device name.

    $ ./picam --alsadev hw:1,0
    configuring devices
    capturing started


#### Recording

To start recording, create a file named `hooks/start_record` while picam command is running.

    $ touch hooks/start_record

You will see smth like `disk_usage=23% start rec to rec/archive/2017-08-05_16-41-52.ts` in the picam command output.

To stop recording, create a file named `hooks/stop_record`.

    $ touch hooks/stop_record

You will see `stop rec` in the picam command output.

The recorded MPEG-TS file is in `rec/archive/` directory.

To convert MPEG-TS to MP4, run:

```bash
ffmpeg -i test.ts -c:v copy -c:a copy -bsf:a aac_adtstoasc test.mp4
# or
avconv -i test.ts -c:v copy -c:a copy -bsf:a aac_adtstoasc test.mp4
```

#### Mute/Unmute

To mute microphone temporarily, create a file named `hooks/mute`.

    $ touch hooks/mute

To unmute microphone, create a file named `hooks/unmute`.

    $ touch hooks/unmute

#### Command options

```
$ picam --help
picam version 1.4.6
Usage: picam [options]

Options:
 [video]
  -w, --width <num>   Width in pixels (default: 1280)
  -h, --height <num>  Height in pixels (default: 720)
  -v, --videobitrate <num>  Video bit rate (default: 2000000)
                      Set 0 to disable rate control
  -f, --fps <num>     Frame rate (default: 30.0)
  -g, --gopsize <num>  GOP size (default: same value as fps)
  --vfr               Enable variable frame rate. GOP size will be
                      dynamically controlled.
  --minfps <num>      Minimum frames per second. Implies --vfr.
                      It might not work if width / height >= 1.45.
  --maxfps <num>      Maximum frames per second. Implies --vfr.
                      It might not work if width / height >= 1.45.
  --rotation <num>    Image rotation in clockwise degrees
                      (0, 90, 180, 270)
  --hflip             Flip image horizontally
  --vflip             Flip image vertically
  --avcprofile <str>  Set AVC/H.264 profile to one of:
                      constrained_baseline/baseline/main/high
                      (default: constrained_baseline)
  --avclevel <value>  Set AVC/H.264 level (default: 3.1)
  --qpmin <num>       Minimum quantization level (0..51)
  --qpmax <num>       Maximum quantization level (0..51)
  --qpinit <num>      Initial quantization level
  --dquant <num>      Slice DQuant level
 [audio]
  -c, --channels <num>  Audio channels (1=mono, 2=stereo)
                      Default is mono. If it fails, stereo is used.
  -r, --samplerate <num>  Audio sample rate (default: 48000)
  -a, --audiobitrate <num>  Audio bit rate (default: 40000)
  --alsadev <dev>     ALSA microphone device (default: hw:0,0)
  --volume <num>      Amplify audio by multiplying the volume by <num>
                      (default: 1.0)
  --noaudio           Disable audio capturing
  --audiopreview      Enable audio preview
  --audiopreviewdev <dev>  Audio preview output device (default: plughw:0,0)
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
  --autoex            Enable automatic control of camera exposure between
                      daylight and night modes. This forces --vfr enabled.
  --autoexthreshold <num>  When average value of Y (brightness) for
                      10 milliseconds of captured image falls below <num>,
                      camera exposure will change to night mode. Otherwise
                      camera exposure is in daylight mode. Implies --autoex.
                      (default: 5.0)
                      If --verbose option is enabled as well, average value of
                      Y is printed like y=28.0.
  --ex <value>        Set camera exposure. Implies --vfr. <value> is one of:
                        off auto night nightpreview backlight spotlight sports
                        snow beach verylong fixedfps antishake fireworks
                        largeaperture smallaperture
  --wb <value>        Set white balance. <value> is one of:
                        off: Disable white balance control
                        auto: Automatic white balance control (default)
                        sun: The sun provides the light source
                        cloudy: The sun provides the light source through clouds
                        shade: Light source is the sun and scene is in the shade
                        tungsten: Light source is tungsten
                        fluorescent: Light source is fluorescent
                        incandescent: Light source is incandescent
                        flash: Light source is a flash
                        horizon: Light source is the sun on the horizon
  --wbred <num>       Red gain. Implies "--wb off". (0.0 .. 8.0)
  --wbblue <num>      Blue gain. Implies "--wb off". (0.0 .. 8.0)
  --metering <value>  Set metering type. <value> is one of:
                        average: Center weight average metering (default)
                        spot: Spot (partial) metering
                        matrix: Matrix or evaluative metering
                        backlit: Assume a backlit image
  --evcomp <num>      Set Exposure Value compensation (-24..24) (default: 0)
  --shutter <num>     Set shutter speed in microseconds (default: auto).
                      Implies --vfr.
  --iso <num>         Set ISO sensitivity (100..800) (default: auto)
  --roi <x,y,w,h>     Set region of interest (crop rect) in ratio (0.0-1.0)
                      (default: 0,0,1,1)
  -p, --preview       Display fullscreen preview
  --previewrect <x,y,width,height>
                      Display preview window at specified position
  --opacity           Preview window opacity
                      (0=transparent..255=opaque; default=255)
  --blank[=0xAARRGGBB]  Set the video background color to black (or optional ARGB value)
  --query             Query camera capabilities then exit
 [timestamp] (may be a bit heavy on Raspberry Pi 1)
  --time              Enable timestamp
  --timeformat <spec>  Timestamp format (see "man strftime" for spec)
                       (default: "%a %b %d %l:%M:%S %p")
  --timelayout <spec>  Timestamp position (relative mode)
                       layout is comma-separated list of:
                        top middle bottom  left center right
                       (default: bottom,right)
  --timehorizmargin <px>  Horizontal margin from edge (default: 10).
                          Effective only if --timelayout is used.
  --timevertmargin <px>  Vertical margin from edge (default: 10).
                         Effective only if --timelayout is used.
  --timepos <x,y>     Timestamp position (absolute mode)
  --timefontname <name>  Timestamp font name (default: FreeMono:style=Bold)
  --timefontfile <file>  Timestamp font file. This invalidates --timefontname.
  --timefontface <num>  Timestamp font face index (default: 0).
                        Effective only if --timefontfile is used.
  --timept <pt>       Text size in points (default: 14.0)
  --timedpi <num>     DPI for calculating text size (default: 96)
  --timecolor <hex>   Text color (default: ffffff)
  --timestrokecolor <hex>  Text stroke color (default: 000000)
                      Note that texts are rendered in grayscale.
  --timestrokewidth <pt>  Text stroke border radius (default: 1.3).
                          To disable stroking borders, set this value to 0.
  --timespacing <px>  Additional letter spacing (default: 0)
 [misc]
  --recordbuf <num>   Start recording from <num> keyframes ago
                      (must be >= 1; default: 5)
  --statedir <dir>    Set state dir (default: state)
  --hooksdir <dir>    Set hooks dir (default: hooks)
  -q, --quiet         Suppress all output except errors
  --verbose           Enable verbose output
  --version           Print program version
  --help              Print this help
```

#### White balance

Camera white balance can be set either via command line option (e.g. `--wb sun`) or hooks. To change the white balance while picam is running, create `hooks/wb_<value>`, where `<value>` is the name of white balance.

For example, the following command will dynamically change the white balance to **sun**.

    $ touch hooks/wb_sun

Available white balance modes are: **off**, **auto**, **sun**, **cloudy**, **shade**, **tungsten**, **fluorescent**, **incandescent**, **flash**, and **horizon**.


#### Exposure Control

Camera exposure control can be set either via command line option (e.g. `--ex night`) or hooks. To change the exposure control while picam is running, start picam with `--vfr` or `--ex` option, then create `hooks/ex_<value>`, where `<value>` is the name of exposure control.

For example, the following command will dynamically change the exposure control to **night**.

    $ touch hooks/ex_night

For the list of available exposure control values, see `picam --help`.

| Value | Description |
| ----- | ----------- |
| `off` | Disable exposure control |
| `auto` | Automatic exposure |
| `night` | Exposure at night |
| `nightpreview` | Shorter exposure than `night` |
| `backlight` | Exposure with backlight illuminating the subject |
| `spotlight` | Exposure with a spotlight illuminating the subject |
| `sports` | Exposure for sports |
| `snow` | Exposure for the subject in snow |
| `beach` | Exposure for the subject at a beach |
| `verylong` | Long exposure |
| `fixedfps` | Constrain FPS to a fixed value |
| `antishake` | Antishake mode |
| `fireworks` | Optimized for fireworks |
| `largeaperture` | Exposure when using a large aperture on the camera |
| `smallaperture` | Exposure when using a small aperture on the camera |


#### Recordbuf

`recordbuf` is a parameter which controls how many past keyframes should be included at the start of a recording. For example, `recordbuf=1` means that a recording will start from the last keyframe, and `recordbuf=2` means that a recording will start from the second last keyframe relative to when `hooks/start_record` is created. The minimum value of `recordbuf` is 1.

##### Global and per-recording recordbuf

*Added in version 1.4.0*

There are two types of recordbuf; global and per-recording. Global recordbuf is the default value for all recordings. Per-recording recordbuf only applies to the current recording. Per-recording recordbuf must be less than or equal to global recordbuf.

##### Setting global recordbuf

Global recordbuf can be specified by either `--recordbuf` option or hooks/set_recordbuf.

```bash
# Set global recordbuf to 30
echo 30 > hooks/set_recordbuf
```

##### Setting per-recording recordbuf

Per-recording recordbuf has a default value which is the same value as global recordbuf. Per-recording recordbuf can be specified via `hooks/start_record`.

    # Start recording with per-recording recordbuf set to 2
    $ echo recordbuf=2 > hooks/start_record

#### Overlaying text (subtitle)

*Added in version 1.4.0*

picam can display text with correct ligatures and kerning, with a font of your choice. To display a text, create hooks/subtitle.

    $ echo 'text=Houston, we have a problem' > hooks/subtitle

[<img src="https://github.com/iizukanao/picam/raw/master/images/subtitle_intro_small.png" alt="Subtitle example image" style="max-width:100%;" width="500" height="281"></a>](https://github.com/iizukanao/picam/raw/master/images/subtitle_intro.png)

Each line of hooks/subtitle must be in the format of `key=value`. Lines starting with `#` will be ignored. Supported keys are:

| Key | Description | Default value |
| :-- | :---------- | :------------ |
| text | UTF-8 encoded text. `\n` will be treated as a line break. | (none) |
| font_name | Font name which can be recognized by Fontconfig | sans |
| font_file | Path to font file. If this is specified, **font_name** will not be used. | (none) |
| face_index | Font face index. Effective only if **font_file** is specified. | 0 |
| pt | Text size in points | 28.0 |
| dpi | DPI for calculating text size | 96 |
| duration | Number of seconds the text appears on the screen. If this is set to 0, the text will be displayed indefinitely. | 7.0 |
| layout_align | Layout alignment of the text box on the screen. Comma-separated list of: top middle bottom  left center right | bottom,center |
| horizontal_margin | Horizontal margin from the nearest edge in pixels. Does nothing when **pos** is specified or **layout_align** has "center". | 0 |
| vertical_margin | Vertical margin from the nearest edge in pixels. Does nothing when **pos** is specified or **layout_align** has "middle". | 35 |
| pos | Absolute position of the text box on the screen. This invalidates **layout_align** settings. | (none) |
| text_align | Text alignment inside the positioned box (left, center, right) | center |
| line_height | Line spacing is multiplied by this number | 1.0 |
| tab_scale | Tab width is multiplied by this number | 1.0 |
| letter_spacing | Additional letter spacing in pixels | 0 |
| color | Text color in hex color code. Rendered in grayscale. | ffffff |
| stroke_color | Text stroke (outline) color in hex color code. Rendered in grayscale. | 000000 |
| stroke_width | Text stroke (outline) border radius in points | 1.0 |
| in_preview | Visibility of the text in the preview | 1 |
| in_video | Visibility of the text in the encoded video | 1 |

NOTE: On the first generation models of Raspberry Pi (before Pi 2), subtitles cause CPU usage high and the video frame rate may drop below 30 fps.

##### Examples

    $ cat example1
    text=What goes up\nmust come down\nfinally floor AV Wa
    font_name=serif
    pt=40
    $ cat example1 > hooks/subtitle

[<img src="https://github.com/iizukanao/picam/raw/master/images/subtitle_example1_small.png" alt="Subtitle example 1" style="max-width:100%;" width="500" height="281"></a>](https://github.com/iizukanao/picam/raw/master/images/subtitle_example1.png)

    $ cat example2
    text=お気の毒ですが\n冒険の書は\n消えちゃいました☆
    font_file=/home/pi/uzura.ttf
    pt=46
    $ cat example2 > hooks/subtitle

[<img src="https://github.com/iizukanao/picam/raw/master/images/subtitle_example2_small.png" alt="Subtitle example 2" style="max-width:100%;" width="500" height="281"></a>](https://github.com/iizukanao/picam/raw/master/images/subtitle_example2.png)

    $ cat example3
    text=♨☀♻♥⚠
    font_file=/home/pi/NotoSansCJKjp-Regular.otf
    pt=120
    layout_align=middle,center
    letter_spacing=40
    $ cat example3 > hooks/subtitle

[<img src="https://github.com/iizukanao/picam/raw/master/images/subtitle_example3_small.png" alt="Subtitle example 3" style="max-width:100%;" width="500" height="281"></a>](https://github.com/iizukanao/picam/raw/master/images/subtitle_example3.png)

    $ cat example4
    text=●REC
    font_name=FreeSans
    pt=40
    layout_align=top,right
    horizontal_margin=30
    vertical_margin=30
    color=000000
    stroke_width=0
    duration=0
    $ cat example4 > hooks/subtitle

[<img src="https://github.com/iizukanao/picam/raw/master/images/subtitle_example4_small.png" alt="Subtitle example 4" style="max-width:100%;" width="500" height="281"></a>](https://github.com/iizukanao/picam/raw/master/images/subtitle_example4.png)

#### Changing the filename for recording

*Added in version 1.4.0*

To change the directory and/or filename for the recorded file, specify `dir` and/or `filename` parameters in `hooks/start_record`.

    # Start recording to /tmp/myout.ts
    $ echo -e "dir=/tmp\nfilename=myout.ts" > hooks/start_record

#### Determine the length of a recorded file

*Added in version 1.4.0*

The file state/*recorded_filename* has some info about the recording.

    $ cat state/2015-11-19_01-18-09.ts
    duration_pts=2083530
    duration_sec=23.150333

You can remove `state/*.ts` files if you do not need them.


### HTTP Live Streaming (HLS)

HTTP Live Streaming is disabled by default. To enable HTTP Live Streaming and generate files in /run/shm/hls, run:

    $ ./picam -o /run/shm/hls

#### Serving HLS

[Set up nginx](https://www.raspberrypi.org/documentation/remote-access/web-server/nginx.md) (ignore "Additional - Install PHP" step), then open */etc/nginx/sites-available/default* with a text editor and add the following code inside `server { ... }` block.

```
	location /hls/ {
		root /run/shm;
	}
```

Restart the nginx server with `sudo service nginx restart` then run picam with `-o /run/shm/hls` option. The HLS will be available at http://YOUR-PI-IP/hls/index.m3u8

#### Enabling encryption

Optionally you can enable encryption for HTTP Live Streaming. We will use the following settings as an example.

- **HTTP Live Streaming output directory**: `/run/shm/hls/`
- **Encryption key**: `0xf0f1f2f3f4f5f6f7f8f9fafbfcfdfeff`
- **Encryption IV**:  `0x000102030405060708090a0b0c0d0e0f`
- **Encryption key file**: `enc.key`

First you have to create a file named `enc.key` which contains 16-byte encryption key. To create such file, run:

    $ echo -n $'\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff' > enc.key

Put `enc.key` in `/run/shm/hls/` directory. Then, run picam with the following options:

    $ ./picam -o /run/shm/hls --hlsenc --hlsenckeyuri enc.key \
      --hlsenckey f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff \
      --hlsenciv 000102030405060708090a0b0c0d0e0f

You can watch the HTTP Live Streaming by accessing `/run/shm/hls/index.m3u8` via HTTP or HTTPS with QuickTime Player.


### Using picam in combination with nginx-rtmp-module

To use picam with [nginx-rtmp-module](https://github.com/arut/nginx-rtmp-module), add the following lines to `nginx.conf`:

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


### Publishing to Ustream

To upload streams from picam to Ustream, run ffmpeg with the following options. `RTMP_URL` and `STREAM_KEY` can be obtained from Ustream's Channel settings &rarr; Broadcast settings &rarr; Encoder settings.

    $ ffmpeg -i tcp://127.0.0.1:8181?listen -c:v copy -c:a aac -strict -2 -ar 44100 -ab 40000 -f flv RTMP_URL/STREAM_KEY

<img src="https://github.com/iizukanao/picam/raw/master/images/ustream.png" alt="Encoder settings on Ustream" style="max-width:100%;" width="600" height="459">

Then, run picam to start streaming.

    $ picam --tcpout tcp://127.0.0.1:8181


### Recommended hardware

#### USB microphone

Any cheap USB microphone should work as long as it is supported by ALSA. I have tested this program with the combination of:

- USB to 3.5mm audio adapter: [PLANEX PL-US35AP](http://www.planex.co.jp/product/usb/pl-us35ap/)
- Microphone: [ELECOM MS-STM95](http://www2.elecom.co.jp/multimedia/microphone/ms-stm95/)

### License

LGPL v2.1, as some parts of FFmpeg source is included.

### Contributors

- Linus Styrén ([@Linkaan](https://github.com/Linkaan))
- White Moustache ([@nalajcie](https://github.com/nalajcie))
