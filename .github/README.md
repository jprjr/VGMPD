# VGMPD

This is my own spin of [Music Player Daemon](https://github.com/MusicPlayerDaemon/MPD/releases)
with added plugins specifically for playing videogame music.

I'll warn you up front - I'm not really a C++ programmer. Hopefully
my code isn't terrible.

## Changes from upstream

* Game Music Emu plugin: use Kode54's fork
    * supports for VGM chipsets and SGC files
* Game Music Emu plugin: load M3U sidecar files
* Game Music Emu plugin: add fade out time to total song length
* Game Music Emu plugin: remove track number from track titles
* Game Music Emu plugin: load SGC files
* New LazyUSF plugin (use `--enable-lazyusf`) for Nintendo 64 music in `.miniusf` format

## Installation

You'll need some software that's likely not in your distro's repository:

* Kode54's fork of Game_Music_Emu
* psflib
* lazyusf

I have forks of those repositories with a CMakeList.txt file added, to make it
quick and easy to compile on OSX/Linux:

```bash
git clone -b add-cmakelists --recursive https://bitbucket.org/jprjr/game_music_emu.git
git clone -b add-cmakelists https://bitbucket.org/jprjr/psflib.git
git clone -b add-cmakelists https://bitbucket.org/jprjr/lazyusf.git

mkdir build-gme build-psflib build-lazyusf

pushd build-gme
cmake -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_CXX_FLAGS="-DHAVE_ZLIB_H" -DCMAKE_C_FLAGS="-DHAVE_ZLIB_H" ../game_music_emu
make
make install
popd

pushd build-psflib
cmake -DCMAKE_INSTALL_PREFIX=/usr/local ../psflib
make
make install
popd

pushd build-lazyusf
cmake -DCMAKE_INSTALL_PREFIX=/usr/local ../lazyusf
make
make install
popd
```

From there, you should be able to clone this repo and build as normal.

## Options

You can configure plugin-specific options in your MPD.conf file.

Here's the default settings for lazyusf:


```
decoder {
    plugin "lazyusf"
    hle "true"
    sample_rate "0"
}
```

High-level emulation is enabled, and resampling is disabled.
