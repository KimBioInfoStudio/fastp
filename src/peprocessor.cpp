#include "peprocessor.h"
#include "fastqreader.h"
#include <iostream>
#include <unistd.h>
#include <functional>
#include <thread>
#include <memory.h>
#include <deque>
#include <algorithm>
#include <chrono>
#include "util.h"
#include "adaptertrimmer.h"
#include "basecorrector.h"
#include "jsonreporter.h"
#include "htmlreporter.h"
#include "polyx.h"
#include "trace_profiler.h"

PairEndProcessor::PairEndProcessor(Options* opt){
    mOptions = opt;
    mLeftReaderFinished = false;
    mRightReaderFinished = false;
    mFinishedThreads = 0;
    mFilter = new Filter(opt);
    mUmiProcessor = new UmiProcessor(opt);

    int isizeBufLen = mOptions->insertSizeMax + 1;
    mInsertSizeHist = new atomic_long[isizeBufLen];
    memset(mInsertSizeHist, 0, sizeof(atomic_long)*isizeBufLen);
    mLeftWriter =  NULL;
    mRightWriter = NULL;
    mUnpairedLeftWriter =  NULL;
    mUnpairedRightWriter = NULL;
    mMergedWriter = NULL;
    mFailedWriter = NULL;
    mOverlappedWriter = NULL;
    shouldStopReading = false;

    mDuplicate = NULL;
    if(mOptions->duplicate.enabled) {
        mDuplicate = new Duplicate(mOptions);
    }

    mLeftPackReadCounter = 0;
    mRightPackReadCounter = 0;
    mPackProcessedCounter = 0;
    mLeftInFlightPacks = 0;
    mRightInFlightPacks = 0;
    mLeftInFlightBytes = 0;
    mRightInFlightBytes = 0;
    mAvgPackBytes = 256 * 1024;
    int flightCap = std::max(4, mOptions->thread + 2);
    mLeftFlightBatch.configure(flightCap, 32);
    mRightFlightBatch.configure(flightCap, 32);
    mLeftFlightBatch.setSync(&mBackpressureMutex, &mBackpressureCv, &shouldStopReading);
    mRightFlightBatch.setSync(&mBackpressureMutex, &mBackpressureCv, &shouldStopReading);
    mAdaptivePackLimit = PACK_IN_MEM_LIMIT;
    mAdaptiveByteLimit = 64L * 1024L * 1024L;
    mRawChunksInFlightLimit = 8;
    mBackpressureInputUsWindow = 0;
    mWorkerWaitInputUsWindow = 0;
    mAutoTuneStop = true;
    mAutoTuneThread = NULL;
    initAdaptiveBackpressure();
}

PairEndProcessor::~PairEndProcessor() {
    stopRuntimeAutotune();
    delete mInsertSizeHist;
    if(mDuplicate) {
        delete mDuplicate;
        mDuplicate = NULL;
    }
    delete[] mLeftInputLists;
    delete[] mRightInputLists;
}

void PairEndProcessor::initAdaptiveBackpressure() {
    int packLimit = PACK_IN_MEM_LIMIT * 4 + mOptions->thread * 32;
    if (packLimit < PACK_IN_MEM_LIMIT)
        packLimit = PACK_IN_MEM_LIMIT;
    if (packLimit > 2048)
        packLimit = 2048;
    if (mOptions->readsToProcess > 0) {
        long maxByReads = mOptions->readsToProcess / PACK_SIZE;
        if (maxByReads < 16)
            maxByReads = 16;
        if (packLimit > maxByReads)
            packLimit = (int)maxByReads;
    }
    mAdaptivePackLimit.store(packLimit, std::memory_order_relaxed);

    long bytesLimit = (long)packLimit * 512L * 1024L;
    if (bytesLimit < 64L * 1024L * 1024L)
        bytesLimit = 64L * 1024L * 1024L;
    if (bytesLimit > 2L * 1024L * 1024L * 1024L)
        bytesLimit = 2L * 1024L * 1024L * 1024L;
    mAdaptiveByteLimit.store(bytesLimit, std::memory_order_relaxed);

    // Keep a bounded read-ahead queue for raw gzip chunks.
    int rawChunks = 8 + mOptions->thread / 2;
    if (rawChunks < 4)
        rawChunks = 4;
    if (rawChunks > 64)
        rawChunks = 64;
    mRawChunksInFlightLimit.store(rawChunks, std::memory_order_relaxed);
}

long PairEndProcessor::effectiveByteLimit() const {
    const long avg = mAvgPackBytes.load(std::memory_order_relaxed);
    const long packLimit = mAdaptivePackLimit.load(std::memory_order_relaxed);
    const long byteLimit = mAdaptiveByteLimit.load(std::memory_order_relaxed);
    long dyn = packLimit * avg * 3;
    if (dyn < byteLimit / 2)
        dyn = byteLimit / 2;
    if (dyn > 2L * 1024L * 1024L * 1024L)
        dyn = 2L * 1024L * 1024L * 1024L;
    return dyn;
}

bool PairEndProcessor::shouldThrottleInput(bool isLeft) const {
    return inputPressureLevel(isLeft) > 0;
}

int PairEndProcessor::inputPressureLevel(bool isLeft) const {
    const long packs = isLeft ? mLeftInFlightPacks.load(std::memory_order_relaxed)
                              : mRightInFlightPacks.load(std::memory_order_relaxed);
    const long bytes = isLeft ? mLeftInFlightBytes.load(std::memory_order_relaxed)
                              : mRightInFlightBytes.load(std::memory_order_relaxed);
    const long packLimit = mAdaptivePackLimit.load(std::memory_order_relaxed);
    const long byteLimit = effectiveByteLimit();
    if (packLimit <= 0 || byteLimit <= 0)
        return 0;

    const double packRatio = (double)packs / (double)packLimit;
    const double byteRatio = (double)bytes / (double)byteLimit;
    const double ratio = std::max(packRatio, byteRatio);

    if (ratio >= 0.98)
        return 2; // hard queue-full throttling
    if (ratio >= 0.85)
        return 1; // soft pacing
    return 0;
}

void PairEndProcessor::startRuntimeAutotune() {
    mAutoTuneStop.store(false, std::memory_order_relaxed);
    if (mAutoTuneThread == NULL)
        mAutoTuneThread = new std::thread(std::bind(&PairEndProcessor::runtimeAutotuneTask, this));
}

void PairEndProcessor::stopRuntimeAutotune() {
    mAutoTuneStop.store(true, std::memory_order_relaxed);
    mBackpressureCv.notify_all();
    if (mAutoTuneThread) {
        mAutoTuneThread->join();
        delete mAutoTuneThread;
        mAutoTuneThread = NULL;
    }
}

