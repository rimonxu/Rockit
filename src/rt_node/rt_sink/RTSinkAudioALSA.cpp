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
 * Author: shika.zhou@rock-chips.com
 *   Date: 2019/2/21
 * Module: audio pcm data use alsa-lib output
 *
 */

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "RTSinkAudioALSA"

#ifdef DEBUG_FLAG
#undef DEBUG_FLAG
#endif
#define DEBUG_FLAG 0x0

#include "RTSinkAudioALSA.h"   // NOLINT
#include "rt_metadata.h"       // NOLINT
#include "RTMediaMetaKeys.h"   // NOLINT
#include "RTMediaBuffer.h"     // NOLINT
#include "rt_message.h"        // NOLINT

void* sink_audio_alsa_loop(void* ptrNode) {
    RTSinkAudioALSA* audiosink = reinterpret_cast<RTSinkAudioALSA*>(ptrNode);
    audiosink->runTask();
    return RT_NULL;
}

RTSinkAudioALSA::RTSinkAudioALSA()
        : mALSASinkCtx(RT_NULL),
          mQueueBuffer(RT_NULL),
          mPoolBuffer(RT_NULL),
          mCodecId(0),
          mProfile(0),
          mSize(0),
          mStart(RT_FALSE),
          mCountPull(0),
          mCountPush(0),
          mEventLooper(RT_NULL),
          mSampleRate(48000),
          mChannels(2),
          mDataSize(4096),
          mPlayStatus(PLAY_STOPPED),
          mCurPosition(0) {
    mThread = new RtThread(sink_audio_alsa_loop, reinterpret_cast<void*>(this));
    mThread->setName("SinkAlsa");
    mDeque = deque_create(10);
    RT_ASSERT(RT_NULL != mDeque);
    mVolManager = new ALSAVolumeManager();

    mLockBuffer = new RtMutex();
    RT_ASSERT(RT_NULL != mLockBuffer);
}

RTSinkAudioALSA::~RTSinkAudioALSA() {
    release();
}

RT_RET RTSinkAudioALSA::init(RtMetaData *metaData) {
    RT_ASSERT(RT_NULL != metaData);
    if (!mALSASinkCtx) {
        metaData->findInt32(kKeyACodecSampleRate, &mSampleRate);
        metaData->findInt32(kKeyACodecChannels, &mChannels);
        if (RT_OK == openAlsaSound()) {
            if (RT_OK != setAlsaSoundParams(metaData)) {
                usleepData(mSampleRate, mChannels, mDataSize);
                RT_LOGE("setAlsaSoundParams fail!");
            }
        } else {
            RT_LOGE("openAlsaSound fail!");
        }
    } else {
        INT32 tmpChannels = 0;
        INT32 tmpSamplerate = 0;
        metaData->findInt32(kKeyACodecSampleRate, &tmpSamplerate);
        metaData->findInt32(kKeyACodecChannels, &tmpChannels);
        RT_LOGD("tmpChannels = %d, tmpSamplerate =%d", tmpChannels, tmpSamplerate);
        if ((tmpChannels != mChannels) || (tmpSamplerate != mSampleRate)) {
            if (RT_OK != setAlsaSoundParams(metaData)) {
                usleepData(mSampleRate, mChannels, mDataSize);
                RT_LOGE("setAlsaSoundParams fail!");
            }
        }

        mChannels = tmpChannels;
        mSampleRate = tmpSamplerate;
    }
    return RT_OK;
}

RT_RET RTSinkAudioALSA::release() {
    onFlush();
    onStop();

    if (mDeque != NULL) {
        deque_destory(&mDeque);
    }

    rt_safe_delete(mThread);
    rt_safe_delete(mVolManager);
    rt_safe_delete(mLockBuffer);
    mCurPosition = 0;
    closeSoundCard();
    return RT_OK;
}

RT_RET RTSinkAudioALSA::pullBuffer(RTMediaBuffer** mediaBuf) {
    *mediaBuf = RT_NULL;
    RT_RET  err = RT_ERR_NULL_PTR;
    RT_DequeEntry entry = deque_pop(mDeque);

    if (RT_NULL != entry.data) {
        *mediaBuf = reinterpret_cast<RTMediaBuffer*>(entry.data);
        err = RT_OK;
    } else {
        err = RT_ERR_NULL_PTR;
    }

    return err;
}

