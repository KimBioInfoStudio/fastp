#include "writerthread.h"
#include "util.h"
#include "isal_compress.h"
#include <memory.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <thread>
#include <algorithm>

static const size_t ISAL_BATCH_SIZE = 512 << 10; // 512 KB accumulation threshold

// Adaptive timeout constants for flight batch compression
static constexpr int64_t ADAPTIVE_MIN_TIMEOUT_US = 2000;   // 2ms floor
static constexpr int64_t ADAPTIVE_MAX_TIMEOUT_US = 50000;  // 50ms ceiling
static constexpr double  ADAPTIVE_EMA_ALPHA = 0.2;

static inline string compressGzipOrDie(const string& raw, int level) {
    if (raw.empty()) {
        return string();
    }
    string compressed = isal_gzip_compress(raw.data(), raw.size(), level);
    if (compressed.empty()) {
        error_exit("ISA-L gzip compression failed");
    }
    return compressed;
}

WriterThread::WriterThread(Options* opt, string filename, bool isSTDOUT){
    mOptions = opt;

    mWriter1 = NULL;

    mInputCompleted = false;
    mFilename = filename;
    // Auto-detect: use parallel ISA-L gzip when writing to .gz files (not STDOUT)
    mPreCompressed = !isSTDOUT && ends_with(filename, ".gz");

    // Pre-compute ISA-L compression level (map fastp 1-9 -> ISA-L 0-3)
    mIsalLevel = 1;
    if(mOptions->compression <= 2)
        mIsalLevel = 0;
    else if(mOptions->compression <= 5)
        mIsalLevel = 1;
    else if(mOptions->compression <= 7)
        mIsalLevel = 2;
    else
        mIsalLevel = 3;

    // pwrite mode: multi-threaded file output (not STDOUT, not single-threaded)
    mPwriteMode = !isSTDOUT && mOptions->thread > 1 && !ends_with(filename, ".zst");
    mFd = -1;
    mOffsetRing = NULL;
    mNextSeq = NULL;
    mBufferLists = NULL;
    mCompressInFlightBytes = 0;
    mCompressInFlightByteLimit = 0;
    mCompressInFlightChunkLimit = 0;
    initCompressionFlightControl();

    if (mPwriteMode) {
        mFd = open(mFilename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (mFd < 0) {
            error_exit("Failed to open for pwrite: " + mFilename);
        }
        mOffsetRing = new OffsetSlot[OFFSET_RING_SIZE];
        mNextSeq = new size_t[mOptions->thread];
        for (int t = 0; t < mOptions->thread; t++)
            mNextSeq[t] = t;
        mAccumBuf = new string[mOptions->thread];
        mWorkingBufferList = 0;
        mBufferLength = 0;
    } else {
        initWriter(filename, isSTDOUT);
        initBufferLists();
        mWorkingBufferList = 0;
        mBufferLength = 0;
        mAccumBuf = new string[mOptions->thread];
    }

    // Adaptive timeout state (shared by both modes)
    auto now = std::chrono::steady_clock::now();
    mLastInputTs = new std::chrono::steady_clock::time_point[mOptions->thread];
    mLastFlushTs = new std::chrono::steady_clock::time_point[mOptions->thread];
    mIngressBpsEma = new double[mOptions->thread]();
    mDynamicTimeoutUs = new int64_t[mOptions->thread];
    for (int t = 0; t < mOptions->thread; t++) {
        mLastInputTs[t] = now;
        mLastFlushTs[t] = now;
        mDynamicTimeoutUs[t] = ADAPTIVE_MAX_TIMEOUT_US;
    }
}

WriterThread::~WriterThread() {
    cleanup();
}

bool WriterThread::isCompleted()
{
    if (mPwriteMode) return true;  // writer thread exits immediately
    return mInputCompleted && (mBufferLength==0);
}

bool WriterThread::setInputCompleted() {
    if (mPwriteMode) {
        setInputCompletedPwrite();
        mInputCompleted = true;
        return true;
    }
    // Flush remaining accumulated data (workers already joined, safe to produce)
    if(mPreCompressed) {
        for(int t=0; t<mOptions->thread; t++) {
            if(!mAccumBuf[t].empty()) {
                string compressed = compressGzipOrDie(mAccumBuf[t], mIsalLevel);
                enqueueCompressedChunk(t, std::move(compressed));
                mAccumBuf[t].clear();
            }
        }
    }
    mInputCompleted = true;
    for(int t=0; t<mOptions->thread; t++) {
        mBufferLists[t]->setProducerFinished();
    }
    mOutputCv.notify_all();
    return true;
}

void WriterThread::setInputCompletedPwrite() {
    int W = mOptions->thread;

    // Find cumulative offset after the last processed pack
    size_t lastSeq = 0;
    bool anyProcessed = false;
    for (int t = 0; t < W; t++) {
        if (mNextSeq[t] != (size_t)t) {  // worker processed at least 1 pack
            size_t workerLastSeq = mNextSeq[t] - W;
            if (!anyProcessed || workerLastSeq > lastSeq) {
                lastSeq = workerLastSeq;
                anyProcessed = true;
            }
        }
    }

    size_t offset = anyProcessed ?
        mOffsetRing[lastSeq & (OFFSET_RING_SIZE - 1)].cumulative_offset.load(std::memory_order_relaxed) : 0;

    ftruncate(mFd, offset);
}

void WriterThread::output(){
    if (mPwriteMode) return;  // no-op
    SingleProducerSingleConsumerList<string*>* list =  mBufferLists[mWorkingBufferList];
    if(!list->canBeConsumed()) {
        std::unique_lock<std::mutex> lk(mOutputMutex);
        mOutputCv.wait_for(lk, std::chrono::microseconds(200), [&]() {
            return list->canBeConsumed() || (mInputCompleted && mBufferLength.load(std::memory_order_relaxed) == 0);
        });
    } else {
        string* str = list->consume();
        size_t bytes = str->length();
        mWriter1->write(str->data(), str->length());
        delete str;
        mBufferLength--;
        releaseCompressedChunk(bytes);
        mWorkingBufferList = (mWorkingBufferList+1)%mOptions->thread;
    }
}


void WriterThread::input(int tid, string* data) {
    if (mPwriteMode) {
        inputPwrite(tid, data);
        return;
    }
    if(mPreCompressed && !data->empty()) {
        auto now = std::chrono::steady_clock::now();
        updateAdaptiveTimeout(tid, data->size(), now);

        // Adaptive timeout flush (before accumulating new data)
        if (!mAccumBuf[tid].empty()) {
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                now - mLastFlushTs[tid]).count();
            if (elapsed_us >= mDynamicTimeoutUs[tid]) {
                string compressed = compressGzipOrDie(mAccumBuf[tid], mIsalLevel);
                enqueueCompressedChunk(tid, std::move(compressed));
                mAccumBuf[tid].clear();
                mLastFlushTs[tid] = now;
            }
        }

        // Flight batching: accumulate raw data, compress when >= threshold
        mAccumBuf[tid].append(data->data(), data->length());
        delete data;
        if(mAccumBuf[tid].size() >= ISAL_BATCH_SIZE) {
            string compressed = compressGzipOrDie(mAccumBuf[tid], mIsalLevel);
            enqueueCompressedChunk(tid, std::move(compressed));
            mAccumBuf[tid].clear();
            mLastFlushTs[tid] = now;
        }
        return;
    }
    mBufferLists[tid]->produce(data);
    mBufferLength++;
    mOutputCv.notify_one();
}