void PairEndProcessor::runtimeAutotuneTask() {
    using namespace std::chrono;

    const int minPackLimit = PACK_IN_MEM_LIMIT;
    const int maxPackLimit = 4096;
    const long minByteLimit = 64L * 1024L * 1024L;
    const long maxByteLimit = 2L * 1024L * 1024L * 1024L;
    const int minRawChunks = 4;
    const int maxRawChunks = 64;

    const int intervalMs = 250;
    const int cooldownTicks = 2;
    const double targetBackpressureLow = 0.10;
    const double targetBackpressureHigh = 0.30;
    const double targetWaitInputHigh = 0.04;
    int cooldown = 0;

    while (!mAutoTuneStop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(milliseconds(intervalMs));
        if (mAutoTuneStop.load(std::memory_order_relaxed))
            break;

        const long bpUs = mBackpressureInputUsWindow.exchange(0, std::memory_order_relaxed);
        const long wwUs = mWorkerWaitInputUsWindow.exchange(0, std::memory_order_relaxed);

        const long leftPacks = mLeftInFlightPacks.load(std::memory_order_relaxed);
        const long rightPacks = mRightInFlightPacks.load(std::memory_order_relaxed);
        const long leftBytes = mLeftInFlightBytes.load(std::memory_order_relaxed);
        const long rightBytes = mRightInFlightBytes.load(std::memory_order_relaxed);
        const long inFlightPacks = std::max(leftPacks, rightPacks);
        const long inFlightBytes = std::max(leftBytes, rightBytes);

        const int packLimitCur = mAdaptivePackLimit.load(std::memory_order_relaxed);
        const long byteLimitCur = mAdaptiveByteLimit.load(std::memory_order_relaxed);
        const int rawChunksCur = mRawChunksInFlightLimit.load(std::memory_order_relaxed);

        const long writerBacklog = std::max(
            mLeftWriter ? mLeftWriter->bufferLength() : 0L,
            mRightWriter ? mRightWriter->bufferLength() : 0L);

        const double baseReaders = mOptions->interleavedInput ? 1.0 : 2.0;
        const double bpRatio = bpUs / (intervalMs * 1000.0 * baseReaders);
        const double wwRatio = wwUs / (intervalMs * 1000.0 * std::max(1, mOptions->thread));

        bool changed = false;
        if (cooldown > 0)
            cooldown--;

        const bool queueNearCap =
            inFlightPacks > (packLimitCur * 90) / 100
            || inFlightBytes > (byteLimitCur * 90) / 100;
        const bool hardPressure =
            writerBacklog > packLimitCur * 3
            || inFlightPacks > (packLimitCur * 98) / 100
            || inFlightBytes > (byteLimitCur * 98) / 100;
        const bool moderatePressure =
            writerBacklog > packLimitCur * 2
            || queueNearCap;
        const bool needMoreFeed =
            wwRatio > targetWaitInputHigh
            || (bpRatio > targetBackpressureHigh
                && !moderatePressure);
        const bool needLessPressure =
            hardPressure
            && wwRatio < 0.01
            && bpRatio < targetBackpressureLow;

        if (cooldown == 0 && needLessPressure) {
            int nextPack = (packLimitCur * 90) / 100;
            long nextBytes = (byteLimitCur * 90) / 100;
            int rawStep = std::max(1, rawChunksCur / 6);
            int nextRawChunks = rawChunksCur - rawStep;

            if (nextPack < minPackLimit) nextPack = minPackLimit;
            if (nextBytes < minByteLimit) nextBytes = minByteLimit;
            if (nextRawChunks < minRawChunks) nextRawChunks = minRawChunks;

            if (nextPack != packLimitCur || nextBytes != byteLimitCur || nextRawChunks != rawChunksCur) {
                mAdaptivePackLimit.store(nextPack, std::memory_order_relaxed);
                mAdaptiveByteLimit.store(nextBytes, std::memory_order_relaxed);
                mRawChunksInFlightLimit.store(nextRawChunks, std::memory_order_relaxed);
                changed = true;
            }
        } else if (cooldown == 0 && needMoreFeed) {
            int nextPack = (packLimitCur * 120) / 100;
            long nextBytes = (byteLimitCur * 120) / 100;
            int rawStep = std::max(1, rawChunksCur / 5);
            int nextRawChunks = rawChunksCur + rawStep;

            if (queueNearCap && bpRatio > targetBackpressureHigh && wwRatio < targetWaitInputHigh) {
                nextPack = (packLimitCur * 125) / 100;
                nextBytes = (byteLimitCur * 125) / 100;
            }

            if (nextPack > maxPackLimit) nextPack = maxPackLimit;
            if (nextBytes > maxByteLimit) nextBytes = maxByteLimit;
            if (nextRawChunks > maxRawChunks) nextRawChunks = maxRawChunks;

            if (nextPack != packLimitCur || nextBytes != byteLimitCur || nextRawChunks != rawChunksCur) {
                mAdaptivePackLimit.store(nextPack, std::memory_order_relaxed);
                mAdaptiveByteLimit.store(nextBytes, std::memory_order_relaxed);
                mRawChunksInFlightLimit.store(nextRawChunks, std::memory_order_relaxed);
                changed = true;
            }
        }

        if (changed) {
            cooldown = cooldownTicks;
            mBackpressureCv.notify_all();
        }
    }
}

void PairEndProcessor::onPackProduced(bool isLeft, int rawBytes) {
    atomic_long& packs = isLeft ? mLeftInFlightPacks : mRightInFlightPacks;
    long packsBefore = packs.load(std::memory_order_relaxed);
    if (mOptions->interleavedInput) {
        if (isLeft)
            mLeftFlightBatch.acquireForNextPack(packsBefore);
        else
            mRightFlightBatch.acquireForNextPack(packsBefore);
    }

    if (rawBytes < 0)
        rawBytes = 0;
    if (isLeft) {
        packs.fetch_add(1, std::memory_order_relaxed);
        mLeftInFlightBytes.fetch_add(rawBytes, std::memory_order_relaxed);
    } else {
        packs.fetch_add(1, std::memory_order_relaxed);
        mRightInFlightBytes.fetch_add(rawBytes, std::memory_order_relaxed);
    }

    const long prev = mAvgPackBytes.load(std::memory_order_relaxed);
    const long next = (prev * 7 + rawBytes) / 8;
    if (next > 0)
        mAvgPackBytes.store(next, std::memory_order_relaxed);

    mBackpressureCv.notify_all();
}

void PairEndProcessor::onPackConsumed(int leftRawBytes, int rightRawBytes) {
    if (leftRawBytes < 0)
        leftRawBytes = 0;
    if (rightRawBytes < 0)
        rightRawBytes = 0;

    long lp = mLeftInFlightPacks.fetch_sub(1, std::memory_order_relaxed) - 1;
    long rp = mRightInFlightPacks.fetch_sub(1, std::memory_order_relaxed) - 1;
    if (lp < 0)
        mLeftInFlightPacks.store(0, std::memory_order_relaxed);
    if (rp < 0)
        mRightInFlightPacks.store(0, std::memory_order_relaxed);

    long lb = mLeftInFlightBytes.fetch_sub(leftRawBytes, std::memory_order_relaxed) - leftRawBytes;
    long rb = mRightInFlightBytes.fetch_sub(rightRawBytes, std::memory_order_relaxed) - rightRawBytes;
    if (lb < 0)
        mLeftInFlightBytes.store(0, std::memory_order_relaxed);
    if (rb < 0)
        mRightInFlightBytes.store(0, std::memory_order_relaxed);

    if (mOptions->interleavedInput) {
        mLeftFlightBatch.releaseAfterConsume(lp);
        mRightFlightBatch.releaseAfterConsume(rp);
    }
    mBackpressureCv.notify_all();
}

