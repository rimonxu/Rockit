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
 *   Date: 2019/04/22
 *
 */

#include <string.h>

#include "rt_hash_table.h"    // NOLINT
#include "RTPersistObject.h"  // NOLINT

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "PersistObject"

struct object_item {
    INT32 mUUID;
    INT32 mKey;
    void* mObject;
    PERSIST_FREE mFree;
};

static void* sPersistHash = RT_NULL;
void  persist_object_register(INT32 uuid, void* object, RTObjKey key, PERSIST_FREE func_free) {
    if (RT_NULL == sPersistHash) {
        sPersistHash = rt_hash_table_create(12, hash_ptr_func, hash_ptr_compare);
    }
    struct RtHashTable *hash = reinterpret_cast<struct RtHashTable*>(sPersistHash);
    if (RT_NULL == persist_object_find(uuid, key)) {
        struct object_item* item = rt_malloc(object_item);
        item->mUUID      = uuid;
        item->mKey       = key;
        item->mObject    = object;
        item->mFree      = func_free;
        rt_hash_table_insert(hash, reinterpret_cast<void*>(key),
                                 reinterpret_cast<void*>(item));
    }
}

void  persist_object_delete(INT32 uuid, RTObjKey key) {
    if (RT_NULL == sPersistHash) {
        return;
    }

    struct object_item*  item     = NULL;
    struct rt_hash_node* hashItem = RT_NULL;
    struct rt_hash_node* lastItem = RT_NULL;
    struct RtHashTable*  hash     = reinterpret_cast<struct RtHashTable*>(sPersistHash);
    lastItem = rt_hash_table_find_root(hash, reinterpret_cast<void*>(key));
    for (hashItem = lastItem->next; hashItem != RT_NULL; hashItem = hashItem->next) {
        item = reinterpret_cast<object_item*>(hashItem->data);
        if (uuid == item->mUUID) {
            lastItem->next = hashItem->next;
            if (RT_NULL != item->mFree) {
                item->mFree(item->mObject);
            } else {
                RT_LOGE("error, has none func_free");
            }
            rt_safe_free(item);
            rt_safe_free(hashItem);
            break;
        }
        lastItem = hashItem;
    }
}

void* persist_object_find(INT32 uuid, RTObjKey key) {
    if (RT_NULL == sPersistHash) {
        return RT_NULL;
    }
    void                *persist  = RT_NULL;
    struct object_item  *item     = RT_NULL;
    struct rt_hash_node *hashItem = RT_NULL;
    struct rt_hash_node *rootItem = RT_NULL;
    struct RtHashTable  *hash     = reinterpret_cast<struct RtHashTable*>(sPersistHash);
    rootItem = rt_hash_table_find_root(hash, reinterpret_cast<void*>(key));
    for (hashItem = rootItem->next; hashItem != RT_NULL; hashItem = hashItem->next) {
        item = reinterpret_cast<object_item*>(hashItem->data);
        if (uuid == item->mUUID) {
            persist = item->mObject;
            break;
        }
    }
    return persist;
}

class PersistFake {
 public:
    explicit PersistFake(int uuid) { mUUID = uuid; RT_LOGE("+PersistFake +key=%d", mUUID);}
    ~PersistFake() { RT_LOGE("~PersistFake ~key=%d", mUUID);}
 private:
    int mUUID;
};

#define UUID1 3333
#define UUID2 6666

void PersistFakeFree(void* raw_ptr) {
    PersistFake* persist = reinterpret_cast<PersistFake*>(raw_ptr);
    rt_safe_delete(persist);
}

void persist_object_unit_test() {
    PersistFake* fake1 = new PersistFake(UUID1);
    PersistFake* fake2 = new PersistFake(UUID2);

    persist_object_register(UUID1, fake1, RT_OBJ_TYPE_ALSA, &PersistFakeFree);
    persist_object_register(UUID2, fake2, RT_OBJ_TYPE_ALSA, &PersistFakeFree);

    void* find_ptr1 = persist_object_find(UUID1, RT_OBJ_TYPE_ALSA);
    void* find_ptr2 = persist_object_find(UUID2, RT_OBJ_TYPE_ALSA);
    RT_LOGE("register and find: src_obj = %p, find_str = %p", fake1, find_ptr1);
    RT_LOGE("register and find: src_obj = %p, find_str = %p", fake2, find_ptr2);

    persist_object_delete(UUID1, RT_OBJ_TYPE_ALSA);
    persist_object_delete(UUID2, RT_OBJ_TYPE_ALSA);
    find_ptr1 = persist_object_find(UUID1, RT_OBJ_TYPE_ALSA);
    find_ptr2 = persist_object_find(UUID2, RT_OBJ_TYPE_ALSA);
    RT_LOGE("delete and find: src_obj = %p, find_str = %p", fake1, find_ptr1);
    RT_LOGE("delete and find: src_obj = %p, find_str = %p", fake2, find_ptr2);
}
