# picam cross-compiling guide

This file explains how to build picam on low-performance models such as Raspberry Pi 1 or Zero. The whole process takes about 2-3 hours on a standard Linux box.

## For Raspberry Pi 2 or 3 users

If you have Raspberry Pi 2 or 3, you can build picam without additional machine. Please refer to [BUILDING.md](BUILDING.md) which is much easier than cross compiling.

## Prerequisites

We need to install some packages used to build crosstool-ng

### On Debian based systems

    $ apt-get install flex bison automake gperf libtool patch texinfo ncurses-dev help2man

### On Fedora based systems

    $ yum install flex bison automake gperf libtool texinfo patch gcc gcc-c++ gmp-devel help2man
    $ yum install ncurses-devel ncurses glibc-devel glibc-static libstdc++-static


## Install crosstool-ng

First we need to install crosstool-ng on a powerful Linux machine (not the Raspberry Pi) to be able to cross-compile ffmpeg.

    $ mkdir ~/pi
    $ cd ~/pi
    $ wget http://crosstool-ng.org/download/crosstool-ng/crosstool-ng-1.22.0.tar.xz
    $ tar xvf crosstool-ng-1.22.0.tar.xz
    $ cd crosstool-ng
    $ ./configure --prefix=/opt/cross
    $ make
    $ sudo make install
    $ export PATH=$PATH:/opt/cross/bin
    $ cd ..
    $ mkdir ctng
    $ cd ctng
    $ ct-ng menuconfig

Then we need to configure the cross compiler for our Raspberry Pi version. The following configurations are for crosstool-ng 1.22.0, and they may not be applicable to later versions.

### For Raspberry Pi generation 1 (A, B, A+, B+, zero)

- Paths and misc options
    - Check "Try features marked as EXPERIMENTAL"
    - Set "Prefix directory" to "/opt/cross/x-tools/${CT_TARGET}"
    - (OPTIONAL) Set "Number of parallel jobs" to amount of cores your processor has
- Target options
    - Set "Target Architecture" to "arm"
    - Set "Endianness" to "Little endian"
    - Set "Bitness" to "32-bit"
    - Set "Emit assembly for CPU" to "arm1176jzf-s"
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
    - Set "binutils version" to "2.25.1"
- C-library
    - Set "C library" to "glibc"
    - Set "glibc version" to "2.22"
- C compiler
    - Check "Show Linaro versions"
    - Set "gcc version" to "linaro-4.9-2015.06"
    - Set "gcc extra config" to "--with-float=hard"
    - Check "Link libstdc++ statically into the gcc binary"
    - Check "C++" under "Additional supported languages"

### For Raspberry Pi generation 2

- Paths and misc options
    - Check "Try features marked as EXPERIMENTAL"
    - Set "Prefix directory" to "/opt/cross/x-tools/${CT_TARGET}"
    - (OPTIONAL) Set "Number of parallel jobs" to amount of cores your processor has
- Target options
    - Set "Target Architecture" to "arm"
    - Set "Endianness" to "Little endian"
    - Set "Bitness" to "32-bit"
    - Set "Emit assembly for CPU" to "cortex-a7"
    - Set "Use specific FPU" to "neon-vfpv4"
    - Set "Floating point" to "hardware (FPU)"
    - Set "Default instruction set mode" to "arm"
    - Check "Use EABI"
- Toolchain options
    - Set "Tuple's vendor string" to "rpi"
- Operating System
    - Set "Target OS" to "linux"
- Binary utilities
    - Set "Binary format" to "ELF"
    - Set "binutils version" to "2.25.1"
- C-library
    - Set "C library" to "glibc"
    - Set "glibc version" to "2.22"
- C compiler
    - Check "Show Linaro versions"
    - Set "gcc version" to "linaro-4.9-2015.06"
    - Set "gcc extra config" to "--with-float=hard"
    - Check "Link libstdc++ statically into the gcc binary"
    - Check "C++" under "Additional supported languages"

Save the configuration and build.

    $ sudo chown -R $USER /opt/cross
    $ ct-ng build
    $ export CCPREFIX="/opt/cross/x-tools/arm-rpi-linux-gnueabihf/bin/arm-rpi-linux-gnueabihf-"


## Build fdk-aac

We will use $HOME/pi/ as a working directory and put compiled binaries and header files in $HOME/pi/build/. Set the environment variable $PIBUILD.

    $ export PIBUILD=$HOME/pi/build
    $ mkdir $PIBUILD
    $ cd ~/pi

Download fdk-aac-0.1.5.tar.gz (or the latest version) from https://sourceforge.net/projects/opencore-amr/files/fdk-aac/ and run the following commands.

    $ tar zxvf fdk-aac-0.1.5.tar.gz
    $ cd fdk-aac-0.1.5
    $ CC=${CCPREFIX}gcc CXX=${CCPREFIX}g++ ./configure --host=arm-rpi-linux-gnueabi --prefix=$PIBUILD
    $ make
    $ make install