void PairEndProcessor::initOutput() {
    if(!mOptions->unpaired1.empty())
        mUnpairedLeftWriter = new WriterThread(mOptions, mOptions->unpaired1);

    if(!mOptions->unpaired2.empty() && mOptions->unpaired2 != mOptions->unpaired1)
        mUnpairedRightWriter = new WriterThread(mOptions, mOptions->unpaired2);

    if(mOptions->merge.enabled) {
        if(!mOptions->merge.out.empty())
            mMergedWriter = new WriterThread(mOptions, mOptions->merge.out);
    }

    if(!mOptions->failedOut.empty())
        mFailedWriter = new WriterThread(mOptions, mOptions->failedOut);

    if(!mOptions->overlappedOut.empty())
        mOverlappedWriter = new WriterThread(mOptions, mOptions->overlappedOut);

    if(mOptions->out1.empty() && !mOptions->outputToSTDOUT)
        return;

    mLeftWriter = new WriterThread(mOptions, mOptions->out1, mOptions->outputToSTDOUT);
    if(!mOptions->out2.empty())
        mRightWriter = new WriterThread(mOptions, mOptions->out2);
}

void PairEndProcessor::closeOutput() {
    if(mLeftWriter) {
        delete mLeftWriter;
        mLeftWriter = NULL;
    }
    if(mRightWriter) {
        delete mRightWriter;
        mRightWriter = NULL;
    }
    if(mMergedWriter) {
        delete mMergedWriter;
        mMergedWriter = NULL;
    }
    if(mFailedWriter) {
        delete mFailedWriter;
        mFailedWriter = NULL;
    }
    if(mOverlappedWriter) {
        delete mOverlappedWriter;
        mOverlappedWriter = NULL;
    }
    if(mUnpairedLeftWriter) {
        delete mUnpairedLeftWriter;
        mUnpairedLeftWriter = NULL;
    }
    if(mUnpairedRightWriter) {
        delete mUnpairedRightWriter;
        mUnpairedRightWriter = NULL;
    }
}

void PairEndProcessor::initConfig(ThreadConfig* config) {
    if(mOptions->out1.empty())
        return;
    if(mOptions->split.enabled) {
        config->initWriterForSplit();
    }
}


bool PairEndProcessor::process(){
    if(!mOptions->split.enabled)
        initOutput();
    startRuntimeAutotune();

    std::thread* readerLeft = NULL;
    std::thread* readerRight = NULL;
    std::thread* readerInterveleaved = NULL;

    mLeftInputLists = new SingleProducerSingleConsumerList<RawPack*>*[mOptions->thread];
    mRightInputLists = new SingleProducerSingleConsumerList<RawPack*>*[mOptions->thread];

    ThreadConfig** configs = new ThreadConfig*[mOptions->thread];
    for(int t=0; t<mOptions->thread; t++){
        mLeftInputLists[t] = new SingleProducerSingleConsumerList<RawPack*>();
        mRightInputLists[t] = new SingleProducerSingleConsumerList<RawPack*>();
        configs[t] = new ThreadConfig(mOptions, t, true);
        configs[t]->setInputListPair(mLeftInputLists[t], mRightInputLists[t]);
        initConfig(configs[t]);
    }

    if(mOptions->interleavedInput)
        readerInterveleaved= new std::thread(std::bind(&PairEndProcessor::interleavedReaderTask, this));
    else {
        readerLeft = new std::thread(std::bind(&PairEndProcessor::readerTask, this, true));
        readerRight = new std::thread(std::bind(&PairEndProcessor::readerTask, this, false));
    }

    std::thread** threads = new thread*[mOptions->thread];
    for(int t=0; t<mOptions->thread; t++){
        threads[t] = new std::thread(std::bind(&PairEndProcessor::processorTask, this, configs[t]));
    }

    std::thread* leftWriterThread = NULL;
    std::thread* rightWriterThread = NULL;
    std::thread* unpairedLeftWriterThread = NULL;
    std::thread* unpairedRightWriterThread = NULL;
    std::thread* mergedWriterThread = NULL;
    std::thread* failedWriterThread = NULL;
    std::thread* overlappedWriterThread = NULL;
    if(mLeftWriter)
        leftWriterThread = new std::thread(std::bind(&PairEndProcessor::writerTask, this, mLeftWriter));
    if(mRightWriter)
        rightWriterThread = new std::thread(std::bind(&PairEndProcessor::writerTask, this, mRightWriter));
    if(mUnpairedLeftWriter)
        unpairedLeftWriterThread = new std::thread(std::bind(&PairEndProcessor::writerTask, this, mUnpairedLeftWriter));
    if(mUnpairedRightWriter)
        unpairedRightWriterThread = new std::thread(std::bind(&PairEndProcessor::writerTask, this, mUnpairedRightWriter));
    if(mMergedWriter)
        mergedWriterThread = new std::thread(std::bind(&PairEndProcessor::writerTask, this, mMergedWriter));
    if(mFailedWriter)
        failedWriterThread = new std::thread(std::bind(&PairEndProcessor::writerTask, this, mFailedWriter));
    if(mOverlappedWriter)
        overlappedWriterThread = new std::thread(std::bind(&PairEndProcessor::writerTask, this, mOverlappedWriter));

    if(readerInterveleaved) {
        readerInterveleaved->join();
    } else {
        readerLeft->join();
        readerRight->join();
    }
    for(int t=0; t<mOptions->thread; t++){
        threads[t]->join();
    }

    if(!mOptions->split.enabled) {
        if(leftWriterThread)
            leftWriterThread->join();
        if(rightWriterThread)
            rightWriterThread->join();
        if(unpairedLeftWriterThread)
            unpairedLeftWriterThread->join();
        if(unpairedRightWriterThread)
            unpairedRightWriterThread->join();
        if(mergedWriterThread)
            mergedWriterThread->join();
        if(failedWriterThread)
            failedWriterThread->join();
        if(overlappedWriterThread)
            overlappedWriterThread->join();
    }
    stopRuntimeAutotune();

    if(mOptions->verbose)
        loginfo("start to generate reports\n");

    // merge stats and filter results
    vector<Stats*> preStats1;
    vector<Stats*> postStats1;
    vector<Stats*> preStats2;
    vector<Stats*> postStats2;
    vector<FilterResult*> filterResults;
    for(int t=0; t<mOptions->thread; t++){
        preStats1.push_back(configs[t]->getPreStats1());
        postStats1.push_back(configs[t]->getPostStats1());
        preStats2.push_back(configs[t]->getPreStats2());
        postStats2.push_back(configs[t]->getPostStats2());
        filterResults.push_back(configs[t]->getFilterResult());
    }
    Stats* finalPreStats1 = Stats::merge(preStats1);
    Stats* finalPostStats1 = Stats::merge(postStats1);
    Stats* finalPreStats2 = Stats::merge(preStats2);
    Stats* finalPostStats2 = Stats::merge(postStats2);
    FilterResult* finalFilterResult = FilterResult::merge(filterResults);

    cerr << "Read1 before filtering:"<<endl;
    finalPreStats1->print();
    cerr << endl;
    cerr << "Read2 before filtering:"<<endl;
    finalPreStats2->print();
    cerr << endl;
    if(!mOptions->merge.enabled) {
        cerr << "Read1 after filtering:"<<endl;
        finalPostStats1->print();
        cerr << endl;
        cerr << "Read2 after filtering:"<<endl;
        finalPostStats2->print();
    } else {
        cerr << "Merged and filtered:"<<endl;
        finalPostStats1->print();
    }

    cerr << endl;
    cerr << "Filtering result:"<<endl;
    finalFilterResult->print();

    double dupRate = 0.0;
    if(mOptions->duplicate.enabled) {
        dupRate = mDuplicate->getDupRate();
        cerr << endl;
        cerr << "Duplication rate: " << dupRate * 100.0 << "%" << endl;
    }

    // insert size distribution
    int peakInsertSize = getPeakInsertSize();
    cerr << endl;
    cerr << "Insert size peak (evaluated by paired-end reads): " << peakInsertSize << endl;

    if(mOptions->merge.enabled) {
        cerr << endl;
        cerr << "Read pairs merged: " << finalFilterResult->mMergedPairs << endl;
        if(finalPostStats1->getReads() > 0) {
            double postMergedPercent = 100.0 * finalFilterResult->mMergedPairs / finalPostStats1->getReads();
            double preMergedPercent = 100.0 * finalFilterResult->mMergedPairs / finalPreStats1->getReads();
            cerr << "% of original read pairs: " << preMergedPercent << "%" << endl;
            cerr << "% in reads after filtering: " << postMergedPercent << "%" << endl;
        }
        cerr << endl;
    }

    // make JSON report
    JsonReporter jr(mOptions);
    jr.setDup(dupRate);
    jr.setInsertHist(mInsertSizeHist, peakInsertSize);
    jr.report(finalFilterResult, finalPreStats1, finalPostStats1, finalPreStats2, finalPostStats2);

    // make HTML report
    HtmlReporter hr(mOptions);
    hr.setDup(dupRate);
    hr.setInsertHist(mInsertSizeHist, peakInsertSize);
    hr.report(finalFilterResult, finalPreStats1, finalPostStats1, finalPreStats2, finalPostStats2);

    // clean up
    for(int t=0; t<mOptions->thread; t++){
        delete threads[t];
        threads[t] = NULL;
        delete configs[t];
        configs[t] = NULL;
    }

    if(readerInterveleaved) {
        delete readerInterveleaved;
    } else {
        delete readerLeft;
        delete readerRight;
    }

    delete finalPreStats1;
    delete finalPostStats1;
    delete finalPreStats2;
    delete finalPostStats2;
    delete finalFilterResult;

    delete[] threads;
    delete[] configs;

    if(leftWriterThread)
        delete leftWriterThread;
    if(rightWriterThread)
        delete rightWriterThread;
    if(unpairedLeftWriterThread)
        delete unpairedLeftWriterThread;
    if(unpairedRightWriterThread)
        delete unpairedRightWriterThread;
    if(mergedWriterThread)
        delete mergedWriterThread;
    if(failedWriterThread)
        delete failedWriterThread;
    if(overlappedWriterThread)
        delete overlappedWriterThread;

    if(!mOptions->split.enabled)
        closeOutput();

    return true;
}

