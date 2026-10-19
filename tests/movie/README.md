# Movie Regression Tests

These exercise the real movie scheduler, SDK player and decoder. They do not
install anything or measure Vita gameplay performance.

Use Linux with Clang, the ARM hard-float cross toolchain and `qemu-arm`.
Keep the checkout path free of spaces. On Apple Silicon, use a native ARM64
Linux container, not an x86 container running another emulation layer.
From the repository root:

```sh
bash tests/movie/run_schedule.sh
export NP_TEST_RUNNER='qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf'
make -f tests/movie/thp-tests.mk -j2 T=armhf B="$PWD/build/movie-sdk" \
  RUN="$NP_TEST_RUNNER" check
make -f tests/movie/thp-tests.mk -j2 T=armhf B="$PWD/build/movie-sdk" \
  "$PWD/build/movie-sdk/tests/test_thp_skip"
MSC_DISC_FILES=/path/to/your/extracted/files \
  qemu-arm -cpu cortex-a9 -L /usr/arm-linux-gnueabihf \
  build/movie-sdk/tests/test_thp_skip
```

The last test needs your own `Art/movies/introtest.thp` and
`Art/movies/stad_classroom.thp`. Set `MSC_DISC_FILES` to the parent of `Art`;
game data is not included. Supplying that variable to the SDK `check` command
also enables its real-movie player test rather than skipping that portion.

Coverage: 11 scheduler cases, audio-preserving picture skips, queue backpressure,
read waits, finite EOF, retry safety, read-ring reuse, looping, close/reopen and native mixer
exclusion. The integration test compares decoded audio and visible image bytes
against an uninterrupted decode of the same input.
