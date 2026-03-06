/*
MIT License

Copyright (c) 2021 Shifu Chen <chen@haplox.com>

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
*/

#include "writer.h"
#include "util.h"
#include <string.h>
#include <stdint.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>

#if __has_include(<libdeflate.h>)
#include <libdeflate.h>
#else
#include "libdeflate.h"
#endif

#define ZSTD_STATIC_LINKING_ONLY
#if __has_include(<zstd.h>)
#include <zstd.h>
#elif __has_include("/opt/homebrew/include/zstd.h")
#include "/opt/homebrew/include/zstd.h"
#else
#include "zstd.h"
#endif

class Writer::Backend {
public:
    virtual ~Backend() {}
    virtual bool writeBlock(FILE* fp, const char* data, size_t size) = 0;
    virtual bool finalize(FILE* fp) { (void)fp; return true; }
    virtual bool isCompressed() const = 0;
};

class RawBackend : public Writer::Backend {
public:
    bool writeBlock(FILE* fp, const char* data, size_t size) {
        size_t ret = fwrite(data, 1, size, fp);
        return ret > 0;
    }
    bool isCompressed() const { return false; }
};

class GzipBackend : public Writer::Backend {
public:
    explicit GzipBackend(int level) {
        mCompressor = libdeflate_alloc_compressor(level);
        if(mCompressor == NULL) {
            error_exit("Failed to alloc libdeflate_alloc_compressor, please check the libdeflate library.");
        }
    }

    ~GzipBackend() {
        if(mCompressor) {
            libdeflate_free_compressor(mCompressor);
            mCompressor = NULL;
        }
    }

    bool writeBlock(FILE* fp, const char* data, size_t size) {
        size_t bound = libdeflate_gzip_compress_bound(mCompressor, size);
        void* out = malloc(bound);
        if(out == NULL)
            return false;
        size_t outsize = libdeflate_gzip_compress(mCompressor, data, size, out, bound);
        bool ok = false;
        if(outsize > 0) {
            size_t ret = fwrite(out, 1, outsize, fp);
            ok = ret > 0;
        }
        free(out);
        return ok;
    }

    bool isCompressed() const { return true; }

private:
    libdeflate_compressor* mCompressor;
};

class ZstdBackend : public Writer::Backend {
public:
    struct FrameMeta {
        uint32_t cSize;
        uint32_t dSize;
    };

    ZstdBackend(int level, const Options* opt) {
        mCCtx = ZSTD_createCCtx();
        if(mCCtx == NULL) {
            error_exit("Failed to create ZSTD_CCtx");
        }
        mMaxWorkers = ZstdBackend::globalMaxWorkers();
        if(opt) {
            if(opt->threadZstdWorkersBudget >= 0)
                mMaxWorkers = std::min(mMaxWorkers, opt->threadZstdWorkersBudget);
        }
        registerZstdWriter();
        mEmaBlockBytes = 0.0;
        mEmaBlockMicros = 0.0;
        mLastTune = std::chrono::steady_clock::now();
        mBlocksSinceTune = 0;
        mCurrentWorkers = 0;

        size_t r1 = ZSTD_CCtx_setParameter(mCCtx, ZSTD_c_compressionLevel, level);
        if(ZSTD_isError(r1))
            error_exit("Failed to set zstd compression level");
        size_t r2 = ZSTD_CCtx_setParameter(mCCtx, ZSTD_c_nbWorkers, mCurrentWorkers);
        if(ZSTD_isError(r2))
            error_exit("Failed to set zstd nbWorkers");
    }

    ~ZstdBackend() {
        unregisterZstdWriter();
        if(mCCtx) {
            ZSTD_freeCCtx(mCCtx);
            mCCtx = NULL;
        }
    }

    bool writeBlock(FILE* fp, const char* data, size_t size) {
        if(size > 0xFFFFFFFFULL)
            return false;
        auto t0 = std::chrono::steady_clock::now();
        size_t bound = ZSTD_compressBound(size);
        void* out = malloc(bound);
        if(out == NULL)
            return false;
        size_t outsize = ZSTD_compress2(mCCtx, out, bound, data, size);
        bool ok = false;
        if(!ZSTD_isError(outsize)) {
            if(outsize > 0xFFFFFFFFULL) {
                free(out);
                return false;
            }
            size_t ret = fwrite(out, 1, outsize, fp);
            ok = ret > 0;
            if(ok) {
                FrameMeta fm;
                fm.cSize = (uint32_t)outsize;
                fm.dSize = (uint32_t)size;
                mFrames.push_back(fm);
            }
        }
        free(out);
        auto t1 = std::chrono::steady_clock::now();
        double usec = (double)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        tuneWorkers(size, usec);
        return ok;
    }

