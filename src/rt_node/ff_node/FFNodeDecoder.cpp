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
 * Author: rimon.xu@rock-chips.com
 *   Date: 2018/12/11
 * Module: use ffmpeg as decoder
 *
 * author: martin.cheng@rock-chips.com
 *   Date: 2019/01/04
 */

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "FFNodeDecoder"

#ifdef DEBUG_FLAG
#undef DEBUG_FLAG
#endif
#define DEBUG_FLAG 0x0

#include "FFNodeDecoder.h"          // NOLINT
#include "RTObjectPool.h"           // NOLINT
#include "FFMPEGAdapter.h"          // NOLINT
#include "rt_metadata.h"            // NOLINT
#include "RTMediaMetaKeys.h"        // NOLINT
#include "rt_thread.h"              // NOLINT
#include "rt_dequeue.h"             // NOLINT
#include "RTMediaBuffer.h"          // NOLINT
#include "FFAdapterCodec.h"         // NOLINT
#include "rt_message.h"             // NOLINT
#include "RTAllocatorStore.h"       // NOLINT
#include "RTAllocatorBase.h"        // NOLINT

#define MAX_INPUT_BUFFER_COUNT      30
#define MAX_OUTPUT_BUFFER_COUNT     8

void* ff_codec_loop(void* ptr_node) {
    FFNodeDecoder* node = reinterpret_cast<FFNodeDecoder*>(ptr_node);
    node->runTask();
    return RT_NULL;
}

RTObject *allocFFInputBuffer(void *arg) {
    return new RTMediaBuffer(NULL, 0);
}

FFNodeDecoder::FFNodeDecoder()
        : mFFCodec(RT_NULL),
          mPacketPool(RT_NULL),
          mFramePool(RT_NULL),
          mLinearAllocator(RT_NULL),
          mEventLooper(RT_NULL),
          mMetaInput(RT_NULL),
          mMetaOutput(RT_NULL),
          mTrackType(RTTRACK_TYPE_UNKNOWN),
          mStarted(RT_FALSE),
          mCountPull(0),
          mCountPush(0),
          mUsePool(RT_FALSE),
          mByPass(RT_FALSE) {
    mProcThread = new RtThread(ff_codec_loop, reinterpret_cast<void*>(this));
    mProcThread->setName("FFDecoder");

    mPacketQ = deque_create();
    RT_ASSERT(RT_NULL != mPacketQ);

    mFrameQ = deque_create();
    RT_ASSERT(RT_NULL != mFrameQ);

    mLockPacketQ = new RtMutex();
    RT_ASSERT(RT_NULL != mLockPacketQ);

    mLockFrameQ = new RtMutex();
    RT_ASSERT(RT_NULL != mLockFrameQ);

    mTrackParms       = rt_malloc(RTTrackParms);
}

FFNodeDecoder::~FFNodeDecoder() {
    onFlush();

    release();
    rt_safe_free(mTrackParms);
    rt_safe_delete(mLockPacketQ);
    rt_safe_delete(mLockFrameQ);
    deque_destory(&mPacketQ);
    deque_destory(&mFrameQ);
}

