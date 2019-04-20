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
 * Author: martin.cheng@rock-chips.com
 *   Date: 2019/03/10
 *   Task: build player streamline with node bus.
 */

#include "RTNDKNodePlayer.h"
#include "RTNode.h"           // NOLINT
#include "RTNodeDemuxer.h"    // NOLINT
#include "RTNodeAudioSink.h"  // NOLINT
#include "rt_header.h"        // NOLINT
#include "rt_hash_table.h"    // NOLINT
#include "rt_array_list.h"    // NOLINT
#include "rt_message.h"       // NOLINT
#include "rt_msg_handler.h"   // NOLINT
#include "rt_msg_looper.h"    // NOLINT

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "RTNDKNodePlayer"
#ifdef DEBUG_FLAG
#undef DEBUG_FLAG
#endif
#define DEBUG_FLAG 0x0

struct NodePlayerContext {
    RTNodeBus*          mNodeBus;
    RTMediaDirector*    mDirector;
    // thread used for data transfer between plugins
    RtThread*           mDeliverThread;
    struct RTMsgLooper* mLooper;
    UINT32              mState;
    RTSeekType          mSeekFlag;
    RT_BOOL             mLooping;
    RtMetaData         *mCmdOptions;
    RtMutex            *mNodeLock;
    INT64               mSaveSeekTimeUs;
    INT64               mWantSeekTimeUs;
    INT64               mCurTimeUs;
    INT64               mDuration;
    RTProtocolType      mProtocolType;
    RTPlayerListener*   mListener;
    RT_CALLBACK_T       mRT_Callback;
    INT32               mRT_Callback_Type;
    void *              mRT_Callback_Data;
};

void* thread_deliver_proc(void* pNodePlayer) {
    RTNDKNodePlayer* pPlayer = reinterpret_cast<RTNDKNodePlayer*>(pNodePlayer);
    if (RT_NULL != pPlayer) {
        // @TODO data transferring between plugins
        // @THIS data transferring for audio player
        pPlayer->startAudioPlayerProc();
    }
    return RT_NULL;
}

RTNDKNodePlayer::RTNDKNodePlayer() {
    mPlayerCtx = rt_malloc(NodePlayerContext);
    rt_memset(mPlayerCtx, 0, sizeof(NodePlayerContext));

    // node-bus manages node plugins
    mNodeBus = new RTNodeBus();

    // Message Queue Mechanism
    mPlayerCtx->mLooper  = new RTMsgLooper();
    mPlayerCtx->mLooper->setHandler(this);  // message handler is player

    // param config and performance collection
    mPlayerCtx->mDirector = new RTMediaDirector();

    // thread used for data transfer between plugins
    mPlayerCtx->mDeliverThread = NULL;
    mPlayerCtx->mRT_Callback   = NULL;
    mPlayerCtx->mLooping       = RT_FALSE;
    mPlayerCtx->mProtocolType  = RT_PROTOCOL_NONE;
    mPlayerCtx->mCmdOptions    = new RtMetaData();
    mPlayerCtx->mNodeLock      = new RtMutex();

    init();

    setCurState(RT_STATE_IDLE);
}

RTNDKNodePlayer::~RTNDKNodePlayer() {
    this->release();
    rt_safe_free(mNodeBus);
    RT_LOGD("done, ~RTNDKNodePlayer()");
}

RT_RET RTNDKNodePlayer::release() {
    RT_RET err = checkRuntime("release");
    if (RT_OK != err) {
        return err;
    }

    // @review: release resources in player context
    mPlayerCtx->mLooper->stop();
    rt_safe_delete(mPlayerCtx->mLooper);
    rt_safe_delete(mPlayerCtx->mDirector);
    rt_safe_delete(mPlayerCtx->mCmdOptions);
    rt_safe_delete(mPlayerCtx->mNodeLock);
    rt_safe_free(mPlayerCtx);

    // @review: release node bus
    rt_safe_delete(mNodeBus);

    return err;
}

RT_RET RTNDKNodePlayer::init() {
    RT_RET err = checkRuntime("init");
    if (RT_OK != err) {
        return err;
    }

    if (RT_NULL != mPlayerCtx->mLooper) {
        mPlayerCtx->mLooper->setName("MsgQueue");
        mPlayerCtx->mLooper->start();
    }
    return err;
}

