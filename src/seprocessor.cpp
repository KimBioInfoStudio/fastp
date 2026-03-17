#include "seprocessor.h"
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
#include "jsonreporter.h"
#include "htmlreporter.h"
#include "adaptertrimmer.h"
#include "polyx.h"
#include "trace_profiler.h"

SingleEndProcessor::SingleEndProcessor(Options* opt){
    mOptions = opt;
    mReaderFinished = false;
    mFinishedThreads = 0;
    mFilter = new Filter(opt);
    mUmiProcessor = new UmiProcessor(opt);
    mLeftWriter =  NULL;
    mFailedWriter = NULL;

    mDuplicate = NULL;
    if(mOptions->duplicate.enabled) {
        mDuplicate = new Duplicate(mOptions);
    }

    mPackReadCounter = 0;
    mPackProcessedCounter = 0;
    mInFlightPacks = 0;
    mInFlightBytes = 0;
    mAvgPackBytes = 256 * 1024;
    int flightCap = std::max(4, mOptions->thread + 2);
    mFlightBatch.configure(flightCap, 32);
    mFlightBatch.setSync(&mBackpressureMutex, &mBackpressureCv, NULL);
    mAdaptivePackLimit = PACK_IN_MEM_LIMIT;
    mAdaptiveByteLimit = 64L * 1024L * 1024L;
    mRawChunksInFlightLimit = 8;
    mBackpressureInputUsWindow = 0;
    mWorkerWaitInputUsWindow = 0;
    mAutoTuneStop = true;
    mAutoTuneThread = NULL;
    initAdaptiveBackpressure();
}

SingleEndProcessor::~SingleEndProcessor() {
    stopRuntimeAutotune();
    delete mFilter;
    if(mDuplicate) {
        delete mDuplicate;
        mDuplicate = NULL;
    }
    delete[] mInputLists;
}

void SingleEndProcessor::initAdaptiveBackpressure() {
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

    int rawChunks = 8 + mOptions->thread / 2;
    if (rawChunks < 4)
        rawChunks = 4;
    if (rawChunks > 64)
        rawChunks = 64;
    mRawChunksInFlightLimit.store(rawChunks, std::memory_order_relaxed);
}