    bool finalize(FILE* fp) {
        // Write zstd seek table in a skippable frame (format compatible with contrib/seekable_format).
        // This keeps full-stream decompression compatibility with standard zstd decoders.
        uint32_t const seekableMagic = 0x8F92EAB1U;
        size_t const entriesBytes = mFrames.size() * 8;
        size_t const seekTableLen = ZSTD_SKIPPABLEHEADERSIZE + entriesBytes + 9;
        if(seekTableLen > 0xFFFFFFFFULL)
            return false;

        auto writeLE32 = [&](uint32_t v) -> bool {
            unsigned char b[4];
            b[0] = (unsigned char)(v & 0xFF);
            b[1] = (unsigned char)((v >> 8) & 0xFF);
            b[2] = (unsigned char)((v >> 16) & 0xFF);
            b[3] = (unsigned char)((v >> 24) & 0xFF);
            return fwrite(b, 1, 4, fp) == 4;
        };

        // Skippable frame header
        if(!writeLE32((uint32_t)(ZSTD_MAGIC_SKIPPABLE_START | 0xE)))
            return false;
        if(!writeLE32((uint32_t)seekTableLen - ZSTD_SKIPPABLEHEADERSIZE))
            return false;

        // Entries: compressedSize + decompressedSize (checksum disabled)
        for(size_t i=0; i<mFrames.size(); i++) {
            if(!writeLE32(mFrames[i].cSize))
                return false;
            if(!writeLE32(mFrames[i].dSize))
                return false;
        }

        // Footer: numFrames + descriptor + seekableMagic
        if(!writeLE32((uint32_t)mFrames.size()))
            return false;
        unsigned char descriptor = 0; // checksumFlag = 0
        if(fwrite(&descriptor, 1, 1, fp) != 1)
            return false;
        if(!writeLE32(seekableMagic))
            return false;

        return true;
    }

    bool isCompressed() const { return true; }

private:
    static int globalMaxWorkers() {
        unsigned hw = std::thread::hardware_concurrency();
        if(hw == 0)
            hw = 8;
        int maxWorkers = (int)hw - 2;
        if(maxWorkers < 1)
            maxWorkers = 1;
        if(maxWorkers > 64)
            maxWorkers = 64;
        return maxWorkers;
    }

    static int activeZstdWriters() {
        int n = sActiveWriters.load(std::memory_order_relaxed);
        return n > 0 ? n : 1;
    }

    static void registerZstdWriter() {
        sActiveWriters.fetch_add(1, std::memory_order_relaxed);
    }

    static void unregisterZstdWriter() {
        int n = sActiveWriters.fetch_sub(1, std::memory_order_relaxed) - 1;
        if(n < 0)
            sActiveWriters.store(0, std::memory_order_relaxed);
    }

    void tuneWorkers(size_t inSize, double elapsedUsec) {
        // EMA on block size and end-to-end block latency (compress + write).
        if(mEmaBlockBytes <= 0.0) {
            mEmaBlockBytes = (double)inSize;
            mEmaBlockMicros = elapsedUsec > 0 ? elapsedUsec : 1.0;
        } else {
            mEmaBlockBytes = mEmaBlockBytes * 0.8 + (double)inSize * 0.2;
            mEmaBlockMicros = mEmaBlockMicros * 0.8 + (elapsedUsec > 0 ? elapsedUsec : 1.0) * 0.2;
        }
        mBlocksSinceTune++;

        auto now = std::chrono::steady_clock::now();
        auto deltaMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - mLastTune).count();
        if(mBlocksSinceTune < 8 && deltaMs < 200)
            return;
        mBlocksSinceTune = 0;
        mLastTune = now;

        int target = 0;
        if(mEmaBlockBytes >= (8.0 * 1024 * 1024))
            target = 8;
        else if(mEmaBlockBytes >= (4.0 * 1024 * 1024))
            target = 6;
        else if(mEmaBlockBytes >= (1.0 * 1024 * 1024))
            target = 4;
        else if(mEmaBlockBytes >= (256.0 * 1024))
            target = 2;

