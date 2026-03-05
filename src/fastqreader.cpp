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

#include "fastqreader.h"
#include "util.h"
#include <string.h>
#include <cassert>

#if __has_include(<isa-l/igzip_lib.h>)
#include <isa-l/igzip_lib.h>
#else
#include "igzip_lib.h"
#endif

#if __has_include(<zstd.h>)
#include <zstd.h>
#elif __has_include("/opt/homebrew/include/zstd.h")
#include "/opt/homebrew/include/zstd.h"
#else
#include "zstd.h"
#endif

#define FQ_BUF_SIZE (1<<23)
#define IGZIP_IN_BUF_SIZE (1<<22)
#define GZIP_HEADER_BYTES_REQ (1<<16)

class FastqRawReaderBase {
public:
    virtual ~FastqRawReaderBase() {}
    virtual void fill(char* outBuf, int outCap, int& outLen) = 0;
    virtual bool eof() const = 0;
    virtual bool finished() const = 0;
    virtual size_t bytesRead() const = 0;
    virtual bool isCompressed() const = 0;
};

class FastqRawReader : public FastqRawReaderBase {
public:
    explicit FastqRawReader(const string& filename) {
        mOwnsFile = filename != "/dev/stdin";
        mFile = mOwnsFile ? fopen(filename.c_str(), "rb") : stdin;
        if(mFile == NULL) {
            error_exit("Failed to open file: " + filename);
        }
    }

    ~FastqRawReader() {
        if(mFile && mOwnsFile) {
            fclose(mFile);
            mFile = NULL;
        }
    }

    void fill(char* outBuf, int outCap, int& outLen) {
        outLen = 0;
        if(!eof())
            outLen = (int)fread(outBuf, 1, (size_t)outCap, mFile);
    }

    bool eof() const { return feof(mFile); }
    bool finished() const { return feof(mFile); }
    size_t bytesRead() const {
        long v = ftell(mFile);
        return v > 0 ? (size_t)v : 0;
    }
    bool isCompressed() const { return false; }

private:
    FILE* mFile;
    bool mOwnsFile;
};

class FastqGzReader : public FastqRawReaderBase {
public:
    explicit FastqGzReader(const string& filename) {
        mFilename = filename;
        mFile = fopen(filename.c_str(), "rb");
        if(mFile == NULL) {
            error_exit("Failed to open file: " + filename);
        }

        mInputBufferSize = IGZIP_IN_BUF_SIZE;
        mInputBuffer = new unsigned char[mInputBufferSize];
        mInputUsedBytes = 0;

        isal_gzip_header_init(&mGzipHeader);
        isal_inflate_init(&mGzipState);
        mGzipState.crc_flag = ISAL_GZIP_NO_HDR_VER;
        mGzipState.next_in = mInputBuffer;
        mGzipState.avail_in = fread(mGzipState.next_in, 1, mInputBufferSize, mFile);
        mInputUsedBytes += mGzipState.avail_in;
        int ret = isal_read_gzip_header(&mGzipState, &mGzipHeader);
        if (ret != ISAL_DECOMP_OK) {
            error_exit("igzip: Error invalid gzip header found: "  + filename);
        }
    }

    ~FastqGzReader() {
        if(mFile) {
            fclose(mFile);
            mFile = NULL;
        }
        if(mInputBuffer) {
            delete[] mInputBuffer;
            mInputBuffer = NULL;
        }
    }

