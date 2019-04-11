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
 *   date: 2019/04/11
 */

#include "rt_header.h"          // NOLINT
#include "rt_time.h"            // NOLINT
#include "rt_thread.h"          // NOLINT

#include "player_test_utils.h"  // NOLINT
#include "RTNDKMediaPlayer.h"   // NOLINT

#include "rt_check.h"           // NOLINT
#include "RTObject.h"           // NOLINT

static int cmd_count = 0;

typedef struct testThreadParam {
    RTNDKMediaPlayer *player;
    const char *uri;
} testThreadParam;

#define MAX_TEST_COUNT 1000000
void *control_stream_loop_xa(void* param) {
    testThreadParam *arg = reinterpret_cast<testThreadParam *>(param);
    while (cmd_count < MAX_TEST_COUNT) {
        arg->player->reset();
        arg->player->setDataSource(arg->uri, RT_NULL);
        arg->player->prepare();
        arg->player->start();
        cmd_count++;
        RtTime::sleepMs(RtTime::randInt()%200 + 50);
    }
    return RT_NULL;
}

void *control_stream_loop_xb(void* param) {
    testThreadParam *arg = reinterpret_cast<testThreadParam *>(param);
    while (cmd_count < MAX_TEST_COUNT) {
        arg->player->reset();
        arg->player->setDataSource(arg->uri, RT_NULL);
        arg->player->prepare();
        arg->player->start();
        cmd_count++;
        RtTime::sleepMs(RtTime::randInt()%200 + 50);
    }
    return RT_NULL;
}

RT_RET unit_test_player_switch_rand(const char* uri1, const char* uri2, bool rand) {
    RtThread *cmdThreadA = RT_NULL;
    RtThread *cmdThreadB = RT_NULL;
    testThreadParam param1;
    testThreadParam param2;
    RTNDKMediaPlayer*  ndkPlayer = new RTNDKMediaPlayer();

    ndkPlayer->setDataSource(uri1, RT_NULL);
    ndkPlayer->prepare();
    ndkPlayer->start();

    param1.player = ndkPlayer;
    param1.uri = uri1;
    param2.player = ndkPlayer;
    param2.uri = uri2;
    // simulation of control distribution
    RtTime::sleepMs(1000);
    if (rand) {
        cmd_count  = 0;
        cmdThreadA = new RtThread(control_stream_loop_xa, &param1);
        cmdThreadA->setName("cmd_rand1");
        cmdThreadA->start();
        cmdThreadB = new RtThread(control_stream_loop_xb, &param2);
        cmdThreadB->setName("cmd_rand2");
        cmdThreadB->start();
    }

    // wait util of playback complete
    ndkPlayer->wait(10000000000000);
    while (1) {
        RtTime::sleepMs(5);
    }
    cmd_count = MAX_TEST_COUNT;
    ndkPlayer->pause();
    ndkPlayer->stop();
    ndkPlayer->reset();

    rt_safe_delete(ndkPlayer);
    rt_safe_delete(cmdThreadB);
    rt_safe_delete(cmdThreadA);
    return RT_OK;
}

int main(int argc, char **argv) {
    const char* uri1 = NULL;
    const char* uri2 = NULL;
    bool  rand = false;
    switch (argc) {
      case 3:
        uri1 = argv[1];
        uri2 = argv[2];
        rand = true;
        break;
      default:
        RT_LOGE("set param failed!");
        RT_LOGE("Usage:");
        RT_LOGE("./case_player_fast_switch <uri1> <uri2>");
        return 0;
        break;
    }

    rt_mem_record_reset();
    RTObject::resetTraces();

    /* your unit test */
    if (NULL != uri1 && NULL != uri2) {
        unit_test_player_switch_rand(uri1, uri2, rand);
    }

    rt_mem_record_dump();
    RTObject::dumpTraces();
    return 0;
}