RT_RET FFNodeDecoder::init(RtMetaData *metadata) {
    RT_LOGD("call, init");

    if (!metadata->findInt32(kKeyCodecType, reinterpret_cast<INT32 *>(&mTrackType))) {
        RT_LOGE("track type is unset!!");
        return RT_ERR_UNKNOWN;
    }
    metadata->findInt32(kKeyCodecByePass, reinterpret_cast<INT32 *>(&mByPass));
    RT_LOGD("FFNodeDecoder::init bypass  %d", mByPass);

    mFFCodec = fa_decode_create(metadata, mTrackType);
    if (!mFFCodec) {
        RT_LOGE("fa_video_decode_open failed");
        return RT_ERR_UNKNOWN;
    }

    // @Best Practice: mMetaInput is created by dexmuer, but delete by decoder
    mMetaInput = metadata;
    rt_medatdata_goto_trackpar(metadata, mTrackParms);

    // @Best Practice: mMetaOutput is created and deleted by decoder
    mMetaOutput = new RtMetaData;
    mMetaOutput->clear();
    switch (mTrackParms->mCodecType) {
      case RTTRACK_TYPE_VIDEO:
        mMetaOutput->setInt32(kKeyCodecID,  mTrackParms->mCodecID);
        mMetaOutput->setInt32(kKeyFrameW,   mTrackParms->mVideoWidth);
        mMetaOutput->setInt32(kKeyFrameH,   mTrackParms->mVideoHeight);
        break;
      case RTTRACK_TYPE_AUDIO:
        mMetaOutput->setInt32(kKeyCodecID,          mTrackParms->mCodecID);
        mMetaOutput->setInt32(kKeyACodecChannels,   mTrackParms->mAudioChannels);
        mMetaOutput->setInt32(kKeyACodecSampleRate, mTrackParms->mAudioSampleRate);
        break;
      default:
        break;
    }

    // TODO(frame count): max frame count should set by config.
    mFramePool  = new RTMediaBufferPool(MAX_OUTPUT_BUFFER_COUNT);
    mPacketPool = new RTMediaBufferPool(MAX_INPUT_BUFFER_COUNT);
    if (!metadata->findPointer(kKeyMemAllocator, (void **)&mLinearAllocator)) {
        RT_LOGE("Not find memory allocator from config!");
        return RT_ERR_UNKNOWN;
    }
    RT_ASSERT(RT_NULL != mLinearAllocator);

    allocateBuffersOnPort(RT_PORT_INPUT);
    allocateBuffersOnPort(RT_PORT_OUTPUT);

    return RT_OK;
}

RT_RET FFNodeDecoder::allocateBuffersOnPort(RTPortType port) {
    UINT32 i = 0;
    RT_RET ret = RT_OK;
    switch (port) {
        case RT_PORT_INPUT: {
            RTMediaBuffer *buffer[MAX_INPUT_BUFFER_COUNT];
            for (i = 0; i < MAX_INPUT_BUFFER_COUNT; i++) {
                buffer[i] = new RTMediaBuffer(RT_NULL, 0);
                mPacketPool->registerBuffer(buffer[i]);
            }
        }
            break;
        case RT_PORT_OUTPUT: {
            RTMediaBuffer *buffer[MAX_OUTPUT_BUFFER_COUNT];
            for (i = 0; i < MAX_OUTPUT_BUFFER_COUNT; i++) {
                INT32 buf_size = 0;
                if (mTrackType == RTTRACK_TYPE_VIDEO) {
                    buf_size = mTrackParms->mVideoWidth * mTrackParms->mVideoHeight * 3 / 2;
                } else if (mTrackType == RTTRACK_TYPE_AUDIO) {
                    buf_size = 1024 * 4 * 10;
                } else {
                    RT_LOGE("unknown track type: %d", mTrackType);
                    return RT_ERR_UNKNOWN;
                }
                ret = mLinearAllocator->newBuffer(buf_size, &(buffer[i]));
                if (RT_OK != ret) {
                    RT_LOGE("allocator new buffer failed");
                    return RT_ERR_UNKNOWN;
                }
            }
            for (i = 0; i < MAX_OUTPUT_BUFFER_COUNT; i++) {
                mFramePool->registerBuffer(buffer[i]);
            }
        }
            break;
        default:
            RT_LOGE("unknown port! port: %d", port);
            return RT_ERR_UNKNOWN;
    }

    return RT_OK;
}

RT_RET FFNodeDecoder::release() {
    fa_video_decode_destroy(&mFFCodec);

    rt_safe_delete(mPacketPool);
    rt_safe_delete(mFramePool);
    rt_safe_delete(mMetaInput);
    rt_safe_delete(mMetaOutput);

    // thread release
    rt_safe_delete(mProcThread);

    return RT_OK;
}

RT_RET FFNodeDecoder::dequeBuffer(RTMediaBuffer **data, RTPortType port) {
    RT_RET ret = RT_OK;
    RT_DequeEntry entry;
    switch (port) {
        case RT_PORT_INPUT:
            if (mPacketPool != RT_NULL) {
                mPacketPool->acquireBuffer(data, RT_TRUE);
            }
            if (*data) {
                (*data)->getMetaData()->setInt32(kKeyCodecType, mTrackType);
            } else {
                // RT_LOGD("FFNodeDecoder::dequeBuffer NULL");
                ret   = RT_ERR_LIST_EMPTY;
            }
            break;
        case RT_PORT_OUTPUT: {
            RtMutex::RtAutolock autoLock(mLockFrameQ);
            entry = deque_pop(mFrameQ);
            if (entry.data) {
                *data = reinterpret_cast<RTMediaBuffer *>(entry.data);
                (*data)->getMetaData()->setInt32(kKeyCodecType, mTrackType);
            } else {
                ret = RT_ERR_LIST_EMPTY;
            }
        }
            break;
        default:
            RT_LOGE("unknown port! port: %d", port);
            return RT_ERR_UNKNOWN;
    }

    return ret;
}

