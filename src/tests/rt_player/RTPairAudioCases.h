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
 * author: martin.cheng@rock-chips.com
 *   date: 2019/02/27
 */

#ifndef SRC_TESTS_RT_PLAYER_RTPAIRAUDIOCASES_H_
#define SRC_TESTS_RT_PLAYER_RTPAIRAUDIOCASES_H_

#include "rt_header.h"          // NOLINT
#include "rt_test_header.h"     // NOLINT

#ifdef  __cplusplus
extern "C" {
#endif

RT_RET unit_test_pair_audio_player_case_easy(const char* media_one, const char* media_two);
RT_RET unit_test_pair_audio_player_case_hard(const char* media_one, const char* media_two);

#ifdef  __cplusplus
}
#endif

#endif  // SRC_TESTS_RT_PLAYER_RTPAIRAUDIOCASES_H_