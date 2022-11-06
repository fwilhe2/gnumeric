# Gnumeric

This is a fork of http://www.gnumeric.org/ used for educational purposes.

## Changes

- [CI build for Github Actions](https://github.com/fwilhe2/gnumeric/commit/31c73a73935ab71cf470e5a3000785b9e4afc8f2)
- [VSCode DevContainer](https://github.com/fwilhe2/gnumeric/commit/dce32005f86a95cc11fc9ecd9c96cc61ee375999)

## Building

```
./autogen.sh
./configure
make
```

## Running the locally built version

```
LD_LIBRARY_PATH=$PWD/src/.libs ./src/gnumeric
```

## License

Gnumeric is available under your choice of two licenses:

    GPL version 2 -- see the file COPYING-gpl2
    GPL version 3 -- see the file COPYING-gpl3