void WriterThread::inputPwrite(int tid, string* data) {
    size_t seq = mNextSeq[tid];
    string writeData;

    if (mPreCompressed) {
        // Compress each pack individually to maintain correct file ordering.
        // Cross-pack accumulation is incompatible with interleaved sequence
        // numbering: worker 0 handles packs 0,3,6,... so batching them would
        // place pack 0+3 data at pack 3's offset, before packs 1 and 2.
        writeData = compressGzipOrDie(*data, mIsalLevel);
        delete data;
    } else {
        // .fq: write pack data directly
        writeData = std::move(*data);
        delete data;
    }

    // Wait for previous pack's cumulative offset (progressive backoff)
    size_t offset = 0;
    if (seq > 0) {
        size_t prevSlot = (seq - 1) & (OFFSET_RING_SIZE - 1);
        int spins = 0;
        while (mOffsetRing[prevSlot].published_seq.load(std::memory_order_acquire) != seq - 1) {
            if (++spins <= 32) {
#if defined(__aarch64__)
                __asm__ volatile("yield");
#elif defined(__x86_64__) || defined(__i386__)
                __asm__ volatile("pause");
#endif
            } else {
                std::this_thread::yield();
                spins = 0;
            }
        }
        offset = mOffsetRing[prevSlot].cumulative_offset.load(std::memory_order_relaxed);
    }

    // pwrite data (skip if zero-size passthrough)
    size_t wsize = writeData.size();
    size_t written = 0;
    if (wsize > 0) {
        while (written < wsize) {
            ssize_t ret = pwrite(mFd, writeData.data() + written, wsize - written, offset + written);
            if (ret < 0) {
                if (errno == EINTR) continue;
                error_exit("pwrite failed: " + string(strerror(errno)));
            }
            if (ret == 0) {
                error_exit("pwrite returned 0 (disk full?)");
            }
            written += ret;
        }
    }

    // Publish cumulative offset for next pack
    size_t mySlot = seq & (OFFSET_RING_SIZE - 1);
    mOffsetRing[mySlot].cumulative_offset.store(offset + written, std::memory_order_relaxed);
    mOffsetRing[mySlot].published_seq.store(seq, std::memory_order_release);

    mNextSeq[tid] += mOptions->thread;
}

