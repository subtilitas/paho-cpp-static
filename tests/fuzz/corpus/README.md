Seed corpora for the fuzz targets.

Each file is one input. They are not hand-written: they were produced by
running the fuzzer and then minimised with `-merge=1`, which keeps only the
inputs that reach coverage no other input does. Together they are 9552 bytes
and cover 1455 edges in the client target and 38 in the codec target.

They exist so that a short run finds regressions immediately rather than
rediscovering the interesting shapes first. `ctest -L fuzz` replays them with
`-runs=2000`, which is a count rather than a clock, so the result does not vary
with how loaded the machine is.

Regenerate after a change that moves the parser:

    cmake -S . -B build-fuzz -DMQTT_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
    cmake --build build-fuzz --parallel
    ./build-fuzz/tests/fuzz_client -runs=100000 -max_len=256 /tmp/new
    ./build-fuzz/tests/fuzz_client -merge=1 tests/fuzz/corpus/fuzz_client /tmp/new

`-max_len=256` matches the fuzz configuration's receive buffer: a longer input
cannot reach anything a 256-byte one does not, and only makes the corpus bigger.
