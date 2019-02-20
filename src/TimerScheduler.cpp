/**
 * The MIT License (MIT)
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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "TimerScheduler.hpp"

#include <chrono>
#include <functional>
#include <thread>
#include <condition_variable>
#include <map>
#include <unordered_map>
#include <vector>


class TimerSchedulerImpl
{
public:
    TimerSchedulerImpl() = delete;
    TimerSchedulerImpl(const TimerSchedulerImpl&) = delete;
    TimerSchedulerImpl& operator=(const TimerSchedulerImpl &) = delete;
    TimerSchedulerImpl(TimerSchedulerImpl &&) = delete;
    TimerSchedulerImpl & operator=(TimerSchedulerImpl &&) = delete;

    static inline void reserve(size_t anticipatedNumberOfTimers)
    {
        mTimerHandleToTimeoutTimeMap.reserve(anticipatedNumberOfTimers);
    }

    static inline void run()
    {
        mThread = std::thread(timerThreadLoop);
    }

    static inline TimerScheduler::TimerHandle addTimer(const std::chrono::milliseconds& period, TimerScheduler::TimerCallback callback)
    {
        // Compute timeout immediately (before locking mutex)
        TimeoutTime timeoutTime = std::chrono::steady_clock::now() + period;

        bool needToWakeThread(false);

        Timer timer;
        timer.callback = callback;
        timer.period = period;

        {
            std::lock_guard<std::mutex> lock(mMutex);
            // Find next available handle value
            while(mTimerHandleToTimeoutTimeMap.count(mNextAvailableHandleHint) > 0)
            {
                ++mNextAvailableHandleHint;
            }
            timer.handle = mNextAvailableHandleHint;
            ++mNextAvailableHandleHint; // Prepare for next use

            mTimerHandleToTimeoutTimeMap[timer.handle] = timeoutTime;
            auto iter = mTimeoutTimeToTimerMap.insert(TimeoutTimeToTimerMap::value_type(timeoutTime, timer));
            if(iter == mTimeoutTimeToTimerMap.begin())
            {
                needToWakeThread = true;
            }
        }

        if(needToWakeThread)
        {
            // wake up thread to adjust timeout
            mCondition.notify_one();
        }

        return timer.handle;
    }

    static void removeTimer(TimerScheduler::TimerHandle handle)
    {
        bool needToWakeThread(false);

        {
            std::lock_guard<std::mutex> lock(mMutex);

            if(mTimerHandleToTimeoutTimeMap.count(handle) > 0)
            {
                // Use the reverse mapping to get the timeout time. Iterate over the equal keys
                // to find the one with the right handle.
                const TimeoutTime& timeoutTime = mTimerHandleToTimeoutTimeMap[handle];
                const auto range = mTimeoutTimeToTimerMap.equal_range(timeoutTime);
                for(auto iter = range.first; iter != range.second; iter++)
                {
                    if(iter->second.handle == handle)
                    {
                        if(iter == mTimeoutTimeToTimerMap.begin())
                        {
                            needToWakeThread = true;
                        }

                        mTimeoutTimeToTimerMap.erase(iter);
                        break;
                    }
                }
            }
        }

        if(needToWakeThread)
        {
            // wake up thread to adjust timeout
            mCondition.notify_one();
        }
    }

private:
    static void timerThreadLoop();
    static void checkForTimeouts();
    static void waitForNextTimeout();

    struct Timer
    {
        TimerScheduler::TimerHandle handle;
        TimerScheduler::TimerCallback callback;
        std::chrono::milliseconds period;
    };

    using TimeoutTime = std::chrono::steady_clock::time_point;
    using TimeoutTimeToTimerMap = std::multimap<TimeoutTime, Timer>;
    using TimerHandleToTimeoutTimeMap = std::unordered_map<TimerScheduler::TimerHandle, TimeoutTime>;

    // Timer data:
    // Multimap for TimeoutTime -> Timer object
    static TimeoutTimeToTimerMap mTimeoutTimeToTimerMap;
    // Hash table for reverse lookup of TimerHandle -> TimeoutTime, for timer removal
    static TimerHandleToTimeoutTimeMap mTimerHandleToTimeoutTimeMap;
    // Hint for next available handle value (could be in use, so must check first)
    static TimerScheduler::TimerHandle mNextAvailableHandleHint;

    static std::condition_variable mCondition;

    static std::mutex mMutex;

    static std::thread mThread;
};

std::multimap<TimerSchedulerImpl::TimeoutTime, TimerSchedulerImpl::Timer> TimerSchedulerImpl::mTimeoutTimeToTimerMap;
std::unordered_map<TimerScheduler::TimerHandle, TimerSchedulerImpl::TimeoutTime> TimerSchedulerImpl::mTimerHandleToTimeoutTimeMap;
TimerScheduler::TimerHandle TimerSchedulerImpl::mNextAvailableHandleHint{1};
std::condition_variable TimerSchedulerImpl::mCondition;
std::mutex TimerSchedulerImpl::mMutex;
std::thread TimerSchedulerImpl::mThread;


void TimerScheduler::reserve(size_t anticipatedNumberOfTimers)
{
    TimerSchedulerImpl::reserve(anticipatedNumberOfTimers);
}

void TimerScheduler::run()
{
    TimerSchedulerImpl::run();
}

TimerScheduler::TimerHandle TimerScheduler::addTimer(const std::chrono::milliseconds& period, TimerCallback callback)
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
    // check for timeouts
    std::vector<Timer> timedOutTimers;
    {
        std::lock_guard<std::mutex> lock(mMutex);

        const auto now = std::chrono::steady_clock::now(); // get time AFTER mutex has been locked
        const auto iterFirst = mTimeoutTimeToTimerMap.begin();
        auto iter = iterFirst;
        for(; iter != mTimeoutTimeToTimerMap.end(); iter++)
        {
            if(iter->first <= now)
            {
                timedOutTimers.push_back(iter->second);
            }
            else
            {
                break;
            }
        }
        if(timedOutTimers.size() > 0)
        {
            mTimeoutTimeToTimerMap.erase(iterFirst, iter);
        }

        // re-insert timed out timers, reusing their handle
        for(const auto& timer : timedOutTimers)
        {
            TimeoutTime timeoutTime = now + timer.period;
            mTimeoutTimeToTimerMap.insert(TimeoutTimeToTimerMap::value_type(timeoutTime, timer));
            mTimerHandleToTimeoutTimeMap[timer.handle] = timeoutTime; // update the reverse mapping
        }
    }

    // call the callbacks
    for(const auto& timer : timedOutTimers)
    {
        timer.callback(timer.handle);
    }
}

void TimerSchedulerImpl::waitForNextTimeout()
{
    std::unique_lock<std::mutex> lock(mMutex);
    if(mTimeoutTimeToTimerMap.size() > 0)
    {
        // wait for next timeout to happen
        mCondition.wait_until(lock, mTimeoutTimeToTimerMap.begin()->first);
    }
    else
    {
        // If there are no timers, wait indefinitely (will wake up and reevaluate if a timer is scheduled).
        mCondition.wait(lock);
    }
}
