/*
 * fuzz_parser.c -- AFL++ / libFuzzer target for the HTTP parser.
 *
 * Build with AFL++:
 *   CC=afl-cc cmake -B build-fuzz -DFUZZ=1 -DCMAKE_BUILD_TYPE=Debug
 *   cmake --build build-fuzz --target fuzz_parser
 *   afl-fuzz -i fuzz/corpus -o fuzz/findings -- ./build-fuzz/fuzz_parser @@
 *
 * Build with libFuzzer (clang):
 *   clang -std=c23 -g -fsanitize=address,fuzzer \
 *     -Iinclude fuzz/fuzz_parser.c src/parser.c src/log.c \
 *     -o fuzz_parser_libfuzzer
 *   ./fuzz_parser_libfuzzer fuzz/corpus
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "cserve.h"

/*
 * AFL++ persistent-mode entry point.
 * Also acts as LLVMFuzzerTestOneInput for libFuzzer.
 */
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > INBUF_SIZE)
        size = INBUF_SIZE;

    parser_t p;
    cs_parser_init(&p);
    cs_parser_feed(&p, data, size);
    return 0;
}

#ifdef __AFL_FUZZ_TESTCASE_LEN

/* AFL++ persistent mode shim */
__AFL_FUZZ_INIT();

int
main(void)
{
    __AFL_INIT();
    uint8_t *buf = __AFL_FUZZ_TESTCASE_BUF;
    while (__AFL_LOOP(10000)) {
        size_t len = (size_t)__AFL_FUZZ_TESTCASE_LEN;
        LLVMFuzzerTestOneInput(buf, len);
    }
    return 0;
}

#else

/* Standalone: read from file argument (for one-shot testing) */
#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: fuzz_parser <input-file>\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) { perror(argv[1]); return 1; }
    uint8_t buf[INBUF_SIZE];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    LLVMFuzzerTestOneInput(buf, n);
    return 0;
}

#endif
