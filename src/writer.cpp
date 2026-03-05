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

#if __has_include(<libdeflate.h>)
#include <libdeflate.h>
#else
#include "libdeflate.h"
#endif

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
    ZstdBackend(int level, int workers) {
        mCCtx = ZSTD_createCCtx();
        if(mCCtx == NULL) {
            error_exit("Failed to create ZSTD_CCtx");
        }

        if(workers < 1)
            workers = 1;
        if(workers > 64)
            workers = 64;

        size_t r1 = ZSTD_CCtx_setParameter(mCCtx, ZSTD_c_compressionLevel, level);
        if(ZSTD_isError(r1))
            error_exit("Failed to set zstd compression level");
        size_t r2 = ZSTD_CCtx_setParameter(mCCtx, ZSTD_c_nbWorkers, workers);
        if(ZSTD_isError(r2))
            error_exit("Failed to set zstd nbWorkers");
    }

    ~ZstdBackend() {
        if(mCCtx) {
            ZSTD_freeCCtx(mCCtx);
            mCCtx = NULL;
        }
    }

    bool writeBlock(FILE* fp, const char* data, size_t size) {
        size_t bound = ZSTD_compressBound(size);
        void* out = malloc(bound);
        if(out == NULL)
            return false;
        size_t outsize = ZSTD_compress2(mCCtx, out, bound, data, size);
        bool ok = false;
        if(!ZSTD_isError(outsize)) {
            size_t ret = fwrite(out, 1, outsize, fp);
            ok = ret > 0;
        }
        free(out);
        return ok;
    }

    bool isCompressed() const { return true; }

private:
    ZSTD_CCtx* mCCtx;
};

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
        int workers = mOptions->thread > 1 ? mOptions->thread : 1;
        mBackend = new ZstdBackend(mCompression, workers);
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