RT_RET FFNodeDecoder::queueBuffer(RTMediaBuffer* data, RTPortType port) {
    RT_RET ret = RT_OK;
    switch (port) {
        case RT_PORT_INPUT:
            if (data) {
                RtMutex::RtAutolock autoLock(mLockPacketQ);
                deque_push(mPacketQ, reinterpret_cast<void *>(data));
            } else {
                RT_LOGE("data is NULL!");
                ret = RT_ERR_UNKNOWN;
            }
            break;
        case RT_PORT_OUTPUT:
            data->release();
            break;
        default:
            RT_LOGE("unknown port! port: %d", port);
            return RT_ERR_UNKNOWN;
    }

    return ret;
}

RT_RET FFNodeDecoder::pullBuffer(RTMediaBuffer** data) {
    mCountPull++;
    return dequeBuffer(data, RT_PORT_OUTPUT);
}

RT_RET FFNodeDecoder::pushBuffer(RTMediaBuffer*  data) {
    mCountPush++;
    return queueBuffer(data, RT_PORT_INPUT);
}

RT_RET FFNodeDecoder::runCmd(RT_NODE_CMD cmd, RtMetaData *metadata) {
    RT_RET err = RT_OK;
    switch (cmd) {
    case RT_NODE_CMD_INIT:
        err = this->init(metadata);
        break;
    case RT_NODE_CMD_START:
        err = this->onStart();
        break;
    case RT_NODE_CMD_STOP:
        err = this->onStop();
        break;
    case RT_NODE_CMD_FLUSH:
        err = this->onFlush();
        break;
    case RT_NODE_CMD_PAUSE:
        err = this->onPause();
        break;
    case RT_NODE_CMD_RESET:
        err = this->onReset();
        break;
    case RT_NODE_CMD_PREPARE:
        err = this->onPrepare();
    default:
        RT_LOGE("unkown command: %d", cmd);
        err = RT_ERR_UNKNOWN;
        break;
    }

    return err;
}

RT_RET FFNodeDecoder::setEventLooper(RTMsgLooper* eventLooper) {
    mEventLooper = eventLooper;
    return RT_OK;
}

void FFNodeDecoder::signalError(UINT32 what) {
    RTMessage *msg = new RTMessage();
    msg->setWhat(what);
    mEventLooper->post(msg, 0ll);
}

RtMetaData* FFNodeDecoder::queryFormat(RTPortType port) {
    RtMetaData *nMeta = RT_NULL;
    switch (port) {
    case RT_PORT_INPUT:
        nMeta = mMetaInput;
        break;
    case RT_PORT_OUTPUT:
        nMeta = mMetaOutput;
        break;
    default:
        break;
    }
    return nMeta;
}

RTNodeStub* FFNodeDecoder::queryStub() {
    return &ff_node_decoder;
}