RT_RET RTSinkAudioALSA::pushBuffer(RTMediaBuffer* mediaBuf) {
    mCountPush++;
    RT_RET  err = RT_ERR_NULL_PTR;
    RtMutex::RtAutolock autoLock(mLockBuffer);
    if (RT_NULL != mediaBuf) {
        err = deque_push_tail(mDeque, mediaBuf);
    }

    return err;
}

RT_RET RTSinkAudioALSA::runCmd(RT_NODE_CMD cmd, RtMetaData *metaData) {
    RT_RET err = RT_OK;

    switch (cmd) {
    case RT_NODE_CMD_INIT:
        err = this->init(metaData);
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
    default:
        RT_LOGE("unkown command: %d", cmd);
        err = RT_ERR_UNKNOWN;
        break;
    }

    return err;
}

RT_RET RTSinkAudioALSA::setEventLooper(RTMsgLooper* eventLooper) {
    mEventLooper = eventLooper;
    return RT_OK;
}

RtMetaData* RTSinkAudioALSA::queryFormat(RTPortType port) {
    return RT_NULL;
}

RTNodeStub* RTSinkAudioALSA::queryStub() {
    return &rt_sink_audio_alsa;
}

RT_RET  RTSinkAudioALSA::setVolume(int user_vol) {
    RT_LOGD("SetVolume user_vol = %d", user_vol);
    if (mVolManager) {
        mVolManager->setVolume(user_vol);
    } else {
        RT_LOGE("mVolManager is NULL");
    }
    return RT_OK;
}

INT32 RTSinkAudioALSA::getVolume() {
    int user_vol = 0;

    if (mVolManager) {
        user_vol = mVolManager->getVolume();
    } else {
        RT_LOGE("mVolManager is NULL");
    }

    RT_LOGD("GetVolume user_vol = %d", user_vol);
    return user_vol;
}

RT_RET RTSinkAudioALSA::setMute(RT_BOOL muted) {
    RT_LOGD("set Mute muted = %d", muted);
    if (mVolManager) {
        mVolManager->setMute(muted);
    } else {
        RT_LOGE("mVolManager is NULL");
    }
    return RT_OK;
}

RT_BOOL RTSinkAudioALSA::getMute() {
    RT_BOOL muted = RT_FALSE;

    if (mVolManager) {
        muted = mVolManager->getMute();
    } else {
        RT_LOGE("mVolManager is NULL");
    }

    RT_LOGD("GetMute muted = %d", muted);
    return muted;
}

RT_RET RTSinkAudioALSA::onStart() {
    RT_RET err = RT_OK;
    RT_LOGD("Audio Sink Thread... begin");
    if (THREAD_LOOP != mThread->getState()) {
        mThread->start();
    }
    mPlayStatus = PLAY_START;
    return err;
}

RT_RET RTSinkAudioALSA::onStop() {
    RT_RET err = RT_OK;
    mPlayStatus = PLAY_STOPPED;
    if (mThread) {
        mThread->requestInterruption();
        mThread->join();
    }
    onFlush();
    return err;
}

RT_RET RTSinkAudioALSA::onPause() {
    mPlayStatus = PLAY_PAUSED;
    return RT_OK;
}

RT_RET RTSinkAudioALSA::onFlush() {
    int i;
    RTMediaBuffer *mediaBuf = NULL;
    if (mDeque) {
        RtMutex::RtAutolock autoLock(mLockBuffer);
        UINT32 size = deque_size(mDeque);
        RT_LOGE("deque_size(mDeque) = %d", deque_size(mDeque));
        for (i = 0; i < size; i++) {
            pullBuffer(&mediaBuf);

            // @review: return buffer to media-buffer-pool
            if (RT_NULL != mediaBuf) {
                mediaBuf->release();
                mediaBuf = NULL;
            }
        }
    }
    mCurPosition = 0;
    return RT_OK;
}

RT_RET RTSinkAudioALSA::onReset() {
    mPlayStatus = PLAY_STOPPED;
    return RT_OK;
}