## Build ffmpeg

You can't use ffmpeg that can be installed via `apt-get` or `pacman`.

### Copy ALSA headers and libs from Raspberry Pi (for Raspbian users)

Log in to Raspberry Pi and install **libasound2-dev** and **libssl-dev** via `apt-get`.

    $ sudo apt-get install libasound2-dev libssl-dev

Return to the compiling machine. Set an environment variable $PIUSR, and copy ALSA headers and libs from Raspberry Pi.

    $ export PIUSR=$HOME/pi/usr
    $ mkdir $PIUSR
    $ cd $PIUSR
    $ mkdir include lib
    $ rsync -rav pi@raspberrypi:/usr/include/alsa/ $PIUSR/include/alsa/
    $ rsync -rav pi@raspberrypi:/usr/lib/arm-linux-gnueabihf/libasound.so* $PIUSR/lib/
    $ tree --charset=ascii
    .
    |-- include
    |   `-- alsa
    |       |-- alisp.h
    :       :   :
    |       |-- sound
    |       |   |-- asound_fm.h
    :       :   :   :
    |       |   `-- type_compat.h
    |       |-- timer.h
    |       |-- use-case.h
    |       `-- version.h
    `-- lib
        |-- libasound.so -> libasound.so.2.0.0
        |-- libasound.so.2 -> libasound.so.2.0.0
        `-- libasound.so.2.0.0

    4 directories, 39 files


### Copy ALSA headers and libs from Raspberry Pi (for Arch Linux users)

Log in to Raspberry Pi and install **alsa-lib** and **alsa-utils** via `pacman`.

    $ sudo pacman -S alsa-lib alsa-utils

Return to the compiling machine. Set an environment variable $PIUSR, and copy ALSA headers and libs from Raspberry Pi.

    $ export PIUSR=$HOME/pi/usr
    $ mkdir $PIUSR
    $ cd $PIUSR
    $ mkdir include lib
    $ rsync -rav pi@raspberrypi:/usr/include/alsa/ $PIUSR/include/alsa/
    $ rsync -rav pi@raspberrypi:/usr/lib/libasound.so* $PIUSR/lib/
    $ tree --charset=ascii
    .
    |-- include
    |   `-- alsa
    |       |-- alisp.h
    :       :   :
    |       |-- sound
    |       |   |-- asound_fm.h
    :       :   :   :
    |       |   `-- type_compat.h
    |       |-- timer.h
    |       |-- use-case.h
    |       `-- version.h
    `-- lib
        |-- libasound.so -> libasound.so.2.0.0
        |-- libasound.so.2 -> libasound.so.2.0.0
        `-- libasound.so.2.0.0

    4 directories, 39 files


### Compile ffmpeg

Fetch ffmpeg source and configure it:

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

If you are running on a system with multiple cores you can invoke make with the -j option. You get the number of cores with the first command.

    $ cat /proc/cpuinfo | grep processor | wc -l
    $ make -j <num cores>
    $ make install


## Transfer the files to Raspberry Pi

Transfer the contents of $PIBUILD directory to Raspberry Pi.

    $ rsync -rav $PIBUILD/ pi@raspberrypi:build/

Then, put those files into /usr/local/ so that you have /usr/local/bin/ffmpeg.

    (on Raspberry Pi)
    $ cd ~/build
    $ ls
    bin  include  lib  share
    $ sudo rsync -rav ./ /usr/local/

We are done with the powerful machine which we used for cross compiling.

If you don't have /etc/ld.so.conf.d/libc.conf on Raspberry Pi, create that file with the following contents. On Raspbian, /etc/ld.so.conf.d/libc.conf is installed by default.

    /usr/local/lib

Run `ldconfig`.

    $ sudo ldconfig

Run `ffmpeg -codecs | grep fdk` and make sure that the output has this line:

     DEA.L. aac                  AAC (Advanced Audio Coding) (decoders: aac libfdk_aac ) (encoders: aac libfdk_aac )


## Build libilclient

From now on, all steps are performed on Raspberry Pi.

    $ cd /opt/vc/src/hello_pi/libs/ilclient
    $ make

If you do not have the /opt/vc/src/ directory, download the firmware from https://github.com/raspberrypi/firmware and put all the contents under /opt/vc/src/. On Raspbian, /opt/vc/src/ is installed by default.


## Install dependencies

Install **libfontconfig1-dev** and **libharfbuzz-dev** via `apt-get`.

    $ sudo apt-get install libfontconfig1-dev libharfbuzz-dev


## Build picam

    $ cd (to anywhere you like)
    $ git clone https://github.com/iizukanao/picam.git
    $ cd picam
    $ chmod +x whichpi
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
