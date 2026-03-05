#ifndef PE_PROCESSOR_H
#define PE_PROCESSOR_H

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
#include "overlapanalysis.h"
#include "writerthread.h"
#include "duplicate.h"
#include "flight_batch_manager.h"


using namespace std;

typedef struct ReadPairRepository ReadPairRepository;

class PairEndProcessor{
public:
    PairEndProcessor(Options* opt);
    ~PairEndProcessor();
    bool process();

private:
    bool processPairEnd(ReadPack* leftPack, ReadPack* rightPack, ThreadConfig* config);
    ReadPack* parseRawPack(RawPack* rawPack);
    void readerTask(bool isLeft);
    void interleavedReaderTask();
    void processorTask(ThreadConfig* config);
    void initConfig(ThreadConfig* config);
    void initOutput();
    void closeOutput();
    void statInsertSize(Read* r1, Read* r2, OverlapResult& ov, int frontTrimmed1 = 0, int frontTrimmed2 = 0);
    int getPeakInsertSize();
    void writerTask(WriterThread* config);
    void initAdaptiveBackpressure();
    void onPackProduced(bool isLeft, int rawBytes);
    void onPackConsumed(int leftRawBytes, int rightRawBytes);
    bool shouldThrottleInput(bool isLeft) const;
    int inputPressureLevel(bool isLeft) const;
    long effectiveByteLimit() const;
    void startRuntimeAutotune();
    void stopRuntimeAutotune();
    void runtimeAutotuneTask();

private:
    atomic_bool mLeftReaderFinished;
    atomic_bool mRightReaderFinished;
    atomic_int mFinishedThreads;
    Options* mOptions;
    Filter* mFilter;
    UmiProcessor* mUmiProcessor;
    atomic_long* mInsertSizeHist;
    WriterThread* mLeftWriter;
    WriterThread* mRightWriter;
    WriterThread* mUnpairedLeftWriter;
    WriterThread* mUnpairedRightWriter;
    WriterThread* mMergedWriter;
    WriterThread* mFailedWriter;
    WriterThread* mOverlappedWriter;
    Duplicate* mDuplicate;
    SingleProducerSingleConsumerList<RawPack*>** mLeftInputLists;
    SingleProducerSingleConsumerList<RawPack*>** mRightInputLists;
    size_t mLeftPackReadCounter;
    size_t mRightPackReadCounter;
    atomic_long mPackProcessedCounter;
    atomic_long mLeftInFlightPacks;
    atomic_long mRightInFlightPacks;
    atomic_long mLeftInFlightBytes;
    atomic_long mRightInFlightBytes;
    atomic_long mAvgPackBytes;
    FlightBatchManager mLeftFlightBatch;
    FlightBatchManager mRightFlightBatch;
    atomic_int mAdaptivePackLimit;
    atomic_long mAdaptiveByteLimit;
    atomic_int mRawChunksInFlightLimit;
    atomic_long mBackpressureInputUsWindow;
    atomic_long mWorkerWaitInputUsWindow;
    atomic_bool mAutoTuneStop;
    thread* mAutoTuneThread;
    mutable mutex mBackpressureMutex;
    condition_variable mBackpressureCv;
    atomic_bool shouldStopReading;
};


#endif