RT_RET RTNDKNodePlayer::reset() {
    RT_RET err = checkRuntime("reset");
    if (RT_OK != err) {
        return err;
    }

    UINT32 curState = this->getCurState();
    if (RT_STATE_IDLE == curState) {
        RTMediaUtil::dumpStateError(curState, __FUNCTION__);
        return RT_OK;
    }
    if (RT_STATE_STOPPED != curState) {
        RT_LOGE("Need stop() before reset(), state(%d)", curState);
        this->stop();
    }

    // shutdown thread of data delivering between plugins
    if (RT_NULL != mPlayerCtx->mDeliverThread) {
        mPlayerCtx->mDeliverThread->requestInterruption();
        mPlayerCtx->mDeliverThread->join();
        rt_safe_delete(mPlayerCtx->mDeliverThread);
        mPlayerCtx->mDeliverThread = RT_NULL;
    }

    // nodebus be operated by multithread
    RtMutex::RtAutolock autoLock(mPlayerCtx->mNodeLock);

    // @TODO: do reset player
    mNodeBus->excuteCommand(RT_NODE_CMD_RESET);

    // release all [RTNodes] in node_bus;
    // but NO NEED to release [NodeStub].
    err = mNodeBus->releaseNodes();
    this->setCurState(RT_STATE_IDLE);
    return err;
}

RT_RET RTNDKNodePlayer::setDataSource(RTMediaUri *mediaUri) {
    RT_RET err = checkRuntime("setDataSource");
    if (RT_OK != err) {
        return err;
    }

    // nodebus be operated by multithread
    RtMutex::RtAutolock autoLock(mPlayerCtx->mNodeLock);

    UINT32 curState = getCurState();
    switch (curState) {
      case RT_STATE_IDLE:
        if (mediaUri->mUri[0] == RT_NULL) {
            this->setCurState(RT_STATE_IDLE);
        } else {
            mPlayerCtx->mProtocolType = RTMediaUtil::getMediaProtocol(mediaUri->mUri);
            err = mNodeBus->autoBuild(mediaUri);
            if (RT_OK != err) {
                if (RT_NULL == mNodeBus->getRootNode(BUS_LINE_ROOT)) {
                    RT_LOGE("fail to init demuxer");
                    mPlayerCtx->mLooper->flush();
                    RTMessage* msg = new RTMessage(RT_MEDIA_ERROR, RT_NULL, this);
                    mPlayerCtx->mLooper->send(msg, 0);
                    mPlayerCtx->mLooper->requestExit();
                    return RT_ERR_UNKNOWN;
                }
            } else {
                this->setCurState(RT_STATE_INITIALIZED);
            }
        }
        break;
      default:
        RTMediaUtil::dumpStateError(curState, __FUNCTION__);
        break;
    }
    return err;
}

RT_RET RTNDKNodePlayer::prepare() {
    RT_RET err = checkRuntime("prepare");
    if (RT_OK != err) {
        return err;
    }

    UINT32 curState = this->getCurState();
    if ((RT_STATE_INITIALIZED != curState) && (RT_STATE_STOPPED != curState)) {
        RTMediaUtil::dumpStateError(curState, __FUNCTION__);
        return RT_OK;
    }

    if (RT_STATE_INITIALIZED == curState) {
        err = mNodeBus->autoBuildCodecSink();
        if (RT_OK != err) {
            RT_LOGE("fail to use node-bus to build codec sink");
        }
    }

    setCurState(RT_STATE_PREPARING);

    // @TODO: do prepare player
    mNodeBus->excuteCommand(RT_NODE_CMD_PREPARE);


    this->onPreparedDone();
    setCurState(RT_STATE_PREPARED);

    RTMessage* msg = new RTMessage(RT_MEDIA_PREPARED, RT_NULL, this);
    mPlayerCtx->mLooper->post(msg, 0);

    postSeekIfNecessary();

    return RT_OK;
}

