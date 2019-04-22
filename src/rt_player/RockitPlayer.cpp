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
 * Author: hery.xu@rock-chips.com
 *   Date: 2019/03/13
 *   Task: RockitPlayer
 */
#include "rt_header.h"         // NOLINT
#include "RockitPlayer.h"      // NOLINT
#include "RTNDKMediaPlayer.h"  // NOLINT
#include "rt_message.h"        // NOLINT
#include "RTNDKMediaDef.h"     // NOLINT
#include "RTMediaDef.h"     // NOLINT
#include "RTMediaMetaKeys.h"   // NOLINT
#include "rt_type.h"           // NOLINT
#include <cstring>             // NOLINT
#include <pthread.h>           // NOLINT

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "RockitPlayer"


enum PlayCase {
    PCM_PLAY = 0,
    LOCAL_PLAY,
    ALL_PLAY,
};

enum PlayProtocolType {
    PRO_LOCAL,
    PRO_HTTP,
};

struct RockitContext {
    RTNDKMediaPlayer *local_player;
    RTNDKMediaPlayer *pcm_player;
    RT_BOOL           player_select;
    RtMutex*          mDataLock;
    RtMutex*          mPcmLock;
    RT_CALLBACK_T     mRT_Callback;
    void*             mRT_Callback_Data;
    int               mProtocolType;
    const char*       url;
    RT_BOOL           mplayer_stop_flag;
};

void Rockit_InitCodec(void *player, RTTrackType tType, int samplerate, int channels, int play_flag) {
    RT_LOGD("Rockit_InitCodec");
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    if (NULL != rtplayer) {
        RtMetaData *node_meta = new RtMetaData();
        node_meta->setInt32(kKeyCodecByePass, 1);
        node_meta->setInt32(kKeyCodecType,  tType);
        node_meta->setInt32(kKeyVCodecWidth, 1280);
        node_meta->setInt32(kKeyVCodecHeight, 720);
        node_meta->setInt32(kKeyACodecSampleRate, samplerate);
        node_meta->setInt32(kKeyACodecChannels, channels);

        if (play_flag == LOCAL_PLAY) {
            rtplayer->local_player->setCodecMeta(reinterpret_cast<void *>(node_meta));
        } else if (play_flag == PCM_PLAY) {
            rtplayer->pcm_player->setCodecMeta(reinterpret_cast<void *>(node_meta));
        }
    }
    RT_LOGD("Rockit_InitCodec Done");
}

void * Rockit_ReadData(void *player) {
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    if (NULL != rtplayer) {
        RT_LOGD("Rockit_ReadData rtplayer->url = %s", rtplayer->url);
        FILE *fp = fopen(rtplayer->url, "r");
        if (fp == NULL) {
            RT_LOGD("Rockit_ReadData open file fail, errno = %d", errno);
            return NULL;
        }
        char *dataPtr = NULL;
        dataPtr = reinterpret_cast<char *>(malloc(1024*5));
        rtplayer->mplayer_stop_flag = 0;
        while (!feof(fp) && !rtplayer->mplayer_stop_flag) {
            rtplayer->mDataLock->lock();
            unsigned int size = fread(dataPtr, 1, 1024*5, fp);
            if (size > 0) {
                rtplayer->local_player->writeData(dataPtr, size, 0/*PCM stream*/, 0/*only es use*/);
            }
            rtplayer->mDataLock->unlock();
        }

        if (feof(fp) && !rtplayer->mplayer_stop_flag) {
            rtplayer->local_player->writeData(NULL, 0, 0/*PCM stream*/, 0/*only es use*/);
        }
        if (dataPtr) {
            free(dataPtr);
            dataPtr = NULL;
        }
        if (fp) {
            fclose(fp);
            fp = NULL;
        }
    }
    RT_LOGD("Rockit_ReadData Done");
    return NULL;
}

