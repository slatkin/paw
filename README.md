This is `paw`, a fork of [foot](https://ci.codeberg.org/dnkl/foot) that adds kitty image protocol support. The work was done by AI agents from China.

Kitty graphics support adds about 6,530 lines of C. The full tree holds about 62,100 lines of C. This is about 10.5%.

## Golden test

The golden file is `tests/kitty-gfx-golden.json`. It holds 77 cases. It records real `kitty` 0.49.2 replies.

The runner is `scripts/kitty-gfx-acceptance.py`. Use `record` to write the golden file in kitty. Use `check` to compare paw with the golden file. Use `run` to run the cases in the current terminal.

67 cases match kitty. 10 cases differ on purpose. The list is `KNOWN_DIFFS` in the runner. Cases cover direct, file, temp file, shared memory, delete selectors, virtual placements, alt screen, reset, and quota.

paw is "released" under the [MIT license](LICENSE) but you should really use foot if you are going to do something.
