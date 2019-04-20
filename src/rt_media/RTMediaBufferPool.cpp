/*
 * Copyright 2019 Rockchip Electronics Co. LTD
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
 * author: rimon.xu@rock-chips.com
 *   date: 2019/03/07
 * module: RTMediaBuffer Pool
 */

#include "RTMediaBufferPool.h"      // NOLINT
#include "rt_array_list.h"          // NOLINT
#include "RTMediaBuffer.h"          // NOLINT

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "MediaBufferPool"

struct RTMediaBufferPool::RTBufferList {
    RtMutex        *mLock;
    UINT32          mMaxBufferCount;
    UINT32          mBufferSize;
    RtCondition    *mCondition;
    RtArrayList    *mBuffers;
};

RTMediaBufferPool::RTMediaBufferPool(UINT32 max_buffer_count)
    : mBufferList(new RTBufferList()) {
    mBufferList->mMaxBufferCount = max_buffer_count;

    RT_ASSERT(RT_NULL != mBufferList->mBuffers);

    mBufferList->mLock = new RtMutex();
    RT_ASSERT(RT_NULL != mBufferList->mLock);

    mBufferList->mCondition = new RtCondition();
    RT_ASSERT(RT_NULL != mBufferList->mCondition);

    mBufferList->mBuffers = array_list_create_with_capacity(max_buffer_count);
    RT_ASSERT(RT_NULL != mBufferList->mBuffers);
    mBufferList->mMaxBufferCount = max_buffer_count;
    mBufferList->mBufferSize = RT_MaxU32;
}

RTMediaBufferPool::RTMediaBufferPool(UINT32 max_buffer_count, UINT32 buffer_size)
    : mBufferList(new RTBufferList()),
      mRunning(RT_FALSE) {
    RT_ASSERT(RT_NULL != mBufferList->mBuffers);

    mBufferList->mLock = new RtMutex();
    RT_ASSERT(RT_NULL != mBufferList->mLock);

    mBufferList->mCondition = new RtCondition();
    RT_ASSERT(RT_NULL != mBufferList->mCondition);

    mBufferList->mBuffers = array_list_create_with_capacity(max_buffer_count);
    RT_ASSERT(RT_NULL != mBufferList->mBuffers);
    mBufferList->mMaxBufferCount = max_buffer_count;
    mBufferList->mBufferSize = buffer_size;
}

RTMediaBufferPool::~RTMediaBufferPool() {
    releaseAllBuffers();

    if (RT_NULL != mBufferList->mBuffers) {
        array_list_destroy(mBufferList->mBuffers);
        mBufferList->mBuffers = RT_NULL;
    }

    rt_safe_delete(mBufferList->mLock);
    rt_safe_delete(mBufferList->mCondition);
    rt_safe_delete(mBufferList);
}

RT_RET RTMediaBufferPool::registerBuffer(RTMediaBuffer *buffer) {
    RtMutex::RtAutolock autoLock(mBufferList->mLock);
    UINT32 list_size = array_list_get_size(mBufferList->mBuffers);
    if (list_size < mBufferList->mMaxBufferCount) {
        buffer->setObserver(this);
        array_list_add(mBufferList->mBuffers, reinterpret_cast<void *>(buffer));
    } else {
        RT_LOGE("buffer list is full! size: %d", list_size);
        return RT_ERR_LIST_FULL;
    }

    return RT_OK;
}

RT_BOOL RTMediaBufferPool::hasBuffer() {
    for (UINT32 idx = 0; idx < array_list_get_size(mBufferList->mBuffers); idx++) {
        RTMediaBuffer *buffer = reinterpret_cast<RTMediaBuffer *>
                                    (array_list_get_data(mBufferList->mBuffers, idx));
        if (buffer && buffer->refsCount() == 0) {
            return RT_TRUE;
        }
    }
    return RT_FALSE;
}

RT_RET RTMediaBufferPool::start() {
    mRunning = RT_TRUE;
}
RT_RET RTMediaBufferPool::stop() {
    mRunning = RT_FALSE;
    signalBufferReturned(RT_NULL);
}

RT_RET RTMediaBufferPool::acquireBuffer(
        RTMediaBuffer **out,
        RT_BOOL block,
        UINT32 request_size) {
    RtMutex::RtAutolock autoLock(mBufferList->mLock);

    while (mRunning) {
        *out = RT_NULL;
        RTMediaBuffer *buffer = RT_NULL;
        RTMediaBuffer *it = RT_NULL;
        INT32 count = array_list_get_size(mBufferList->mBuffers);
        if (count == 0) {
            RT_LOGE("pool is empty! no buffer acquire.");
            return RT_ERR_LIST_EMPTY;
        }
        for (UINT32 idx = 0; idx < count; idx++) {
            it = reinterpret_cast<RTMediaBuffer *>
                     (array_list_get_data(mBufferList->mBuffers, idx));
            if (it && it->refsCount() == 0
                  && it->getSize() >= request_size) {
                buffer = it;
                break;
            }
        }

        if (buffer != RT_NULL) {
            buffer->addRefs();
            buffer->reset();
            *out = buffer;
            return RT_OK;
        }

        if (!block) {
            RT_LOGE("not found avail buffer, and unblock");
            *out = RT_NULL;
            return RT_ERR_NULL_PTR;
        }

        mBufferList->mCondition->wait(mBufferList->mLock);
    }
}

void RTMediaBufferPool::signalBufferReturned(RTMediaBuffer *buffer) {
    RtMutex::RtAutolock autoLock(mBufferList->mLock);
    mBufferList->mCondition->signal();
}

RT_RET RTMediaBufferPool::releaseAllBuffers() {
    RT_RET          ret = RT_OK;
    RtMutex::RtAutolock autoLock(mBufferList->mLock);
    UINT32 size = array_list_get_size(mBufferList->mBuffers);
    for (UINT32 idx = 0; idx < size; idx++) {
        RTMediaBuffer *buffer = reinterpret_cast<RTMediaBuffer *>
                                    (array_list_get_data(mBufferList->mBuffers, 0));
        if (buffer->refsCount() != 0) {
            RT_LOGD("has buffer still in used, buffer: %p, index: %d, refsCount: %d",
                     buffer, idx, buffer->refsCount());
        }
        buffer->setObserver(RT_NULL);
        buffer->release();
        array_list_remove(mBufferList->mBuffers, reinterpret_cast<void *>(buffer));
    }

    return ret;
}