int PairEndProcessor::getPeakInsertSize() {
    int peak = 0;
    long maxCount = -1;
    for(int i=0; i<mOptions->insertSizeMax; i++) {
        if(mInsertSizeHist[i] > maxCount) {
            peak = i;
            maxCount = mInsertSizeHist[i];
        }
    }
    return peak;
}

ReadPack* PairEndProcessor::parseRawPack(RawPack* rawPack) {
    if (rawPack->directReadPack) {
        ReadPack* pack = rawPack->directPack;
        if (pack)
            pack->rawBytes = 0;
        delete rawPack;
        return pack;
    }

    int count = rawPack->readCount;
    int rawBytes = rawPack->length;
    Read** data = new Read*[count];
    char* buf = rawPack->buffer->data + rawPack->offset;
    int len = rawPack->length;
    int pos = 0;
    bool phred64 = mOptions->phred64;

    for (int i = 0; i < count; i++) {
        string* name = new string();
        string* seq = new string();
        string* strand = new string();
        string* qual = new string();

        // parse 4 lines per FASTQ record
        string* fields[4] = {name, seq, strand, qual};
        for (int f = 0; f < 4; f++) {
            const char* nl = (const char*)memchr(buf + pos, '\n', len - pos);
            int lineEnd;
            if (nl)
                lineEnd = nl - buf;
            else
                lineEnd = len;
            int lineLen = lineEnd - pos;
            // strip \r
            if (lineLen > 0 && buf[pos + lineLen - 1] == '\r')
                lineLen--;
            fields[f]->assign(buf + pos, lineLen);
            pos = (nl ? lineEnd + 1 : lineEnd);
        }

        data[i] = new Read(name, seq, strand, qual, phred64);

        if (name->empty() || (*name)[0] != '@') {
            error_exit("invalid FASTQ record: name line does not start with '@'");
        }
        if (strand->empty() || (*strand)[0] != '+') {
            error_exit("invalid FASTQ record: strand line does not start with '+'");
        }
        if (seq->length() != qual->length()) {
            error_exit("invalid FASTQ record: sequence and quality lengths differ");
        }
    }

    rawPack->buffer->release();
    delete rawPack;

    ReadPack* pack = new ReadPack;
    pack->data = data;
    pack->count = count;
    pack->rawBytes = rawBytes;
    return pack;
}

