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
 * Author: martin.cheng@rock-chips.com
 *   Date: 2018/11/03
 *   Task: construct and manage pipeline of media node
 */

#include "rt_hash_table.h"    // NOLINT

#include "rt_string_utils.h"  // NOLINT
#include "RTMediaMetaKeys.h"  // NOLINT
#include "RTNodeBus.h"        // NOLINT
#include "RTNodeDemuxer.h"    // NOLINT
#include "RTNodeAudioSink.h"  // NOLINT
#include "RTNodeHeader.h"     // NOLINT

#include "FFNodeDecoder.h"    // NOLINT
#include "FFNodeEncoder.h"    // NOLINT
#include "FFNodeDemuxer.h"    // NOLINT

#ifdef OS_LINUX
#include "RTSinkAudioALSA.h"   // NOLINT
#include "RTNodeSinkAWindow.h" // NOLINT
#include "HWNodeMpiDecoder.h"  // NOLINT
#include "HWNodeMpiEncoder.h"  // NOLINT
#endif

#ifdef OS_WINDOWS
#include "RTNodeSinkGLES.h"    // NOLINT
#include "RTSinkAudioWASAPI.h" // NOLINT
#endif
#include "RTAllocatorStore.h"       // NOLINT
#include "RTAllocatorBase.h"        // NOLINT

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "RTNodeBus"
#ifdef DEBUG_FLAG
#undef DEBUG_FLAG
#endif
#define DEBUG_FLAG 0x1

typedef enum _NODE_BUS_STATE {
    NODE_ALL_EMPTY,
    NODE_ALL_OK,
    NODE_BUS_EMPTY,
    NODE_BUS_OK,
    NODE_BUS_MAX,
} NODE_BUS_STATE;

struct NodeBusContext {
    RtHashTable    *mNodeBus;
    RtHashTable    *mNodeAll;
    RTMediaUri     *mSetting;
    RTNodeDemuxer  *mDemuxer;
    RTNode*         mRootNodes[BUS_LINE_MAX];
    RTAllocator    *mLinearAllocator;
    RtMetaData     *mVideoMeta;
    RtMetaData     *mAudioMeta;
} NodeBusContext;

RTNode* bus_find_and_add_demuxer(RTNodeBus *pNodeBus, RTMediaUri *setting);
RTNode* bus_find_and_add_codec(RTNodeBus *pNodeBus, RTNode *demuxer, \
                         RTTrackType tType, BUS_LINE_TYPE lType);
RTNode* bus_find_and_add_sink(RTNodeBus *pNodeBus, RTNode *codec, BUS_LINE_TYPE lType);

UINT32  node_hash_func(UINT32 bucktes, const void *key) {
    void *tmp_key = const_cast<void *>(key);
    return *(reinterpret_cast<UINT32 *>(tmp_key)) % (bucktes);
}

RTNodeBus::RTNodeBus() {
    mBusCtx = rt_malloc(struct NodeBusContext);
    rt_memset(mBusCtx, 0, sizeof(struct NodeBusContext));
    mBusCtx->mNodeBus = rt_hash_table_create((RT_NODE_TYPE_MAX - RT_NODE_TYPE_BASE),
                                       hash_ptr_func, hash_ptr_compare);
    mBusCtx->mNodeAll = rt_hash_table_create((RT_NODE_TYPE_MAX - RT_NODE_TYPE_BASE),
                                       hash_ptr_func, hash_ptr_compare);
    RT_LOGD("mBusCtx->mNodeBus = %p; mBusCtx->mNodeAll=%p", \
             mBusCtx->mNodeBus, mBusCtx->mNodeAll);
    mBusCtx->mSetting = RT_NULL;
    mBusCtx->mDemuxer = RT_NULL;
    mBusCtx->mVideoMeta = RT_NULL;
    mBusCtx->mAudioMeta = RT_NULL;
    mBusCtx->mLinearAllocator = RT_NULL;

    RTAllocatorStore::priorAvailLinearAllocator(RT_NULL, &(mBusCtx->mLinearAllocator));
    RT_ASSERT(RT_NULL != mLinearAllocator);

    clearNodeBus();
    registerCoreStubs();
}