RT_RET RTNDKNodePlayer::start() {
    RT_RET err = checkRuntime("start");
    if (RT_OK != err) {
        return err;
    }

    // nodebus be operated by multithread
    RtMutex::RtAutolock autoLock(mPlayerCtx->mNodeLock);

    UINT32 curState = getCurState();
    RTMessage* msg  = RT_NULL;
    switch (curState) {
      case RT_STATE_PREPARED:
      case RT_STATE_PAUSED:
      case RT_STATE_COMPLETE:
        // @TODO: do resume player
        mNodeBus->excuteCommand(RT_NODE_CMD_START);

        // thread used for data transferring between plugins
        if (RT_NULL == mPlayerCtx->mDeliverThread) {
            mPlayerCtx->mDeliverThread = new RtThread(thread_deliver_proc, this);
            mPlayerCtx->mDeliverThread->setName("BusDataProc");
            mPlayerCtx->mDeliverThread->start();
        }

        msg = new RTMessage(RT_MEDIA_STARTED, RT_NULL, this);
        mPlayerCtx->mLooper->post(msg, 0);
        this->setCurState(RT_STATE_STARTED);
        break;
      default:
        RTMediaUtil::dumpStateError(curState, __FUNCTION__);
        break;
    }
    return err;
}

RT_RET RTNDKNodePlayer::pause() {
    RT_RET err = checkRuntime("pause");
    if (RT_OK != err) {
        return err;
    }

    // nodebus be operated by multithread
    RtMutex::RtAutolock autoLock(mPlayerCtx->mNodeLock);

    UINT32 curState = this->getCurState();
    RTMessage* msg  = RT_NULL;
    switch (curState) {
      case RT_STATE_PAUSED:
        RTMediaUtil::dumpStateError(curState, __FUNCTION__);
        break;
      case RT_STATE_STARTED:
        // @TODO: do pause player
        mNodeBus->excuteCommand(RT_NODE_CMD_PAUSE);

        msg = new RTMessage(RT_MEDIA_PAUSED, RT_NULL, this);
        mPlayerCtx->mLooper->post(msg, 0);
        this->setCurState(RT_STATE_PAUSED);
        break;
      default:
        RTMediaUtil::dumpStateError(curState, __FUNCTION__);
        break;
    }
    return err;
}

RT_RET RTNDKNodePlayer::stop() {
    RT_RET err = checkRuntime("stop");
    if (RT_OK != err) {
        return err;
    }

    UINT32 curState = this->getCurState();
    RTMessage* msg  = RT_NULL;
    switch (curState) {
      case RT_STATE_STOPPED:
        RTMediaUtil::dumpStateError(curState, __FUNCTION__);
        break;
      case RT_STATE_PREPARING:
      case RT_STATE_PREPARED:
      case RT_STATE_STARTED:
        this->pause();
      case RT_STATE_PAUSED:
      case RT_STATE_COMPLETE:
      case RT_STATE_ERROR:
        // nodebus be operated by multithread
        RtMutex::RtAutolock autoLock(mPlayerCtx->mNodeLock);

        // @TODO: do stop player
        mNodeBus->excuteCommand(RT_NODE_CMD_STOP);
        mPlayerCtx->mLooper->flush();
        msg = new RTMessage(RT_MEDIA_STOPPED, RT_NULL, this);
        mPlayerCtx->mLooper->post(msg, 0);
        mPlayerCtx->mCurTimeUs = 0;
        mPlayerCtx->mDuration  = 0;
        this->setCurState(RT_STATE_STOPPED);
        break;
    }

    return err;
}