bool PairEndProcessor::processPairEnd(ReadPack* leftPack, ReadPack* rightPack, ThreadConfig* config){
    if(leftPack->count != rightPack->count) {
        cerr << endl;
        cerr << "WARNING: different read numbers of the " << mPackProcessedCounter << " pack" << endl;
        cerr << "Read1 pack size: " << leftPack->count << endl;
        cerr << "Read2 pack size: " << rightPack->count << endl;
        cerr << "Ignore the unmatched reads" << endl << endl;
        shouldStopReading = true;
        mBackpressureCv.notify_all();
    }
    int tid = config->getThreadId();

    // build output on stack strings, move to heap only when handing off to writers
    string outstr1, outstr2, unpairedOut1, unpairedOut2;
    string singleOutput, mergedOutput, failedOut, overlappedOut;
    // reserve capacity for main outputs to avoid repeated reallocation
    const size_t estimatedCapacity = leftPack->count * 320;
    outstr1.reserve(estimatedCapacity);
    outstr2.reserve(estimatedCapacity);

    int readPassed = 0;
    int mergedCount = 0;
    for(int p=0;p<leftPack->count && p<rightPack->count;p++){
        Read* or1 = leftPack->data[p];
        Read* or2 = rightPack->data[p];

        int lowQualNum1 = 0;
        int nBaseNum1 = 0;
        int lowQualNum2 = 0;
        int nBaseNum2 = 0;

        // stats the original read before trimming
        config->getPreStats1()->statRead(or1);
        config->getPreStats2()->statRead(or2);

        // handling the duplication profiling
        bool dedupOut = false;
        if(mDuplicate) {
            bool isDup = mDuplicate->checkPair(or1, or2);
            if(mOptions->duplicate.dedup && isDup)
                dedupOut = true;
        }

        // filter by index
        if(mOptions->indexFilter.enabled && mFilter->filterByIndex(or1, or2)) {
            delete or1;
            or1 = NULL;
            delete or2;
            or2 = NULL;
            continue;
        }

        // fix MGI
        if(mOptions->fixMGI) {
            or1->fixMGI();
            or2->fixMGI();
        }
        // umi processing
        if(mOptions->umi.enabled)
            mUmiProcessor->process(or1, or2);

        // trim in head and tail, and apply quality cut in sliding window
        int frontTrimmed1 = 0;
        int frontTrimmed2 = 0;
        Read* r1 = mFilter->trimAndCut(or1, mOptions->trim.front1, mOptions->trim.tail1, frontTrimmed1);
        Read* r2 = mFilter->trimAndCut(or2, mOptions->trim.front2, mOptions->trim.tail2, frontTrimmed2);

        if(r1 != NULL && r2!=NULL) {
            if(mOptions->polyGTrim.enabled)
                PolyX::trimPolyG(r1, r2, config->getFilterResult(), mOptions->polyGTrim.minLen);
        }
        bool isizeEvaluated = false;
        bool isAdapterDimer = false;
        if(r1 != NULL && r2!=NULL && (mOptions->adapter.enabled || mOptions->correction.enabled)){
            OverlapResult ov = OverlapAnalysis::analyze(r1, r2, mOptions->overlapDiffLimit, mOptions->overlapRequire, mOptions->overlapDiffPercentLimit/100.0, mOptions->adapter.allowGapOverlapTrimming);
            // we only use thread 0 to evaluae ISIZE
            if(config->getThreadId() == 0) {
                statInsertSize(r1, r2, ov, frontTrimmed1, frontTrimmed2);
                isizeEvaluated = true;
            }
            if(mOptions->correction.enabled && !ov.hasGap) {
                // no gap allowed for overlap correction
                BaseCorrector::correctByOverlapAnalysis(r1, r2, config->getFilterResult(), ov);
            }
            if(mOptions->adapter.enabled) {
                bool trimmed = AdapterTrimmer::trimByOverlapAnalysis(r1, r2, config->getFilterResult(), ov, frontTrimmed1, frontTrimmed2);
                bool trimmed1 = trimmed;
                bool trimmed2 = trimmed;
                if(!trimmed){
                    if(mOptions->adapter.hasSeqR1)
                        trimmed1 = AdapterTrimmer::trimBySequence(r1, config->getFilterResult(), mOptions->adapter.sequence, false);
                    if(mOptions->adapter.hasSeqR2)
                        trimmed2 = AdapterTrimmer::trimBySequence(r2, config->getFilterResult(), mOptions->adapter.sequenceR2, true);
                }
                if(mOptions->adapter.hasFasta) {
                    trimmed1 |= AdapterTrimmer::trimByMultiSequences(r1, config->getFilterResult(), mOptions->adapter.seqsInFasta, false, !trimmed1);
                    trimmed2 |= AdapterTrimmer::trimByMultiSequences(r2, config->getFilterResult(), mOptions->adapter.seqsInFasta, true, !trimmed2);
                }

                // Check for adapter dimer: both reads shorter than threshold after adapter trimming
                // AND adapters were detected in at least one of the reads (requires evidence)
                if(r1 != NULL && r2 != NULL && (trimmed1 || trimmed2) &&
                   r1->length() <= mOptions->adapter.dimerMaxLen &&
                   r2->length() <= mOptions->adapter.dimerMaxLen) {
                    isAdapterDimer = true;
                }
            }
        }

        if(r1 != NULL && r2!=NULL && mOverlappedWriter) {
            OverlapResult ov = OverlapAnalysis::analyze(r1, r2, mOptions->overlapDiffLimit, mOptions->overlapRequire, 0);
            if(ov.overlapped) {
                Read* overlappedRead = new Read(new string(*r1->mName), new string(r1->mSeq->substr(max(0,ov.offset)), ov.overlap_len), new string(*r1->mStrand), new string(r1->mQuality->substr(max(0,ov.offset)), ov.overlap_len));
                overlappedRead->appendToString(&overlappedOut);
                delete overlappedRead;
            }
        }

        if(config->getThreadId() == 0 && !isizeEvaluated && r1 != NULL && r2!=NULL) {
            OverlapResult ov = OverlapAnalysis::analyze(r1, r2, mOptions->overlapDiffLimit, mOptions->overlapRequire, mOptions->overlapDiffPercentLimit/100.0);
            statInsertSize(r1, r2, ov, frontTrimmed1, frontTrimmed2);
            isizeEvaluated = true;
        }

        if(r1 != NULL && r2!=NULL) {
            if(mOptions->polyXTrim.enabled)
                PolyX::trimPolyX(r1, r2, config->getFilterResult(), mOptions->polyXTrim.minLen);
        }

        if(r1 != NULL && r2!=NULL) {
            if( mOptions->trim.maxLen1 > 0 && mOptions->trim.maxLen1 < r1->length())
                r1->resize(mOptions->trim.maxLen1);
            if( mOptions->trim.maxLen2 > 0 && mOptions->trim.maxLen2 < r2->length())
                r2->resize(mOptions->trim.maxLen2);
        }

        Read* merged = NULL;
        // merging mode
        bool mergeProcessed = false;
        if(mOptions->merge.enabled && r1 && r2) {
            OverlapResult ov = OverlapAnalysis::analyze(r1, r2, mOptions->overlapDiffLimit, mOptions->overlapRequire, mOptions->overlapDiffPercentLimit/100.0);
            if(ov.overlapped) {
                merged = OverlapAnalysis::merge(r1, r2, ov);
                int result = mFilter->passFilter(merged);
                config->addFilterResult(result, 2);
                if(result == PASS_FILTER) {
                    merged->appendToString(&mergedOutput);
                    config->getPostStats1()->statRead(merged);
                    readPassed++;
                    mergedCount++;
                }
                delete merged;
                mergeProcessed = true;
            } else if(mOptions->merge.includeUnmerged){
                int result1 = mFilter->passFilter(r1);
                int result2 = mFilter->passFilter(r2);

                if(isAdapterDimer) {
                    result1 = FAIL_ADAPTER_DIMER;
                    result2 = FAIL_ADAPTER_DIMER;
                }

                config->addFilterResult(result1, 1);
                if(result1 == PASS_FILTER && !dedupOut) {
                    r1->appendToString(&mergedOutput);
                    config->getPostStats1()->statRead(r1);
                }

                config->addFilterResult(result2, 1);
                if(result2 == PASS_FILTER && !dedupOut) {
                    r2->appendToString(&mergedOutput);
                    config->getPostStats1()->statRead(r2);
                }
                if(result1 == PASS_FILTER && result2 == PASS_FILTER )
                    readPassed++;
                mergeProcessed = true;
            }
        }

        if(!mergeProcessed) {

            int result1 = mFilter->passFilter(r1);
            int result2 = mFilter->passFilter(r2);

            if(isAdapterDimer) {
                result1 = FAIL_ADAPTER_DIMER;
                result2 = FAIL_ADAPTER_DIMER;
            }

            config->addFilterResult(max(result1, result2), 2);

            if(!dedupOut) {

                if( r1 != NULL &&  result1 == PASS_FILTER && r2 != NULL && result2 == PASS_FILTER ) {

                    if(mOptions->outputToSTDOUT && !mOptions->merge.enabled) {
                        r1->appendToString(&singleOutput);
                        r2->appendToString(&singleOutput);
                    } else {
                        r1->appendToString(&outstr1);
                        r2->appendToString(&outstr2);
                    }

                    // stats the read after filtering
                    if(!mOptions->merge.enabled) {
                        config->getPostStats1()->statRead(r1);
                        config->getPostStats2()->statRead(r2);
                    }

                    readPassed++;
                } else if( r1 != NULL &&  result1 == PASS_FILTER) {
                    if(mUnpairedLeftWriter) {
                        r1->appendToString(&unpairedOut1);
                        if(mFailedWriter)
                            or2->appendToStringWithTag(&failedOut, FAILED_TYPES[result2]);
                    } else {
                        if(mFailedWriter) {
                            or1->appendToStringWithTag(&failedOut, "paired_read_is_failing");
                            or2->appendToStringWithTag(&failedOut, FAILED_TYPES[result2]);
                        }
                    }
                } else if( r2 != NULL && result2 == PASS_FILTER) {
                    if(mUnpairedRightWriter) {
                        r2->appendToString(&unpairedOut2);
                        if(mFailedWriter)
                            or1->appendToStringWithTag(&failedOut,FAILED_TYPES[result1]);
                    } else if(mUnpairedLeftWriter) {
                        r2->appendToString(&unpairedOut1);
                        if(mFailedWriter)
                            or1->appendToStringWithTag(&failedOut,FAILED_TYPES[result1]);
                    }  else {
                        if(mFailedWriter) {
                            or1->appendToStringWithTag(&failedOut, FAILED_TYPES[result1]);
                            or2->appendToStringWithTag(&failedOut, "paired_read_is_failing");
                        }
                    }
                }
            }
        }

        // if no trimming applied, r1 should be identical to or1
        if(r1 != or1 && r1 != NULL) {
            delete r1;
            r1 = NULL;
        }
        // if no trimming applied, r2 should be identical to or2
        if(r2 != or2 && r2 != NULL) {
            delete r2;
            r2 = NULL;
        }

        if(or1) {
            delete or1;
            or1 = NULL;
        }
        if(or2) {
            delete or2;
            or2 = NULL;
        }
    }

	if(mOptions->split.enabled) {
        // split output by each worker thread
        if(!mOptions->out1.empty())
            config->getWriter1()->writeString(outstr1);
        if(!mOptions->out2.empty())
            config->getWriter2()->writeString(outstr2);
    }

    if(mMergedWriter) {
        // move to heap for writer thread ownership
        mMergedWriter->input(tid, new string(std::move(mergedOutput)));
    }

    if(mFailedWriter) {
        mFailedWriter->input(tid, new string(std::move(failedOut)));
    }

    if(mOverlappedWriter) {
        mOverlappedWriter->input(tid, new string(std::move(overlappedOut)));
    }

    // normal output by left/right writer thread
    if(mRightWriter && mLeftWriter) {
        // write PE - move to heap for writer thread ownership
        mLeftWriter->input(tid, new string(std::move(outstr1)));
        mRightWriter->input(tid, new string(std::move(outstr2)));
    } else if(mLeftWriter) {
        // write singleOutput
        mLeftWriter->input(tid, new string(std::move(singleOutput)));
    }
    // output unpaired reads
    if(mUnpairedLeftWriter && mUnpairedRightWriter) {
        mUnpairedLeftWriter->input(tid, new string(std::move(unpairedOut1)));
        mUnpairedRightWriter->input(tid, new string(std::move(unpairedOut2)));
    } else if(mUnpairedLeftWriter) {
        mUnpairedLeftWriter->input(tid, new string(std::move(unpairedOut1)));
    }

    if(mOptions->split.byFileLines)
        config->markProcessed(readPassed);
    else
        config->markProcessed(leftPack->count);

    if(mOptions->merge.enabled) {
        config->addMergedPairs(mergedCount);
    }

    const int leftRawBytes = leftPack->rawBytes;
    const int rightRawBytes = rightPack->rawBytes;
    delete[] leftPack->data;
    delete[] rightPack->data;
    delete leftPack;
    delete rightPack;

    mPackProcessedCounter++;
    onPackConsumed(leftRawBytes, rightRawBytes);

    return true;
}