void Rockit_Callback(int p_event, void *player) {
    RT_LOGD("Rockit_Callback p_event = %d", p_event);
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    if (NULL != rtplayer) {
        if (p_event == Rockit_MediaPlayerEndReached_pcm) {
            rtplayer->mRT_Callback(Rockit_MediaPlayerEndReached_pcm, rtplayer->mRT_Callback_Data);
        } else {
           if (rtplayer->mProtocolType == PRO_LOCAL) {
               rtplayer->mRT_Callback(Rockit_MediaPlayerEndReached_kit, rtplayer->mRT_Callback_Data);
           } else {
               rtplayer->mRT_Callback(Rockit_MediaPlayerEndReached_vlc, rtplayer->mRT_Callback_Data);
           }
       }
    }
    RT_LOGD("Rockit_Callback Done");
}

void * Rockit_CreatePlayer() {
    RT_LOGD("Rockit_CreatePlayer");
    struct RockitContext* mPlayerCtx = rt_malloc(RockitContext);
    mPlayerCtx->mDataLock      = new RtMutex();
    RtMutex::RtAutolock autoLock(mPlayerCtx->mDataLock);
    mPlayerCtx->local_player =  new RTNDKMediaPlayer();
    mPlayerCtx->pcm_player =    new RTNDKMediaPlayer();
    mPlayerCtx->player_select  = LOCAL_PLAY;
    mPlayerCtx->mProtocolType  = PRO_LOCAL;
    mPlayerCtx->mRT_Callback   = NULL;
    mPlayerCtx->mRT_Callback_Data = NULL;
    mPlayerCtx->url            = NULL;
    mPlayerCtx->mplayer_stop_flag = 1;
    mPlayerCtx->mPcmLock       = new RtMutex();
    RT_LOGD("Rockit_CreatePlayer Done");
    return reinterpret_cast<void *>(mPlayerCtx);
}

int Rockit_EventAttach(void * player, int type, Rockit_callback_t callback, void *opaque) {
    RT_LOGD("Rockit_EventAttach");
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    RtMutex::RtAutolock autoLock(rtplayer->mDataLock);
    if (NULL != rtplayer) {
        rtplayer->mRT_Callback = callback;
        rtplayer->mRT_Callback_Data = opaque;
        rtplayer->local_player->setCallBack(Rockit_Callback, type, rtplayer);
        rtplayer->pcm_player->setCallBack(Rockit_Callback, Rockit_MediaPlayerEndReached_pcm, rtplayer);
        RT_LOGD("Rockit_EventAttach Done");
        return RT_OK;
    }
    RT_LOGD("Rockit_EventAttach err");
    return RT_ERR_UNKNOWN;
}

int Rockit_PlayAudio_PCM(void *player) {
    RT_LOGD("Rockit_PlayAudio_PCM");
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    rt_status ret;

    ret = rtplayer->pcm_player->getState();
    RtMutex::RtAutolock autoLock(rtplayer->mDataLock);
    RT_LOGD("Rockit_PlayAudio_PCM status = %d", ret);
    if (ret == RT_STATE_COMPLETE) {
        RT_LOGD("Rockit_PlayAudio_PCM stop");
        rtplayer->pcm_player->stop();
        RT_LOGD("Rockit_PlayAudio_PCM reset");
        rtplayer->pcm_player->reset();
    }
    ret = rtplayer->pcm_player->getState();
    RT_LOGD("Rockit_PlayAudio_PCM another status = %d", ret);
    if (NULL != rtplayer && (ret == RT_STATE_IDLE)) {
        rtplayer->player_select = PCM_PLAY;
        Rockit_InitCodec(player, RTTRACK_TYPE_AUDIO, 24000, 1, PCM_PLAY);
        rtplayer->pcm_player->setDataSource(RT_NULL, RT_NULL);
        RT_LOGD("Rockit_PlayAudio_PCM prepare");
        rtplayer->pcm_player->prepare();
        RT_LOGD("Rockit_PlayAudio_PCM start");
        rtplayer->pcm_player->start();
        RT_LOGD("Rockit_PlayAudio_PCM Done");
        return RT_OK;
    }
    RT_LOGD("Rockit_PlayAudio_PCM have Done");
    return RT_ERR_UNKNOWN;
}

int Rockit_ReleasePlayer(void *player) {
    RT_LOGD("Rockit_ReleasePlayer");
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    RtMutex::RtAutolock autoLock(rtplayer->mDataLock);
    if (NULL != rtplayer) {
        rtplayer->local_player->stop();
        rtplayer->local_player->reset();
        rtplayer->pcm_player->stop();
        rtplayer->pcm_player->reset();
        delete rtplayer->local_player;
        delete rtplayer->pcm_player;
        delete rtplayer;
    }
    RT_LOGD("Rockit_ReleasePlayer Done");
    return RT_ERR_UNKNOWN;
}

