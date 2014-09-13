# picam Compilation Guide

This document describes how to build picam. The whole process takes about 2-3 hours with a standard Linux box.


## Install crosstool-ng

First we need to install crosstool-ng on a powerful Linux machine (not Raspberry Pi) to be able to cross-compile ffmpeg.

    $ mkdir ~/pi
    $ cd ~/pi
    $ wget http://crosstool-ng.org/download/crosstool-ng/crosstool-ng-1.20.0.tar.bz2
    $ tar jxvf crosstool-ng-1.20.0.tar.bz2
    $ cd crosstool-ng-1.20.0
    $ ./configure --prefix=/opt/cross
    $ make
    $ sudo make install
    $ export PATH=$PATH:/opt/cross/bin
    $ cd ..
    $ mkdir ctng
    $ cd ctng
    $ ct-ng menuconfig

Then a configuration screen appears. The followings are for crosstool-ng 1.20.0, and it may not be applicable to later versions.

- Paths and misc options
    - Check "Try features marked as EXPERIMENTAL"
    - Set "Prefix directory" to "/opt/cross/x-tools/${CT_TARGET}"
- Target options
    - Set "Target Architecture" to "arm"
    - Set "Endianness" to "Little endian"
    - Set "Bitness" to "32-bit"
    - Set "Architecture level" to "armv6zk"
    - Set "Emit assembly for CPU" to "arm1176jzf-s"
    - Set "Tune for CPU" to "arm1176jzf-s"
    - Set "Use specific FPU" to "vfp"
    - Set "Floating point" to "hardware (FPU)"
    - Set "Default instruction set mode" to "arm"
    - Check "Use EABI"
- Toolchain options
    - Set "Tuple's vendor string" to "rpi"
- Operating System
    - Set "Target OS" to "linux"
- Binary utilities
    - Set "Binary format" to "ELF"
    - Set "binutils version" to "2.22"
- C-library
    - Set "C library" to "eglibc"
    - Set "eglibc version" to "2_13"
- C compiler
    - Check "Show Linaro versions"
    - Set "gcc version" to "linaro-4.8-2014.01"
    - Check "C++" under "Additional supported languages"
    - Set "gcc extra config" to "--with-float=hard"
    - Check "Link libstdc++ statically into the gcc binary"

Save the configuration and build.

    $ sudo chown -R $USER /opt/cross
    $ ct-ng build
    $ export CCPREFIX="/opt/cross/x-tools/arm-rpi-linux-gnueabi/bin/arm-rpi-linux-gnueabi-"


## Build fdk-aac

We will use $HOME/pi/ as a working directory and put compiled binaries and header files in $HOME/pi/build/. Set the environment variable $PIBUILD.

    $ export PIBUILD=$HOME/pi/build
    $ mkdir $PIBUILD
    $ cd ~/pi

Download fdk-aac-0.1.3.tar.gz (or the latest version) from http://sourceforge.net/projects/opencore-amr/.

    $ tar zxvf fdk-aac-0.1.3.tar.gz
    $ cd fdk-aac-0.1.3
    $ CC=${CCPREFIX}gcc CXX=${CCPREFIX}g++ ./configure --host=arm-rpi-linux-gnueabi --prefix=$PIBUILD
    $ make
    $ make install


## Build ffmpeg

You cannot use ffmpeg that can be installed via `apt-get`.

Before compiling ffmpeg, you have to copy ALSA header files and shared libraries from Raspberry Pi. Log in to Raspberry Pi and install **libasound2** and **libasound2-dev** via `apt-get`. Then you have /usr/include/alsa/ and /usr/lib/arm-linux-gnueabihf/libasound.so*. Copy those files from Raspberry Pi, and put them in $HOME/pi/ so that you have $HOME/pi/usr/include/ and $HOME/pi/usr/lib/. Set another environment variable $PIUSR.

    $ export PIUSR=$HOME/pi/usr
    $ ls $PIUSR
    include  lib

Next, fetch ffmpeg source and configure it:

    $ cd ~/pi
    $ git clone git://source.ffmpeg.org/ffmpeg.git
    $ cd ffmpeg
    $ PKG_CONFIG_PATH=$PKG_CONFIG_PATH:$PIBUILD/lib/pkgconfig CC=${CCPREFIX}gcc CXX=${CCPREFIX}g++ ./configure --enable-cross-compile --cross-prefix=${CCPREFIX} --arch=armel --target-os=linux --prefix=$PIBUILD --extra-cflags="-I$PIBUILD/include -I$PIUSR/include -fPIC" --extra-ldflags="-L$PIBUILD/lib -L$PIUSR/lib -fPIC" --enable-libfdk-aac --pkg-config=`which pkg-config`

In the output of `configure`, make sure that:

- "Enabled indevs" has "alsa"
- "Enabled encoders" has "libfdk_aac"

If all goes well, build ffmpeg.

    $ make
    $ make install


## Transfer the files to Raspberry Pi

Transfer the contents of $PIBUILD directory to Raspberry Pi, then put them in /usr/local/ so that you have /usr/local/bin/ffmpeg.

If you have transferred the files to ~/build/, you can install them like this:

    $ cd ~/build
    $ ls
    bin  include  lib  share
    $ sudo rsync -rav ./ /usr/local/

After you install the files, run `ldconfig` on Raspberry Pi.

    $ sudo ldconfig

Run `ffmpeg -codecs | grep fdk` and make sure that the output has this line:

     DEA.L. aac                  AAC (Advanced Audio Coding) (decoders: aac libfdk_aac ) (encoders: aac libfdk_aac )

We have done with the powerful machine which was used for cross compiling.


## Build libilclient

From now on, all steps are performed on Raspberry Pi.

    $ cd /opt/vc/src/hello_pi/libs/ilclient
    $ make

If you do not have the above directory, download the firmware from https://github.com/raspberrypi/firmware and copy all the contents inside /opt/vc/src/.


## Build picam

    $ git clone https://github.com/iizukanao/picam.git
    $ cd picam
    $ make

If you want to save some disk space, strip the binary.

    $ strip picam

Check that picam runs without errors.

    $ ./picam --help


## Configuration for Arch Linux

If you are using Arch Linux, you need to have these two lines in /boot/config.txt.

    start_file=start_x.elf
    fixup_file=fixup_x.dat

Also, assign at least 128MB for GPU memory.

    gpu_mem_512=128

Reboot the Raspberry Pi for the changes to take effect.
