/*
 * fuzz_url.c -- AFL++ / libFuzzer target for URL decode and path safety.
 *
 * Build with AFL++:
 *   CC=afl-cc cmake -B build-fuzz -DFUZZ=1 -DCMAKE_BUILD_TYPE=Debug
 *   cmake --build build-fuzz --target fuzz_url
 *   afl-fuzz -i fuzz/corpus -o fuzz/findings -- ./build-fuzz/fuzz_url @@
 *
 * Build with libFuzzer (clang):
 *   clang -std=c23 -g -fsanitize=address,fuzzer \
 *     -Iinclude fuzz/fuzz_url.c src/util.c src/log.c \
 *     -o fuzz_url_libfuzzer
 *   ./fuzz_url_libfuzzer fuzz/corpus
 */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "cserve.h"

#define FAKE_DOCROOT "/var/www"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > PATH_MAX)
        size = PATH_MAX;

    char decoded[PATH_MAX];
    int n = cs_url_decode((const char *)data, size,
                          decoded, sizeof(decoded));

    /*
     * Exercise path safety on whatever the decoder produced.
     * cs_path_safe() must never crash regardless of input.
     */
    if (n > 0)
        cs_path_safe(FAKE_DOCROOT, decoded);

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
        fprintf(stderr, "usage: fuzz_url <input-file>\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) { perror(argv[1]); return 1; }
    uint8_t buf[PATH_MAX];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    LLVMFuzzerTestOneInput(buf, n);
    return 0;
}

#endif