void PairEndProcessor::statInsertSize(Read* r1, Read* r2, OverlapResult& ov, int frontTrimmed1, int frontTrimmed2) {
    int isize = mOptions->insertSizeMax;
    if(ov.overlapped) {
        if(ov.offset > 0)
            isize = r1->length() + r2->length() - ov.overlap_len + frontTrimmed1 + frontTrimmed2;
        else
            isize = ov.overlap_len + frontTrimmed1 + frontTrimmed2;
    }

    if(isize > mOptions->insertSizeMax)
        isize = mOptions->insertSizeMax;

    mInsertSizeHist[isize]++;
}

void PairEndProcessor::readerTask(bool isLeft)
{
    trace::TaskBreakdown task(isLeft ? "pe.reader.r1" : "pe.reader.r2", "pe.reader");
    if(mOptions->verbose) {
        if(isLeft)
            loginfo("start to load data of read1");
        else
            loginfo("start to load data of read2");
    }
    long lastReported = 0;
    int slept = 0;
    long readNum = 0;
    std::deque<std::pair<char*, int> > rawQueue;
    std::mutex rawMu;
    std::condition_variable rawCvNotEmpty;
    std::condition_variable rawCvNotFull;
    bool rawDone = false;
    std::atomic_bool stopRawProducer(false);

    std::thread rawProducer([&]() {
        FastqReader reader(isLeft ? mOptions->in1 : mOptions->in2, true, mOptions->phred64, mOptions->thread);
        while (!shouldStopReading && !stopRawProducer) {
            int rawLen = 0;
            char* rawData = reader.readRawBuffer(rawLen);
            if (!rawData)
                break;
            std::unique_lock<std::mutex> lk(rawMu);
            rawCvNotFull.wait(lk, [&]() {
                const int rawLimit = mRawChunksInFlightLimit.load(std::memory_order_relaxed);
                return rawQueue.size() < (size_t)rawLimit || shouldStopReading || stopRawProducer;
            });
            if (shouldStopReading || stopRawProducer) {
                lk.unlock();
                delete[] rawData;
                break;
            }
            rawQueue.push_back(std::make_pair(rawData, rawLen));
            lk.unlock();
            rawCvNotEmpty.notify_one();
        }
        std::unique_lock<std::mutex> lk(rawMu);
        rawDone = true;
        lk.unlock();
        rawCvNotEmpty.notify_all();
    });

    char* leftover = NULL;
    int leftoverLen = 0;
    bool needToBreak = false;

    while(true){
        if(shouldStopReading)
            break;

        int rawLen = 0;
        char* rawData = NULL;
        {
            const uint64_t t0 = trace::nowUs();
            std::unique_lock<std::mutex> lk(rawMu);
            rawCvNotEmpty.wait(lk, [&]() { return !rawQueue.empty() || rawDone || shouldStopReading; });
            const uint64_t t1 = trace::nowUs();
            task.addGap("wait_raw_chunk", t1 > t0 ? t1 - t0 : 0);

            if ((rawQueue.empty() && rawDone) || shouldStopReading)
                break;

            std::pair<char*, int> item = rawQueue.front();
            rawQueue.pop_front();
            rawData = item.first;
            rawLen = item.second;
            lk.unlock();
            rawCvNotFull.notify_one();
        }

        // build working buffer: leftover + new raw data
        char* workBuf;
        int workLen;
        if (leftoverLen > 0) {
            workLen = leftoverLen + rawLen;
            workBuf = new char[workLen];
            memcpy(workBuf, leftover, leftoverLen);
            memcpy(workBuf + leftoverLen, rawData, rawLen);
            delete[] leftover;
            leftover = NULL;
            leftoverLen = 0;
            delete[] rawData;
            rawData = NULL;
        } else {
            workBuf = rawData;
            workLen = rawLen;
        }

        RawBuffer* buffer = new RawBuffer(workBuf, workLen);

        int pos = 0;
        int packStart = 0;
        int recordsInPack = 0;
        int newlineCount = 0;
        int lastRecordEnd = 0;

        while (pos < workLen) {
            const char* nl = (const char*)memchr(workBuf + pos, '\n', workLen - pos);
            if (!nl)
                break;
            pos = (nl - workBuf) + 1;
            newlineCount++;

            if (newlineCount == 4) {
                newlineCount = 0;
                recordsInPack++;
                lastRecordEnd = pos;

                if (mOptions->readsToProcess > 0 && readNum + recordsInPack >= mOptions->readsToProcess) {
                    needToBreak = true;
                    if (recordsInPack > 0) {
                        RawPack* pack = new RawPack;
                        pack->buffer = buffer;
                        pack->offset = packStart;
                        pack->length = lastRecordEnd - packStart;
                        pack->readCount = recordsInPack;
                        buffer->addRef();
                        if (isLeft) {
                            mLeftInputLists[mLeftPackReadCounter % mOptions->thread]->produce(pack);
                            mLeftPackReadCounter++;
                            onPackProduced(true, pack->length);
                        } else {
                            mRightInputLists[mRightPackReadCounter % mOptions->thread]->produce(pack);
                            mRightPackReadCounter++;
                            onPackProduced(false, pack->length);
                        }
                    }
                    recordsInPack = 0;
                    packStart = lastRecordEnd;
                    break;
                }

                if (recordsInPack == PACK_SIZE) {
                    RawPack* pack = new RawPack;
                    pack->buffer = buffer;
                    pack->offset = packStart;
                    pack->length = lastRecordEnd - packStart;
                    pack->readCount = recordsInPack;
                    buffer->addRef();
                    if (isLeft) {
                        mLeftInputLists[mLeftPackReadCounter % mOptions->thread]->produce(pack);
                        mLeftPackReadCounter++;
                        onPackProduced(true, pack->length);
                    } else {
                        mRightInputLists[mRightPackReadCounter % mOptions->thread]->produce(pack);
                        mRightPackReadCounter++;
                        onPackProduced(false, pack->length);
                    }

                    readNum += recordsInPack;
                    recordsInPack = 0;
                    packStart = lastRecordEnd;

                    while (true) {
                        const int pressure = inputPressureLevel(isLeft);
                        if (pressure <= 0 || shouldStopReading)
                            break;
                        if (pressure < 2)
                            break;

                        const uint64_t t0 = trace::nowUs();
                        slept++;
                        std::unique_lock<std::mutex> lk(mBackpressureMutex);
                        mBackpressureCv.wait(lk, [&]() {
                            return inputPressureLevel(isLeft) < 2 || shouldStopReading;
                        });
                        const uint64_t t1 = trace::nowUs();
                        if (t1 > t0)
                            mBackpressureInputUsWindow.fetch_add((long)(t1 - t0), std::memory_order_relaxed);
                        task.addGap("queue_full_wait", t1 > t0 ? t1 - t0 : 0);
                    }
                    const int packLimit = mAdaptivePackLimit.load(std::memory_order_relaxed);
                    if (packLimit > 0 && readNum % (PACK_SIZE * packLimit) == 0 && mLeftWriter) {
                        while ((mLeftWriter && mLeftWriter->bufferLength() > packLimit) || (mRightWriter && mRightWriter->bufferLength() > packLimit)) {
                            const uint64_t t0 = trace::nowUs();
                            slept++;
                            usleep(1000);
                            const uint64_t t1 = trace::nowUs();
                            task.addGap("backpressure_writer", t1 > t0 ? t1 - t0 : 0);
                        }
                    }
                }
            }
        }

        // Don't emit partial packs mid-stream: carry un-emitted complete
        // records + incomplete trailing record as leftover to the next buffer.
        // This keeps L/R pack counts in lockstep for PE mode.
        if (packStart < workLen && !needToBreak) {
            leftoverLen = workLen - packStart;
            leftover = new char[leftoverLen];
            memcpy(leftover, workBuf + packStart, leftoverLen);
        }

        buffer->release();

        if (mOptions->verbose && readNum >= lastReported + 1000000) {
            lastReported = (readNum / 1000000) * 1000000;
            string msg;
            if(isLeft)
                msg = "Read1: ";
            else
                msg = "Read2: ";
            msg += "loaded " + to_string((lastReported/1000000)) + "M reads";
            loginfo(msg);
        }

        if (needToBreak)
            break;
    }

    stopRawProducer = true;

    // final leftover
    if (leftoverLen > 0) {
        int nlCount = 0;
        int records = 0;
        for (int i = 0; i < leftoverLen; i++) {
            if (leftover[i] == '\n') {
                nlCount++;
                if (nlCount == 4) {
                    nlCount = 0;
                    records++;
                }
            }
        }
        if (nlCount == 3)
            records++;
        if (records > 0) {
            RawBuffer* buf = new RawBuffer(leftover, leftoverLen);
            RawPack* pack = new RawPack;
            pack->buffer = buf;
            pack->offset = 0;
            pack->length = leftoverLen;
            pack->readCount = records;
            buf->addRef();
            if (isLeft) {
                mLeftInputLists[mLeftPackReadCounter % mOptions->thread]->produce(pack);
                mLeftPackReadCounter++;
                onPackProduced(true, pack->length);
            } else {
                mRightInputLists[mRightPackReadCounter % mOptions->thread]->produce(pack);
                mRightPackReadCounter++;
                onPackProduced(false, pack->length);
            }
            buf->release();
        } else {
            delete[] leftover;
        }
        leftover = NULL;
        leftoverLen = 0;
    }

    for(int t=0; t<mOptions->thread; t++) {
        if(isLeft)
            mLeftInputLists[t]->setProducerFinished();
        else
            mRightInputLists[t]->setProducerFinished();
    }
    mBackpressureCv.notify_all();

    if(mOptions->verbose) {
        if(isLeft) {
            mLeftReaderFinished = true;
            loginfo("Read1: loading completed with " + to_string(mLeftPackReadCounter) + " packs");
        }
        else {
            mRightReaderFinished = true;
            loginfo("Read2: loading completed with " + to_string(mRightPackReadCounter) + " packs");
        }
    }

    rawCvNotFull.notify_all();
    rawCvNotEmpty.notify_all();
    rawProducer.join();
}

