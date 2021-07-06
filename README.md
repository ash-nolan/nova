# The Nova Programming Language
Nova is a work-in-progress C-like systems programming language designed for
educational purposes.

**The Nova Project is in an early stage of development and should be considered
pre-alpha software. There is currently not much to see and the code is probably
riddled with bugs.**

## Dependencies
+ POSIX-compatible `make`
+ Supported toolchain containing:
  + C99 compiler (POSIX `c99`, `clang`, `gcc`, etc.)
  + `ld`
+ [`nasm`](https://www.nasm.us/)
+ `clang-format` (development only)

Dependencies can be installed on Debian-based distros (amd64) with:
```sh
$ apt-get install build-essential clang clang-format nasm
```

## Building
The script `misc/build-debug.sh` will perform a full build/test cycle using
appropriate debug flags. This is likely the script you want to be using as your
build driver when working on the compiler:
```sh
$ sh misc/build-debug.sh
```
By default `build-debug.sh` will run the `clean` and `all` make targets, but
this can be overridden. See `sh misc/build-debug.sh --help` for details.

Specific compiler/compiler-flag combinations include:
```sh
$ make <targets> CFLAGS='$(C99_DBG)'  # POSIX c99 (debug)
$ make <targets>                      # POSIX c99 (release)

$ # Use CC=clang for Clang or CC=gcc for GCC
$ make <targets> CC=clang CFLAGS='$(GNU_DBG)'              # clang/gcc (debug)
$ make <targets> CC=clang CFLAGS='$(GNU_DBG) $(SANITIZE)'  # clang/gcc (debug with Address Sanitizer)
$ make <targets> CC=clang CFLAGS='$(GNU_REL)'              # clang/gcc (release)
```

## Installing
The `install` target will install the Nova toolchain into the directory
specified by `$NOVA_HOME` (default `$HOME/.nova`). Run `make install` with
`$NOVA_HOME` specified as the directory of your choice, add `$NOVA_HOME` to your
`.bashrc` (or equivalent), and then finally add `$NOVA_HOME/bin` to your your
`$PATH` if necessary.
```sh
$ make install
```
```sh
# Add this to your .bashrc
if [ -d "$HOME/.nova" ]; then
    NOVA_HOME="$HOME/.nova"
    PATH="$NOVA_HOME/bin:$PATH"
fi
```

## License
Nova is distributed under the terms of the Apache License (Version 2.0).

See LICENSE for more information.
