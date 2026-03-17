#include "flight_batch_manager.h"

FlightBatchManager::FlightBatchManager() {
    mCapacity = 0;
    mPacksPerBatch = 1;
    mUsedCredits = 0;
    mMutex = NULL;
    mCv = NULL;
    mStopFlag = NULL;
}

void FlightBatchManager::configure(int capacity, int packsPerBatch) {
    mCapacity = capacity > 0 ? capacity : 0;
    mPacksPerBatch = packsPerBatch > 0 ? packsPerBatch : 1;
    mUsedCredits.store(0, std::memory_order_relaxed);
}

void FlightBatchManager::setSync(std::mutex* mu, std::condition_variable* cv, std::atomic_bool* stopFlag) {
    mMutex = mu;
    mCv = cv;
    mStopFlag = stopFlag;
}

void FlightBatchManager::acquireForNextPack(long packsBefore) {
    if (mCapacity <= 0)
        return;
    if (packsBefore < 0 || packsBefore % mPacksPerBatch != 0)
        return;
    if (mMutex == NULL || mCv == NULL)
        return;

    while (true) {
        long now = mUsedCredits.load(std::memory_order_relaxed);
        if (now < mCapacity) {
            if (mUsedCredits.compare_exchange_weak(now, now + 1, std::memory_order_relaxed))
                return;
            continue;
        }
        std::unique_lock<std::mutex> lk(*mMutex);
        mCv->wait(lk, [&]() {
            return mUsedCredits.load(std::memory_order_relaxed) < mCapacity
                || (mStopFlag && mStopFlag->load(std::memory_order_relaxed));
        });
        if (mStopFlag && mStopFlag->load(std::memory_order_relaxed))
            return;
    }
}

void FlightBatchManager::releaseAfterConsume(long packsAfter) {
    if (mCapacity <= 0)
        return;
    if (packsAfter < 0 || packsAfter % mPacksPerBatch != 0)
        return;

    long prev = mUsedCredits.fetch_sub(1, std::memory_order_relaxed) - 1;
    if (prev < 0)
        mUsedCredits.store(0, std::memory_order_relaxed);
    if (mCv)
        mCv->notify_all();
}

long FlightBatchManager::usedCredits() const {
    return mUsedCredits.load(std::memory_order_relaxed);
}