RTNodeBus::~RTNodeBus() {
    RT_ASSERT(RT_NULL != mBusCtx);

    RT_LOGD("call, ~RTNodeBus");
    rt_hash_table_destory(mBusCtx->mNodeBus);
    rt_hash_table_destory(mBusCtx->mNodeAll);
    mBusCtx->mNodeBus = RT_NULL;
    mBusCtx->mNodeAll = RT_NULL;

    // NodeBusSetting is released by its producer
    mBusCtx->mSetting = RT_NULL;
    mBusCtx->mDemuxer = RT_NULL;
    rt_safe_delete(mBusCtx->mLinearAllocator);

    rt_safe_free(mBusCtx);

    RT_LOGD("done, ~RTNodeBus");
}

RT_RET RTNodeBus::autoBuild(RTMediaUri* mediaUri) {
    // create [demxuer] by setting
    mBusCtx->mDemuxer = reinterpret_cast<RTNodeDemuxer*>(bus_find_and_add_demuxer(this, mediaUri));
    mBusCtx->mRootNodes[BUS_LINE_ROOT] = mBusCtx->mDemuxer;

    if (RT_NULL == mBusCtx->mDemuxer) {
        RT_LOGE("fail to find and init node-demuxer");
        return RT_ERR_NULL_PTR;
    }
    return RT_OK;
}

RT_RET RTNodeBus::registerMetadata(RtMetaData *codecMeta) {
    RT_ASSERT(RT_NULL != codecMeta);
    RT_LOGD("RTNodeBus::registerMetadata");
    int tType = -1;
    codecMeta->findInt32(kKeyCodecType, &tType);

    if (tType == RTTRACK_TYPE_VIDEO) {
        RT_LOGD("RTNodeBus::registerMetadata video");
        mBusCtx->mVideoMeta = codecMeta;
    } else if (tType == RTTRACK_TYPE_AUDIO) {
        RT_LOGD("RTNodeBus::registerMetadata audio");
        mBusCtx->mAudioMeta = codecMeta;
    }
    return RT_OK;
}

RT_RET RTNodeBus::autoBuildCodecSink() {
    // create [codecs] by meta from demxuer
    RT_LOGD("RTNodeBus::autoBuildCodecSink");
    RTNode *codec_v = bus_find_and_add_codec(this, mBusCtx->mDemuxer, \
                                       RTTRACK_TYPE_VIDEO, BUS_LINE_VIDEO);
    if (codec_v != RT_NULL) {
        mBusCtx->mVideoMeta = RT_NULL;
        nodeChainAppend(codec_v, BUS_LINE_VIDEO);
    }
    RTNode *codec_a = bus_find_and_add_codec(this, mBusCtx->mDemuxer, \
                                       RTTRACK_TYPE_AUDIO, BUS_LINE_AUDIO);
    if (codec_a != RT_NULL) {
        mBusCtx->mAudioMeta = RT_NULL;
        nodeChainAppend(codec_a, BUS_LINE_AUDIO);
    }
    #if TODO_FLAG
    RTNode *codec_s = bus_find_and_add_codec(this, mBusCtx->mDemuxer, \
                                       RTTRACK_TYPE_SUBTITLE, BUS_LINE_SUBTE);
    nodeChainAppend(codec_s, BUS_LINE_SUBTE);
    #endif

    // create [sink] by meta from codecs
    RTNode *sink_v = bus_find_and_add_sink(this, codec_v, BUS_LINE_VIDEO);
    nodeChainAppend(sink_v, BUS_LINE_VIDEO);
    RTNode *sink_a = bus_find_and_add_sink(this, codec_a, BUS_LINE_AUDIO);
    nodeChainAppend(sink_a, BUS_LINE_AUDIO);
    #if TODO_FLAG
    RTNode *sink_s = bus_find_and_add_sink(this, codec_s, BUS_LINE_SUBTE);
    nodeChainAppend(sink_s, BUS_LINE_SUBTE);
    #endif

    nodeChainDumper(BUS_LINE_VIDEO);
    nodeChainDumper(BUS_LINE_AUDIO);
    nodeChainDumper(BUS_LINE_SUBTE);
    RT_LOGD("RTNodeBus::autoBuildCodecSink OUT");

    return RT_OK;
}

RTNode* RTNodeBus::getRootNode(BUS_LINE_TYPE lType) {
    return mBusCtx->mRootNodes[lType];
}

