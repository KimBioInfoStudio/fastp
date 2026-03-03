#ifndef ISAL_COMPRESS_H
#define ISAL_COMPRESS_H

#include <string>
#include <cstdlib>
#if __has_include(<isa-l/igzip_lib.h>)
#include <isa-l/igzip_lib.h>
#else
#include "igzip_lib.h"
#endif

using namespace std;

// Compress data to a single gzip member using ISA-L stateless deflate.
// Thread-safe: all state is stack/heap-local, no shared mutable data.
// ISA-L levels: 0 (fastest, minimal compression) to 3 (best compression).
// Returns compressed gzip data as a string.
inline string isal_gzip_compress(const char* data, size_t len, int level = 1) {
    struct isal_zstream stream;
    isal_deflate_stateless_init(&stream);

    stream.gzip_flag = IGZIP_GZIP;
    stream.end_of_stream = 1;
    stream.flush = NO_FLUSH;
    stream.level = level;

    // Allocate level buffer for levels > 0
    uint8_t* level_buf = NULL;
    size_t level_buf_size = 0;
    switch (level) {
        case 1: level_buf_size = ISAL_DEF_LVL1_DEFAULT; break;
        case 2: level_buf_size = ISAL_DEF_LVL2_DEFAULT; break;
        case 3: level_buf_size = ISAL_DEF_LVL3_DEFAULT; break;
        default: break; // level 0 needs no buffer
    }
    if (level_buf_size > 0) {
        level_buf = (uint8_t*)malloc(level_buf_size);
        stream.level_buf = level_buf;
        stream.level_buf_size = level_buf_size;
    }

    stream.next_in = (uint8_t*)data;
    stream.avail_in = len;

    // Worst-case output: input size + gzip overhead
    size_t out_buf_size = len + 512 + (len >> 3);
    if (out_buf_size < 1024) out_buf_size = 1024;

    string result;
    result.resize(out_buf_size);
    stream.next_out = (uint8_t*)&result[0];
    stream.avail_out = out_buf_size;

    int ret = isal_deflate_stateless(&stream);

    if (level_buf) free(level_buf);

    if (ret == STATELESS_OVERFLOW) {
        // Extremely rare: output bigger than input + overhead. Retry with larger buffer.
        out_buf_size = len * 2 + 1024;
        result.resize(out_buf_size);
        isal_deflate_stateless_init(&stream);
        stream.gzip_flag = IGZIP_GZIP;
        stream.end_of_stream = 1;
        stream.flush = NO_FLUSH;
        stream.level = level;
        if (level_buf_size > 0) {
            level_buf = (uint8_t*)malloc(level_buf_size);
            stream.level_buf = level_buf;
            stream.level_buf_size = level_buf_size;
        }
        stream.next_in = (uint8_t*)data;
        stream.avail_in = len;
        stream.next_out = (uint8_t*)&result[0];
        stream.avail_out = out_buf_size;
        ret = isal_deflate_stateless(&stream);
        if (level_buf) free(level_buf);
    }

    if (ret != COMP_OK) {
        result.clear();
        return result;
    }

    result.resize(stream.total_out);
    return result;
}

#endif
