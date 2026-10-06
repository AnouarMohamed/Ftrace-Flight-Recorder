#!/bin/bash -eu

$CC $CFLAGS -std=c11 -Isrc \
    tests/fuzz/fuzz_parse_size.c src/util.c \
    $LIB_FUZZING_ENGINE -o $OUT/fuzz_parse_size
