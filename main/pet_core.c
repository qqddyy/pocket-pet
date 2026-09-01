// main/pet_core.c —— 口袋宠物核心逻辑实现。
// 数值曲线与文案互相独立:改数值只动这里。
#include "pet_core.h"

#include <string.h>

// ---- 自然节奏(秒/点)----
#define HUNGER_DECAY_S 20    // 饱食每 20 秒 -1(100→0 约 33 分钟)
#define CLEAN_DECAY_S  45    // 清洁每 45 秒 -1(约 75 分钟)
#define MOOD_DECAY_S   35    // 心情每 35 秒 -1
#define MOOD_BAD_S     15    // 饥饿/脏污(<20)时,心情每 15 秒再 -1
#define GROW_PASSIVE_S 12    // 三维都 >=50 时,每 12 秒成长 +1

// ---- 互动收益 ----
#define FEED_HUNGER 30
#define CLEAN_CLEAN 35
#define PAT_MOOD    12
#define FEED_GROW   4
#define CLEAN_GROW  3
#define PAT_GROW    2
#define ACT_MOOD    4    // 喂食/清洁附带的心情小幅回升
#define FULL_LIMIT  95   // 高于此值视为"已满",互动无效

// 每阶段配置。索引必须与 pet_stage_t 枚举一致。
// need: 本阶段进化所需成长值(成年无下一阶段,仅作成长条上限展示)
typedef struct {
    const char *name;
    uint32_t    need;
} stage_cfg_t;

static const stage_cfg_t S_TABLE[PET_STAGE_COUNT] = {
    {"蛋",   50},     // 十几次互动/几分钟细心照看即孵化
    {"幼年", 300},    // 需要持续照料一段时间才长大
    {"成年", 1000},   // 最终形态,继续攒成长仅作陪伴记录
};

// xorshift32,线性同余家族里最简单的一档;仅供游戏随机,不作安全用途。
static uint32_t s_rng = 0x2F6E2B1u;

static void clamp_int(int *v, int lo, int hi)
{
    if (*v < lo) *v = lo;
    if (*v > hi) *v = hi;
}

uint32_t pet_rand(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

const char *pet_stage_name(pet_stage_t s) { return S_TABLE[s].name; }
uint32_t    pet_stage_need(pet_stage_t s) { return S_TABLE[s].need; }

void pet_reset(pet_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->stage  = PET_EGG;
    st->hunger = 80;
    st->clean  = 90;
    st->mood   = 70;
    s_rng      = 0x2F6E2B1u;
}

// 进化判定:growth 达到本阶段上限 → 下一阶段(成年封顶)
static void try_evolve(pet_state_t *st)
{
    if (st->stage == PET_ADULT) return;
    if (st->growth >= S_TABLE[st->stage].need) {
        st->stage++;
        st->growth = 0;
    }
}

void pet_tick(pet_state_t *st)
{
    st->age_s++;

    // 状态自然衰减(整数分频,避免浮点)
    if (++st->t_hunger >= HUNGER_DECAY_S) { st->t_hunger = 0; if (st->hunger > 0) st->hunger--; }
    if (++st->t_clean  >= CLEAN_DECAY_S)  { st->t_clean  = 0; if (st->clean  > 0) st->clean--; }
    if (++st->t_mood   >= MOOD_DECAY_S)   { st->t_mood   = 0; if (st->mood   > 0) st->mood--; }
    // 又饿又脏时,心情加速变差(提醒主人该照顾了)
    if ((st->hunger < 20 || st->clean < 20) && ++st->t_mood_bad >= MOOD_BAD_S) {
        st->t_mood_bad = 0;
        if (st->mood > 0) st->mood--;
    }

    // 被动成长:被照顾得好(三维>=50)时缓慢成长;照看蛋同样有效
    if (st->hunger >= 50 && st->clean >= 50 && st->mood >= 50) {
        if (++st->t_grow >= GROW_PASSIVE_S) {
            st->t_grow = 0;
            st->growth++;
        }
    }

    try_evolve(st);
}

pet_result_t pet_do_action(pet_state_t *st, pet_action_t act, const char **out_text)
{
    if (out_text) *out_text = "";
    const bool is_egg = (st->stage == PET_EGG);
    pet_result_t r = PET_R_OK;

    switch (act) {
    case PET_ACT_FEED:
        if (st->hunger >= FULL_LIMIT) {
            if (out_text) *out_text = "团子已经吃得饱饱的";
            return PET_R_FULL;
        }
        st->hunger += FEED_HUNGER;
        st->mood   += ACT_MOOD;
        st->growth += FEED_GROW;
        st->feed_count++;
        if (out_text) *out_text = is_egg ? "蛋壳被焐得暖暖的" : "团子吃得津津有味";
        break;
    case PET_ACT_CLEAN:
        if (st->clean >= FULL_LIMIT) {
            if (out_text) *out_text = "团子本来就很干净";
            return PET_R_FULL;
        }
        st->clean  += CLEAN_CLEAN;
        st->mood   += ACT_MOOD;
        st->growth += CLEAN_GROW;
        st->clean_count++;
        if (out_text) *out_text = is_egg ? "蛋壳擦得亮晶晶" : "团子洗得干干净净";
        break;
    case PET_ACT_PAT:
        if (st->mood >= FULL_LIMIT) {
            if (out_text) *out_text = "团子眯着眼,已经很满足";
            return PET_R_FULL;
        }
        st->mood   += PAT_MOOD;
        st->growth += PAT_GROW;
        st->pat_count++;
        if (out_text) *out_text = is_egg ? "蛋里传来轻轻的回应" : "团子眯起眼睛很享受";
        break;
    default:
        return PET_R_FULL;
    }

    clamp_int(&st->hunger, 0, 100);
    clamp_int(&st->clean, 0, 100);
    clamp_int(&st->mood, 0, 100);
    try_evolve(st);
    return r;
}