void PairEndProcessor::interleavedReaderTask()
{
    trace::TaskBreakdown task("pe.reader.interleaved", "pe.reader");
    if(mOptions->verbose)
        loginfo("start to load data");
    long lastReported = 0;
    int slept = 0;
    long readNum = 0;
    Read** dataLeft = new Read*[PACK_SIZE];
    Read** dataRight = new Read*[PACK_SIZE];
    memset(dataLeft, 0, sizeof(Read*)*PACK_SIZE);
    memset(dataRight, 0, sizeof(Read*)*PACK_SIZE);
    FastqReaderPair reader(mOptions->in1, mOptions->in2, true, mOptions->phred64, true, mOptions->thread);
    int count=0;
    bool needToBreak = false;
    ReadPair* pair = new ReadPair();
    auto wrapDirectPack = [](Read** reads, int count) -> RawPack* {
        ReadPack* parsed = new ReadPack;
        parsed->data = reads;
        parsed->count = count;
        parsed->rawBytes = 0;

        RawPack* pack = new RawPack;
        pack->directReadPack = true;
        pack->directPack = parsed;
        return pack;
    };
    while(true){
        reader.read(pair);
        if(pair->eof() || needToBreak){
            RawPack* packLeft = wrapDirectPack(dataLeft, count);
            RawPack* packRight = wrapDirectPack(dataRight, count);

            mLeftInputLists[mLeftPackReadCounter % mOptions->thread]->produce(packLeft);
            mLeftPackReadCounter++;
            onPackProduced(true, 0);

            mRightInputLists[mRightPackReadCounter % mOptions->thread]->produce(packRight);
            mRightPackReadCounter++;
            onPackProduced(false, 0);

            dataLeft = NULL;
            dataRight = NULL;
            break;
        }
        dataLeft[count] = pair->mLeft;
        dataRight[count] = pair->mRight;
        count++;
        if(mOptions->readsToProcess >0 && count + readNum >= mOptions->readsToProcess) {
            needToBreak = true;
        }
        if(mOptions->verbose && count + readNum >= lastReported + 1000000) {
            lastReported = count + readNum;
            string msg = "loaded " + to_string((lastReported/1000000)) + "M read pairs";
            loginfo(msg);
        }
        if(count == PACK_SIZE || needToBreak){
            RawPack* packLeft = wrapDirectPack(dataLeft, count);
            RawPack* packRight = wrapDirectPack(dataRight, count);

            mLeftInputLists[mLeftPackReadCounter % mOptions->thread]->produce(packLeft);
            mLeftPackReadCounter++;
            onPackProduced(true, 0);

            mRightInputLists[mRightPackReadCounter % mOptions->thread]->produce(packRight);
            mRightPackReadCounter++;
            onPackProduced(false, 0);

            dataLeft = new Read*[PACK_SIZE];
            dataRight = new Read*[PACK_SIZE];
            memset(dataLeft, 0, sizeof(Read*)*PACK_SIZE);
            memset(dataRight, 0, sizeof(Read*)*PACK_SIZE);
            while(true){
                const int pressure = std::max(inputPressureLevel(true), inputPressureLevel(false));
                if (pressure <= 0 || shouldStopReading)
                    break;
                if (pressure < 2)
                    break;

                const uint64_t t0 = trace::nowUs();
                slept++;
                std::unique_lock<std::mutex> lk(mBackpressureMutex);
                mBackpressureCv.wait(lk, [&]() {
                    return std::max(inputPressureLevel(true), inputPressureLevel(false)) < 2 || shouldStopReading;
                });
                const uint64_t t1 = trace::nowUs();
                if (t1 > t0)
                    mBackpressureInputUsWindow.fetch_add((long)(t1 - t0), std::memory_order_relaxed);
                task.addGap("queue_full_wait", t1 > t0 ? t1 - t0 : 0);
            }
            readNum += count;
            const int packLimit = mAdaptivePackLimit.load(std::memory_order_relaxed);
            if(packLimit > 0 && readNum % (PACK_SIZE * packLimit) == 0 && mLeftWriter) {
                while( (mLeftWriter && mLeftWriter->bufferLength() > packLimit) || (mRightWriter && mRightWriter->bufferLength() > packLimit) ){
                    const uint64_t t0 = trace::nowUs();
                    slept++;
                    usleep(1000);
                    const uint64_t t1 = trace::nowUs();
                    task.addGap("backpressure_writer", t1 > t0 ? t1 - t0 : 0);
                }
            }
            count = 0;
        }
    }

    delete pair;

    for(int t=0; t<mOptions->thread; t++) {
        mLeftInputLists[t]->setProducerFinished();
        mRightInputLists[t]->setProducerFinished();
    }
    mBackpressureCv.notify_all();

    if(mOptions->verbose) {
        loginfo("interleaved: loading completed with " + to_string(mLeftPackReadCounter) + " packs");
    }

    mLeftReaderFinished = true;
    mRightReaderFinished = true;

    if(dataLeft != NULL)
        delete[] dataLeft;
    if(dataRight != NULL)
        delete[] dataRight;
}

