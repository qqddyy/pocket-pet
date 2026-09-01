// main/pet_core.h —— 口袋宠物核心逻辑(纯 C,不依赖 ESP-IDF / LVGL,便于主机测试)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PET_STAGE_COUNT 3

// 成长阶段:蛋 → 幼年 → 成年
typedef enum {
    PET_EGG = 0,
    PET_BABY,
    PET_ADULT,
} pet_stage_t;

// 三种互动(宠物页 OK 执行当前选中的互动)
typedef enum {
    PET_ACT_FEED = 0,   // 喂食
    PET_ACT_CLEAN,      // 清洁
    PET_ACT_PAT,        // 抚摸
    PET_ACT_COUNT,
} pet_action_t;

// 互动结果
typedef enum {
    PET_R_OK = 0,       // 有效互动
    PET_R_FULL,         // 已饱/已净/已满足,本次无效
} pet_result_t;

typedef struct {
    pet_stage_t stage;        // 当前阶段
    int         hunger;       // 饱食 0..100(随时间下降,喂食恢复)
    int         clean;        // 清洁 0..100
    int         mood;         // 心情 0..100
    uint32_t    growth;       // 当前阶段成长值(满则进化)
    uint32_t    age_s;        // 通电累计秒(年龄;无 RTC,断电不流逝,防重启时间错乱)
    uint32_t    feed_count;   // 累计喂食
    uint32_t    clean_count;  // 累计清洁
    uint32_t    pat_count;    // 累计抚摸
    uint8_t     t_hunger;     // 衰减/成长分频计数器(随档保存,掉档不丢节奏)
    uint8_t     t_clean;
    uint8_t     t_mood;
    uint8_t     t_mood_bad;   // 饥饿/脏污加速心情变差的分频计数
    uint8_t     t_grow;
} pet_state_t;

// ---- 阶段配置(常量,定义于 pet_core.c) ----
const char *pet_stage_name(pet_stage_t s);   // "蛋"/"幼年"/"成年"
uint32_t    pet_stage_need(pet_stage_t s);   // 该阶段进化所需成长值(成年为展示上限)

// ---- 状态机 ----
void        pet_reset(pet_state_t *st);      // 开新档:一颗崭新的蛋
void        pet_tick(pet_state_t *st);       // 每秒:状态衰减+被动成长+进化判定
pet_result_t pet_do_action(pet_state_t *st, pet_action_t act,
                           const char **out_text);   // 执行互动,返回提示文案

// 简单确定性 RNG(xorshift32,游戏随机专用)
uint32_t    pet_rand(void);

#ifdef __cplusplus
}
#endif