RT_RET RTNodeBus::summary(INT32 fd, RT_BOOL full /*= RT_FALSE*/) {
    RT_ASSERT((RT_NULL != mBusCtx)&&(mBusCtx->mNodeBus));

    RT_LOGD("dump used nodes in node_bus ...");
    struct rt_hash_node *list, *node;
    UINT32 num_buckets = rt_hash_table_get_num_buckets(mBusCtx->mNodeBus);
    for (UINT32 bucket = 0; bucket < num_buckets; bucket++) {
        list = rt_hash_table_get_bucket(mBusCtx->mNodeBus, bucket);
        for (node = list->next; node != RT_NULL; node = node->next) {
            if ((RT_NULL != node) && (RT_NULL != node->data)) {
                RTNode* plugin = reinterpret_cast<RTNode*>(node->data);
                RT_LOGD("%-16s RTNode(name=%s, ptr=%p) hash=%p next=%p",
                           rt_node_type_name(plugin->queryStub()->mNodeType),
                           plugin->queryStub()->mNodeName, plugin, node, node->next);
            }
        }
    }
    RT_LOGD("dump used nodes in node_bus ... DONE!!!");

    return RT_OK;
}

RT_RET RTNodeBus::registerCoreStubs() {
    RT_ASSERT(RT_NULL != mBusCtx);
    #ifdef HAVE_MPI
    registerStub(&hw_node_mpi_decoder);
    registerStub(&hw_node_mpi_encoder);
    #endif

    registerStub(&ff_node_demuxer);
    registerStub(&ff_node_decoder);
    registerStub(&ff_node_video_encoder);
    #ifdef OS_WINDOWS
    registerStub(&rt_sink_display_gles);
    registerStub(&rt_sink_audio_wasapi);
    #endif
    #ifdef OS_LINUX
    registerStub(&rt_sink_audio_alsa);
    #endif
    return RT_OK;
}

RTNodeStub* findStub(RT_NODE_TYPE nType, BUS_LINE_TYPE lType) {
    RTNodeStub* stub = RT_NULL;
    switch (nType) {
    case RT_NODE_TYPE_DEMUXER:
        stub = &ff_node_demuxer;
        break;
    case RT_NODE_TYPE_DECODER:
        if (lType == BUS_LINE_AUDIO) {
            stub = &ff_node_decoder;
        } else if (lType == BUS_LINE_VIDEO) {
            #ifdef HAVE_MPI
            stub = &hw_node_mpi_decoder;
            #endif
            #ifdef OS_WINDOWS
            stub = &ff_node_decoder;
            #endif
        }
        break;
    case RT_NODE_TYPE_ENCODER:
        stub = &ff_node_video_encoder;
        break;
    case RT_NODE_TYPE_SINK:
        switch (lType) {
          case BUS_LINE_VIDEO:
            #ifdef OS_WINDOWS
            stub = &rt_sink_display_gles;
            #endif
            break;
          case BUS_LINE_AUDIO:
            #ifdef OS_LINUX
            stub = &rt_sink_audio_alsa;
            #endif
            #ifdef OS_WINDOWS
            stub = &rt_sink_audio_wasapi;
            #endif
            break;
          default:
            break;
        }
        break;
    default:
        break;
    }
    return stub;
}

RT_RET RTNodeBus::registerStub(RTNodeStub *nStub) {
    RT_ASSERT((RT_NULL != mBusCtx) && (RT_NULL != nStub));

    INT32 nType = nStub->mNodeType;
    rt_hash_table_insert(mBusCtx->mNodeAll, reinterpret_cast<void*>(nType), nStub);
    return RT_OK;
}

RT_RET RTNodeBus::registerNode(RTNode *pNode) {
    RT_ASSERT((RT_NULL != mBusCtx) && (RT_NULL != pNode));

    INT32 nType  = pNode->queryStub()->mNodeType;
    pNode->mNext = RT_NULL;
    pNode->mPrev = RT_NULL;
    rt_hash_table_insert(mBusCtx->mNodeBus, reinterpret_cast<void*>(nType), pNode);
    return RT_OK;
}

static BUS_LINE_TYPE probeBusType(const char* nodeRole, BUS_LINE_TYPE lType) {
    switch (lType) {
      case BUS_LINE_ROOT:
        if (strstr(nodeRole, "demuxer")) {
            return BUS_LINE_ROOT;
        }
        break;
      case BUS_LINE_VIDEO:
        if (strstr(nodeRole, "video")) {
            return BUS_LINE_VIDEO;
        }
        break;
      case BUS_LINE_AUDIO:
        if (strstr(nodeRole, "audio")) {
            return BUS_LINE_AUDIO;
        }
        break;
      case BUS_LINE_SUBTE:
        if (strstr(nodeRole, "subte")) {
            return BUS_LINE_SUBTE;
        }
        break;
      default:
        break;
    }

    return BUS_LINE_MAX;
}