RT_RET RTSinkAudioALSA::openAlsaSound() {
    mALSASinkCtx = alsa_snd_create(WRITE_DEVICE_NAME);

    if (RT_NULL == mALSASinkCtx) {
        RT_LOGE("Fail to alsa_snd_create");
        return RT_ERR_NULL_PTR;
    }

    return RT_OK;
}

RT_RET RTSinkAudioALSA::setAlsaSoundParams(RtMetaData *metaData) {
    RT_RET err = RT_OK;
    //  try to config hw params of alsa.
    err = alsa_set_snd_hw_params(mALSASinkCtx, metaData);
    if (err == RT_ERR_INIT) {
        RT_LOGE("Failed to set HW parameters!");
        closeSoundCard();
        return err;
    }

    // try to config sw params of alsa.
    err = alsa_set_snd_sw_params(mALSASinkCtx);
    if (err == RT_ERR_INIT) {
        RT_LOGE("Failed to set SW parameters!");
        closeSoundCard();
        return err;
    }

    return RT_OK;
}

RT_RET RTSinkAudioALSA::closeSoundCard() {
    if (RT_NULL != mALSASinkCtx) {
        alsa_snd_destroy(mALSASinkCtx);
        mALSASinkCtx = RT_NULL;
    }
}

RT_VOID RTSinkAudioALSA::usleepData(INT32 samplerate, INT32 channels, INT32 bytes) {
    // @review pay special attention to the scope of the data type
    //         INT32_MAX =((int32_t)2147483647); UINT32_MAX =((uint32_t)4294967295)
    UINT64 duration = (bytes * 1000000llu) / (2*channels) / samplerate;
    RtTime::sleepUs(duration);
}

RT_RET RTSinkAudioALSA::runTask() {
    RTMediaBuffer *input = NULL;
    int ret;
    while (THREAD_LOOP == mThread->getState()) {
        RT_RET err = RT_OK;
        if (mPlayStatus == PLAY_PAUSED) {
            // RT_LOGD("%s,%d,now runtask is pause",__FUNCTION__,__LINE__);
            usleepData(mSampleRate, mChannels, mDataSize);
            mCurPosition = 0;
            continue;
        }
        if (!input) {
            RtMutex::RtAutolock autoLock(mLockBuffer);
            pullBuffer(&input);
        }

        if (!input) {
            if (mCurPosition != 0) {
                UINT64 now = RtTime::getNowTimeMs();
                if (now - mCurPosition > 2000) {
                    if (RT_NULL != mEventLooper) {
                        RT_LOGD("already 2s no data, post EOS message");
                        mCurPosition = 0;
                        RTMessage* eosMsg = new RTMessage(RT_MEDIA_PLAYBACK_COMPLETE, nullptr, nullptr);
                        mEventLooper->post(eosMsg);
                    }
                }
            }
            RtTime::sleepMs(5);
            continue;
        }

        if (mALSASinkCtx && input->getLength()) {
            ret = alsa_snd_write_data(mALSASinkCtx, reinterpret_cast<void *>(input->getData()), input->getLength());
            mCurPosition = RtTime::getNowTimeMs();

            if (ret != input->getLength()) {
                usleepData(mSampleRate, mChannels, mDataSize);
            }
        }

        INT32 eos = 0;
        input->getMetaData()->findInt32(kKeyFrameEOS, &eos);

        // @review: return buffer to media-buffer-pool
        if (RT_NULL != input) {
            input->release();
            input = NULL;
        }

        if (eos && (RT_NULL != mEventLooper)) {
            RT_LOGD("render EOS Flag, post EOS message");
            mCurPosition = 0;
            RTMessage* eosMsg = new RTMessage(RT_MEDIA_PLAYBACK_COMPLETE, nullptr, nullptr);
            mEventLooper->post(eosMsg);
        }

        input = NULL;
    }
    return RT_OK;
}

static RTNode* createSinkAudioALSA() {
    return new RTSinkAudioALSA();
}

struct RTNodeStub rt_sink_audio_alsa {
    .mCreateNode   = createSinkAudioALSA,
    .mNodeType     = RT_NODE_TYPE_SINK,
    .mUsePool      = RT_TRUE,
    .mNodeName     = "rt_sink_audio_alsa",
    .mNodeRole     = "audio",
    .mNodeVersion  = "v1.0",
};
