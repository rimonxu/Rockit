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
* author: Hery.Xu@rock-chips.com
 *   date: 20181102
 * author: martin.cheng@rock-chips.com
 *   date: 20181126
 */


#include "rt_message.h"      // NOLINT
#include "rt_msg_handler.h"  // NOLINT
#include "rt_msg_looper.h"   // NOLINT
#include "rt_string_utils.h" // NOLINT
#include <string>            // NOLINT

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "rt_message"

RTMessage::RTMessage() {
    rt_memset(&mData, 0, sizeof(struct RTMsgData));
    mSync         = RT_FALSE;
    mDoneListener = RT_FALSE;
}

RTMessage::RTMessage(UINT32 what, RT_PTR data, struct RTMsgHandler* handler) {
    rt_memset(&mData, 0, sizeof(struct RTMsgData));
    this->setWhat(what);
    this->setData(data);
    this->setTarget(handler);
    mSync         = RT_FALSE;
    mDoneListener = RT_FALSE;
}

RTMessage::RTMessage(UINT32 what, UINT32 arg32, UINT64 arg64, struct RTMsgHandler* handler /* = RT_NULL */) {
    rt_memset(&mData, 0, sizeof(struct RTMsgData));
    this->setWhat(what);
    this->mData.mArgU32 = arg32;
    this->mData.mArgU64 = arg64;
    this->setTarget(handler);
    mSync         = RT_FALSE;
    mDoneListener = RT_FALSE;
}

void RTMessage::setWhat(UINT32 what) {
    mData.mWhat = what;
}

const UINT32 RTMessage::getWhat() {
    return mData.mWhat;
}

void RTMessage::setData(RT_PTR data) {
    mData.mArgPtr = data;
}

const RT_PTR RTMessage::getData() {
    return mData.mArgPtr;
}

void RTMessage::setWhenUs(UINT64 when) {
    mData.mWhenUs = when;
}

const UINT64 RTMessage::getWhenUs() {
    return mData.mWhenUs;
}

void RTMessage::setTarget(struct RTMsgHandler* handler) {
    mHandler = handler;
}

// performs message deep copy
RTMessage*  RTMessage::dup() {
    RTMessage* msg = new RTMessage();
    msg->setWhat(this->getWhat());
    msg->setWhenUs(this->getWhenUs());
    msg->mData.mArgU32 = this->mData.mArgU32;
    msg->mData.mArgU64 = this->mData.mArgU64;
    msg->mData.mArgPtr = this->mData.mArgPtr;
    return msg;
}

const char* RTMessage::toString() {
#if TODO_FLAG
    buffer = rt_str_sprintf("RTMessage(what=%d, arg_s32=%d, arg_u32=%d, arg_ptr=%p)",
                                     mWhat,
                                     mData.mArgS32, mData.mArgU32,
                                     mData.mArgU64, mData.mArgPtr);
    return rt_str_to_char(buffer);
#else
    std::string buffer("RTMessage");
    return buffer.c_str();
#endif
    return RT_NULL;
}

RT_RET RTMessage::deliver() {
    if (RT_NULL == mHandler) {
        RT_LOGE("Failed to deliver message as target handler(%p) is gone!", mHandler);
        return RT_ERR_BAD;
    }

    return mHandler->deliverMessage(this);
}