        // Feedback: if block latency stays high, allow one extra worker step.
        if(mEmaBlockMicros > 12000.0 && mEmaBlockBytes >= (512.0 * 1024))
            target += 2;
        else if(mEmaBlockMicros > 7000.0 && mEmaBlockBytes >= (256.0 * 1024))
            target += 1;
        else if(mEmaBlockMicros < 2500.0 && mEmaBlockBytes < (512.0 * 1024))
            target -= 1;

        int perWriterCap = mMaxWorkers / activeZstdWriters();
        if(perWriterCap < 0)
            perWriterCap = 0;
        if(target > perWriterCap)
            target = perWriterCap;
        if(target > mMaxWorkers)
            target = mMaxWorkers;
        if(target < 0)
            target = 0;

        // Smooth changes to avoid oscillation.
        int stepTarget = mCurrentWorkers;
        if(target > mCurrentWorkers)
            stepTarget = mCurrentWorkers + 1;
        else if(target < mCurrentWorkers)
            stepTarget = mCurrentWorkers - 1;
        if(stepTarget == mCurrentWorkers)
            return;

        size_t r = ZSTD_CCtx_setParameter(mCCtx, ZSTD_c_nbWorkers, stepTarget);
        if(!ZSTD_isError(r))
            mCurrentWorkers = stepTarget;
    }

    ZSTD_CCtx* mCCtx;
    int mCurrentWorkers;
    int mMaxWorkers;
    double mEmaBlockBytes;
    double mEmaBlockMicros;
    std::chrono::steady_clock::time_point mLastTune;
    int mBlocksSinceTune;
    vector<FrameMeta> mFrames;

    static std::atomic_int sActiveWriters;
};

std::atomic_int ZstdBackend::sActiveWriters(0);

Writer::Writer(Options* opt, string filename, int compression, bool isSTDOUT, bool preCompressed){
    mCompression = compression;
    mFilename = filename;
    mFP = NULL;
    mBackend = NULL;
    mCompressed = false;
    haveToClose = true;
    mBuffer = NULL;
    mBufDataLen = 0;
    mOptions = opt;
    mBufSize = mOptions->writerBufferSize;
    mSTDOUT = isSTDOUT;
    mPreCompressed = preCompressed;
    init();
}

Writer::~Writer(){
    flush();
    if(haveToClose) {
        close();
    }
}

void Writer::flush() {
    if(mBufDataLen > 0) {
        writeInternal(mBuffer, mBufDataLen);
        mBufDataLen = 0;
    }
}

string Writer::filename(){
    return mFilename;
}

void Writer::init(){
    mBuffer = (char*) malloc(mBufSize);
    if(mBuffer == NULL) {
        error_exit("Failed to allocate write buffer with size: " + to_string(mBufSize));
    }

    if(mSTDOUT) {
        mFP = stdout;
        mBackend = new RawBackend();
        mCompressed = false;
        return;
    }

    mFP = fopen(mFilename.c_str(), "wb");
    if(mFP == NULL) {
        error_exit("Failed to write: " + mFilename);
    }

    if(ends_with(mFilename, ".gz") && !mPreCompressed) {
        mBackend = new GzipBackend(mCompression);
    } else if(ends_with(mFilename, ".zst") && !mPreCompressed) {
        mBackend = new ZstdBackend(mCompression, mOptions);
    } else {
        mBackend = new RawBackend();
    }
    mCompressed = mBackend->isCompressed();
}

bool Writer::writeString(const string& str) {
    return write(str.data(), str.length());
}

bool Writer::writeString(string* str) {
    return write(str->data(), str->length());
}

bool Writer::write(const char* strdata, size_t size) {
    if(size + mBufDataLen > mBufSize)
        flush();
    if(size > mBufSize)
        return writeInternal(strdata, size);
    else {
        memcpy(mBuffer + mBufDataLen, strdata, size);
        mBufDataLen += size;
    }
    return true;
}

bool Writer::writeInternal(const char* strdata, size_t size) {
    if(mBackend == NULL || mFP == NULL)
        return false;
    return mBackend->writeBlock(mFP, strdata, size);
}

void Writer::close(){
    if(mBackend && mFP) {
        if(!mBackend->finalize(mFP))
            error_exit("Failed to finalize compressed output: " + mFilename);
    }
    if(mBackend) {
        delete mBackend;
        mBackend = NULL;
    }
    if(mBuffer) {
        free(mBuffer);
        mBuffer = NULL;
    }
    if(mFP && !mSTDOUT) {
        fclose(mFP);
        mFP = NULL;
    }
}

bool Writer::isZipped(){
    return mCompressed;
}
