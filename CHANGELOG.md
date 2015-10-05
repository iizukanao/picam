Change Log
==========

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