void WriterThread::updateAdaptiveTimeout(int tid, size_t bytes,
                                          std::chrono::steady_clock::time_point now) {
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now - mLastInputTs[tid]).count();
    mLastInputTs[tid] = now;
    if (elapsed_us > 0) {
        double instant_bps = static_cast<double>(bytes) / elapsed_us * 1e6;
        mIngressBpsEma[tid] = (1.0 - ADAPTIVE_EMA_ALPHA) * mIngressBpsEma[tid]
                            + ADAPTIVE_EMA_ALPHA * instant_bps;
        double target_us = static_cast<double>(ISAL_BATCH_SIZE)
                         / (mIngressBpsEma[tid] > 1.0 ? mIngressBpsEma[tid] : 1.0) * 1e6;
        int64_t clamped = static_cast<int64_t>(target_us);
        if (clamped < ADAPTIVE_MIN_TIMEOUT_US) clamped = ADAPTIVE_MIN_TIMEOUT_US;
        if (clamped > ADAPTIVE_MAX_TIMEOUT_US) clamped = ADAPTIVE_MAX_TIMEOUT_US;
        mDynamicTimeoutUs[tid] = clamped;
    }
}

void WriterThread::initCompressionFlightControl() {
    // Auto-mode only: bound queued pre-compressed chunks and bytes.
    int chunkLimit = std::max(8, mOptions->thread * 2);
    if (chunkLimit > 256)
        chunkLimit = 256;

    long byteLimit = (long)chunkLimit * 1024L * 1024L;
    if (byteLimit < 16L * 1024L * 1024L)
        byteLimit = 16L * 1024L * 1024L;
    if (byteLimit > 512L * 1024L * 1024L)
        byteLimit = 512L * 1024L * 1024L;

    mCompressInFlightChunkLimit = chunkLimit;
    mCompressInFlightByteLimit = byteLimit;
    mCompressInFlightBytes.store(0, std::memory_order_relaxed);
    mCompressFlight.configure(chunkLimit, 1);
    mCompressFlight.setSync(&mCompressFlightMutex, &mCompressFlightCv, NULL);
}

void WriterThread::enqueueCompressedChunk(int tid, string&& compressed) {
    if (!mPreCompressed) {
        mBufferLists[tid]->produce(new string(std::move(compressed)));
        mBufferLength++;
        mOutputCv.notify_one();
        return;
    }

    const long bytes = (long)compressed.size();
    while (true) {
        long cur = mCompressInFlightBytes.load(std::memory_order_relaxed);
        if (cur + bytes <= mCompressInFlightByteLimit) {
            if (mCompressInFlightBytes.compare_exchange_weak(cur, cur + bytes, std::memory_order_relaxed))
                break;
            continue;
        }
        std::unique_lock<std::mutex> lk(mCompressFlightMutex);
        mCompressFlightCv.wait(lk, [&]() {
            return mCompressInFlightBytes.load(std::memory_order_relaxed) + bytes <= mCompressInFlightByteLimit;
        });
    }

    mCompressFlight.acquireForNextPack(mBufferLength.load(std::memory_order_relaxed));
    mBufferLists[tid]->produce(new string(std::move(compressed)));
    mBufferLength++;
    mOutputCv.notify_one();
}

void WriterThread::releaseCompressedChunk(size_t bytes) {
    if (!mPreCompressed)
        return;

    long remain = mCompressInFlightBytes.fetch_sub((long)bytes, std::memory_order_relaxed) - (long)bytes;
    if (remain < 0)
        mCompressInFlightBytes.store(0, std::memory_order_relaxed);
    mCompressFlight.releaseAfterConsume(mBufferLength.load(std::memory_order_relaxed));
    mCompressFlightCv.notify_all();
}

void WriterThread::cleanup() {
    delete[] mLastInputTs;  mLastInputTs = NULL;
    delete[] mLastFlushTs;  mLastFlushTs = NULL;
    delete[] mIngressBpsEma; mIngressBpsEma = NULL;
    delete[] mDynamicTimeoutUs; mDynamicTimeoutUs = NULL;

    if (mPwriteMode) {
        if (mFd >= 0) {
            close(mFd);
            mFd = -1;
        }
        delete[] mOffsetRing;
        mOffsetRing = NULL;
        delete[] mNextSeq;
        mNextSeq = NULL;
        delete[] mAccumBuf;
        mAccumBuf = NULL;
        return;
    }
    deleteWriter();
    if (mBufferLists) {
        for(int t=0; t<mOptions->thread; t++) {
            delete mBufferLists[t];
        }
        delete[] mBufferLists;
        mBufferLists = NULL;
    }
    delete[] mAccumBuf;
    mAccumBuf = NULL;
}

void WriterThread::deleteWriter() {
    if(mWriter1 != NULL) {
        delete mWriter1;
        mWriter1 = NULL;
    }
}

void WriterThread::initWriter(string filename1, bool isSTDOUT) {
    deleteWriter();
    mWriter1 = new Writer(mOptions, filename1, mOptions->compression, isSTDOUT, mPreCompressed);
}

void WriterThread::initBufferLists() {
    mBufferLists = new SingleProducerSingleConsumerList<string*>*[mOptions->thread];
    for(int t=0; t<mOptions->thread; t++) {
        mBufferLists[t] = new SingleProducerSingleConsumerList<string*>();
    }
}
