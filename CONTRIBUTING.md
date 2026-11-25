# Contributing

Bug reports, fixes and measurements are welcome. Open an [issue](https://github.com/iamkaf/pixzip/issues) for a bug or an idea, and a pull request for a change. Report a suspected vulnerability privately, as the [security policy](SECURITY.md) describes, not in a public issue.

## Keep the format

Version 1 of the file format is frozen ([FORMAT.md](FORMAT.md)). A change may make the encoder or the tool better, but every file it writes must still be one that today's decoders accept, and the decoders must keep accepting every file they accept now. A change that needs a different layout or a different model needs a new version number.

The C encoder and decoder, `encode.c` and `pixzip.c`, are held to the Python reference, `pixzip.py`. A change to how pixels are coded goes into both, and `test.py` checks that they still agree. For pictures of up to 64 colours, the streams must stay bit for bit those of Pinion, the game Pixzip came from; `tests/parity.py` checks that where a checkout of Pinion is at hand.

## Set up

You need a C compiler with AddressSanitizer and UndefinedBehaviorSanitizer, such as Clang, and Python 3. There is nothing else to install: the tests build what they need.

## Run the checks

CI runs these on Ubuntu 24.04, from the repository root:

```sh
gcc -std=c11 -O2 -Wall -Wextra -Werror main.c pixzip.c encode.c lodepng/lodepng.c -o pixzip
gcc -std=c11 -Os -Wall -Wextra -Werror -ffreestanding -nostdlib -c pixzip.c -o pixzip.o
CC=clang python3 test.py
python3 tests/parity.py
```

The first two build the tool and the freestanding decoder with every warning an error. `test.py` builds everything again with both sanitizers and runs the tests the README's [Run the tests](README.md#run-the-tests) section lists, in about ten seconds. `tests/parity.py` skips without a checkout of Pinion. A pull request needs CI to pass.

If you add a check to the decoder, add a test that fails without it. If you change what the encoder writes for the corpus, the corpus digest in `test.py` changes too: say why in the commit.

## Write the change

Keep the decoder small and freestanding: integer arithmetic, no allocation, no static state, no C library. If a change affects its size or the sizes in the README, measure them again the way the README describes, and update the tables. `./pixzip compare corpus/*.png` makes the size table.

Make each commit one change. Write its subject as a short plain sentence, and use its body to say why the change is needed and anything a reader of the diff can't see.
