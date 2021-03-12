# VGMPD

This is my own spin of [Music Player Daemon](https://github.com/MusicPlayerDaemon/MPD/releases)
with added plugins specifically for playing videogame music.

I'll warn you up front - I'm not really a C++ programmer. Hopefully
my code isn't terrible.

**Current Release Branch**: [`v0.22.6-vgmpd`](https://github.com/jprjr/VGMPD/tree/v0.22.6-vgmpd)

## Changes from upstream

* Game Music Emu plugin: remove track number from track titles.
* New LazyUSF decoder plugin for Nintendo 64 music in `.miniusf` format:
    * requires [psflib](https://git.lopez-snowhill.net/chris/psflib) and [lazyusf2](https://git.lopez-snowhill.net/chris/lazyusf2)
* New NSFPlay decoder plugin for NSF/NSFe/NSF2 chiptunes:
    * requires [nsfplay](https://github.com/bbbradsmith/nsfplay)
* New SPC decoder plugin for spc chiptunes:
    * requires [snes_spc](https://github.com/jprjr/snes_spc.git) and [libid666](https://github.com/jprjr/libid666)
    * supports loading from archives/streams supported by MPD (not just local files).
* New VGM decoder plugin for vgm chiptunes:
    * requires [libvgm](https://github.com/ValleyBell/libvgm)
    * supports newer versions of VGM than the GME plugin.
    * supports loading from archives/streams supported by MPD (not just local files).
* New [technicallyflac](https://github.com/jprjr/technicallyflac) encoder plugin:
    * Produces an Ogg stream with FLAC data.
    * Not actually compressed.
    * Uses Ogg chaining to have native, updating metadata instead of relying on Icecast/Shoutcast/other metadata.

## Installation steps for new dependencies

### psflib

`psflib` can be compiled with `make`, it does not have an installation target. So:

```bash
git clone https://git.lopez-snowhill.net/chris/psflib.git
cd psflib
make
sudo cp psflib.h /usr/local/include/psflib.h
sudo cp libpsflib.a /usr/local/lib/libpsflib.a
```

### lazyusf2

* requirements: psflib

`lazyusf2` can be compiled with `make`, it does not have an installation target. So:

```bash
git clone https://git.lopez-snowhill.net/chris/lazyusf2.git
cd lazyusf2
make
sudo cp usf/usf.h /usr/local/include/usf.h
sudo cp liblazyusf2.a /usr/local/lib/liblazyusf2.a
```

### nsfplay

```bash
git clone https://github.com/bbbradsmith/nsfplay.git
cd nsfplay/contrib
make release
sudo make install # installs to /usr/local by default
```

### snes_spc

```bash
git clone https://github.com/jprjr/snes_spc.git
cd snes_spc
make
sudo make install
```

### libid666

```bash
git clone https://github.com/jprjr/libid666.git
cd libid666
make
sudo make install
```

### libvgm

* requirements: zlib

```bash
git clone https://github.com/ValleyBell/libvgm.git
mkdir libvgm-build
cd libvgm-build
cmake ../libvgm
make
sudo make install
```

## Installation

You'll need meson, ninja, and the usual MPD dependencies (see [MPD user manual: compiling from source](https://www.musicpd.org/doc/html/user.html#compiling-from-source)).

```bash
git clone https://github.com/jprjr/VGMPD.git
cd VGMPD
git checkout v0.22.6-vgmpd
meson . output/release --buildtype=debugoptimized -Db_ndebug=true
ninja -C output/release
sudo ninja -C output/release install
```


## New plugin options

Here's the default settings for the new decoder plugins (lazyusf, nsfplay, spc, vgm)


```
decoder {
    plugin "lazyusf"
    hle "true"
    sample_rate "0"
}

decoder {
    plugin "nsfplay"
    RATE "48000"
    PLAY_TIME "180000"
    FADE_TIME "8000"
    LOOP_NUM "2"
}

decoder {
    plugin "spc"
    gain "256"
}

decoder {
    plugin "vgm"
    sample_rate "44100"
    bit_depth "16"
    fade_len "8"
}
```

## Plugin option details

### lazyusf

* **hle** - "true" to use high-level emulation (less accurate, less CPU), "false" to use low-level emulation (more accurate, more CPU).
* **sample_rate** - "0" to use native sample rate, any non-zero value will use lazyusf's internal resampler to output at a different sample rate.

### nsfplay

NSFPlay uses the same config options that the NSFPlay ini file uses.

**MASTER_VOLUME** is a notable config option, the default value is "128" which may be quiet. I use "192" personally.

### spc

* **gain** - set the internal gain applied to output. Default is "256", I use "384".

### vgm
* **sample_rate** - set the internal sample rate, VGM files use 44100 as their base sample rate.
* **bit_depth** - set the output bit depth, default is 16 bits, but you can go up to 24 bits.
* **fade_len** - the default fade length in seconds for looping VGM files.