// @TODO find NodeStub by MIME
RTNodeStub* RTNodeBus::findStub(RTNodeInfo *nodeInfo) {
    RT_ASSERT(RT_NULL != mBusCtx);

    RTNodeStub* stub = RT_NULL;
    struct rt_hash_node* hashNode = RT_NULL;
    struct rt_hash_node* rootNode = rt_hash_table_find_root(mBusCtx->mNodeAll,
                                    reinterpret_cast<void *>(nodeInfo->mNodeType));

    for (hashNode = rootNode->next; hashNode != RT_NULL; hashNode = hashNode->next) {
        stub = reinterpret_cast<RTNodeStub*>(hashNode->data);
        if (nodeInfo->mLineType == probeBusType(stub->mNodeRole, nodeInfo->mLineType)) {
            break;
        } else {
            stub = RT_NULL;
        }
    }

    return stub;
}

// @TODO find RTNode by MIME
RTNode* RTNodeBus::findNode(RTNodeInfo *nodeInfo) {
    RT_ASSERT(RT_NULL != mBusCtx);

    RTNode* node = RT_NULL;
    struct rt_hash_node* hashNode = RT_NULL;
    struct rt_hash_node* rootNode = rt_hash_table_find_root(mBusCtx->mNodeBus,
                                    reinterpret_cast<void *>(nodeInfo->mNodeType));

    for (hashNode = rootNode->next; hashNode != RT_NULL; hashNode = hashNode->next) {
        node = reinterpret_cast<RTNode*>(hashNode->data);
        if (nodeInfo->mLineType == probeBusType(node->queryStub()->mNodeRole, nodeInfo->mLineType)) {
            break;
        } else {
            node = RT_NULL;
        }
    }

    return node;
}

RtMetaData* RTNodeBus::findMetadata(RTNodeInfo *nodeInfo) {
    RT_NODE_TYPE  nType = nodeInfo->mNodeType;
    BUS_LINE_TYPE lType = nodeInfo->mLineType;
    if (lType == BUS_LINE_VIDEO && mBusCtx->mVideoMeta) {
        RT_LOGD("RTNodeBus::findMetadata Video");
        RTCodecID videoType = RT_VIDEO_ID_Unused;
        if (nType == RT_NODE_TYPE_DECODER) {
            mBusCtx->mVideoMeta->findInt32(kKeyCodecID, reinterpret_cast<INT32 *>(&videoType));
        }
        if (videoType != RT_VIDEO_ID_Unused || nType == RT_NODE_TYPE_SINK) {
            return mBusCtx->mVideoMeta;
        }
    } else if (lType == BUS_LINE_AUDIO && mBusCtx->mAudioMeta) {
        RT_LOGD("RTNodeBus::findMetadata Audio");
        RTCodecID audioType = RT_AUDIO_ID_Unused;
        if (nType == RT_NODE_TYPE_DECODER) {
            mBusCtx->mAudioMeta->findInt32(kKeyCodecID, reinterpret_cast<INT32 *>(&audioType));
        }
        if (audioType != RT_AUDIO_ID_Unused || nType == RT_NODE_TYPE_SINK) {
            return mBusCtx->mAudioMeta;
        }
    }
    return RT_NULL;
}

RT_RET RTNodeBus::releaseNodes() {
    int i;
    for (i = 0; i < BUS_LINE_MAX; i++) {
        RTNode* pHead = mBusCtx->mRootNodes[i];
        RTNode* pNode = RT_NULL;
        // run commands in all nodes of active node-chain
        if (pHead == RT_NULL) {
            continue;
        }
        while (pHead->mNext != RT_NULL) {
            pNode = pHead;
            while (pNode->mNext != RT_NULL) {
                pNode = pNode->mNext;
            }
            pNode->mPrev->mNext = RT_NULL;
            rt_safe_delete(pNode);
        }
        rt_safe_delete(pHead);
    }
    if (mBusCtx->mVideoMeta) {
        rt_safe_delete(mBusCtx->mVideoMeta);
        mBusCtx->mVideoMeta = RT_NULL;
    }
    if (mBusCtx->mAudioMeta) {
        rt_safe_delete(mBusCtx->mAudioMeta);
        mBusCtx->mAudioMeta = RT_NULL;
    }
    mBusCtx->mDemuxer = RT_NULL;
    clearNodeBus();

    return RT_OK;
}

