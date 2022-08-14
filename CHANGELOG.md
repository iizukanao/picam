Change Log
==========

Version 2.0.5 *(2022-08-15)*
----------------------------

- Fixed a bug that caused segmentation fault when filename was specified in `hooks/start_record` file. (#177)

Version 2.0.4 *(2022-08-10)*
----------------------------

- Changed the default values for the following options:
  + `--avcprofile` default is `high` (previously `baseline`)
  + `-v, --videobitrate` default is `3000000` (previously `4500000`)
- Fixed memory leak. (#175)
- Fixed bug where HLS was not working on 64-bit OS. (#172)

Version 2.0.3 *(2022-07-25)*
----------------------------

- Fixed bug where sending SIGTERM does not stop picam.

Version 2.0.2 *(2022-07-21)*
----------------------------

- Added the following options. (by @cstillwell89)
  + `--brightness`
  + `--contrast`
  + `--saturation`
  + `--sharpness`

Version 2.0.1 *(2022-07-17)*
----------------------------

- Added `--camera` option.
- Fixed `--version` option.

Version 2.0.0 *(2022-07-07)*
----------------------------

- Built with libcamera.
- Stopped using legacy camera libraries (OpenMAX IL and MMAL).
- Added support for higher video resolutions up to 1920x1080 at 30 fps.
- Changed the default values for the following options:
  + `-w, --width` default is `1920` (previously `1280`)
  + `-h, --height` default is `1080` (previously `720`)
  + `-v, --videobitrate` default is `4500000` (previously `2000000`)
  + `--avcprofile` default is `baseline` (previously `constrained_baseline`)
  + `--avclevel` default is `4.1` (previously `3.1`)
- Added command line option `--hdmi` which selects HDMI port for video preview. Only works in console mode.
- Removed the following options due to technical limitations:
  + `--rotation` (For 180 degree rotation, use `--hflip --vflip` instead)
  + `--qpmin`
  + `--qpmax`
  + `--qpinit`
  + `--dquant`
  + `--aperture`
  + `--iso`
  + `--opacity`
  + `--blank`
  + `--mode`
- Changed the values for the following options. For the available values, please run `picam --help`.
  + `--ex`
  + `--wb`
  + `--metering`
- For NoIR camera users, `--wb greyworld` option is no longer available. Instead, pass `LIBCAMERA_RPI_TUNING_FILE` environment variable to picam like this: `LIBCAMERA_RPI_TUNING_FILE=/usr/share/libcamera/ipa/raspberrypi/ov5647_noir.json ./picam`

### Known issues

- There are some noise in audio preview (`--audiopreview` option) if video resolution is 1920x1080.
- If X Window System (desktop environment) is running, video fps will drop due to system load.
- EGL preview does not work on Raspberry Pi 3.
- Some problems may occur in the EGL preview.

Version 1.4.11 *(2021-04-29)*
-----------------------------

- Fixed flicker between subtitle changes (#159)

Version 1.4.10 *(2021-02-04)*
-----------------------------

- Fixed HLS issue (#152) (by @marler8997)
- Fixed build issues with the latest ffmpeg (by @marler8997)

Version 1.4.9 *(2020-07-13)*
-----------------------------

- Added `--wb greyworld` option.

Version 1.4.8 *(2019-07-17)*
-----------------------------

- Fixed bug where picam hangs up when large difference in video image occurs (#128).

Version 1.4.7 *(2018-09-17)*
-----------------------------

- Fixed issue with the latest firmware.
- Fixed compile error (#97).

Version 1.4.6 *(2017-02-13)*
-----------------------------

- Added `--roi` option.

Version 1.4.5 *(2017-01-01)*
-----------------------------

- Fixed bug where `--evcomp` does not work properly.

Version 1.4.4 *(2016-10-13)*
-----------------------------

- Added `--ex` option.

Version 1.4.3 *(2016-09-05)*
-----------------------------

- Display texts in preview (by @nalajcie)
- bugfix: Memory leak when using --time option

Version 1.4.2 *(2016-07-24)*
-----------------------------

- Add tab_scale directive to hooks/subtitle
- Fix stream name for --rtspout option
- bugfix: picam does not generate frames in VFR
- Improve build settings and instructions (by @Linkaan)
- Add option to set backgroud color for the preview (by @nalajcie)
- Other small fixes

Version 1.4.1 *(2015-11-26)*
-----------------------------

- Add `--wbred` and `--wbblue` options.
- Add wbred and wbblue hooks.

Version 1.4.0 *(2015-11-23)*
-----------------------------

### Major changes

- Add timestamp feature.
- Add subtitle feature.
- Introduce concepts of global and per-recording recordbuf.
- Now it is able to specify the directory and filename for each recording.

### Minor changes

- Set state/record to false after the recording is actually complete.
- Change the format of state/*.ts files.

Version 1.3.3 *(2015-10-06)*
-----------------------------

- Add audio preview feature (`--audiopreview` and `--audiopreviewdev` options).
- Change default value of `--opacity` to 0.

Version 1.3.2 *(2015-09-01)*
-----------------------------

- Add `--opacity` option.

Version 1.3.1 *(2015-08-05)*
-----------------------------

- Show error when picam receives an invalid hook
- Fix white balance control for the latest firmware
- Add hooks for white balance control

Version 1.3.0 *(2015-04-04)*
-----------------------------

- Add support for various AVC profiles and levels (--avcprofile, --avclevel).
- Add support for 1080p (1920x1080) resolution.

Version 1.2.10 *(2015-03-25)*
-----------------------------

- Add option to control white balance (`--wb`).
- Add options to control exposure (`--metering`, `--evcomp`, `--shutter`, `--iso`).
- Fix: Generate correct PTS and DTS in variable frame rate mode.
- Change protocol used in `--rtspout` to work with node-rtsp-rtmp-server.


Version 1.2.9 *(2015-03-01)*
----------------------------

- Change PTS calculation for the latest Raspberry Pi firmware.
- Remove `--ptsstep` option.