RT_RET RTNDKNodePlayer::seekTo(INT64 usec) {
    RT_RET err = checkRuntime("seekTo");
    if (RT_OK != err) {
        return err;
    }

    RT_LOGE("seek %lld ms", usec/1000);
    UINT32 curState = this->getCurState();
    switch (curState) {
      case RT_STATE_IDLE:
      case RT_STATE_INITIALIZED:
      case RT_STATE_PREPARING:
        mPlayerCtx->mWantSeekTimeUs = -1;
        mPlayerCtx->mSaveSeekTimeUs = usec;
        RT_LOGE("seek %lld ms", usec/1000);
        RTMediaUtil::dumpStateError(curState, "seekTo, save only");
        break;
      case RT_STATE_PREPARED:
      case RT_STATE_PAUSED:
      case RT_STATE_STARTED:
      case RT_STATE_COMPLETE:
        // @TODO: do pause player
        RT_LOGE("seek %lld ms; mPlayerCtx: %p; mSeekFlag %d", usec/1000, mPlayerCtx, mPlayerCtx->mSeekFlag);
        switch (mPlayerCtx->mSeekFlag) {
          case RT_SEEK_DOING:
            RT_LOGE("seek %lld ms", usec/1000);
            // mPlayerCtx->mWantSeekTimeUs = -1;
            mPlayerCtx->mSaveSeekTimeUs = usec;
            break;
          default:
            RT_LOGE("seek %lld ms", usec/1000);
            mPlayerCtx->mSaveSeekTimeUs = -1;
            mPlayerCtx->mWantSeekTimeUs = usec;
            postSeekIfNecessary();
            break;
        }
        break;
      default:
        RTMediaUtil::dumpStateError(curState, __FUNCTION__);
        break;
    }

    return err;
}


RT_RET RTNDKNodePlayer::setLooping(RT_BOOL loop) {
    RT_RET err = checkRuntime("setLooping");
    if (RT_OK != err) {
        return err;
    }
    mPlayerCtx->mLooping = loop;
    return err;
}

RT_RET RTNDKNodePlayer::setListener(RTPlayerListener* listener) {
    RT_RET err = checkRuntime("setLooping");
    if (RT_OK != err) {
        return err;
    }
    if (RT_NULL != mPlayerCtx->mListener) {
        RT_LOGE("check, mListener isn't NULL!");
    }
    mPlayerCtx->mListener = listener;
}

RT_RET RTNDKNodePlayer::post(const char* caller, RTMessage* msg) {
    RT_RET err = checkRuntime("post message");
    if (RT_OK != err) {
        return err;
    }
    UINT32 what = RT_MEDIA_CMD_NOP;
    if (msg->getWhat() > RT_MEDIA_CMD_NOP) {
        what = msg->getWhat() - RT_MEDIA_CMD_NOP;
        RT_LOGE("%24s requested post message:%s", caller, mMediaCmds[what].name);
        mPlayerCtx->mLooper->post(msg, 0);
    }
}

RT_RET RTNDKNodePlayer::send(const char* caller, RTMessage* msg) {
    RT_RET err = checkRuntime("send message");
    if (RT_OK != err) {
        return err;
    }
    UINT32 what = RT_MEDIA_CMD_NOP;
    if (msg->getWhat() > RT_MEDIA_CMD_NOP) {
        what = msg->getWhat() - RT_MEDIA_CMD_NOP;
        RT_LOGE("%s requested to send message:%s", caller, mMediaCmds[what].name);
        mPlayerCtx->mLooper->send(msg, 0);
    }
}

RT_RET RTNDKNodePlayer::wait(int64_t timeUs) {
    UINT32 curState = RT_STATE_IDLE;
    UINT32 loopFlag = 1;
    timeUs = (timeUs < 1000000)? 20000000:timeUs;
    do {
        curState = this->getCurState();
        switch (curState) {
          case RT_STATE_IDLE:
          case RT_STATE_ERROR:
          case RT_STATE_COMPLETE:
            loopFlag = 0;
            break;
          default:
            RtTime::sleepMs(5);
            break;
        }
    } while ((1 == loopFlag)&&(mPlayerCtx->mCurTimeUs < timeUs));
    RT_LOGE("done, ndk-node-player completed playback! current:%lldms, timeout:%lldms",
             mPlayerCtx->mCurTimeUs/1000, timeUs/1000);

    return RT_OK;
}