RT_RET RTNodeBus::nodeChainAppend(RTNode *pNode, BUS_LINE_TYPE lType) {
    RT_ASSERT(RT_NULL != mBusCtx);
    if ((RT_NULL == mBusCtx) || (NULL == pNode)) {
        // RT_LOGE("%-16s -> invalid RTNode(0x%p)", mBusLineNames[lType].name, pNode);
        return RT_ERR_NULL_PTR;
    }

    RTNode **nRoot = RT_NULL;
    nRoot = &(mBusCtx->mRootNodes[lType]);
    pNode->mNext = RT_NULL;
    pNode->mPrev = RT_NULL;
    if (RT_NULL == *nRoot) {
        *nRoot = pNode;
    } else {
        (*nRoot)->mNext = pNode;
        pNode->mPrev = *nRoot;
    }
    RT_LOGE("%-16s -> add RTNode(ptr=0x%p, name=%s)", mBusLineNames[lType].name,
               pNode, pNode->queryStub()->mNodeName);

    return RT_OK;
}

RT_RET RTNodeBus::nodeChainDumper(BUS_LINE_TYPE lType) {
    RT_ASSERT(RT_NULL != mBusCtx);

    RTNode* nRoot = mBusCtx->mRootNodes[lType];
    if (RT_NULL != nRoot) {
        RTNode* rtNode = nRoot;
        RT_LOGE("%-16s -> dump all nodes", mBusLineNames[lType].name);
        while (RT_NULL != rtNode) {
            RT_LOGD("%-12s { name:%-18s ptr:0x%p }",
                     rt_node_type_name(rtNode->queryStub()->mNodeType),
                     rtNode->queryStub()->mNodeName, rtNode);
            rtNode = rtNode->mNext;
        }
        RT_LOGE("\r\n...");
    }

    return RT_OK;
}

RT_RET RTNodeBus::nodeChainDriver(RTNode *pNode, BUS_LINE_TYPE lType) {
    RTMediaBuffer *pMediaBuf;
    while (RT_NULL != pNode) {
        RTNodeAdapter::pushBuffer(pNode, pMediaBuf);
        if (RT_NULL != pNode->mNext) {
            RTNodeAdapter::pullBuffer(pNode->mNext, &pMediaBuf);
        }
        pNode = pNode->mNext;
    }
    return RT_OK;
}

RT_RET RTNodeBus::excuteCommand(RT_NODE_CMD cmd, RtMetaData *option) {
    RT_LOGD("node_bus delivers %s to active nodes", rt_node_cmd_name(cmd));

    RTNodeAdapter::runCmd(mBusCtx->mRootNodes[BUS_LINE_ROOT],   cmd, option);
    RTNodeAdapter::runCmd(mBusCtx->mRootNodes[BUS_LINE_VIDEO],  cmd, option);
    RTNodeAdapter::runCmd(mBusCtx->mRootNodes[BUS_LINE_AUDIO],  cmd, option);
    RTNodeAdapter::runCmd(mBusCtx->mRootNodes[BUS_LINE_SUBTE],  cmd, option);
    RT_LOGD("node_bus delivers %s to active nodes done!!!!!!\r\n", rt_node_cmd_name(cmd));
    return RT_OK;
}

RT_RET RTNodeBus::setMemAllocator(RtMetaData *option) {
    option->setPointer(kKeyMemAllocator, (RT_PTR)mBusCtx->mLinearAllocator);
    return RT_OK;
}


RT_RET RTNodeBus::clearNodeBus() {
    // @review: nodes be released when reset, NEED to clear root nodes.
    mBusCtx->mRootNodes[BUS_LINE_ROOT]  = RT_NULL;
    mBusCtx->mRootNodes[BUS_LINE_VIDEO] = RT_NULL;
    mBusCtx->mRootNodes[BUS_LINE_AUDIO] = RT_NULL;
    mBusCtx->mRootNodes[BUS_LINE_SUBTE] = RT_NULL;
    return RT_OK;
}


RT_BOOL check_setting(RTMediaUri *setting) {
    // @TODO NEED more checks...
    if (RT_NULL == setting->mUri) {
        return RT_FALSE;
    }
    return RT_TRUE;
}

