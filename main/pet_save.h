// main/pet_save.h —— 游戏进度 NVS 持久化。
// 约定:游戏进度(阶段/三维/计数)是运行时数据,只进 NVS,不提交 git;代码/文案进 git。
#pragma once

#include <stdbool.h>

#include "pet_core.h"

#ifdef __cplusplus
extern "C" {
#endif

bool pet_save(const pet_state_t *st);
bool pet_load(pet_state_t *st);
bool pet_has_save(void);

// 声音开关:独立 NVS 键(设置项,与游戏进度 blob 互不影响),无记录时默认开。
bool pet_sound_load(void);
void pet_sound_save(bool on);

#ifdef __cplusplus
}
#endif
