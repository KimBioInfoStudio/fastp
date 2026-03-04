#include "seprocessor.h"
#include "fastqreader.h"
#include <iostream>
#include <unistd.h>
#include <functional>
#include <thread>
#include <memory.h>
#include "util.h"
#include "jsonreporter.h"
#include "htmlreporter.h"
#include "adaptertrimmer.h"
#include "polyx.h"

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
}

SingleEndProcessor::~SingleEndProcessor() {
    delete mFilter;
    if(mDuplicate) {
        delete mDuplicate;
        mDuplicate = NULL;
    }
    delete[] mInputLists;
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

    delete[] pack->data;
    delete pack;

    mPackProcessedCounter++;

    return true;
}

ReadPack* SingleEndProcessor::parseRawPack(RawPack* rawPack) {
    int count = rawPack->readCount;
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
    }

    rawPack->buffer->release();
    delete rawPack;

    ReadPack* pack = new ReadPack;
    pack->data = data;
    pack->count = count;
    return pack;
}

void SingleEndProcessor::readerTask()
{
    if(mOptions->verbose)
        loginfo("start to load data");
    long lastReported = 0;
    int slept = 0;
    long readNum = 0;
    FastqReader reader(mOptions->in1, true, mOptions->phred64);

    // leftover from previous buffer (partial record at buffer boundary)
    char* leftover = NULL;
    int leftoverLen = 0;

    bool needToBreak = false;
    while(true){
        int rawLen = 0;
        char* rawData = reader.readRawBuffer(rawLen);
        if (!rawData) {
            // no more data from reader; leftover handled after loop
            break;
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

                    readNum += recordsInPack;
                    recordsInPack = 0;
                    packStart = lastRecordEnd;

                    // backpressure
                    while (mPackReadCounter - mPackProcessedCounter > PACK_IN_MEM_LIMIT) {
                        slept++;
                        usleep(100);
                    }
                    // writer backpressure
                    if (readNum % (PACK_SIZE * PACK_IN_MEM_LIMIT) == 0 && mLeftWriter) {
                        while (mLeftWriter->bufferLength() > PACK_IN_MEM_LIMIT) {
                            slept++;
                            usleep(1000);
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
            readNum += recordsInPack;
            packStart = lastRecordEnd;

            while (mPackReadCounter - mPackProcessedCounter > PACK_IN_MEM_LIMIT) {
                slept++;
                usleep(100);
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
            buf->release();
        } else {
            delete[] leftover;
        }
        leftover = NULL;
        leftoverLen = 0;
    }

    for(int t=0; t<mOptions->thread; t++)
        mInputLists[t]->setProducerFinished();

    mReaderFinished = true;
    if(mOptions->verbose) {
        loginfo("Loading completed with " + to_string(mPackReadCounter) + " packs");
    }
}

void SingleEndProcessor::processorTask(ThreadConfig* config)
{
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
            usleep(100);
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
