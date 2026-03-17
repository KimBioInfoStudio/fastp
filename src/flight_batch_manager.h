#ifndef FLIGHT_BATCH_MANAGER_H
#define FLIGHT_BATCH_MANAGER_H

#include <atomic>
#include <condition_variable>
#include <mutex>

class FlightBatchManager {
public:
    FlightBatchManager();
    void configure(int capacity, int packsPerBatch);
    void setSync(std::mutex* mu, std::condition_variable* cv, std::atomic_bool* stopFlag = NULL);
    void acquireForNextPack(long packsBefore);
    void releaseAfterConsume(long packsAfter);
    long usedCredits() const;

private:
    int mCapacity;
    int mPacksPerBatch;
    std::atomic_long mUsedCredits;
    std::mutex* mMutex;
    std::condition_variable* mCv;
    std::atomic_bool* mStopFlag;
};

#endif