RT_RET RTNDKNodePlayer::postSeekIfNecessary() {
    RT_RET err = checkRuntime("postSeekIfNecessary");
    if (RT_OK != err) {
        return err;
    }

    // restore seek position if necessary
    if (-1 != mPlayerCtx->mSaveSeekTimeUs) {
         mPlayerCtx->mWantSeekTimeUs = mPlayerCtx->mSaveSeekTimeUs;
    }
    if (-1 == mPlayerCtx->mWantSeekTimeUs) {
        return RT_OK;
    }
    // mini seek margin is 500ms
    INT64 seekDelta = RT_ABS(mPlayerCtx->mWantSeekTimeUs - mPlayerCtx->mCurTimeUs);
    RT_LOGE("mWantSeekTimeUs: %lld us, mCurTimeUs: %lld us",
             mPlayerCtx->mWantSeekTimeUs, mPlayerCtx->mCurTimeUs);
    if (seekDelta > 500*1000) {
        // async seek message
        RTMessage* msg = new RTMessage(RT_MEDIA_SEEK_ASYNC, 0, mPlayerCtx->mWantSeekTimeUs, this);
        mPlayerCtx->mLooper->flush_message(RT_MEDIA_SEEK_ASYNC);
        mPlayerCtx->mLooper->post(msg, 0);
        mPlayerCtx->mSeekFlag = RT_SEEK_DOING;
    } else {
        mPlayerCtx->mSaveSeekTimeUs = -1;
    }

    mPlayerCtx->mWantSeekTimeUs = -1;
    mPlayerCtx->mSaveSeekTimeUs = -1;

    return err;
}

RT_RET RTNDKNodePlayer::summary(INT32 fd) {
    RT_RET err = checkRuntime("summary");
    if (RT_OK != err) {
        return err;
    }

    mNodeBus->summary(0);
    return RT_OK;
}

RT_RET RTNDKNodePlayer::getCurrentPosition(int64_t *usec) {
    *usec = 0;
    RT_RET err = checkRuntime("getCurrentPosition");
    if (RT_OK != err) {
        return err;
    }
    *usec = mPlayerCtx->mCurTimeUs;
    return err;
}

RT_RET RTNDKNodePlayer::getDuration(int64_t *usec) {
    *usec = 0;
    RT_RET err = checkRuntime("getDuration");
    if (RT_OK != err) {
        return err;
    }
    *usec = mPlayerCtx->mDuration;
    return err;
}

RT_RET RTNDKNodePlayer::setCurState(UINT32 newState) {
    RT_RET err = checkRuntime("setCurState");
    if (RT_OK != err) {
        return err;
    }

    RT_LOGE("done, switch state:%s to state:%s", \
             RTMediaUtil::getStateName(mPlayerCtx->mState), \
             RTMediaUtil::getStateName(newState));
    mPlayerCtx->mState = newState;
    return err;
}

UINT32 RTNDKNodePlayer::getCurState() {
    RT_ASSERT(RT_NULL != mPlayerCtx);
    if (RT_NULL != mPlayerCtx) {
        return mPlayerCtx->mState;
    }
    return RT_STATE_ERROR;
}

RT_RET RTNDKNodePlayer::onSeekTo(INT64 usec) {
    RTMessage* msg  = RT_NULL;
    RT_RET err = checkRuntime("onSeekTo");
    if (RT_OK != err) {
        return err;
    }

    // nodebus be operated by multithread
    RtMutex::RtAutolock autoLock(mPlayerCtx->mNodeLock);

    // workflow: pause flush (cache maybe) start
    setCurState(RT_STATE_PAUSED);
    mNodeBus->excuteCommand(RT_NODE_CMD_PAUSE);
    mNodeBus->excuteCommand(RT_NODE_CMD_FLUSH);

    mPlayerCtx->mCmdOptions->clear();
    mPlayerCtx->mCmdOptions->setInt64(kKeySeekTimeUs, usec);
    mNodeBus->excuteCommand(RT_NODE_CMD_SEEK, mPlayerCtx->mCmdOptions);
    mNodeBus->excuteCommand(RT_NODE_CMD_START);

    // post RT_MEDIA_SEEK_COMPLETE
    msg = new RTMessage(RT_MEDIA_SEEK_COMPLETE, RT_NULL, this);
    mPlayerCtx->mLooper->post(msg, 0);
    RT_LOGE("done, seek to target:%lldms", usec/1000);

    return err;
}

RT_RET RTNDKNodePlayer::onPlaybackDone() {
    RT_RET err = checkRuntime("onPlaybackDone");
    if (RT_OK != err) {
        return err;
    }

    // nodebus be operated by multithread
    RtMutex::RtAutolock autoLock(mPlayerCtx->mNodeLock);

    // workflow: pause flush (cache maybe) reset
    mNodeBus->excuteCommand(RT_NODE_CMD_PAUSE);
    mNodeBus->excuteCommand(RT_NODE_CMD_FLUSH);
    mNodeBus->excuteCommand(RT_NODE_CMD_RESET);
    if (mPlayerCtx->mRT_Callback != RT_NULL) {
        mPlayerCtx->mRT_Callback(mPlayerCtx->mRT_Callback_Type, mPlayerCtx->mRT_Callback_Data);
    }
    RT_LOGE("done, onPlaybackDone");

    return RT_OK;
}

