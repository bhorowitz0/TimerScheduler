/**
 * MIT License
 *
 * Copyright (c) 2019 Ben Horowitz
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "TimerScheduler.h"
#include "SpinLock.h"

#include <chrono>
#include <functional>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <vector>


class TimerSchedulerImpl
{
public:
    TimerSchedulerImpl() = delete;

    static inline void start()
    {
        // TODO - protect
        mThread = std::thread(timerThreadLoop);
    }

    static inline TimerScheduler::TimerHandle addTimer(const std::chrono::milliseconds& period, std::function<void()> callback)
    {
        MapKeyType timeoutTime = std::chrono::steady_clock::now() + period;
        MapValue value;
        value.callback = callback;
        value.period = period;

        {
            SpinLock lock(mTimerMapLock);
            value.handle = mNextAvailableHandle++;
            auto iter = mTimers.insert(MapPairType(timeoutTime, value));
            if(iter == mTimers.begin()) {
                // wake up thread to adjust timeout
                mCondVar.notify_one();
            }
        }

        return value.handle;
    }

    static void removeTimer(TimerScheduler::TimerHandle handle)
    {
        bool needToWakeThread(true);

        {
            SpinLock lock(mTimerMapLock);
            //  TODO - optimize this lookup with a hash table...
            for(auto iter = mTimers.begin(); iter != mTimers.end(); iter++) {
                if(iter->second.handle == handle) {
                    if(iter != mTimers.begin()) {
                        needToWakeThread = false;
                    }
                    mTimers.erase(iter);
                    break;
                }
            }
        }

        if(needToWakeThread) {
            // wake up thread to adjust timeout
            mCondVar.notify_one();
        }
    }

private:
    static void timerThreadLoop();
    static void checkForTimeouts();
    static void waitForNextTimeout();

    struct MapValue
    {
        TimerScheduler::TimerHandle handle;
        std::function<void()> callback;
        std::chrono::milliseconds period;
    };

    using MapKeyType = std::chrono::steady_clock::time_point;
    using MapPairType = std::pair<MapKeyType, MapValue>;

    static std::multimap<MapKeyType, MapValue> mTimers;
    static std::atomic_flag mTimerMapLock;

    static TimerScheduler::TimerHandle mNextAvailableHandle;

    static std::thread mThread;

    static std::mutex mMutex;

    static std::condition_variable mCondVar;
};

std::multimap<TimerSchedulerImpl::MapKeyType, TimerSchedulerImpl::MapValue> TimerSchedulerImpl::mTimers;
std::atomic_flag TimerSchedulerImpl::mTimerMapLock{ATOMIC_FLAG_INIT};
TimerScheduler::TimerHandle TimerSchedulerImpl::mNextAvailableHandle{1};
std::thread TimerSchedulerImpl::mThread;
std::mutex TimerSchedulerImpl::mMutex;
std::condition_variable TimerSchedulerImpl::mCondVar;


void TimerScheduler::start()
{
    TimerSchedulerImpl::start();
}

TimerScheduler::TimerHandle TimerScheduler::addTimer(const std::chrono::milliseconds& period, std::function<void()> callback)
{
    return TimerSchedulerImpl::addTimer(period, callback);
}

void TimerScheduler::removeTimer(TimerHandle handle)
{
    TimerSchedulerImpl::removeTimer(handle);
}


void TimerSchedulerImpl::timerThreadLoop()
{
    while(1)
    {
        checkForTimeouts();

        waitForNextTimeout();
    }
}

void TimerSchedulerImpl::checkForTimeouts()
{
    const auto now = std::chrono::steady_clock::now();

    // check for timeouts
    std::vector<MapValue> timedOutTimers;
    {
        SpinLock lock(mTimerMapLock);
        auto iterFirst = mTimers.begin();
        auto iter = iterFirst;
        for(; iter != mTimers.end(); iter++) {
            if(iter->first <= now) {
                timedOutTimers.push_back(iter->second);
            }
            else {
                break;
            }
        }
        if(timedOutTimers.size() > 0) {
            mTimers.erase(iterFirst, iter);
        }

        // re-insert timed out timers, reusing their handle
        for(const auto& mapValue : timedOutTimers) {
            MapKeyType timeoutTime = now + mapValue.period;
            mTimers.insert(MapPairType(timeoutTime, mapValue));
        }
    }

    // call the callbacks
    for(const auto& mapValue : timedOutTimers) {
        mapValue.callback();
    }
}

void TimerSchedulerImpl::waitForNextTimeout()
{
    // get next wakeup time
    MapKeyType nextTimeout;
    {
        SpinLock lock(mTimerMapLock);
        if(mTimers.size() > 0)
        {
            nextTimeout = mTimers.begin()->first;
        }
        else
        {
            // If there are no timers, set to 10s. Will wake and reevaluate if a timer is scheduled.
            nextTimeout = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        }
    }

    // wait for timeout to happen
    std::unique_lock<std::mutex> lock(mMutex);
    mCondVar.wait_until(lock, nextTimeout);
}
