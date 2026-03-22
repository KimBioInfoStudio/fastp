#ifndef WRITER_THREAD_H
#define WRITER_THREAD_H

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include "writer.h"
#include "options.h"
#include <atomic>
#include <mutex>
#include <libdeflate.h>
#include "singleproducersingleconsumerlist.h"

using namespace std;

static constexpr int OFFSET_RING_SIZE = 512;

struct alignas(64) OffsetSlot {
    std::atomic<size_t> cumulative_offset{0};
    std::atomic<size_t> published_seq{SIZE_MAX};
};

class WriterThread{
public:
    WriterThread(Options* opt, string filename, bool isSTDOUT = false);
    ~WriterThread();

    void initWriter(string filename1, bool isSTDOUT = false);
    void initBufferLists();

    void cleanup();

    bool isCompleted();
    void output();
    void input(int tid, string* data);
    bool setInputCompleted();

    long bufferLength() {return mBufferLength;};
    string getFilename() {return mFilename;}
    bool isPwriteMode() {return mPwriteMode;}

private:
    void deleteWriter();
    void inputPwrite(int tid, string* data);
    void flushPwriteBatch(int tid);
    void setInputCompletedPwrite();

private:
    Writer* mWriter1;
    Options* mOptions;
    string mFilename;

    bool mInputCompleted;
    atomic_long mBufferLength;
    SingleProducerSingleConsumerList<string*>** mBufferLists;
    int mWorkingBufferList;

    // pwrite mode: parallel ISA-L gz compression + direct file write
    bool mPwriteMode;
    bool mPreCompressed;
    int mCompressionLevel;
    int mFd;
    OffsetSlot* mOffsetRing;
    size_t* mNextSeq;
    string* mAccumBuf;
    libdeflate_compressor** mCompressors;  // per-worker compressor instances
};

#endif
