#ifndef SE_PROCESSOR_H
#define SE_PROCESSOR_H

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "read.h"
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <atomic>
#include "options.h"
#include "threadconfig.h"
#include "filter.h"
#include "umiprocessor.h"
#include "writerthread.h"
#include "duplicate.h"
#include "singleproducersingleconsumerlist.h"
#include "flight_batch_manager.h"

using namespace std;

class SingleEndProcessor{
public:
    SingleEndProcessor(Options* opt);
    ~SingleEndProcessor();
    bool process();

private:
    bool processSingleEnd(ReadPack* pack, ThreadConfig* config);
    ReadPack* parseRawPack(RawPack* rawPack);
    void readerTask();
    void processorTask(ThreadConfig* config);
    void initConfig(ThreadConfig* config);
    void initOutput();
    void closeOutput();
    void writerTask(WriterThread* config);
    void initAdaptiveBackpressure();
    void onPackProduced(int rawBytes);
    void onPackConsumed(int rawBytes);
    bool shouldThrottleInput() const;
    int inputPressureLevel() const;
    long effectiveByteLimit() const;
    void startRuntimeAutotune();
    void stopRuntimeAutotune();
    void runtimeAutotuneTask();

private:
    Options* mOptions;
    atomic_bool mReaderFinished;
    atomic_int mFinishedThreads;
    Filter* mFilter;
    UmiProcessor* mUmiProcessor;
    WriterThread* mLeftWriter;
    WriterThread* mFailedWriter;
    Duplicate* mDuplicate;
    SingleProducerSingleConsumerList<RawPack*>** mInputLists;
    size_t mPackReadCounter;
    atomic_long mPackProcessedCounter;
    atomic_long mInFlightPacks;
    atomic_long mInFlightBytes;
    atomic_long mAvgPackBytes;
    FlightBatchManager mFlightBatch;
    atomic_int mAdaptivePackLimit;
    atomic_long mAdaptiveByteLimit;
    atomic_int mRawChunksInFlightLimit;
    atomic_long mBackpressureInputUsWindow;
    atomic_long mWorkerWaitInputUsWindow;
    atomic_bool mAutoTuneStop;
    thread* mAutoTuneThread;
    mutable mutex mBackpressureMutex;
    condition_variable mBackpressureCv;
};


#endif
