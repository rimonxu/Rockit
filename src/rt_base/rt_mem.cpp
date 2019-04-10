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
 *   date: 20180704
 */

#include "rt_header.h" // NOLINT
#include "rt_mem.h" // NOLINT
#include "rt_log.h" // NOLINT
#include "rt_os_mem.h" // NOLINT
#include "RTMemService.h" // NOLINT
#include <string.h>
#include <stdio.h>

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "rt_mem"

#define MEM_DEBUG_EN            (0x00000001)
#define MEM_RUNTIME_LOG         (0x00000002)
#define MEM_NODE_LOG            (0x00000004)
#define MEM_EXT_ROOM            (0x00000010)
#define MEM_POISON              (0x00000020)

// default memory align size is set to 32
#define MEM_MAX_INDEX           (0x7fffffff)
#define MEM_ALIGN_MASK          (MEM_ALIGN - 1)
#define MEM_ALIGNED(x)          (((x) + MEM_ALIGN) & (~MEM_ALIGN_MASK))
#define MEM_HEAD_ROOM(debug)    (((debug) & MEM_EXT_ROOM) ? (MEM_ALIGN) : (0))

static UINT8 debug = 0;
static RTMemService _gMemService;

void *rt_mem_malloc(const char *caller, size_t size) {
    UINT32 size_align = MEM_ALIGNED(size);
    UINT32 size_real = (debug & MEM_EXT_ROOM) ? (size_align + 2 * MEM_ALIGN) :
                       (size_align);
    INT32 err = 0;
    void *ptr = NULL;

    err = rt_os_malloc(&ptr, MEM_ALIGN, size_real);
    if (err) {
        return NULL;
    }
    // TODO(debug) : debug memory
    _gMemService.addNode(caller, ptr, size);

    return ptr;
}

void *rt_mem_calloc(const char *caller, size_t size) {
    void *ptr = rt_mem_malloc(caller, size);
    if (ptr)
        memset(ptr, 0, size);
    return ptr;
}

void *rt_mem_realloc(const char *caller, void *ptr, size_t size) {
    void *ptr_new;

    if (NULL == ptr)
        return rt_mem_malloc(caller, size);

    if (0 == size) {
        RT_LOGE("warning: realloc %p to zero size\n", ptr);
        return NULL;
    }

    size_t size_align = MEM_ALIGNED(size);
    void *ptr_real = reinterpret_cast<UINT8 *>(ptr) - MEM_HEAD_ROOM(debug);

    rt_os_realloc(ptr_real, &ptr_new, MEM_ALIGN, size_align);

    // TODO(debug) : debug memory
    UINT32 old_size;
    _gMemService.removeNode(ptr, &old_size);
    _gMemService.addNode(caller, ptr_new, size);

    return ptr_new;
}

void rt_mem_free(const char *caller, void *ptr) {
    if (NULL == ptr)
        return;
    if (ptr < reinterpret_cast<void*>(0xFFF)) {
        RT_LOGE("rt_mem_free caller=%s, ptr=%p", caller, ptr);
        return;
    }

    rt_os_free(ptr);

    // TODO(debug) : debug memory
    UINT32 size;
    _gMemService.removeNode(ptr, &size);

    return;
}

void rt_mem_safe_free(const char *caller, void **ptr) {
    if (ptr && (*ptr)) {
        rt_mem_free(caller, *ptr);
        *ptr = RT_NULL;
    }
}

void *rt_memset(void *str, int c, size_t n) {
    return memset(str, c, n);
}

void *rt_memcpy(void *dst, const void *src, size_t n) {
    return memcpy(dst, src, n);
}

void rt_mem_record_dump() {
    // TODO(debug) : debug memory
    _gMemService.dump();

    return;
}

void rt_mem_record_reset() {
    // TODO(debug) : debug memory
    _gMemService.reset();

    return;
}