RT_RET RTNDKNodePlayer::onPreparedDone() {
    RT_RET err = checkRuntime("onPreparedDone");
    if (RT_OK != err) {
        return err;
    }

    RTNode*          root      = mNodeBus->getRootNode(BUS_LINE_ROOT);
    RTNodeDemuxer*   demuxer   = reinterpret_cast<RTNodeDemuxer*>(root);
    if (RT_NULL != demuxer) {
        mPlayerCtx->mDuration = demuxer->queryDuration();
    }
    return err;
}

RT_RET RTNDKNodePlayer::checkRuntime(const char* caller) {
    if ((RT_NULL == mPlayerCtx) || (RT_NULL == mNodeBus)) {
        RT_LOGE("fail to %s, context in null", caller);
        return RT_ERR_BAD;
    }
    return RT_OK;
}

RT_RET RTNDKNodePlayer::notifyListener(INT32 msg, INT32 ext1, INT32 ext2, void* ptr) {
    if (RT_NULL != mPlayerCtx->mListener) {
        mPlayerCtx->mListener->notify(msg, ext1, ext1, ptr);
    }
}

RT_RET RTNDKNodePlayer::onEventReceived(struct RTMessage* msg) {
    const char* msgName = mMediaEvents[msg->getWhat()].name;
    RT_LOGE("@received event messsage(what=%d, name=%s), notify listener", \
             msg->getWhat(), msgName);
    INT32 arg1 = 0;
    INT32 arg2 = 0;
    switch (msg->getWhat()) {
      case RT_MEDIA_PLAYBACK_COMPLETE:
        if (!mPlayerCtx->mLooping) {
            setCurState(RT_STATE_COMPLETE);
            this->notifyListener(RT_MEDIA_PLAYBACK_COMPLETE, 0, 0, RT_NULL);
            if (mPlayerCtx->mRT_Callback != RT_NULL) {
                mPlayerCtx->mRT_Callback(mPlayerCtx->mRT_Callback_Type, mPlayerCtx->mRT_Callback_Data);
            }
        } else {
            seekTo(0ll);
        }
        break;
      case RT_MEDIA_SEEK_COMPLETE:
        mPlayerCtx->mSeekFlag = RT_SEEK_NO;
        RT_LOGD("seek complete mPlayerCtx: %p mSeekFlag: %d", mPlayerCtx, mPlayerCtx->mSeekFlag);
        mPlayerCtx->mCurTimeUs = mPlayerCtx->mWantSeekTimeUs;
        setCurState(RT_STATE_STARTED);
        this->notifyListener(RT_MEDIA_SEEK_COMPLETE, 0, 0, RT_NULL);
        postSeekIfNecessary();
        break;
      case RT_MEDIA_SEEK_ASYNC:
        onSeekTo(msg->mData.mArgU64);
        break;
      case RT_MEDIA_SUBTITLE_DATA:
      case RT_MEDIA_TIMED_TEXT:
        RT_LOGE("@todo, timed-text...");
        break;
      case RT_MEDIA_STARTED:
      case RT_MEDIA_PREPARED:
      case RT_MEDIA_PAUSED:
      case RT_MEDIA_ERROR:
      case RT_MEDIA_STOPPED:
      case RT_MEDIA_BUFFERING_UPDATE:
      case RT_MEDIA_SET_VIDEO_SIZE:
      case RT_MEDIA_SKIPPED:
      case RT_MEDIA_INFO:
        this->notifyListener(RT_MEDIA_SEEK_COMPLETE, arg1, arg2, RT_NULL);
        break;
      default:
        break;
    }
    RT_LOGE("@done to notify messsage(what=%d, name=%s)\r\n", msg->getWhat(), msgName);
    return RT_OK;
}

