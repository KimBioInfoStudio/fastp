#include "writerthread.h"
#include "util.h"
#include "isal_compress.h"
#include <memory.h>
#include <unistd.h>

static const size_t ISAL_BATCH_SIZE = 512 << 10; // 512 KB accumulation threshold

WriterThread::WriterThread(Options* opt, string filename, bool isSTDOUT){
    mOptions = opt;

    mWriter1 = NULL;

    mInputCompleted = false;
    mFilename = filename;
    // Auto-detect: use parallel ISA-L gzip when writing to .gz files (not STDOUT)
    mPreCompressed = !isSTDOUT && ends_with(filename, ".gz");

    // Pre-compute ISA-L compression level (map fastp 1-9 → ISA-L 0-3)
    mIsalLevel = 1;
    if(mOptions->compression <= 2)
        mIsalLevel = 0;
    else if(mOptions->compression <= 5)
        mIsalLevel = 1;
    else if(mOptions->compression <= 7)
        mIsalLevel = 2;
    else
        mIsalLevel = 3;

    initWriter(filename, isSTDOUT);
    initBufferLists();
    mWorkingBufferList = 0; // 0 ~ mOptions->thread-1
    mBufferLength = 0;

    // Per-thread accumulation buffers for flight batching
    mAccumBuf = new string[mOptions->thread];
}

WriterThread::~WriterThread() {
    cleanup();
}

bool WriterThread::isCompleted() 
{
    return mInputCompleted && (mBufferLength==0);
}

bool WriterThread::setInputCompleted() {
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

void WriterThread::output(){
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
    if(mPreCompressed && !data->empty()) {
        // Flight batching: accumulate raw data, compress when >= 1MB
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

void WriterThread::cleanup() {
    deleteWriter();
    for(int t=0; t<mOptions->thread; t++) {
        delete mBufferLists[t];
    }
    delete[] mBufferLists;
    mBufferLists = NULL;
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