RT_RET FFNodeDecoder::runTask() {
    RTMediaBuffer *input  = RT_NULL;
    RTMediaBuffer *output = RT_NULL;
    while (THREAD_LOOP == mProcThread->getState()) {
        RT_RET err = RT_OK;

        if (!mStarted) {
            if (input) {
                input->release();
                input = RT_NULL;
            }
            RtTime::sleepMs(5);
            continue;
        }

        if (!input) {
            RtMutex::RtAutolock autoLock(mLockPacketQ);
            RT_DequeEntry entry = deque_pop(mPacketQ);
            if (entry.data) {
                input = reinterpret_cast<RTMediaBuffer *>(entry.data);
            }
        }
        if (!output) {
            mFramePool->acquireBuffer(&output, RT_TRUE);
        }

        if (!input || !output || !mStarted) {
            // when seek to target time, input packet may be old time.
            RtTime::sleepMs(5);
            continue;
        }

       if (mByPass == RT_TRUE) {
            if (input->getSize() > 0) {
                if (output->getSize() < input->getSize()) {
                   memcpy(output->getData(), input->getData(), output->getSize());
                } else {
                   memcpy(output->getData(), input->getData(), input->getSize());
                }
                output->setRange(0, input->getSize());
            } else {
                INT32 eos = 0;
                input->getMetaData()->findInt32(kKeyFrameEOS, &eos);
                if (eos) {
                    output->getMetaData()->setInt32(kKeyFrameEOS, 1);
                }
                output->setRange(0, 0);
            }
            RT_LOGD("FFNodeDecoder::runTask output = %p, output->getData() = %p", output, output->getData());
            input->release();
            input = NULL;
            output->getMetaData()->setInt32(kKeyACodecSampleRate, 24000);
            output->getMetaData()->setInt32(kKeyACodecChannels, 1);
            output->setStatus(RT_MEDIA_BUFFER_STATUS_READY);
            RtMutex::RtAutolock autoLock(mLockFrameQ);
            RT_LOGD("deque_size(mFrameQ) = %d", deque_size(mFrameQ));
            deque_push(mFrameQ, output);
            output = NULL;
        } else {
            RT_LOGD_IF(DEBUG_FLAG, "input and output ready, go to decode!");
            err = fa_decode_send_packet(mFFCodec, input);
            if (err) {
                if (err == RT_ERR_TIMEOUT) {
                    RtTime::sleepMs(5);
                }
                continue;
            } else {
                input->release();
                input = NULL;
            }
            err = fa_decode_get_frame(mFFCodec, output);
            if (err) {
                continue;
            } else {
                if (output->getStatus() == RT_MEDIA_BUFFER_STATUS_READY) {
                    RtMutex::RtAutolock autoLock(mLockFrameQ);
                    deque_push(mFrameQ, output);
                    output = NULL;
                }
            }
        }
    }

    if (input) {
        input->release();
        input = RT_NULL;
    }
    if (output) {
        output->release();
        output = RT_NULL;
    }
    RT_LOGD("exit ffmpeg decode run task");
    return RT_OK;
}

RT_RET FFNodeDecoder::onStart() {
    RT_RET err = RT_OK;

    mFramePool->start();
    mPacketPool->start();
    mStarted = RT_TRUE;
    return err;
}

RT_RET FFNodeDecoder::onPause() {
    RT_LOGD("call, pause");
    mStarted = RT_FALSE;
    return RT_OK;
}

RT_RET FFNodeDecoder::onStop() {
    RT_RET err = RT_OK;
    mStarted = RT_FALSE;
    mFramePool->stop();
    mPacketPool->stop();
    mProcThread->requestInterruption();
    mProcThread->join();
    onFlush();
    return err;
}

RT_RET FFNodeDecoder::onReset() {
    RT_LOGD("call, reset and flush in decoder");
    return onFlush();
}

RT_RET FFNodeDecoder::onPrepare() {
    RT_LOGD("call, prepare");
    mProcThread->start();
    return RT_OK;
}

RT_RET FFNodeDecoder::onFlush() {
    RT_LOGD("call, flush");
    RT_RET ret = RT_OK;
    mStarted = RT_FALSE;
    while (deque_size(mPacketQ) > 0) {
        RtMutex::RtAutolock autoLock(mLockPacketQ);
        RTMediaBuffer *pkt  = RT_NULL;
        RT_DequeEntry entry = deque_pop(mPacketQ);
        if (entry.data) {
            pkt = reinterpret_cast<RTMediaBuffer *>(entry.data);
        }
        if (pkt) {
            pkt->release();
        }
    }
    while (deque_size(mFrameQ) > 0) {
        RtMutex::RtAutolock autoLock(mLockFrameQ);
        RTMediaBuffer *frame = RT_NULL;
        RT_DequeEntry entry  = deque_pop(mFrameQ);
        if (entry.data) {
            frame = reinterpret_cast<RTMediaBuffer *>(entry.data);
        }
        if (frame) {
            frame->release();
        }
    }

    return ret;
}

static RTNode* createFFDecoder() {
    return new FFNodeDecoder();
}

struct RTNodeStub ff_node_decoder {
    .mCreateNode     = createFFDecoder,
    .mNodeType       = RT_NODE_TYPE_DECODER,
    .mUsePool        = RT_TRUE,
    .mNodeName       = "ff_node_decoder",
    .mNodeVersion    = "v1.0",
};