int Rockit_PlayAudio(void *player, const char* url, const int pcm) {
    RT_LOGD("Rockit_PlayAudio url = %s, pcm = %d", url, pcm);
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    rtplayer->mplayer_stop_flag = 1;
    rt_status ret;
    RtMutex::RtAutolock autoLock(rtplayer->mDataLock);

    {
        if (NULL != rtplayer) {
            ret = rtplayer->local_player->getState();
            if (ret != RT_STATE_IDLE) {
                RT_LOGD("Rockit_PlayAudio stop");
                rtplayer->local_player->stop();
                RT_LOGD("Rockit_PlayAudio reset");
                rtplayer->local_player->reset();
            }
            rtplayer->player_select = LOCAL_PLAY;
            if (!strncasecmp("/oem", url, 4)) {
                rtplayer->mProtocolType = PRO_LOCAL;
            } else {
                rtplayer->mProtocolType = PRO_HTTP;
            }
            RT_LOGD("Rockit_PlayAudio setDataSource");
            if (!pcm) {
                rtplayer->local_player->setDataSource(url, RT_NULL);
            } else {
                Rockit_InitCodec(player, RTTRACK_TYPE_AUDIO, 16000, 1, LOCAL_PLAY);
                rtplayer->local_player->setDataSource(RT_NULL, RT_NULL);
            }
            RT_LOGD("Rockit_PlayAudio prepare");
            rtplayer->local_player->prepare();
            RT_LOGD("Rockit_PlayAudio start");
            rtplayer->local_player->start();
            RT_LOGD("Rockit_PlayAudio  Done");
            if (pcm) {
                pthread_t th1;
                rtplayer->url = url;
                if (pthread_create(&th1, NULL, Rockit_ReadData, player) == -1) {
                    RT_LOGE("Rockit_PlayAudio create thread fail!!!");
                }
            }
            return RT_OK;
        }
    }
    return RT_ERR_UNKNOWN;
}

int Rockit_PlayerPlay(void *player) {
    RT_LOGD("Rockit_PlayerPlay");
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    RtMutex::RtAutolock autoLock(rtplayer->mDataLock);
    if (NULL != rtplayer) {
        rtplayer->local_player->start();
        RT_LOGD("Rockit_PlayerPlay Done");
        return RT_OK;
    }
    RT_LOGD("Rockit_PlayerPlay err");
    return RT_ERR_UNKNOWN;
}
int Rockit_WriteData(void *player, const char * data, const unsigned int length) {
    RT_LOGD("Rockit_WriteData");
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    RtMutex::RtAutolock autoLock(rtplayer->mPcmLock);
    if (NULL != rtplayer) {
       Rockit_PlayAudio_PCM(player);
       rtplayer->player_select = PCM_PLAY;
       rtplayer->pcm_player->writeData(data, length, 0/*PCM stream*/, 0/*only es use*/);
       RT_LOGD("Rockit_WriteData Done");
       return RT_OK;
    }
    RT_LOGD("Rockit_WriteData err");
    return RT_ERR_UNKNOWN;
}

int Rockit_PlayerStop(void *player) {
    RT_LOGD("Rockit_PlayerStop");
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    rt_status ret;
    RtMutex::RtAutolock autoLock(rtplayer->mDataLock);
    if (NULL != rtplayer) {
        {
            ret = rtplayer->local_player->getState();
            if (ret != RT_STATE_IDLE) {
                RT_LOGD("Rockit_PlayerStop local stop");
                rtplayer->local_player->stop();
                RT_LOGD("Rockit_PlayerStop local reset");
                rtplayer->local_player->reset();
            }
            ret = rtplayer->pcm_player->getState();
            if (ret != RT_STATE_IDLE) {
                RT_LOGD("Rockit_PlayerStop pcm stop");
                rtplayer->pcm_player->stop();
                RT_LOGD("Rockit_PlayerStop pcm reset");
                rtplayer->pcm_player->reset();
            }
        }
        RT_LOGD("Rockit_PlayerStop Done");
        return RT_OK;
    }
    RT_LOGD("Rockit_PlayerStop err");
    return RT_ERR_UNKNOWN;
}

