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
#pragma once

#include <chrono>
#include <functional>
#include <cstdint>

class TimerScheduler
{
public:
    TimerScheduler() = delete;
    TimerScheduler(const TimerScheduler&) = delete;
    TimerScheduler& operator=(const TimerScheduler &) = delete;
    TimerScheduler(TimerScheduler &&) = delete;
    TimerScheduler & operator=(TimerScheduler &&) = delete;

    using TimerHandle = int32_t;
    using TimerCallback = std::function<void(TimerHandle handle)>;

    // Call to set allocation for timer data storage; only has an affect if not the scheduler is not running.
    static void reserve(size_t anticipatedNumberOfTimers);

    // Call to start the scheduler.
    static void run();

    // Call to stop the scheduler. This will also remove all timers.
    // This must be called from a thread context other than the scheduler (if this is called from
    // within a timeout callback it will have no affect).
    static void reset();

    // Add a timer
    static TimerHandle addTimer(const std::chrono::milliseconds& period, TimerCallback callback);

    // Remove a timer
    static void removeTimer(TimerHandle handle);
};