long SingleEndProcessor::effectiveByteLimit() const {
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

bool SingleEndProcessor::shouldThrottleInput() const {
    return inputPressureLevel() > 0;
}

int SingleEndProcessor::inputPressureLevel() const {
    const long packs = mInFlightPacks.load(std::memory_order_relaxed);
    const long bytes = mInFlightBytes.load(std::memory_order_relaxed);
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

void SingleEndProcessor::startRuntimeAutotune() {
    mAutoTuneStop.store(false, std::memory_order_relaxed);
    if (mAutoTuneThread == NULL)
        mAutoTuneThread = new std::thread(std::bind(&SingleEndProcessor::runtimeAutotuneTask, this));
}

void SingleEndProcessor::stopRuntimeAutotune() {
    mAutoTuneStop.store(true, std::memory_order_relaxed);
    mBackpressureCv.notify_all();
    if (mAutoTuneThread) {
        mAutoTuneThread->join();
        delete mAutoTuneThread;
        mAutoTuneThread = NULL;
    }
}

void SingleEndProcessor::runtimeAutotuneTask() {
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

        const long inFlightPacks = mInFlightPacks.load(std::memory_order_relaxed);
        const long inFlightBytes = mInFlightBytes.load(std::memory_order_relaxed);

        const int packLimitCur = mAdaptivePackLimit.load(std::memory_order_relaxed);
        const long byteLimitCur = mAdaptiveByteLimit.load(std::memory_order_relaxed);
        const int rawChunksCur = mRawChunksInFlightLimit.load(std::memory_order_relaxed);

        const long writerBacklog = mLeftWriter ? mLeftWriter->bufferLength() : 0L;

        const double bpRatio = bpUs / (intervalMs * 1000.0);
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

void SingleEndProcessor::onPackProduced(int rawBytes) {
    long packsBefore = mInFlightPacks.load(std::memory_order_relaxed);
    mFlightBatch.acquireForNextPack(packsBefore);

    if (rawBytes < 0)
        rawBytes = 0;
    mInFlightPacks.fetch_add(1, std::memory_order_relaxed);
    mInFlightBytes.fetch_add(rawBytes, std::memory_order_relaxed);

    const long prev = mAvgPackBytes.load(std::memory_order_relaxed);
    const long next = (prev * 7 + rawBytes) / 8;
    if (next > 0)
        mAvgPackBytes.store(next, std::memory_order_relaxed);

    mBackpressureCv.notify_all();
}

void SingleEndProcessor::onPackConsumed(int rawBytes) {
    if (rawBytes < 0)
        rawBytes = 0;
    const long p = mInFlightPacks.fetch_sub(1, std::memory_order_relaxed) - 1;
    if (p < 0)
        mInFlightPacks.store(0, std::memory_order_relaxed);

    const long b = mInFlightBytes.fetch_sub(rawBytes, std::memory_order_relaxed) - rawBytes;
    if (b < 0)
        mInFlightBytes.store(0, std::memory_order_relaxed);

    mFlightBatch.releaseAfterConsume(p);
    mBackpressureCv.notify_all();
}

void SingleEndProcessor::initOutput() {
    if(!mOptions->failedOut.empty())
        mFailedWriter = new WriterThread(mOptions, mOptions->failedOut);
    if(mOptions->out1.empty() && !mOptions->outputToSTDOUT)
        return;
    mLeftWriter = new WriterThread(mOptions, mOptions->out1, mOptions->outputToSTDOUT);
}

void SingleEndProcessor::closeOutput() {
    if(mLeftWriter) {
        delete mLeftWriter;
        mLeftWriter = NULL;
    }
    if(mFailedWriter) {
        delete mFailedWriter;
        mFailedWriter = NULL;
    }
}

void SingleEndProcessor::initConfig(ThreadConfig* config) {
    if(mOptions->out1.empty())
        return;

    if(mOptions->split.enabled) {
        config->initWriterForSplit();
    }
}

bool SingleEndProcessor::process(){
    if(!mOptions->split.enabled)
        initOutput();
    startRuntimeAutotune();

    mInputLists = new SingleProducerSingleConsumerList<RawPack*>*[mOptions->thread];

    ThreadConfig** configs = new ThreadConfig*[mOptions->thread];
    for(int t=0; t<mOptions->thread; t++){
        mInputLists[t] = new SingleProducerSingleConsumerList<RawPack*>();
        configs[t] = new ThreadConfig(mOptions, t, false);
        configs[t]->setInputList(mInputLists[t]);
        initConfig(configs[t]);
    }

    std::thread readerThread(std::bind(&SingleEndProcessor::readerTask, this));

    std::thread** threads = new thread*[mOptions->thread];
    for(int t=0; t<mOptions->thread; t++){
        threads[t] = new std::thread(std::bind(&SingleEndProcessor::processorTask, this, configs[t]));
    }

    std::thread* leftWriterThread = NULL;
    std::thread* failedWriterThread = NULL;
    if(mLeftWriter)
        leftWriterThread = new std::thread(std::bind(&SingleEndProcessor::writerTask, this, mLeftWriter));
    if(mFailedWriter)
        failedWriterThread = new std::thread(std::bind(&SingleEndProcessor::writerTask, this, mFailedWriter));

    readerThread.join();
    for(int t=0; t<mOptions->thread; t++){
        threads[t]->join();
    }

    if(!mOptions->split.enabled) {
        if(leftWriterThread)
            leftWriterThread->join();
        if(failedWriterThread)
            failedWriterThread->join();
    }
    stopRuntimeAutotune();

    if(mOptions->verbose)
        loginfo("start to generate reports\n");

    // merge stats and read filter results
    vector<Stats*> preStats;
    vector<Stats*> postStats;
    vector<FilterResult*> filterResults;
    for(int t=0; t<mOptions->thread; t++){
        preStats.push_back(configs[t]->getPreStats1());
        postStats.push_back(configs[t]->getPostStats1());
        filterResults.push_back(configs[t]->getFilterResult());
    }
    Stats* finalPreStats = Stats::merge(preStats);
    Stats* finalPostStats = Stats::merge(postStats);
    FilterResult* finalFilterResult = FilterResult::merge(filterResults);

    // read filter results to the first thread's
    for(int t=1; t<mOptions->thread; t++){
        preStats.push_back(configs[t]->getPreStats1());
        postStats.push_back(configs[t]->getPostStats1());
    }

    cerr << "Read1 before filtering:"<<endl;
    finalPreStats->print();
    cerr << endl;
    cerr << "Read1 after filtering:"<<endl;
    finalPostStats->print();

    cerr << endl;
    cerr << "Filtering result:"<<endl;
    finalFilterResult->print();

    double dupRate = 0.0;
    if(mOptions->duplicate.enabled) {
        dupRate = mDuplicate->getDupRate();
        cerr << endl;
        cerr << "Duplication rate (may be overestimated since this is SE data): " << dupRate * 100.0 << "%" << endl;
    }

    // make JSON report
    JsonReporter jr(mOptions);
    jr.setDup(dupRate);
    jr.report(finalFilterResult, finalPreStats, finalPostStats);

    // make HTML report
    HtmlReporter hr(mOptions);
    hr.setDup(dupRate);
    hr.report(finalFilterResult, finalPreStats, finalPostStats);

    // clean up
    for(int t=0; t<mOptions->thread; t++){
        delete threads[t];
        threads[t] = NULL;
        delete configs[t];
        configs[t] = NULL;
    }

    delete finalPreStats;
    delete finalPostStats;
    delete finalFilterResult;

    delete[] threads;
    delete[] configs;

    if(leftWriterThread)
        delete leftWriterThread;
    if(failedWriterThread)
        delete failedWriterThread;

    if(!mOptions->split.enabled)
        closeOutput();

    return true;
}

bool SingleEndProcessor::processSingleEnd(ReadPack* pack, ThreadConfig* config){
    // build output on stack strings, move to heap only when handing off to writers
    string outstr, failedOut;
    outstr.reserve(pack->count * 320);
    int tid = config->getThreadId();

    int readPassed = 0;
    for(int p=0;p<pack->count;p++){

        // original read1
        Read* or1 = pack->data[p];

        // stats the original read before trimming
        config->getPreStats1()->statRead(or1);

        // handling the duplication profiling
        bool dedupOut = false;
        if(mDuplicate) {
            bool isDup = mDuplicate->checkRead(or1);
            if(mOptions->duplicate.dedup && isDup)
                dedupOut = true;
        }

        // filter by index
        if(mOptions->indexFilter.enabled && mFilter->filterByIndex(or1)) {
            delete or1;
            continue;
        }

        // fix MGI
        if(mOptions->fixMGI) {
            or1->fixMGI();
        }

        // umi processing
        if(mOptions->umi.enabled)
            mUmiProcessor->process(or1);

        int frontTrimmed = 0;
        // trim in head and tail, and apply quality cut in sliding window
        Read* r1 = mFilter->trimAndCut(or1, mOptions->trim.front1, mOptions->trim.tail1, frontTrimmed);

        if(r1 != NULL) {
            if(mOptions->polyGTrim.enabled)
                PolyX::trimPolyG(r1, config->getFilterResult(), mOptions->polyGTrim.minLen);
        }

        bool isAdapterDimer = false;
        if(r1 != NULL && mOptions->adapter.enabled){
            bool trimmed = false;
            if(mOptions->adapter.hasSeqR1)
                trimmed = AdapterTrimmer::trimBySequence(r1, config->getFilterResult(), mOptions->adapter.sequence, false);
            bool incTrimmedCounter = !trimmed;
            if(mOptions->adapter.hasFasta) {
                trimmed |= AdapterTrimmer::trimByMultiSequences(r1, config->getFilterResult(), mOptions->adapter.seqsInFasta, false, incTrimmedCounter);
            }

            // Check for adapter dimer: read shorter than threshold after adapter trimming
            // AND adapter was detected (requires evidence)
            if(r1 != NULL && trimmed && r1->length() <= mOptions->adapter.dimerMaxLen) {
                isAdapterDimer = true;
            }
        }

        if(r1 != NULL) {
            if(mOptions->polyXTrim.enabled)
                PolyX::trimPolyX(r1, config->getFilterResult(), mOptions->polyXTrim.minLen);
        }

        if(r1 != NULL) {
            if( mOptions->trim.maxLen1 > 0 && mOptions->trim.maxLen1 < r1->length())
                r1->resize(mOptions->trim.maxLen1);
        }

        int result = mFilter->passFilter(r1);

        if(isAdapterDimer)
            result = FAIL_ADAPTER_DIMER;

        config->addFilterResult(result, 1);

        if(!dedupOut) {
            if( r1 != NULL &&  result == PASS_FILTER) {
                r1->appendToString(&outstr);

                // stats the read after filtering
                config->getPostStats1()->statRead(r1);
                readPassed++;
            } else if(mFailedWriter) {
                or1->appendToStringWithTag(&failedOut, FAILED_TYPES[result]);
            }
        }

        delete or1;
        // if no trimming applied, r1 should be identical to or1
        if(r1 != or1 && r1 != NULL)
            delete r1;
    }

    if(mOptions->split.enabled) {
        // split output by each worker thread
        if(!mOptions->out1.empty())
            config->getWriter1()->writeString(outstr);
    }

    if(mLeftWriter) {
        mLeftWriter->input(tid, new string(std::move(outstr)));
    }
    if(mFailedWriter) {
        mFailedWriter->input(tid, new string(std::move(failedOut)));
    }

    if(mOptions->split.byFileLines)
        config->markProcessed(readPassed);
    else
        config->markProcessed(pack->count);

    const int rawBytes = pack->rawBytes;
    delete[] pack->data;
    delete pack;

    mPackProcessedCounter++;
    onPackConsumed(rawBytes);

    return true;
}

ReadPack* SingleEndProcessor::parseRawPack(RawPack* rawPack) {
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
            error_exit("invalid FASTQ record: name line does not start with '@' in " + mOptions->in1);
        }
        if (strand->empty() || (*strand)[0] != '+') {
            error_exit("invalid FASTQ record: strand line does not start with '+' in " + mOptions->in1);
        }
        if (seq->length() != qual->length()) {
            error_exit("invalid FASTQ record: sequence and quality lengths differ in " + mOptions->in1);
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

void SingleEndProcessor::readerTask()
{
    trace::TaskBreakdown task("se.reader.r1", "se.reader");
    if(mOptions->verbose)
        loginfo("start to load data");
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
        FastqReader reader(mOptions->in1, true, mOptions->phred64, mOptions->thread);
        while (!stopRawProducer) {
            int rawLen = 0;
            char* rawData = reader.readRawBuffer(rawLen);
            if (!rawData)
                break;
            std::unique_lock<std::mutex> lk(rawMu);
            rawCvNotFull.wait(lk, [&]() {
                const int rawLimit = mRawChunksInFlightLimit.load(std::memory_order_relaxed);
                return rawQueue.size() < (size_t)rawLimit || stopRawProducer;
            });
            if (stopRawProducer) {
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

    // leftover from previous buffer (partial record at buffer boundary)
    char* leftover = NULL;
    int leftoverLen = 0;

    bool needToBreak = false;
    while(true){
        int rawLen = 0;
        char* rawData = NULL;
        {
            const uint64_t t0 = trace::nowUs();
            std::unique_lock<std::mutex> lk(rawMu);
            rawCvNotEmpty.wait(lk, [&]() { return !rawQueue.empty() || rawDone; });
            const uint64_t t1 = trace::nowUs();
            task.addGap("wait_raw_chunk", t1 > t0 ? t1 - t0 : 0);

            if (rawQueue.empty() && rawDone)
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

        // Create RawBuffer to own workBuf
        RawBuffer* buffer = new RawBuffer(workBuf, workLen);

        // Scan for record boundaries: 4 newlines = 1 FASTQ record
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
                // complete record
                newlineCount = 0;
                recordsInPack++;
                lastRecordEnd = pos;

                // check readsToProcess limit
                if (mOptions->readsToProcess > 0 && readNum + recordsInPack >= mOptions->readsToProcess) {
                    needToBreak = true;
                    // emit what we have
                    if (recordsInPack > 0) {
                        RawPack* pack = new RawPack;
                        pack->buffer = buffer;
                        pack->offset = packStart;
                        pack->length = lastRecordEnd - packStart;
                        pack->readCount = recordsInPack;
                        buffer->addRef();
                        mInputLists[mPackReadCounter % mOptions->thread]->produce(pack);
                        mPackReadCounter++;
                        onPackProduced(pack->length);
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
                    mInputLists[mPackReadCounter % mOptions->thread]->produce(pack);
                    mPackReadCounter++;
                    onPackProduced(pack->length);

                    readNum += recordsInPack;
                    recordsInPack = 0;
                    packStart = lastRecordEnd;

                    // backpressure
                    while (true) {
                        const int pressure = inputPressureLevel();
                        if (pressure <= 0)
                            break;
                        if (pressure < 2)
                            break;

                        const uint64_t t0 = trace::nowUs();
                        slept++;
                        std::unique_lock<std::mutex> lk(mBackpressureMutex);
                        mBackpressureCv.wait(lk, [&]() { return inputPressureLevel() < 2; });
                        const uint64_t t1 = trace::nowUs();
                        if (t1 > t0)
                            mBackpressureInputUsWindow.fetch_add((long)(t1 - t0), std::memory_order_relaxed);
                        task.addGap("queue_full_wait", t1 > t0 ? t1 - t0 : 0);
                    }
                    // writer backpressure
                    const int packLimit = mAdaptivePackLimit.load(std::memory_order_relaxed);
                    if (packLimit > 0 && readNum % (PACK_SIZE * packLimit) == 0 && mLeftWriter) {
                        while (mLeftWriter->bufferLength() > packLimit) {
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

        // Handle remaining complete records in this buffer
        if (recordsInPack > 0 && !needToBreak) {
            RawPack* pack = new RawPack;
            pack->buffer = buffer;
            pack->offset = packStart;
            pack->length = lastRecordEnd - packStart;
            pack->readCount = recordsInPack;
            buffer->addRef();
            mInputLists[mPackReadCounter % mOptions->thread]->produce(pack);
            mPackReadCounter++;
            onPackProduced(pack->length);
            readNum += recordsInPack;
            packStart = lastRecordEnd;

            while (true) {
                const int pressure = inputPressureLevel();
                if (pressure <= 0)
                    break;
                if (pressure < 2)
                    break;

                const uint64_t t0 = trace::nowUs();
                slept++;
                std::unique_lock<std::mutex> lk(mBackpressureMutex);
                mBackpressureCv.wait(lk, [&]() { return inputPressureLevel() < 2; });
                const uint64_t t1 = trace::nowUs();
                if (t1 > t0)
                    mBackpressureInputUsWindow.fetch_add((long)(t1 - t0), std::memory_order_relaxed);
                task.addGap("queue_full_wait", t1 > t0 ? t1 - t0 : 0);
            }
        }

        // Save leftover (partial record at end of buffer)
        if (lastRecordEnd < workLen && !needToBreak) {
            leftoverLen = workLen - lastRecordEnd;
            leftover = new char[leftoverLen];
            memcpy(leftover, workBuf + lastRecordEnd, leftoverLen);
        }

        // Release reader's reference to buffer
        buffer->release();

        if (mOptions->verbose && readNum >= lastReported + 1000000) {
            lastReported = (readNum / 1000000) * 1000000;
            string msg = "loaded " + to_string((lastReported/1000000)) + "M reads";
            loginfo(msg);
        }

        if (needToBreak)
            break;
    }

    stopRawProducer = true;
    rawCvNotFull.notify_all();
    rawCvNotEmpty.notify_all();

    // Handle final leftover as last pack (incomplete buffer at EOF)
    if (leftoverLen > 0) {
        // Count records in leftover
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
        // Handle file without trailing newline
        if (nlCount == 3) {
            records++;
        }
        if (records > 0) {
            RawBuffer* buf = new RawBuffer(leftover, leftoverLen);
            RawPack* pack = new RawPack;
            pack->buffer = buf;
            pack->offset = 0;
            pack->length = leftoverLen;
            pack->readCount = records;
            buf->addRef();
            mInputLists[mPackReadCounter % mOptions->thread]->produce(pack);
            mPackReadCounter++;
            onPackProduced(pack->length);
            buf->release();
        } else {
            delete[] leftover;
        }
        leftover = NULL;
        leftoverLen = 0;
    }

    for(int t=0; t<mOptions->thread; t++)
        mInputLists[t]->setProducerFinished();
    mBackpressureCv.notify_all();

    mReaderFinished = true;
    if(mOptions->verbose) {
        loginfo("Loading completed with " + to_string(mPackReadCounter) + " packs");
    }
    rawProducer.join();
}

void SingleEndProcessor::processorTask(ThreadConfig* config)
{
    trace::TaskBreakdown task("se.worker." + to_string(config->getThreadId() + 1), "se.worker");
    SingleProducerSingleConsumerList<RawPack*>* input = config->getLeftInput();
    while(true) {
        if(config->canBeStopped()){
            break;
        }
        while(input->canBeConsumed()) {
            RawPack* rawData = input->consume();
            ReadPack* data = parseRawPack(rawData);
            processSingleEnd(data, config);
        }
        if(input->isProducerFinished()) {
            if(!input->canBeConsumed()) {
                if(mOptions->verbose) {
                    string msg = "thread " + to_string(config->getThreadId() + 1) + " data processing completed";
                    loginfo(msg);
                }
                break;
            }
        } else {
            const uint64_t t0 = trace::nowUs();
            std::unique_lock<std::mutex> lk(mBackpressureMutex);
            mBackpressureCv.wait(lk, [&]() {
                return config->canBeStopped()
                    || input->canBeConsumed()
                    || (input->isProducerFinished() && !input->canBeConsumed());
            });
            const uint64_t t1 = trace::nowUs();
            if (t1 > t0)
                mWorkerWaitInputUsWindow.fetch_add((long)(t1 - t0), std::memory_order_relaxed);
            task.addGap("wait_input", t1 > t0 ? t1 - t0 : 0);
        }
    }
    input->setConsumerFinished();

    mFinishedThreads++;
    if(mFinishedThreads == mOptions->thread) {
        if(mLeftWriter)
            mLeftWriter->setInputCompleted();
        if(mFailedWriter)
            mFailedWriter->setInputCompleted();
    }

    if(mOptions->verbose) {
        string msg = "thread " + to_string(config->getThreadId() + 1) + " finished";
        loginfo(msg);
    }
}

void SingleEndProcessor::writerTask(WriterThread* config)
{
    trace::TaskBreakdown task("se.writer." + config->getFilename(), "se.writer");
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
