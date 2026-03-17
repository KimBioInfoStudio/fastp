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
#include <iostream>
#include <cstdlib>

static const size_t ISAL_BATCH_MIN_SIZE = 256 << 10;      // 256 KB
static const size_t ISAL_BATCH_DEFAULT_SIZE = 512 << 10;  // 512 KB
static const size_t ISAL_BATCH_MAX_SIZE = 2 << 20;        // 2 MB

// Adaptive timeout constants for flight batch compression
static constexpr int64_t ADAPTIVE_MIN_TIMEOUT_US = 2000;   // 2ms floor
static constexpr int64_t ADAPTIVE_MAX_TIMEOUT_US = 50000;  // 50ms ceiling
static constexpr double  ADAPTIVE_EMA_ALPHA = 0.2;
static std::mutex gWriterLogMutex;

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
    mInputCompletedOnce.store(false, std::memory_order_relaxed);
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
    // pwrite mode only for compressed output (.gz) where parallel ISA-L compression
    // benefits from bypassing the single writer thread. For uncompressed .fq output,
    // the simple writer path is faster (no flight batch / offset ring overhead).
    mPwriteMode = mPreCompressed && !isSTDOUT && mOptions->thread > 1 && !ends_with(filename, ".zst");
    mFd = -1;
    mOffsetRing = NULL;
    mNextSeq = NULL;
    mBufferLists = NULL;
    mCompressInFlightBytes = 0;
    mCompressInFlightByteLimit = 0;
    mCompressInFlightChunkLimit = 0;
    initCompressionFlightControl();
    mFixedBatchMode = false;
    mFixedBatchSize = ISAL_BATCH_DEFAULT_SIZE;
    const char* batchKbEnv = getenv("FASTP_ISAL_BATCH_KB");
    if(batchKbEnv && batchKbEnv[0]) {
        long kb = atol(batchKbEnv);
        if(kb > 0) {
            size_t fixed = (size_t)kb << 10;
            if(fixed < ISAL_BATCH_MIN_SIZE) fixed = ISAL_BATCH_MIN_SIZE;
            if(fixed > ISAL_BATCH_MAX_SIZE) fixed = ISAL_BATCH_MAX_SIZE;
            mFixedBatchMode = true;
            mFixedBatchSize = fixed;
        }
    }
    mFlushBySizeCount.store(0, std::memory_order_relaxed);
    mFlushByTimeoutCount.store(0, std::memory_order_relaxed);
    mFlushByFinalizeCount.store(0, std::memory_order_relaxed);
    mFlushedRawBytes.store(0, std::memory_order_relaxed);
    mFlushedCompressedBytes.store(0, std::memory_order_relaxed);
    mFirstFlushLatencyUs.store(-1, std::memory_order_relaxed);
    mPwriteWaitCalls.store(0, std::memory_order_relaxed);
    mPwriteWaitUsTotal.store(0, std::memory_order_relaxed);
    mPwriteWaitUsMax.store(0, std::memory_order_relaxed);
    mPwriteBytesTotal.store(0, std::memory_order_relaxed);
    mPwriteWrites.store(0, std::memory_order_relaxed);
    mPwriteFirstWriteLatencyUs.store(-1, std::memory_order_relaxed);

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
    mCreatedTs = now;
    mLastInputTs = new std::chrono::steady_clock::time_point[mOptions->thread];
    mLastFlushTs = new std::chrono::steady_clock::time_point[mOptions->thread];
    mIngressBpsEma = new double[mOptions->thread]();
    mDynamicTimeoutUs = new int64_t[mOptions->thread];
    mDynamicBatchTarget = new size_t[mOptions->thread];
    for (int t = 0; t < mOptions->thread; t++) {
        mLastInputTs[t] = now;
        mLastFlushTs[t] = now;
        mDynamicTimeoutUs[t] = ADAPTIVE_MAX_TIMEOUT_US;
        mDynamicBatchTarget[t] = mFixedBatchMode ? mFixedBatchSize : ISAL_BATCH_DEFAULT_SIZE;
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
    if (mInputCompletedOnce.exchange(true, std::memory_order_acq_rel))
        return false;

    if (mPwriteMode) {
        setInputCompletedPwrite();
        mInputCompleted = true;
        return true;
    }
    // Flush remaining accumulated data (workers already joined, safe to produce)
    if(mPreCompressed) {
        for(int t=0; t<mOptions->thread; t++) {
            if(!mAccumBuf[t].empty()) {
                const size_t rawBytes = mAccumBuf[t].size();
                string compressed = compressGzipOrDie(mAccumBuf[t], mIsalLevel);
                enqueueCompressedChunk(t, std::move(compressed));
                mFlushByFinalizeCount.fetch_add(1, std::memory_order_relaxed);
                mFlushedRawBytes.fetch_add((unsigned long long)rawBytes, std::memory_order_relaxed);
                mAccumBuf[t].clear();
            }
        }
        if(mOptions->verbose) {
            const long bySize = mFlushBySizeCount.load(std::memory_order_relaxed);
            const long byTimeout = mFlushByTimeoutCount.load(std::memory_order_relaxed);
            const long byFinalize = mFlushByFinalizeCount.load(std::memory_order_relaxed);
            const unsigned long long rawTotal = mFlushedRawBytes.load(std::memory_order_relaxed);
            const unsigned long long gzTotal = mFlushedCompressedBytes.load(std::memory_order_relaxed);
            const long firstFlushUs = mFirstFlushLatencyUs.load(std::memory_order_relaxed);
            const long flushCnt = bySize + byTimeout + byFinalize;
            std::lock_guard<std::mutex> lk(gWriterLogMutex);
            cerr << "[writer.flight] file=" << mFilename
                 << " flush.size=" << bySize
                 << " flush.timeout=" << byTimeout
                 << " flush.finalize=" << byFinalize
                 << " avg_raw_batch_kb=" << (flushCnt > 0 ? (rawTotal / flushCnt) / 1024ULL : 0ULL)
                 << " batch_mode=" << (mFixedBatchMode ? "fixed" : "auto")
                 << " batch_kb=" << (mFixedBatchMode ? (mFixedBatchSize >> 10) : 0ULL)
                 << " ratio=" << (rawTotal > 0 ? (double)gzTotal / (double)rawTotal : 0.0)
                 << " first_flush_ms=" << (firstFlushUs >= 0 ? firstFlushUs / 1000.0 : -1.0)
                 << endl;
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

    if (mOptions->verbose) {
        const long waitCalls = mPwriteWaitCalls.load(std::memory_order_relaxed);
        const unsigned long long waitUsTotal = mPwriteWaitUsTotal.load(std::memory_order_relaxed);
        const long waitUsMax = mPwriteWaitUsMax.load(std::memory_order_relaxed);
        const unsigned long long bytesTotal = mPwriteBytesTotal.load(std::memory_order_relaxed);
        const long writes = mPwriteWrites.load(std::memory_order_relaxed);
        const long firstWriteUs = mPwriteFirstWriteLatencyUs.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(gWriterLogMutex);
        cerr << "[writer.pwrite] file=" << mFilename
             << " writes=" << writes
             << " bytes=" << bytesTotal
             << " wait_calls=" << waitCalls
             << " wait_us_total=" << waitUsTotal
             << " wait_us_avg=" << (waitCalls > 0 ? (waitUsTotal / (unsigned long long)waitCalls) : 0ULL)
             << " wait_us_max=" << waitUsMax
             << " first_write_ms=" << (firstWriteUs >= 0 ? firstWriteUs / 1000.0 : -1.0)
             << endl;
    }
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
        if(!mFixedBatchMode)
            updateAdaptiveBatchTarget(tid);

        auto flushAccum = [&](long reasonCounter) {
            const size_t rawBytes = mAccumBuf[tid].size();
            if (rawBytes == 0)
                return;
            string compressed = compressGzipOrDie(mAccumBuf[tid], mIsalLevel);
            enqueueCompressedChunk(tid, std::move(compressed));
            mAccumBuf[tid].clear();
            mLastFlushTs[tid] = now;
            if(mFirstFlushLatencyUs.load(std::memory_order_relaxed) < 0) {
                const long us = (long)std::chrono::duration_cast<std::chrono::microseconds>(now - mCreatedTs).count();
                long expected = -1;
                mFirstFlushLatencyUs.compare_exchange_strong(expected, us, std::memory_order_relaxed);
            }
            if(reasonCounter == 0)
                mFlushBySizeCount.fetch_add(1, std::memory_order_relaxed);
            else
                mFlushByTimeoutCount.fetch_add(1, std::memory_order_relaxed);
            mFlushedRawBytes.fetch_add((unsigned long long)rawBytes, std::memory_order_relaxed);
        };

        // Adaptive timeout flush (before accumulating new data)
        if (!mAccumBuf[tid].empty()) {
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                now - mLastFlushTs[tid]).count();
            if (elapsed_us >= mDynamicTimeoutUs[tid]) {
                flushAccum(1);
            }
        }

        // Flight batching: accumulate raw data, compress when >= threshold
        mAccumBuf[tid].append(data->data(), data->length());
        delete data;
        if(mAccumBuf[tid].size() >= mDynamicBatchTarget[tid]) {
            flushAccum(0);
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
        auto waitStart = std::chrono::steady_clock::now();
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
        auto waitEnd = std::chrono::steady_clock::now();
        long waitUs = (long)std::chrono::duration_cast<std::chrono::microseconds>(waitEnd - waitStart).count();
        if(waitUs > 0) {
            mPwriteWaitCalls.fetch_add(1, std::memory_order_relaxed);
            mPwriteWaitUsTotal.fetch_add((unsigned long long)waitUs, std::memory_order_relaxed);
            long curMax = mPwriteWaitUsMax.load(std::memory_order_relaxed);
            while(waitUs > curMax && !mPwriteWaitUsMax.compare_exchange_weak(curMax, waitUs, std::memory_order_relaxed)) {}
        }
        offset = mOffsetRing[prevSlot].cumulative_offset.load(std::memory_order_relaxed);
    }

    // Publish cumulative offset BEFORE pwrite — next worker can start immediately
    // without waiting for this I/O to complete. pwrite to non-overlapping regions
    // is safe to run concurrently.
    size_t wsize = writeData.size();
    size_t mySlot = seq & (OFFSET_RING_SIZE - 1);
    mOffsetRing[mySlot].cumulative_offset.store(offset + wsize, std::memory_order_relaxed);
    mOffsetRing[mySlot].published_seq.store(seq, std::memory_order_release);

    // pwrite data (skip if zero-size passthrough)
    if (wsize > 0) {
        if(mPwriteFirstWriteLatencyUs.load(std::memory_order_relaxed) < 0) {
            const long us = (long)std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - mCreatedTs).count();
            long expected = -1;
            mPwriteFirstWriteLatencyUs.compare_exchange_strong(expected, us, std::memory_order_relaxed);
        }
        size_t written = 0;
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

        mPwriteBytesTotal.fetch_add((unsigned long long)written, std::memory_order_relaxed);
        mPwriteWrites.fetch_add(1, std::memory_order_relaxed);
    }

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
        double target_us = static_cast<double>(mDynamicBatchTarget[tid])
                         / (mIngressBpsEma[tid] > 1.0 ? mIngressBpsEma[tid] : 1.0) * 1e6;
        int64_t clamped = static_cast<int64_t>(target_us);
        if (clamped < ADAPTIVE_MIN_TIMEOUT_US) clamped = ADAPTIVE_MIN_TIMEOUT_US;
        if (clamped > ADAPTIVE_MAX_TIMEOUT_US) clamped = ADAPTIVE_MAX_TIMEOUT_US;
        mDynamicTimeoutUs[tid] = clamped;
    }
}