    void fill(char* outBuf, int outCap, int& outLen) {
        outLen = 0;
        while(outLen == 0) {
            if(feof(mFile) && mGzipState.avail_in==0)
                return;

            if (mGzipState.avail_in == 0) {
                mGzipState.next_in = mInputBuffer;
                mGzipState.avail_in = fread(mGzipState.next_in, 1, mInputBufferSize, mFile);
                mInputUsedBytes += mGzipState.avail_in;
            }

            mGzipState.next_out = (unsigned char*)outBuf;
            mGzipState.avail_out = (uint32_t)outCap;

            int ret = isal_inflate(&mGzipState);
            if (ret != ISAL_DECOMP_OK) {
                error_exit("igzip: encountered while decompressing file: " + mFilename);
            }
            outLen = (int)(mGzipState.next_out - (unsigned char*)outBuf);
            if(feof(mFile) || mGzipState.avail_in>0)
                break;
        }

        if(mGzipState.block_state == ISAL_BLOCK_FINISH) {
            if(!feof(mFile) || mGzipState.avail_in > 0) {
                if (mGzipState.avail_in == 0) {
                    isal_inflate_reset(&mGzipState);
                    mGzipState.next_in = mInputBuffer;
                    mGzipState.avail_in = fread(mGzipState.next_in, 1, mInputBufferSize, mFile);
                    mInputUsedBytes += mGzipState.avail_in;
                } else if (mGzipState.avail_in >= GZIP_HEADER_BYTES_REQ){
                    unsigned char* old_next_in = mGzipState.next_in;
                    size_t old_avail_in = mGzipState.avail_in;
                    isal_inflate_reset(&mGzipState);
                    mGzipState.avail_in = old_avail_in;
                    mGzipState.next_in = old_next_in;
                } else {
                    size_t old_avail_in = mGzipState.avail_in;
                    memmove(mInputBuffer, mGzipState.next_in, mGzipState.avail_in);
                    size_t added = 0;
                    if(!feof(mFile)) {
                        added = fread(mInputBuffer + mGzipState.avail_in, 1, mInputBufferSize - mGzipState.avail_in, mFile);
                        mInputUsedBytes += added;
                    }
                    isal_inflate_reset(&mGzipState);
                    mGzipState.next_in = mInputBuffer;
                    mGzipState.avail_in = old_avail_in + added;
                }
                int ret = isal_read_gzip_header(&mGzipState, &mGzipHeader);
                if (ret != ISAL_DECOMP_OK) {
                    error_exit("igzip: invalid gzip header found");
                }
            }
        }

        if(feof(mFile) && mGzipState.avail_in == 0) {
            if (mGzipState.block_state != ISAL_BLOCK_FINISH || !mGzipState.bfinal) {
                error_exit("igzip: unexpected eof");
            }
        }
    }

    bool eof() const { return feof(mFile); }
    bool finished() const { return feof(mFile) && mGzipState.avail_in == 0; }
    size_t bytesRead() const { return mInputUsedBytes - mGzipState.avail_in; }
    bool isCompressed() const { return true; }

private:
    string mFilename;
    FILE* mFile;
    struct isal_gzip_header mGzipHeader;
    struct inflate_state mGzipState;
    unsigned char* mInputBuffer;
    size_t mInputBufferSize;
    size_t mInputUsedBytes;
};

class FastqZstReader : public FastqRawReaderBase {
public:
    explicit FastqZstReader(const string& filename) {
        mFilename = filename;
        mFile = fopen(filename.c_str(), "rb");
        if(mFile == NULL) {
            error_exit("Failed to open file: " + filename);
        }

        mInputBufferSize = ZSTD_DStreamInSize();
        mInputBuffer = new char[mInputBufferSize];
        mInputSize = 0;
        mInputPos = 0;
        mInputUsedBytes = 0;
        mFinished = false;

        mStream = ZSTD_createDStream();
        if(mStream == NULL) {
            error_exit("zstd: failed to create DStream");
        }
        size_t ret = ZSTD_initDStream(mStream);
        if(ZSTD_isError(ret)) {
            error_exit("zstd: failed to init DStream");
        }
    }

    ~FastqZstReader() {
        if(mStream) {
            ZSTD_freeDStream(mStream);
            mStream = NULL;
        }
        if(mInputBuffer) {
            delete[] mInputBuffer;
            mInputBuffer = NULL;
        }
        if(mFile) {
            fclose(mFile);
            mFile = NULL;
        }
    }

    void fill(char* outBuf, int outCap, int& outLen) {
        outLen = 0;
        while(outLen == 0) {
            if(mFinished)
                return;

            if(mInputPos >= mInputSize) {
                mInputSize = fread(mInputBuffer, 1, mInputBufferSize, mFile);
                mInputPos = 0;
                mInputUsedBytes += mInputSize;
                if(mInputSize == 0) {
                    if(feof(mFile)) {
                        if(!mFinished)
                            error_exit("zstd: unexpected eof while decompressing file: " + mFilename);
                        return;
                    }
                    error_exit("zstd: failed to read compressed input: " + mFilename);
                }
            }

            ZSTD_inBuffer in = {mInputBuffer + mInputPos, mInputSize - mInputPos, 0};
            ZSTD_outBuffer out = {outBuf, (size_t)outCap, 0};
            size_t ret = ZSTD_decompressStream(mStream, &out, &in);
            if(ZSTD_isError(ret)) {
                error_exit("zstd: encountered while decompressing file: " + mFilename + ", reason: " + ZSTD_getErrorName(ret));
            }
            mInputPos += in.pos;
            outLen = (int)out.pos;

            if(ret == 0 && mInputPos >= mInputSize && feof(mFile)) {
                mFinished = true;
                break;
            }
        }
    }