RTNode* bus_find_and_add_demuxer(RTNodeBus *pNodeBus, RTMediaUri *setting) {
    RTNodeStub *nStub   = &ff_node_demuxer;
    RTNode     *demuxer = RT_NULL;
    RtMetaData *pMeta   = RT_NULL;
    RT_RET      err     = RT_OK;
    // init node demuxer
    if (RT_TRUE == check_setting(setting)) {
        demuxer = nStub->mCreateNode();
        pMeta   =  new RtMetaData();
        pMeta->setCString(kKeyFormatUri, setting->mUri);
        pMeta->setCString(kKeyUserAgent, setting->mUserAgent);
        err = RTNodeAdapter::init(demuxer, pMeta);
        if (RT_OK != err) {
            // pMeta will delete by demuxer
            rt_safe_delete(demuxer);
            return RT_NULL;
        }
        pNodeBus->registerNode(demuxer);
    } else {
        RT_LOGE("Fail to create demxuer(uri=0x%p), invalid par...", setting->mUri);
    }

    return demuxer;
}

RTNode* bus_find_and_add_codec(RTNodeBus *pNodeBus, RTNode *demuxer, \
                         RTTrackType tType, BUS_LINE_TYPE lType) {
    if (RT_NULL == pNodeBus) {
        RT_LOGE("%-16s -> invalid demuxer, can't create codec", mBusLineNames[lType].name);
        return RT_NULL;
    }

    RtMetaData *node_meta   = RT_NULL;
    RTNodeStub *node_stub   = RT_NULL;
    RTNode     *node_codec  = RT_NULL;
    RT_RET      err         = RT_OK;

    if (RT_NULL != demuxer) {
        RTNodeDemuxer *pDemuxer = reinterpret_cast<RTNodeDemuxer*>(demuxer);
        INT32       track_idx   = pDemuxer->queryTrackUsed(tType);
        if (BUS_LINE_AUDIO == lType) {
            RT_LOGE("%-16s -> audio track (id=%d)", mBusLineNames[lType].name, track_idx);
        }

        if (track_idx >= 0) {
            node_meta = pDemuxer->queryTrackMeta(track_idx, tType);
            rt_utils_dump_track(node_meta);
        }
    } else {
        RTNodeInfo nodeInfo;
        nodeInfo.mNodeType = RT_NODE_TYPE_DECODER;
        nodeInfo.mLineType = lType;
        node_meta = pNodeBus->findMetadata(&nodeInfo);
        RT_LOGD("node_meta = %p", node_meta);
    }

    if (RT_NULL != node_meta) {
        // @TODO create codec by MIME
        RTNodeInfo nodeInfo;
        nodeInfo.mNodeType = RT_NODE_TYPE_DECODER;
        nodeInfo.mLineType = lType;
        node_stub = pNodeBus->findStub(&nodeInfo);
        if (RT_NULL != demuxer) {
            pNodeBus->setMemAllocator(node_meta);
        }
    }

    if (RT_NULL != node_stub) {
        node_codec = node_stub->mCreateNode();
    }

    if (RT_NULL != node_codec) {
         err = RTNodeAdapter::init(node_codec, node_meta);
        if (RT_OK != err) {
            rt_safe_delete(node_meta);
            rt_safe_delete(node_codec);
            return RT_NULL;
        }
        pNodeBus->registerNode(node_codec);
    } else {
        RT_LOGE("%-16s -> invalid codec()", mBusLineNames[lType].name);
    }
    return node_codec;
}

RTNode* bus_find_and_add_sink(RTNodeBus *pNodeBus, RTNode *codec, BUS_LINE_TYPE lType) {
    RT_RET      err         = RT_OK;

    if (RT_NULL == pNodeBus) {
        RT_LOGE("%-16s -> invalid codec, can't create sink", mBusLineNames[lType].name);
        return RT_NULL;
    }

    RtMetaData *nMeta = RT_NULL;
    if (codec != RT_NULL) {
        nMeta = codec->queryFormat(RT_PORT_OUTPUT);
    } else {
        RTNodeInfo nodeInfo;
        nodeInfo.mNodeType = RT_NODE_TYPE_SINK;
        nodeInfo.mLineType = lType;
        nMeta = pNodeBus->findMetadata(&nodeInfo);
    }

    // @TODO create codec by MIME
    RTNodeStub *nStub = findStub(RT_NODE_TYPE_SINK, lType);
    RTNode     *nSink = (RT_NULL != nStub)?nStub->mCreateNode():RT_NULL;

    if ((RT_NULL != nSink) && (RT_NULL != nMeta)) {
        err = RTNodeAdapter::init(nSink, nMeta);
        if (RT_OK != err) {
            rt_safe_delete(nMeta);
            rt_safe_delete(nSink);
            return RT_NULL;
        }
        pNodeBus->registerNode(nSink);
        rt_utils_dump_track(nMeta);
    } else {
        RT_LOGE("%-16s -> valid codec, but found no sink", mBusLineNames[lType].name);
    }
    return nSink;
}