void WriterThread::updateAdaptiveBatchTarget(int tid) {
    size_t target = ISAL_BATCH_DEFAULT_SIZE;
    double ema = mIngressBpsEma[tid];
    if (ema >= 300.0 * 1024 * 1024)      target = 2 << 20;
    else if (ema >= 180.0 * 1024 * 1024) target = 1 << 20;
    else if (ema >= 90.0  * 1024 * 1024) target = 768 << 10;
    else if (ema <= 24.0  * 1024 * 1024) target = 256 << 10;

    const long inFlight = mCompressInFlightBytes.load(std::memory_order_relaxed);
    if (mCompressInFlightByteLimit > 0) {
        const double fill = (double)inFlight / (double)mCompressInFlightByteLimit;
        if (fill > 0.80)
            target = std::max(ISAL_BATCH_MIN_SIZE, target / 2);
        else if (fill < 0.20 && target < ISAL_BATCH_MAX_SIZE)
            target = std::min(ISAL_BATCH_MAX_SIZE, target + (256 << 10));
    }

    size_t prev = mDynamicBatchTarget[tid];
    size_t smooth = (prev * 3 + target) / 4;
    if (smooth < ISAL_BATCH_MIN_SIZE) smooth = ISAL_BATCH_MIN_SIZE;
    if (smooth > ISAL_BATCH_MAX_SIZE) smooth = ISAL_BATCH_MAX_SIZE;
    mDynamicBatchTarget[tid] = smooth;
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
    mFlushedCompressedBytes.fetch_add((unsigned long long)bytes, std::memory_order_relaxed);
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
    delete[] mDynamicBatchTarget; mDynamicBatchTarget = NULL;

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