int Rockit_PlayerPause(void *player) {
    RT_LOGD("Rockit_PlayerPause");
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    RtMutex::RtAutolock autoLock(rtplayer->mDataLock);
    if (NULL != rtplayer) {
        if (rtplayer->player_select == PCM_PLAY) {
            rtplayer->pcm_player->pause();
        } else {
            rtplayer->local_player->pause();
        }
        RT_LOGD("Rockit_PlayerPause Done");
        return RT_OK;
    }
    RT_LOGD("Rockit_PlayerPause err");
    return RT_ERR_UNKNOWN;
}

int Rockit_PlayerResume(void *player) {
    RT_LOGD("Rockit_PlayerResume");
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    RtMutex::RtAutolock autoLock(rtplayer->mDataLock);
    if (NULL != rtplayer) {
        if (rtplayer->player_select == PCM_PLAY) {
            rtplayer->pcm_player->start();
        } else {
            rtplayer->local_player->start();
        }
        RT_LOGD("Rockit_PlayerResume Done");
        return RT_OK;
    }
    RT_LOGD("Rockit_PlayerResume err");
    return RT_ERR_UNKNOWN;
}

int Rockit_PlayerSeek(void *player, const double time) {
    RT_LOGD("Rockit_PlayerSeek time = %lf", time);
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    RtMutex::RtAutolock autoLock(rtplayer->mDataLock);
    if (NULL != rtplayer) {
        {
            RT_LOGD("Rockit_PlayerSeek Local");
            rtplayer->local_player->seekTo((int64_t)time);
        }
        RT_LOGD("Rockit_PlayerSeek done");
        return RT_OK;
    }
    RT_LOGD("Rockit_PlayerSeek err");
    return RT_ERR_UNKNOWN;
}

Rockit_PlayerState Rockit_GetPlayerState(void *player) {
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    RtMutex::RtAutolock autoLock(rtplayer->mDataLock);
    rt_status ret;

    if (NULL != rtplayer) {
        if (rtplayer->player_select == PCM_PLAY) {
            ret = rtplayer->pcm_player->getState();
        } else {
            ret = rtplayer->local_player->getState();
        }
        RT_LOGD("Rockit_GetPlayerState ret = %d", ret);
        if (ret == RT_STATE_STARTED) {
            return Rockit_PLAYING;
        } else if (ret == RT_STATE_PAUSED) {
            return Rockit_PAUSED;
        } else if (ret == RT_STATE_STOPPED) {
            return Rockit_STOPPED;
        } else {
            return Rockit_START;
        }
        return Rockit_CLOSED;
    }
    RT_LOGD("Rockit_GetPlayerState err");
    return Rockit_CLOSED;
}

int Rockit_GetPlayerDuration(void *player) {
    int64_t usec;
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    RtMutex::RtAutolock autoLock(rtplayer->mDataLock);
    if (NULL != rtplayer) {
        if (rtplayer->player_select == PCM_PLAY) {
            rtplayer->pcm_player->getDuration(&usec);
        } else {
            rtplayer->local_player->getDuration(&usec);
        }
        RT_LOGD("Rockit_GetPlayerDuration Done usec = %lld", usec);
        return usec;
    }
    RT_LOGD("Rockit_GetPlayerDuration err");
    return RT_ERR_UNKNOWN;
}

int Rockit_GetPlayerPosition(void *player) {
    int64_t usec;
    RockitContext * rtplayer = reinterpret_cast<RockitContext *>(player);
    RtMutex::RtAutolock autoLock(rtplayer->mDataLock);
    if (NULL != rtplayer) {
        if (rtplayer->player_select == PCM_PLAY) {
            rtplayer->pcm_player->getCurrentPosition(&usec);
        } else {
            rtplayer->local_player->getCurrentPosition(&usec);
        }
        RT_LOGD("Rockit_GetPlayerPosition Done usec = %lld", usec);
        return usec;
    }
    RT_LOGD("Rockit_GetPlayerPosition err");
    return RT_ERR_UNKNOWN;
}