RT_RET RTNDKNodePlayer::onCmdReceived(struct RTMessage* msg) {
    RT_RET err;
    UINT32 what = msg->getWhat() - RT_MEDIA_CMD_NOP;
    const char* msgName = "unknown";
    if ((what > 0) && (what < RT_MEDIA_CMD_MAX)) {
        msgName = mMediaCmds[what].name;
    }
    RT_LOGE("@received cmd messsage(what=%d, name=%s)", msg->getWhat(), msgName);
    switch (msg->getWhat()) {
      case RT_MEDIA_CMD_SET_DATASOURCE:
        err = this->setDataSource(reinterpret_cast<RTMediaUri*>(msg->mData.mArgPtr));
        break;
      case RT_MEDIA_CMD_PREPARE:
        err = this->prepare();
        break;
      case RT_MEDIA_CMD_SEEKTO:
        err = this->seekTo(msg->mData.mArgU64);
        break;
      case RT_MEDIA_CMD_START:
        err = this->start();
        break;
      case RT_MEDIA_CMD_STOP:
        err = this->stop();
        break;
      case RT_MEDIA_CMD_PAUSE:
        err = this->pause();
        break;
      case RT_MEDIA_CMD_RESET:
        err = this->reset();
        break;
      default:
        break;
    }
    RT_LOGE("@done to handle cmd messsage(what=%d, name=%s)\r\n", msg->getWhat(), msgName);
    return err;
}

/* looper functions or callback of thread */
RT_RET RTNDKNodePlayer::onMessageReceived(struct RTMessage* msg) {
    RT_RET err = checkRuntime("onMessageReceived");
    if (RT_OK != err) {
        return err;
    }

    if (msg->getWhat() > RT_MEDIA_CMD_NOP) {
        err = onCmdReceived(msg);
    } else {
        err = onEventReceived(msg);
    }
    return err;
}

RT_RET RTNDKNodePlayer::startDataLooper() {
    mNodeBus->excuteCommand(RT_NODE_CMD_START);
    return RT_OK;
}

RT_RET RTNDKNodePlayer::writeData(const char * data, const UINT32 length, int flag, int type) {
    RT_ASSERT(RT_NULL != mPlayerCtx);
    RT_LOGD("RTNDKNodePlayer::writeData IN");
    if (RT_WRITEDATA_PCM == flag) {
        RT_LOGD("RTNDKNodePlayer::writeData 1 length = %d", length);
        RTMediaBuffer* esPacket;
        RTNode* decoder   = mNodeBus->getRootNode(BUS_LINE_AUDIO);
        if (decoder != RT_NULL) {
            RT_LOGD("RTNDKNodePlayer::writeData 2 length = %d", length);
            {
                RT_RET err = RTNodeAdapter::dequeCodecBuffer(decoder, &esPacket, RT_PORT_INPUT);
                RT_LOGD("RTNDKNodePlayer::writeData err = %d", err);
                if (RT_NULL != esPacket) {
                    if (length > 0) {
                        RT_LOGD("RTNDKNodePlayer::writeData 3");
                        char *tempdata = rt_malloc_size(char, length);
                        rt_memcpy(tempdata, data, length);
                        esPacket->setData(tempdata, length);
                    } else {
                        RT_LOGD("RTNDKNodePlayer::writeData 4 eos");
                        esPacket->setData(NULL, 0);
                        esPacket->getMetaData()->setInt32(kKeyFrameEOS, 1);
                    }
                    RTNodeAdapter::pushBuffer(decoder, esPacket);
                } else {
                    RT_LOGD("writeData list null");
                }
        }
        } else {
            RT_LOGD("writeData decoder null");
        }
    } else if (RT_WRITEDATA_TS == flag) {
    } else {
        if (RTTRACK_TYPE_VIDEO == type) {
        } else {
        }
    }
    RT_LOGD("RTNDKNodePlayer::writeData OUT");
    return RT_OK;
}

RT_RET RTNDKNodePlayer::setCallBack(RT_CALLBACK_T callback, int p_event, void *p_data) {
    RT_ASSERT(RT_NULL != mPlayerCtx);
    mPlayerCtx->mRT_Callback = callback;
    mPlayerCtx->mRT_Callback_Type = p_event;
    mPlayerCtx->mRT_Callback_Data = p_data;
    return RT_OK;
}

