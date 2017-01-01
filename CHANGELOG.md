Change Log
==========

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