    bool eof() const { return feof(mFile); }
    bool finished() const { return mFinished && mInputPos >= mInputSize && feof(mFile); }
    size_t bytesRead() const { return mInputUsedBytes - (mInputSize - mInputPos); }
    bool isCompressed() const { return true; }

private:
    string mFilename;
    FILE* mFile;
    ZSTD_DStream* mStream;
    char* mInputBuffer;
    size_t mInputBufferSize;
    size_t mInputSize;
    size_t mInputPos;
    size_t mInputUsedBytes;
    bool mFinished;
};

FastqReader::FastqReader(string filename, bool hasQuality, bool phred64, int workerThreads){
    mFilename = filename;
    mRawReader = NULL;
    mZipped = false;
    mFastqBuf = new char[FQ_BUF_SIZE];
    mBufDataLen = 0;
    mBufUsedLen = 0;
    mHasNoLineBreakAtEnd = false;
    mCounter = 0;
    mPhred64 = phred64;
    mHasQuality = hasQuality;
    mHasNoLineBreakAtEnd = false;
    mWorkerThreads = workerThreads;
    init();
}

FastqReader::~FastqReader(){
    close();
    delete[] mFastqBuf;
}

bool FastqReader::hasNoLineBreakAtEnd() {
    return mHasNoLineBreakAtEnd;
}

bool FastqReader::bufferFinished() {
    if(!mRawReader)
        return true;
    return mRawReader->finished();
}

void FastqReader::readToBuf() {
    mBufDataLen = 0;
    if(mRawReader)
        mRawReader->fill(mFastqBuf, FQ_BUF_SIZE, mBufDataLen);
    mBufUsedLen = 0;

    if(bufferFinished() && mBufDataLen>0) {
        if(mFastqBuf[mBufDataLen-1] != '\n')
            mHasNoLineBreakAtEnd = true;
    }
}

char* FastqReader::readRawBuffer(int& dataLen) {
    if (mBufDataLen == 0)
        readToBuf();
    if (mBufDataLen == 0 && bufferFinished()) {
        dataLen = 0;
        return nullptr;
    }
    dataLen = mBufDataLen;
    char* result = mFastqBuf;
    mFastqBuf = new char[FQ_BUF_SIZE];
    mBufDataLen = 0;
    mBufUsedLen = 0;
    return result;
}

void FastqReader::init(){
    if (ends_with(mFilename, ".gz")) {
        mRawReader = new FastqGzReader(mFilename);
    } else if (ends_with(mFilename, ".zst")) {
        mRawReader = new FastqZstReader(mFilename);
    } else {
        mRawReader = new FastqRawReader(mFilename);
    }
    mZipped = mRawReader->isCompressed();
    readToBuf();
}

void FastqReader::getBytes(size_t& bytesRead, size_t& bytesTotal) {
    bytesRead = mRawReader ? mRawReader->bytesRead() : 0;
    ifstream is(mFilename);
    is.seekg (0, is.end);
    bytesTotal = is.tellg();
}

void FastqReader::clearLineBreaks(char* line) {
    int readed = strlen(line);
    if(readed >=2 ){
        if(line[readed-1] == '\n' || line[readed-1] == '\r'){
            line[readed-1] = '\0';
            if(line[readed-2] == '\r')
                line[readed-2] = '\0';
        }
    }
}

bool FastqReader::eof() {
    return mRawReader ? mRawReader->eof() : true;
}

void FastqReader::getLine(string* line){
    int start = mBufUsedLen;
    int end;

    const char* nl = (const char*)memchr(mFastqBuf + start, '\n', mBufDataLen - start);
    if (nl) {
        end = nl - mFastqBuf;
    } else {
        end = mBufDataLen;
    }

    if(end < mBufDataLen || bufferFinished()) {
        int len = end - start;
        if(len > 0 && mFastqBuf[start + len - 1] == '\r')
            len--;
        line->assign(mFastqBuf+start, len);

        if(end < mBufDataLen)
            end++;

        mBufUsedLen = end;
        return ;
    }

    line->assign(mFastqBuf+start, mBufDataLen - start);

    while(true) {
        readToBuf();
        start = 0;
        end = 0;
        if(line->empty()) {
            while(start < mBufDataLen && (mFastqBuf[start] == '\r' || mFastqBuf[start] == '\n'))
                start++;
            end = start;
        } else if(line->back() == '\r' && mBufDataLen > 0 && mFastqBuf[0] == '\n') {
            line->pop_back();
            mBufUsedLen = 1;
            return;
        }

        nl = (const char*)memchr(mFastqBuf + end, '\n', mBufDataLen - end);
        if (nl) {
            end = nl - mFastqBuf;
        } else {
            end = mBufDataLen;
        }

        if(end < mBufDataLen || bufferFinished()) {
            int len = end - start;
            if(len > 0 && mFastqBuf[start + len - 1] == '\r')
                len--;
            line->append(mFastqBuf+start, len);

            if(end < mBufDataLen)
                end++;

            mBufUsedLen = end;
            return;
        }
        line->append(mFastqBuf+start, mBufDataLen - start);
    }

    return;
}

