#include "writerthread.h"
#include "util.h"
#include <memory.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <thread>

WriterThread::WriterThread(Options* opt, string filename, bool isSTDOUT){
    mOptions = opt;
    mWriter1 = NULL;
    mInputCompleted = false;
    mFilename = filename;

    // Detect gz output for parallel compression
    mPreCompressed = !isSTDOUT && ends_with(filename, ".gz");

    mCompressionLevel = mOptions->compression;
    mCompressors = NULL;

    // pwrite mode: workers compress + pwrite in parallel (gz output, multi-threaded)
    mPwriteMode = mPreCompressed && !isSTDOUT && mOptions->thread > 1;
    mFd = -1;
    mOffsetRing = NULL;
    mNextSeq = NULL;
    mBufferLists = NULL;

    if (mPwriteMode) {
        mFd = open(mFilename.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (mFd < 0)
            error_exit("Failed to open for pwrite: " + mFilename);
        mOffsetRing = new OffsetSlot[OFFSET_RING_SIZE];
        mNextSeq = new size_t[mOptions->thread];
        for (int t = 0; t < mOptions->thread; t++)
            mNextSeq[t] = t;
        mAccumBuf = new string[mOptions->thread];
        mCompressors = new libdeflate_compressor*[mOptions->thread];
        for (int t = 0; t < mOptions->thread; t++)
            mCompressors[t] = libdeflate_alloc_compressor(mCompressionLevel);
        mWorkingBufferList = 0;
        mBufferLength = 0;
    } else {
        initWriter(filename, isSTDOUT);
        initBufferLists();
        mWorkingBufferList = 0;
        mBufferLength = 0;
    }
}

WriterThread::~WriterThread() {
    cleanup();
}

bool WriterThread::isCompleted()
{
    if (mPwriteMode) return true;  // no writer thread needed
    return mInputCompleted && (mBufferLength==0);
}

bool WriterThread::setInputCompleted() {
    if (mPwriteMode) {
        setInputCompletedPwrite();
        mInputCompleted = true;
        return true;
    }
    mInputCompleted = true;
    for(int t=0; t<mOptions->thread; t++) {
        mBufferLists[t]->setProducerFinished();
    }
    return true;
}

void WriterThread::setInputCompletedPwrite() {
    // Flush remaining accumulated data for all workers
    for (int t = 0; t < mOptions->thread; t++)
        flushPwriteBatch(t);

    int W = mOptions->thread;
    size_t lastSeq = 0;
    bool anyProcessed = false;
    for (int t = 0; t < W; t++) {
        if (mNextSeq[t] != (size_t)t) {
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
    SingleProducerSingleConsumerList<string*>* list = mBufferLists[mWorkingBufferList];
    if(!list->canBeConsumed()) {
        usleep(100);
    } else {
        string* str = list->consume();
        mWriter1->write(str->data(), str->length());
        delete str;
        mBufferLength--;
        mWorkingBufferList = (mWorkingBufferList+1)%mOptions->thread;
    }
}

void WriterThread::input(int tid, string* data) {
    if (mPwriteMode) {
        inputPwrite(tid, data);
        return;
    }
    mBufferLists[tid]->produce(data);
    mBufferLength++;
}

// 256KB batch: FASTQ's LZ77 window saturates at ~256KB (measured: 41.97% vs
// 41.67% single-stream). Beyond 256KB, compression ratio gains are < 0.15%.
static const size_t PWRITE_BATCH_SIZE = 256 * 1024;

void WriterThread::inputPwrite(int tid, string* data) {
    if (mPreCompressed) {
        // Accumulate raw data, compress as a larger batch for better ratio
        mAccumBuf[tid].append(*data);
        delete data;
        if (mAccumBuf[tid].size() >= PWRITE_BATCH_SIZE)
            flushPwriteBatch(tid);
    } else {
        // Uncompressed: write directly (shouldn't reach here, pwrite only for gz)
        delete data;
    }
}

void WriterThread::flushPwriteBatch(int tid) {
    if (mAccumBuf[tid].empty()) return;

    // Compress with per-worker libdeflate compressor (same algorithm as master)
    const string& raw = mAccumBuf[tid];
    size_t bound = libdeflate_gzip_compress_bound(mCompressors[tid], raw.size());
    string writeData;
    writeData.resize(bound);
    size_t outsize = libdeflate_gzip_compress(mCompressors[tid], raw.data(), raw.size(),
                                               &writeData[0], bound);
    if (outsize == 0)
        error_exit("libdeflate gzip compression failed");
    writeData.resize(outsize);
    mAccumBuf[tid].clear();

    size_t seq = mNextSeq[tid];

    // Wait for previous batch's cumulative offset
    size_t offset = 0;
    if (seq > 0) {
        size_t prevSlot = (seq - 1) & (OFFSET_RING_SIZE - 1);
        while (mOffsetRing[prevSlot].published_seq.load(std::memory_order_acquire) != seq - 1) {
#if defined(__aarch64__)
            __asm__ volatile("yield");
#elif defined(__x86_64__) || defined(__i386__)
            __asm__ volatile("pause");
#endif
        }
        offset = mOffsetRing[prevSlot].cumulative_offset.load(std::memory_order_relaxed);
    }

    // Publish offset BEFORE pwrite
    size_t wsize = writeData.size();
    size_t mySlot = seq & (OFFSET_RING_SIZE - 1);
    mOffsetRing[mySlot].cumulative_offset.store(offset + wsize, std::memory_order_relaxed);
    mOffsetRing[mySlot].published_seq.store(seq, std::memory_order_release);

    // pwrite
    if (wsize > 0) {
        size_t written = 0;
        while (written < wsize) {
            ssize_t ret = pwrite(mFd, writeData.data() + written, wsize - written, offset + written);
            if (ret < 0) {
                if (errno == EINTR) continue;
                error_exit("pwrite failed: " + string(strerror(errno)));
            }
            if (ret == 0)
                error_exit("pwrite returned 0 (disk full?)");
            written += ret;
        }
    }

    mNextSeq[tid] += mOptions->thread;
}

void WriterThread::cleanup() {
    if (mPwriteMode) {
        if (mFd >= 0) { close(mFd); mFd = -1; }
        delete[] mOffsetRing; mOffsetRing = NULL;
        delete[] mNextSeq; mNextSeq = NULL;
        delete[] mAccumBuf; mAccumBuf = NULL;
        if (mCompressors) {
            for (int t = 0; t < mOptions->thread; t++)
                libdeflate_free_compressor(mCompressors[t]);
            delete[] mCompressors; mCompressors = NULL;
        }
        return;
    }
    deleteWriter();
    if (mBufferLists) {
        for(int t=0; t<mOptions->thread; t++)
            delete mBufferLists[t];
        delete[] mBufferLists;
        mBufferLists = NULL;
    }
}

void WriterThread::deleteWriter() {
    if(mWriter1 != NULL) {
        delete mWriter1;
        mWriter1 = NULL;
    }
}

void WriterThread::initWriter(string filename1, bool isSTDOUT) {
    deleteWriter();
    mWriter1 = new Writer(mOptions, filename1, mOptions->compression, isSTDOUT);
}

void WriterThread::initBufferLists() {
    mBufferLists = new SingleProducerSingleConsumerList<string*>*[mOptions->thread];
    for(int t=0; t<mOptions->thread; t++) {
        mBufferLists[t] = new SingleProducerSingleConsumerList<string*>();
    }
}
