# Gnumeric

This is a fork of http://www.gnumeric.org/ used for educational purposes.

## Changes

- [CI build for Github Actions](https://github.com/fwilhe2/gnumeric/commit/31c73a73935ab71cf470e5a3000785b9e4afc8f2)
- [VSCode DevContainer](https://github.com/fwilhe2/gnumeric/commit/dce32005f86a95cc11fc9ecd9c96cc61ee375999)

## Building

Gnumeric requires `libgoffice >= 0.10.61`, which is newer than any current
distribution package (Ubuntu 24.04 ships 0.10.56), so goffice has to be built
from source first:

```
git clone --depth 1 https://gitlab.gnome.org/GNOME/goffice.git
cd goffice
NOCONFIGURE=1 ./autogen.sh
./configure --prefix=/usr/local
make && sudo make install && sudo ldconfig
```

Then build gnumeric itself:

```
./autogen.sh
./configure
make
```

The [DevContainer](.devcontainer/Dockerfile) does all of this for you.

## Running the locally built version

```
LD_LIBRARY_PATH=$PWD/src/.libs ./src/gnumeric
```

## License

Gnumeric is available under your choice of two licenses:

    GPL version 2 -- see the file COPYING-gpl2
    GPL version 3 -- see the file COPYING-gpl3

