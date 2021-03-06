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
 * author: martin.cheng
 *   date: 2018/07/05
 */

#include "RTMemService.h" // NOLINT
#include "rt_os_mem.h" // NOLINT
#include "rt_mem.h" // NOLINT
#include "rt_log.h" // NOLINT

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "RTMemService"

#define MEM_NODE_MAX            (1024)

RTMemService::RTMemService() {
    nodes_max = MEM_NODE_MAX;
    INT32 size = sizeof(MemNode)*nodes_max;
    rt_os_malloc(reinterpret_cast<void **>(&mem_nodes), MEM_ALIGN, size);
    RT_ASSERT(RT_NULL != mem_nodes);

    rt_memset(mem_nodes, 0, sizeof(MemNode)*nodes_max);
    addNode(__FUNCTION__, mem_nodes, size);

    addNode(__FUNCTION__, this, sizeof(RTMemService));
}

RTMemService::~RTMemService() {
}

void RTMemService::addNode(const char *caller, void* ptr, UINT32 size) {
    RT_ASSERT(RT_NULL != mem_nodes);

    MemNode *node = mem_nodes;
    for (UINT32 i = 0; i < nodes_max; i++, node++) {
        if (node->size == 0) {
            node->caller = caller;
            node->ptr    = ptr;
            node->size   = size;
            nodes_cnt++;
            total_size += size;
            break;
        }
    }
}

void RTMemService::removeNode(void* ptr, UINT32 *size) {
    RT_ASSERT(RT_NULL != mem_nodes);

    MemNode *node = mem_nodes;
    for (UINT32 i = 0; i < nodes_max; i++, node++) {
        if ((node->size > 0) && (node->ptr == ptr)) {
            *size = node->size;
            nodes_cnt--;
            total_size -= node->size;
            node->size = 0;
            break;
        }
    }
}

void RTMemService::reset() {
    RT_ASSERT(RT_NULL != mem_nodes);

    MemNode *node = mem_nodes;
    for (UINT32 i = 0; i < nodes_max; i++, node++) {
       node->size   = 0;
       node->ptr    = RT_NULL;
       node->caller = RT_NULL;
    }
    nodes_cnt  = 0;
    total_size = 0;
}

void RTMemService::dump() {
    RT_ASSERT(RT_NULL != mem_nodes);

    RT_LOGE("======= Rockit Memory Summary =======");
    MemNode *node = mem_nodes;
    RT_LOGE("Memory Tatal:%dk, nodes:%d/%d",
                total_size/1024, nodes_cnt, nodes_max);
    for (UINT32 i = 0; i < nodes_max; i++, node++) {
        if (node->size > 0) {
            RT_LOGE("Memory Node:Ptr:%p; Size=%04d; caller=%s",
                     node->ptr, node->size, node->caller);
        }
    }
}

INT32 RTMemService::findNode(const char *caller, void* ptr, UINT32*size) {
    MemNode *node = mem_nodes;

    for (UINT32 i = 0; i < nodes_max; i++, node++) {
        if (node->ptr == ptr) {
            RT_LOGE("nodes[%03d] is found, ptr=%p", i, ptr);
            return i;
        }
    }
    return -1;
}
