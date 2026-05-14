/*
 * fpga_sdram_test.c — simple /dev/mem smoke test for the FPGA-side SDRAM.
 *
 * Run this after wiring an SDRAM controller into Qsys and assigning it a
 * physical address visible to the HPS through the full HPS-to-FPGA bridge.
 *
 * Example:
 *   sudo ./tests/fpga_sdram_test <h2f_bridge_phys_base + sdram_window_offset> 0x04000000
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_SPAN_BYTES (64u * 1024u * 1024u)

static uint64_t parse_u64(const char *text, const char *what)
{
    char *end = NULL;
    unsigned long long value;

    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || !end || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", what, text);
        exit(2);
    }
    return (uint64_t)value;
}

static uint32_t pattern_word(size_t word_index, uint32_t seed)
{
    return seed ^ (uint32_t)(word_index * 0x9E3779B9u);
}

static int run_pattern(volatile uint32_t *words, size_t word_count, uint32_t seed)
{
    size_t i;

    for (i = 0; i < word_count; i++)
        words[i] = pattern_word(i, seed);

    for (i = 0; i < word_count; i++) {
        uint32_t expected = pattern_word(i, seed);
        uint32_t got = words[i];

        if (got != expected) {
            fprintf(stderr,
                    "verify failed at word %zu: expected 0x%08" PRIx32
                    ", got 0x%08" PRIx32 "\n",
                    i, expected, got);
            return 1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    uint64_t phys_base;
    uint64_t span_bytes = DEFAULT_SPAN_BYTES;
    long page_size;
    uint64_t map_base;
    uint64_t map_offset;
    uint64_t map_len;
    volatile uint8_t *map8;
    volatile uint32_t *words;
    size_t word_count;
    int fd;
    int ret = 1;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <phys_base> [span_bytes]\n", argv[0]);
        return 2;
    }

    phys_base = parse_u64(argv[1], "physical base");
    if (argc >= 3)
        span_bytes = parse_u64(argv[2], "span");
    if (span_bytes == 0 || (span_bytes & 3u) != 0) {
        fprintf(stderr, "span must be a non-zero multiple of 4 bytes\n");
        return 2;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        perror("sysconf(_SC_PAGESIZE)");
        return 1;
    }

    map_base = phys_base & ~((uint64_t)page_size - 1u);
    map_offset = phys_base - map_base;
    map_len = map_offset + span_bytes;
    word_count = (size_t)(span_bytes / 4u);

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open(/dev/mem)");
        return 1;
    }

    map8 = mmap(NULL, (size_t)map_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                fd, (off_t)map_base);
    if (map8 == MAP_FAILED) {
        perror("mmap(/dev/mem)");
        close(fd);
        return 1;
    }

    words = (volatile uint32_t *)(map8 + map_offset);

    printf("fpga_sdram_test: base=0x%08" PRIx64 " span=%" PRIu64 " bytes (%zu words)\n",
           phys_base, span_bytes, word_count);

    if (run_pattern(words, word_count, 0xA5A55A5Au) != 0)
        goto out;
    if (run_pattern(words, word_count, 0x3C6EF372u) != 0)
        goto out;

    printf("fpga_sdram_test: PASS\n");
    ret = 0;

out:
    munmap((void *)map8, (size_t)map_len);
    close(fd);
    return ret;
}
