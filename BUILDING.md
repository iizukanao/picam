# picam build instructions

This file explains how to build picam yourself. The whole process takes under an hour on Raspberry Pi 2 or 3.


# Steps

## Install required packages

```sh
$ sudo apt-get install git libasound2-dev libssl-dev libfontconfig1-dev libharfbuzz-dev
```

(NOTE: `$` denotes command prompt. Do not enter `$` when entering commands.)


## Build and install fdk-aac

Install [fdk-aac](https://sourceforge.net/projects/opencore-amr/files/fdk-aac/) with the following commands.

```sh
$ wget https://downloads.sourceforge.net/project/opencore-amr/fdk-aac/fdk-aac-2.0.2.tar.gz
$ tar zxvf fdk-aac-2.0.2.tar.gz
$ cd fdk-aac-2.0.2
$ ./configure
$ make -j4
(takes 3-4 minutes)
$ sudo make install
```


## Build and install ffmpeg

NOTE: **Do not use `apt-get` for installing ffmpeg.**

Download ffmpeg source and configure it:

```sh
$ git clone https://git.ffmpeg.org/ffmpeg.git
$ cd ffmpeg
$ ./configure --enable-libfdk-aac
(takes about one minute)
```

In the output of `configure`, make sure that there is `libfdk_aac` in `Enabled encoders`.

Run the following commands to build and install ffmpeg.

```sh
$ make -j4
(takes 25-40 minutes)
$ sudo make install
```

Run `ldconfig` in order to resolve dynamic linker problems.

```sh
$ sudo ldconfig
```


## Build picam

```sh
$ git clone https://github.com/iizukanao/picam.git
$ cd picam
$ mkdir build
$ cd build
$ cmake ..
$ make -j4
```

You can save some disk space by running `strip`.

```sh
$ strip picam
```

Check if picam runs without errors.

```
$ ./picam --help
```
