# Fixture definitions for the import tests

`tests/fixtures/` reads these to check the fixture import against real definitions.

## Open Fixture Library files (`ofl/`)

Six fixture definitions from the [Open Fixture Library](https://open-fixture-library.org),
downloaded from its website (which adds `fixtureKey`, `manufacturerKey` and `oflURL` to each
file), byte for byte as published. Each one covers a part of the format the import has to get
right:

| File | What it exercises |
|---|---|
| `adb/alc4.json` | a matrix whose pixel keys are listed out of order (`eachPixelABC` sorts them), a pixel group given as a list, template channels resolved with a group key |
| `aputure/c300d.json` | a switching channel whose dependency's default switches it to nothing |
| `audibax/boston-60.json` | a percentage default, fine channels, colour and gobo wheels, capabilities marked as needing checking, a strobe channel with no open state |
| `arri/l5-c.json` | 16-bit channels whose values are written at 8 bits (`dmxValueResolution`) |
| `american-dj/crazy-pocket-8.json` | pixel groups by name pattern and `"all"`, a `repeatFor` list of groups, template channels per group |
| `ayrton/diablo-s.json` | a CMY head written as `ColorIntensity` cyan, magenta and yellow; a pan/tilt speed sharing its range; two focus channels |

The Open Fixture Library is published under the MIT licence:

```
MIT License

Copyright (c) 2017 Florian & Felix Edelmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## GDTF

No file from GDTF Share is committed. The GDTF tests write their own `description.xml`, one
small one per case, in the test source itself, and zip it in memory where the case is about
the archive.
