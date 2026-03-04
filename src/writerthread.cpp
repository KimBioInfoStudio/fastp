#include "writerthread.h"
#include "util.h"
#include "isal_compress.h"
#include <memory.h>
#include <unistd.h>
#include <fcntl.h>

static const size_t ISAL_BATCH_SIZE = 512 << 10; // 512 KB accumulation threshold

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
    mPwriteMode = !isSTDOUT && mOptions->thread > 1;
    mFd = -1;
    mOffsetRing = NULL;
    mNextSeq = NULL;
    mBufferLists = NULL;

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
                string compressed = isal_gzip_compress(
                    mAccumBuf[t].data(), mAccumBuf[t].size(), mIsalLevel);
                mBufferLists[t]->produce(new string(std::move(compressed)));
                mBufferLength++;
                mAccumBuf[t].clear();
            }
        }
    }
    mInputCompleted = true;
    for(int t=0; t<mOptions->thread; t++) {
        mBufferLists[t]->setProducerFinished();
    }
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

    // Flush remaining .gz accum buffers sequentially (worker 0, 1, ..., W-1)
    if (mPreCompressed) {
        for (int t = 0; t < W; t++) {
            if (!mAccumBuf[t].empty()) {
                string compressed = isal_gzip_compress(
                    mAccumBuf[t].data(), mAccumBuf[t].size(), mIsalLevel);
                if (!compressed.empty()) {
                    size_t written = 0;
                    while (written < compressed.size()) {
                        ssize_t ret = pwrite(mFd, compressed.data() + written,
                                           compressed.size() - written, offset + written);
                        if (ret <= 0) break;
                        written += ret;
                    }
                    offset += compressed.size();
                }
                mAccumBuf[t].clear();
            }
        }
    }

    ftruncate(mFd, offset);
}

void WriterThread::output(){
    if (mPwriteMode) return;  // no-op
    SingleProducerSingleConsumerList<string*>* list =  mBufferLists[mWorkingBufferList];
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
    if(mPreCompressed && !data->empty()) {
        // Flight batching: accumulate raw data, compress when >= threshold
        mAccumBuf[tid].append(data->data(), data->length());
        delete data;
        if(mAccumBuf[tid].size() >= ISAL_BATCH_SIZE) {
            string compressed = isal_gzip_compress(
                mAccumBuf[tid].data(), mAccumBuf[tid].size(), mIsalLevel);
            mBufferLists[tid]->produce(new string(std::move(compressed)));
            mBufferLength++;
            mAccumBuf[tid].clear();
        }
        return;
    }
    mBufferLists[tid]->produce(data);
    mBufferLength++;
}

void WriterThread::inputPwrite(int tid, string* data) {
    size_t seq = mNextSeq[tid];
    string writeData;

    if (mPreCompressed) {
        // .gz: accumulate raw data, compress when threshold reached
        mAccumBuf[tid].append(data->data(), data->length());
        delete data;
        if (mAccumBuf[tid].size() >= ISAL_BATCH_SIZE) {
            writeData = isal_gzip_compress(
                mAccumBuf[tid].data(), mAccumBuf[tid].size(), mIsalLevel);
            mAccumBuf[tid].clear();
        }
        // else: zero-size passthrough (writeData stays empty)
    } else {
        // .fq: write pack data directly
        writeData = std::move(*data);
        delete data;
    }

    // Wait for previous pack's cumulative offset
    size_t offset = 0;
    if (seq > 0) {
        size_t prevSlot = (seq - 1) & (OFFSET_RING_SIZE - 1);
        while (mOffsetRing[prevSlot].published_seq.load(std::memory_order_acquire) != seq - 1) {
            // spin-wait
        }
        offset = mOffsetRing[prevSlot].cumulative_offset.load(std::memory_order_relaxed);
    }

    // pwrite data (skip if zero-size passthrough)
    size_t wsize = writeData.size();
    if (wsize > 0) {
        size_t written = 0;
        while (written < wsize) {
            ssize_t ret = pwrite(mFd, writeData.data() + written, wsize - written, offset + written);
            if (ret <= 0) break;
            written += ret;
        }
    }

    // Publish cumulative offset for next pack
    size_t mySlot = seq & (OFFSET_RING_SIZE - 1);
    mOffsetRing[mySlot].cumulative_offset.store(offset + wsize, std::memory_order_relaxed);
    mOffsetRing[mySlot].published_seq.store(seq, std::memory_order_release);

    mNextSeq[tid] += mOptions->thread;
}

void WriterThread::cleanup() {
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