void PairEndProcessor::processorTask(ThreadConfig* config)
{
    trace::TaskBreakdown task("pe.worker." + to_string(config->getThreadId() + 1), "pe.worker");
    SingleProducerSingleConsumerList<RawPack*>* inputLeft = config->getLeftInput();
    SingleProducerSingleConsumerList<RawPack*>* inputRight = config->getRightInput();
    while(true) {
        if(config->canBeStopped()){
            break;
        }
        while(inputLeft->canBeConsumed() && inputRight->canBeConsumed()) {
            RawPack* rawLeft = inputLeft->consume();
            RawPack* rawRight = inputRight->consume();
            ReadPack* dataLeft = parseRawPack(rawLeft);
            ReadPack* dataRight = parseRawPack(rawRight);
            processPairEnd(dataLeft, dataRight, config);
        }
        if(inputLeft->isProducerFinished() && !inputLeft->canBeConsumed()) {
            break;
        } else if(inputRight->isProducerFinished() && !inputRight->canBeConsumed()) {
            break;
        } else {
            const uint64_t t0 = trace::nowUs();
            std::unique_lock<std::mutex> lk(mBackpressureMutex);
            mBackpressureCv.wait(lk, [&]() {
                return config->canBeStopped()
                    || (inputLeft->canBeConsumed() && inputRight->canBeConsumed())
                    || (inputLeft->isProducerFinished() && !inputLeft->canBeConsumed())
                    || (inputRight->isProducerFinished() && !inputRight->canBeConsumed());
            });
            const uint64_t t1 = trace::nowUs();
            if (t1 > t0)
                mWorkerWaitInputUsWindow.fetch_add((long)(t1 - t0), std::memory_order_relaxed);
            task.addGap("wait_input", t1 > t0 ? t1 - t0 : 0);
        }
    }
    inputLeft->setConsumerFinished();
    inputRight->setConsumerFinished();

    mFinishedThreads++;
    if(mOptions->verbose) {
        string msg = "thread " + to_string(config->getThreadId() + 1) + " data processing completed";
        loginfo(msg);
    }

    if(mFinishedThreads == mOptions->thread) {
        if(mLeftWriter)
            mLeftWriter->setInputCompleted();
        if(mRightWriter)
            mRightWriter->setInputCompleted();
        if(mUnpairedLeftWriter)
            mUnpairedLeftWriter->setInputCompleted();
        if(mUnpairedRightWriter)
            mUnpairedRightWriter->setInputCompleted();
        if(mMergedWriter)
            mMergedWriter->setInputCompleted();
        if(mFailedWriter)
            mFailedWriter->setInputCompleted();
        if(mOverlappedWriter)
            mOverlappedWriter->setInputCompleted();
    }

    if(mOptions->verbose) {
        string msg = "thread " + to_string(config->getThreadId() + 1) + " finished";
        loginfo(msg);
    }
}

void PairEndProcessor::writerTask(WriterThread* config)
{
    trace::TaskBreakdown task("pe.writer." + config->getFilename(), "pe.writer");
    while(true) {
        if(config->isCompleted()){
            // last check for possible threading related issue
            config->output();
            break;
        }
        config->output();
    }

    if(mOptions->verbose) {
        string msg = config->getFilename() + " writer finished";
        loginfo(msg);
    }
}