RT_RET RTNDKNodePlayer::startAudioPlayerProc() {
    RT_RET           err       = RT_OK;
    RTNode*          root      = mNodeBus->getRootNode(BUS_LINE_ROOT);
    RTNodeDemuxer*   demuxer   = reinterpret_cast<RTNodeDemuxer*>(root);
    RTNode*          decoder   = mNodeBus->getRootNode(BUS_LINE_AUDIO);
    RTNodeAudioSink* audiosink = RT_NULL;
    if (RT_NULL != decoder) {
        audiosink = reinterpret_cast<RTNodeAudioSink*>(decoder->mNext);
        decoder->setEventLooper(mPlayerCtx->mLooper);
        audiosink->setEventLooper(mPlayerCtx->mLooper);
    }

    if ((RT_NULL == decoder) || (RT_NULL == audiosink)) {
        return RT_ERR_BAD;
    }

    RTMediaBuffer* frame   = RT_NULL;
    RTMediaBuffer* esPacket = RT_NULL;

    while (mPlayerCtx->mDeliverThread->getState() == THREAD_LOOP) {
        RT_BOOL validAudioPkt = RT_FALSE;

        // nodebus be operated by multithread
        RtMutex::RtAutolock autoLock(mPlayerCtx->mNodeLock);

        UINT32 curState = this->getCurState();
        if (RT_STATE_STARTED != curState) {
            RtTime::sleepMs(2);
            continue;
        }

        /**
         * 1. acquire avail audio packet from demuxer
         */
        if (demuxer != RT_NULL) {
            INT32 audio_idx  = demuxer->queryTrackUsed(RTTRACK_TYPE_AUDIO);
            /**
             * 1.1 deqeue buffer from decoder object pool
             */
            if (RT_NULL == esPacket) {
                RTNodeAdapter::dequeCodecBuffer(decoder, &esPacket, RT_PORT_INPUT);
            }
            /**
             * 1.2 deqeue avail audio packet from demuxer
             */
            if (RT_NULL != esPacket) {
                err = RTNodeAdapter::pullBuffer(demuxer, &esPacket);
                if (RT_OK == err) {
                    validAudioPkt = RT_TRUE;
                } else if (RT_ERR_LIST_EMPTY != err) {
                    RT_LOGE("pull buffer failed from demuxer. err: %d", err);
                }
            }
        }

        /**
         * 2. push avail audio packet to decoder
         */
        if (validAudioPkt) {
            if (DEBUG_FLAG) {
                UINT32 size = 0;
                INT32 eos   = 0;
                size = esPacket->getSize();
                esPacket->getMetaData()->findInt32(kKeyFrameEOS, &eos);
                RT_LOGD_IF(DEBUG_FLAG, "audio es-packet(ptr=%p, size=%d, eos=%d)", esPacket, size, eos);
            }
            // push es-packet to decoder
            RTNodeAdapter::pushBuffer(decoder, esPacket);
            esPacket = RT_NULL;
            validAudioPkt = RT_FALSE;
        }

        /**
         * 3. acquire avail frame from decoder
         */
        err = RTNodeAdapter::pullBuffer(decoder, &frame);
        if (RT_OK != err && RT_ERR_LIST_EMPTY != err) {
            RT_LOGE("pull buffer failed from decoder. err: %d", err);
        }

        if (frame) {
            if (frame->getStatus() == RT_MEDIA_BUFFER_STATUS_READY) {
                INT32 eos = 0;
                INT64 timeUs = 0ll;
                frame->getMetaData()->findInt32(kKeyFrameEOS, &eos);
                frame->getMetaData()->findInt64(kKeyFramePts, &timeUs);
                if (!eos) {
                    mPlayerCtx->mCurTimeUs = timeUs;
                }
                RT_LOGD_IF(DEBUG_FLAG, "audio frame(ptr=0x%p, size=%d, timeUs=%lldms, eos=%d)",
                        frame->getData(), frame->getLength(), timeUs/1000, eos);
                RTNodeAdapter::pushBuffer(audiosink, frame);
            }

            frame = NULL;
        }
    }

    if (esPacket) {
        esPacket->release();
    }

    return RT_OK;
}
