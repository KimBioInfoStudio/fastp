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
#include <condition_variable>
#include "singleproducersingleconsumerlist.h"
#include <chrono>
#include "flight_batch_manager.h"
#ifdef __linux__
#include "io_uring_raw.h"
struct IoUringPendingWrite {
    std::string data;
    size_t offset;
};
#endif

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
    void inputPwriteSync(int tid, string* data, const string& writeData, size_t offset);
    void setInputCompletedPwrite();
#ifdef __linux__
    void inputIoUring(int tid, string* data);
    void drainIoUringCqes();
    void flushIoUring();
#endif
    void updateAdaptiveTimeout(int tid, size_t bytes, std::chrono::steady_clock::time_point now);
    void updateAdaptiveBatchTarget(int tid);
    void initCompressionFlightControl();
    void enqueueCompressedChunk(int tid, string&& compressed);
    void releaseCompressedChunk(size_t bytes);

private:
    Writer* mWriter1;
    Options* mOptions;
    string mFilename;

    // for spliting output
    bool mInputCompleted;
    std::atomic_bool mInputCompletedOnce;
    atomic_long mBufferLength;
    SingleProducerSingleConsumerList<string*>** mBufferLists;
    int mWorkingBufferList;
    bool mPreCompressed;
    int mIsalLevel;
    string* mAccumBuf;  // per-thread accumulation for flight batching

    // pwrite parallel write mode
    bool mPwriteMode;
    int mFd;
    OffsetSlot* mOffsetRing;
    size_t* mNextSeq;  // per-worker pack sequence counter

    // Adaptive timeout for flight batch compression
    std::chrono::steady_clock::time_point* mLastInputTs;
    std::chrono::steady_clock::time_point* mLastFlushTs;
    std::chrono::steady_clock::time_point mCreatedTs;
    double* mIngressBpsEma;
    int64_t* mDynamicTimeoutUs;
    size_t* mDynamicBatchTarget;
    bool mFixedBatchMode;
    size_t mFixedBatchSize;
    std::atomic_long mFlushBySizeCount;
    std::atomic_long mFlushByTimeoutCount;
    std::atomic_long mFlushByFinalizeCount;
    std::atomic_ullong mFlushedRawBytes;
    std::atomic_ullong mFlushedCompressedBytes;
    std::atomic_long mFirstFlushLatencyUs;

    // pwrite path stats
    std::atomic_long mPwriteWaitCalls;
    std::atomic_ullong mPwriteWaitUsTotal;
    std::atomic_long mPwriteWaitUsMax;
    std::atomic_ullong mPwriteBytesTotal;
    std::atomic_long mPwriteWrites;
    std::atomic_long mPwriteFirstWriteLatencyUs;

    // io_uring async write (Linux only)
#ifdef __linux__
    IoUringRaw* mIoUring;
    std::mutex mIoUringMutex;
    std::atomic<int> mIoUringOutstanding{0};
#endif

    // Output wakeup for non-pwrite writer thread
    std::mutex mOutputMutex;
    std::condition_variable mOutputCv;

    // Pre-compress queue flight control (auto mode)
    FlightBatchManager mCompressFlight;
    std::mutex mCompressFlightMutex;
    std::condition_variable mCompressFlightCv;
    std::atomic_long mCompressInFlightBytes;
    long mCompressInFlightByteLimit;
    int mCompressInFlightChunkLimit;
};

#endif