Read* FastqReader::read(){
    if(mBufUsedLen >= mBufDataLen && bufferFinished()) {
        return NULL;
    }

    string* name = new string();
    string* sequence = new string();
    string* strand = new string();
    string* quality = new string();

    getLine(name);
    while((name->empty() && !(mBufUsedLen >= mBufDataLen && bufferFinished())) || (!name->empty() && (*name)[0]!='@')){
        getLine(name);
    }
    if(name->empty())
        return NULL;

    getLine(sequence);
    getLine(strand);
    getLine(quality);

    if (strand->empty() || (*strand)[0]!='+') {
        cerr << *name << endl;
        cerr << "Expected '+', got " << *strand << endl;
        cerr << "Your FASTQ may be invalid, please check the tail of your FASTQ file" << endl;
        return NULL;
    }

    if(quality->length() != sequence->length()) {
        cerr << "ERROR: sequence and quality have different length:" << endl;
        cerr << *name << endl;
        cerr << *sequence << endl;
        cerr << *strand << endl;
        cerr << *quality << endl;
        cerr << "Your FASTQ may be invalid, please check the tail of your FASTQ file" << endl;
        return NULL;
    }

    return new Read(name, sequence, strand, quality, mPhred64);
}

void FastqReader::close(){
    if(mRawReader) {
        delete mRawReader;
        mRawReader = NULL;
    }
}

bool FastqReader::isZipFastq(string filename) {
    if (ends_with(filename, ".fastq.gz"))
        return true;
    else if (ends_with(filename, ".fq.gz"))
        return true;
    else if (ends_with(filename, ".fasta.gz"))
        return true;
    else if (ends_with(filename, ".fa.gz"))
        return true;
    else if (ends_with(filename, ".fastq.zst"))
        return true;
    else if (ends_with(filename, ".fq.zst"))
        return true;
    else if (ends_with(filename, ".fasta.zst"))
        return true;
    else if (ends_with(filename, ".fa.zst"))
        return true;
    else
        return false;
}

bool FastqReader::isFastq(string filename) {
    if (ends_with(filename, ".fastq"))
        return true;
    else if (ends_with(filename, ".fq"))
        return true;
    else if (ends_with(filename, ".fasta"))
        return true;
    else if (ends_with(filename, ".fa"))
        return true;
    else if (ends_with(filename, ".fastq.zst"))
        return true;
    else if (ends_with(filename, ".fq.zst"))
        return true;
    else if (ends_with(filename, ".fasta.zst"))
        return true;
    else if (ends_with(filename, ".fa.zst"))
        return true;
    else
        return false;
}

bool FastqReader::isZipped(){
    return mZipped;
}

bool FastqReader::test(){
    bool passed = true;
    FastqReader reader1("testdata/R1.fq");
    FastqReader reader2("testdata/R1.fq");
    Read* r1 = NULL;
    Read* r2 = NULL;
    int i=0;
    while(true){
        i++;
        r1=reader1.read();
        r2=reader2.read();
        if(r1 == NULL || r2==NULL)
            break;
        delete r1;
        delete r2;
    }
    return passed;
}

FastqReaderPair::FastqReaderPair(FastqReader* left, FastqReader* right){
    mLeft = left;
    mRight = right;
}

FastqReaderPair::FastqReaderPair(string leftName, string rightName, bool hasQuality, bool phred64, bool interleaved, int workerThreads){
    mInterleaved = interleaved;
    mLeft = new FastqReader(leftName, hasQuality, phred64, workerThreads);
    if(mInterleaved)
        mRight = NULL;
    else
        mRight = new FastqReader(rightName, hasQuality, phred64, workerThreads);
}

FastqReaderPair::~FastqReaderPair(){
    if(mLeft){
        delete mLeft;
        mLeft = NULL;
    }
    if(mRight){
        delete mRight;
        mRight = NULL;
    }
}

void FastqReaderPair::read(ReadPair* pair){
    Read* l = mLeft->read();
    Read* r = NULL;
    if(mInterleaved)
        r = mLeft->read();
    else
        r = mRight->read();
    pair->setPair(l, r);
}
