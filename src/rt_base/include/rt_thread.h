/*
 * Copyright 2018 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * author: martin.cheng@rock-chips.com
 *   date: 20180719
 */

#include "rt_header.h" // NOLINT

#ifndef SRC_RT_BASE_INCLUDE_RT_THREAD_H_
#define SRC_RT_BASE_INCLUDE_RT_THREAD_H_

// RtTaskSlot: atomic task, short time consumption
class RtRunnable {
 public:
    virtual ~RtRunnable() {}
    virtual void run(void* args) = 0;
};

typedef enum {
    THREAD_IDLE  = 0,
    THREAD_LOOP,
    THREAD_EXIT,
    THREAD_MAX,
} ThreadState;

class RtThread {
 public:
    // RtTaskSlot: atomic task, short time consumption
    typedef void* (*RtTaskSlot)(void*);

    explicit RtThread(RtTaskSlot  taskslot, void* data = NULL);
    explicit RtThread(RtRunnable* runnable, void* data = NULL);

    /**
     * Non-virtual, do not subclass.
     */
    ~RtThread();

    /**
     * Starts the thread. Returns false if the thread could not be started.
     */
    RT_BOOL start();

    /**
     * Set and Get Thread Name.
     */
    void        setName(const char* name);
    const char* getName();
    INT32       getState();

    /**
     * Waits for the thread to finish.
     * If the thread has not started, returns immediately.
     */
    void join();
    void requestInterruption();

 public:
    static INT32 getThreadID();

 public:
    void                *mData;
    RtMutex             *mLock;
};

#endif  // SRC_RT_BASE_INCLUDE_RT_THREAD_H_
