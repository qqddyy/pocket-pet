// main/pet_ui.c —— 界面实现。
// 内存策略:C3 无 PSRAM,LVGL 池有限 → 单一根屏,页面对象按需创建/销毁,同一时刻只活一页。
// 文本一律 UTF-8 中文字面量,由 font_game_22(思源黑体子集)渲染,缺失字形会画空白。
// 宠物形象用 LVGL 基础图元(圆/胶囊/弧)拼装:蛋=奶油色胶囊+斑点,幼年/成年=圆团子+
// 眼睛+心情嘴形(微笑弧/平嘴/沮丧弧),阶段或心情档位变化时才重建,避免闪烁。
#include "pet_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsp_display.h"
#include "lvgl.h"

LV_FONT_DECLARE(font_game_22);

#define UI_W 240
#define UI_H 320

// 中文字体是 22px,行高按 26px 留白排布
#define LINE_H 26

// 宠物配色
#define C_BODY   0xFFCF7D   // 团子身体暖黄
#define C_EGG    0xF2E6C9   // 蛋壳奶油色
#define C_SPOT   0xDCC9A0   // 蛋壳斑点
#define C_FACE   0x4A3728   // 眼睛/嘴深棕

static lv_obj_t            *s_root;      // 常驻根屏
static lv_obj_t            *s_page;      // 当前页对象组(置于 root 上)
static game_page_t          s_page_id = PAGE_TITLE;
static const pet_state_t   *s_st;
static lv_timer_t          *s_banner_timer;
static int                  s_sel;
static bool                 s_sound_on = true;   // 声音开关(仅显示;播放控制在 app)

// 页面内各对象句柄(仅活动页有效)
static lv_obj_t *p_title, *p_banner, *p_hint;
static lv_obj_t *act_rows[PET_ACT_COUNT];
static lv_obj_t *stat_bar[3];
static lv_obj_t *status_rows[9];
static lv_obj_t *menu_rows[5];

// 宠物形象部件池(都挂在 s_face 容器上;嘴固定是最后一个,便于单独换表情)
#define FACE_PART_MAX 6
static lv_obj_t *f_parts[FACE_PART_MAX];
static int       f_part_cnt;
static int       s_face_stage = -1;   // -1 = 未建
static int       s_mood_tier  = -1;   // -1 = 未建(蛋无表情);2 微笑 1 平嘴 0 沮丧

// 动效:形象整体挂在 s_face 容器上,移动/旋转只动这一个对象;
// timer 定随机节奏,lv_anim 在 LVGL 任务上下文(持锁)跑,创建/删除均在锁内路径。
static lv_obj_t   *s_face;              // 形象容器
static lv_timer_t *s_walk_timer;        // 待机溜达节拍(幼年/成年)
static lv_timer_t *s_blink_timer;       // 随机眨眼节拍(幼年/成年)
static lv_timer_t *s_blink_restore;     // 眨眼复原一次性 timer
static lv_timer_t *s_wobble_timer;      // 蛋摇摆节拍(蛋)
static int         s_eye_idx[2];        // 两眼在 f_parts 的下标
static lv_point_t  s_eye_pos[2];        // 两眼左上角坐标(压扁后复原用)
static int16_t     s_eye_d;             // 眼睛直径

static lv_obj_t *label_new(const char *text, int x, int y, int w, lv_align_t align)
{
    lv_obj_t *l = lv_label_create(s_page);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &font_game_22, 0);
    // 深色背景 UI:必须显式白色文本,否则用主题默认深色字=黑屏不可见
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_width(l, w);
    lv_obj_align(l, align, x, y);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

// 根治"数据行叠行":LVGL 的 DOT 截断只对固定高度 label 生效(lv_label.c 要求
// size.y > label 高,而高度自适应的 label 永不满足),靠它必复发。
// 这里在源头解决:实测文本像素宽,超宽就按 UTF-8 边界截尾加"…"——无论数值
// 多长,单行永远放得下,叠行在物理上不可能发生。
static void fit_text(char *buf, size_t cap, int32_t max_w)
{
    // ⚠ max_width 必须给大值:lv_text_get_next_line 里 `while(... max_width>0)`
    // 对 0 会一行都不进,量宽恒为 0,截断逻辑整个失效。
    const int32_t INF_W = 100000;
    lv_point_t sz;
    lv_txt_get_size(&sz, buf, &font_game_22, 0, 0, INF_W, LV_TEXT_FLAG_NONE);
    if (sz.x <= max_w) return;   // 放得下,原样

    const size_t dlen = strlen("\xE2\x80\xA6");   // "…" UTF-8 3 字节
    size_t len = strlen(buf);
    while (len > dlen) {
        // 回退一个 UTF-8 字符(跳过 continuation bytes 0b10xxxxxx)
        size_t cut = len - 1;
        while (cut > 0 && ((uint8_t)buf[cut] & 0xC0) == 0x80) cut--;
        // 若刚好是"…"本身则连它一起删
        if (cut >= dlen && strncmp(buf + cut, "\xE2\x80\xA6", dlen) == 0) cut -= dlen;
        len = cut;
        if (len + dlen + 1 > cap) { len = cap - dlen - 1; }
        memcpy(buf + len, "\xE2\x80\xA6", dlen);
        buf[len + dlen] = 0;
        lv_txt_get_size(&sz, buf, &font_game_22, 0, 0, INF_W, LV_TEXT_FLAG_NONE);
        if (sz.x <= max_w) return;
        len = strlen(buf) - dlen;   // 去掉"…"继续缩
    }
    // 兜底:全删只剩"…"
    snprintf(buf, cap, "\xE2\x80\xA6");
}

static lv_obj_t *page_new(void);   // 前向声明,见 pet_ui_init 之后

// 统一"高亮当前选项、其余置灰"的小工具:rows 为选项 label 数组
static void paint_select(lv_obj_t *const *rows, int count, int sel)
{
    for (int i = 0; i < count; i++) {
        if (!rows[i]) continue;
        lv_obj_set_style_text_color(rows[i], lv_palette_main(i == sel ? LV_PALETTE_AMBER : LV_PALETTE_GREY), 0);
        lv_obj_set_style_text_opa(rows[i], i == sel ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

// 页面标题(每页第一行):label 虽 TOP_MID 满宽,但文本默认左对齐,
// 必须再设 text_align=CENTER 文字才真正水平居中
static lv_obj_t *title_new(const char *text, int y)
{
    lv_obj_t *l = label_new(text, 0, y, UI_W, LV_ALIGN_TOP_MID);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    return l;
}

// ---------------- 宠物形象(基础图元拼装) ----------------

// 实心圆(自动登记进部件池,挂 s_face 容器)
static lv_obj_t *circle_new(int cx, int cy, int d, uint32_t color)
{
    if (f_part_cnt >= FACE_PART_MAX) return NULL;
    lv_obj_t *o = lv_obj_create(s_face);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    f_parts[f_part_cnt++] = o;
    return o;
}

// 胶囊形(蛋壳:宽为直径、高更高的两端全圆)
static lv_obj_t *oval_new(int cx, int cy, int w, int h, uint32_t color)
{
    if (f_part_cnt >= FACE_PART_MAX) return NULL;
    lv_obj_t *o = lv_obj_create(s_face);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, cx - w / 2, cy - h / 2);
    lv_obj_set_style_radius(o, w / 2, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    f_parts[f_part_cnt++] = o;
    return o;
}

// 嘴形:tier 2=微笑弧(U) 1=平嘴 0=沮丧弧(∩)。弧用 lv_arc 的背景弧段画。
static lv_obj_t *mouth_new(int cx, int cy, int d, int tier)
{
    if (f_part_cnt >= FACE_PART_MAX) return NULL;
    if (tier == 1) {   // 平嘴:小横条
        lv_obj_t *o = lv_obj_create(s_face);
        lv_obj_set_size(o, d * 8 / 20, 4);
        lv_obj_set_pos(o, cx - d * 8 / 40, cy - 2);
        lv_obj_set_style_radius(o, 2, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(C_FACE), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        lv_obj_set_style_pad_all(o, 0, 0);
        f_parts[f_part_cnt++] = o;
        return o;
    }
    lv_obj_t *a = lv_arc_create(s_face);
    lv_obj_set_size(a, d, d);
    lv_obj_set_pos(a, cx - d / 2, cy - d / 2);
    lv_arc_set_rotation(a, 0);
    // LVGL 角度:0=3点方向,顺时针增。20..160 过底部(90)→U 形笑;200..340 过顶部(270)→∩ 形沮丧
    if (tier == 2) lv_arc_set_bg_angles(a, 20, 160);
    else           lv_arc_set_bg_angles(a, 200, 340);
    lv_arc_set_range(a, 0, 100);
    lv_arc_set_value(a, 0);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(a, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(C_FACE), LV_PART_MAIN);
    // 只要背景弧:进度弧与旋钮全部隐掉
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    f_parts[f_part_cnt++] = a;
    return a;
}

static int mood_tier_of(int mood)
{
    return mood >= 70 ? 2 : (mood >= 40 ? 1 : 0);
}

// ---------------- 动效(anim exec 回调运行于 LVGL 任务,lvgl_port 已持锁) ----------------
// 设计:所有位移/旋转只动 s_face 容器一个对象,部件随容器整体动。
// anim 以 (var, exec_cb) 为身份,不同动效用不同 exec_cb,可精确增删不误伤。

// 溜达 x:线性位移(速度 40px/s),ready 里收掉步态蹦跳
static void walk_x_exec_cb(void *var, int32_t v) { lv_obj_set_x((lv_obj_t *)var, v); }

// 溜达 y:单次 260ms 一蹦(0→-6→0,sin 半波),无限重复
static void walk_y_exec_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, (int32_t)(-6.0f * sinf((float)M_PI * v / 100.0f)));
}

static void walk_x_ready_cb(lv_anim_t *a)
{
    lv_anim_delete(a->var, walk_y_exec_cb);   // 到站,停蹦
    lv_obj_set_y((lv_obj_t *)a->var, 0);
}

// 开心蹦:0→-14→0,350ms(互动成功反馈)
static void react_exec_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, (int32_t)(-14.0f * sinf((float)M_PI * v / 100.0f)));
}
static void react_ready_cb(lv_anim_t *a) { lv_obj_set_y((lv_obj_t *)a->var, 0); }

// 蛋摇摆:v 为 0..100 进度(700ms)。⚠ 不用容器旋转——transform 走 LVGL 软件分层
// 渲染(ARGB 中间层+逐条旋转混合),240x170 区域在 C3+40KB 池上直接把渲染任务拖死
// (实测整机失去响应)。改用平移摇晃:左右摆 ±4px 两周期 + 中途微抬 3px,
// 与溜达同款 invalidate 机制,开销恒定。
static int32_t s_wobble_base_x;   // 摇摆起始 x(结束后归位)

static void wobble_exec_cb(void *var, int32_t v)
{
    const float t  = v / 100.0f;
    const int sway = (int32_t)(4.0f * sinf(4.0f * (float)M_PI * t));          // 左右两摆
    const int lift = (int32_t)(-1.5f * (1.0f - cosf(2.0f * (float)M_PI * t))); // 单次微抬
    lv_obj_set_x((lv_obj_t *)var, s_wobble_base_x + sway);
    lv_obj_set_y((lv_obj_t *)var, lift);
}
static void wobble_ready_cb(lv_anim_t *a)
{
    lv_obj_set_x((lv_obj_t *)a->var, s_wobble_base_x);
    lv_obj_set_y((lv_obj_t *)a->var, 0);
}

// 溜达节拍:随机 2.5~5s 发起一次;沮丧(心情<40)趴窝不动
static void walk_timer_cb(lv_timer_t *t)
{
    lv_timer_set_period(t, 2500 + pet_rand() % 2500);
    if (!s_face || !s_st || s_st->mood < 40) return;
    if (lv_anim_get(s_face, walk_x_exec_cb)) return;   // 已在走
    if (lv_anim_get(s_face, react_exec_cb)) return;    // 蹦跳中不打断

    const int32_t cur = lv_obj_get_x(s_face);
    const int32_t target = -60 + (int32_t)(pet_rand() % 121);   // 中心 x ∈ [60,180]
    if (target == cur) return;
    const int32_t dist = target > cur ? target - cur : cur - target;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_face);
    lv_anim_set_values(&a, cur, target);
    lv_anim_set_duration(&a, dist * 1000 / 40);
    lv_anim_set_exec_cb(&a, walk_x_exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_ready_cb(&a, walk_x_ready_cb);
    lv_anim_start(&a);

    // 蹦跳步态与 x 平行跑,到站由 walk_x_ready_cb 收掉
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_face);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_exec_cb(&a, walk_y_exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

// 眨眼复原:眼睛尺寸/位置还原
static void blink_restore_cb(lv_timer_t *t)
{
    for (int i = 0; i < 2; i++) {
        lv_obj_t *e = f_parts[s_eye_idx[i]];
        if (!e) continue;
        lv_obj_set_size(e, s_eye_d, s_eye_d);
        lv_obj_set_pos(e, s_eye_pos[i].x, s_eye_pos[i].y);
    }
    s_blink_restore = NULL;
    lv_timer_delete(t);
}

// 眨眼节拍:随机 2~6s;眼睛压到 2px 高(垂直居中),120ms 后复原
static void blink_timer_cb(lv_timer_t *t)
{
    lv_timer_set_period(t, 2000 + pet_rand() % 4000);
    if (!s_face) return;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *e = f_parts[s_eye_idx[i]];
        if (!e) continue;
        lv_obj_set_size(e, s_eye_d, 2);
        lv_obj_set_pos(e, s_eye_pos[i].x, s_eye_pos[i].y + (s_eye_d - 2) / 2);
    }
    s_blink_restore = lv_timer_create(blink_restore_cb, 120, NULL);
    lv_timer_set_repeat_count(s_blink_restore, 1);
}

// 蛋摇摆节拍:随机 4~8s
static void wobble_timer_cb(lv_timer_t *t)
{
    lv_timer_set_period(t, 4000 + pet_rand() % 4000);
    if (!s_face || lv_anim_get(s_face, wobble_exec_cb)) return;
    s_wobble_base_x = lv_obj_get_x(s_face);   // 蛋不溜达,恒为 0,稳妥起见仍取实际值

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_face);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_exec_cb(&a, wobble_exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_ready_cb(&a, wobble_ready_cb);
    lv_anim_start(&a);
}

// 停掉全部动效节拍与动画(切页/换阶段前必须调,防悬垂:
// LVGL 9 删对象不会自动删挂在其上的用户 anim)
static void anims_stop(void)
{
    if (s_walk_timer)   { lv_timer_delete(s_walk_timer);   s_walk_timer = NULL; }
    if (s_blink_timer)  { lv_timer_delete(s_blink_timer);  s_blink_timer = NULL; }
    if (s_wobble_timer) { lv_timer_delete(s_wobble_timer); s_wobble_timer = NULL; }
    if (s_blink_restore) { lv_timer_delete(s_blink_restore); s_blink_restore = NULL; }
    if (s_face) lv_anim_delete(s_face, NULL);   // x/y 步态/蹦/摇摆全收
}

static void face_destroy(void)
{
    anims_stop();
    if (s_face) {
        lv_obj_delete(s_face);   // 部件随容器一起销毁
        s_face = NULL;
    }
    memset(f_parts, 0, sizeof(f_parts));
    f_part_cnt   = 0;
    s_face_stage = -1;
    s_mood_tier  = -1;
}

// 按阶段重建整张脸(切阶段时调用):先建容器,再拼部件,最后挂动效节拍
static void face_rebuild(pet_stage_t stage, int mood)
{
    face_destroy();
    s_face_stage = (int)stage;

    // 形象容器:透明,只作整体移动/旋转的载体
    s_face = lv_obj_create(s_page);
    lv_obj_set_size(s_face, 240, 170);
    lv_obj_set_pos(s_face, 0, 0);
    lv_obj_set_style_border_width(s_face, 0, 0);
    lv_obj_set_style_pad_all(s_face, 0, 0);
    lv_obj_set_style_radius(s_face, 0, 0);
    lv_obj_set_style_bg_opa(s_face, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_face, LV_OBJ_FLAG_SCROLLABLE);

    if (stage == PET_EGG) {
        // 蛋:奶油色胶囊 + 两枚浅斑;无表情,状态由三维条表达
        oval_new(120, 102, 62, 78, C_EGG);
        circle_new(106, 88, 8, C_SPOT);
        circle_new(134, 114, 8, C_SPOT);
        s_wobble_timer = lv_timer_create(wobble_timer_cb, 4000 + pet_rand() % 4000, NULL);
        return;
    }
    if (stage == PET_BABY) {
        circle_new(120, 102, 72, C_BODY);
        s_eye_idx[0] = f_part_cnt; circle_new(107, 92, 8, C_FACE);
        s_eye_idx[1] = f_part_cnt; circle_new(133, 92, 8, C_FACE);
        s_eye_d = 8;
        s_eye_pos[0].x = 107 - 4; s_eye_pos[0].y = 92 - 4;
        s_eye_pos[1].x = 133 - 4; s_eye_pos[1].y = 92 - 4;
        s_mood_tier = mood_tier_of(mood);
        mouth_new(120, 104, 22, s_mood_tier);
    } else {   // PET_ADULT
        circle_new(120, 104, 92, C_BODY);
        s_eye_idx[0] = f_part_cnt; circle_new(101, 88, 10, C_FACE);
        s_eye_idx[1] = f_part_cnt; circle_new(139, 88, 10, C_FACE);
        s_eye_d = 10;
        s_eye_pos[0].x = 101 - 5; s_eye_pos[0].y = 88 - 5;
        s_eye_pos[1].x = 139 - 5; s_eye_pos[1].y = 88 - 5;
        s_mood_tier = mood_tier_of(mood);
        mouth_new(120, 108, 28, s_mood_tier);
    }
    s_walk_timer  = lv_timer_create(walk_timer_cb, 2500 + pet_rand() % 2500, NULL);
    s_blink_timer = lv_timer_create(blink_timer_cb, 2000 + pet_rand() % 4000, NULL);
}

// 仅换嘴(心情档位变化时;嘴是部件池最后一个,单独换避免整脸重建闪烁)
static void mouth_update(int mood)
{
    if (s_face_stage != PET_BABY && s_face_stage != PET_ADULT) return;
    const int tier = mood_tier_of(mood);
    if (tier == s_mood_tier) return;

    if (f_part_cnt > 0 && f_parts[f_part_cnt - 1]) {
        lv_obj_delete(f_parts[f_part_cnt - 1]);
        f_parts[f_part_cnt - 1] = NULL;
        f_part_cnt--;
    }
    const int d  = (s_face_stage == PET_BABY) ? 22 : 28;
    const int cy = (s_face_stage == PET_BABY) ? 104 : 108;
    s_mood_tier = tier;
    mouth_new(120, cy, d, tier);
}

// ---------------- 各页构建 ----------------
static void build_title(void)
{
    title_new("口  袋  宠  物", 50);
    label_new("点击任意键进入", 0, 130, UI_W, LV_ALIGN_TOP_MID);
    label_new("v0.1.0 · 电子宠物", 0, 280, UI_W, LV_ALIGN_TOP_MID);
}

static void build_pet(void)
{
    p_title = title_new("", 8);

    // 互动选择:三格横排,上下键切换,确定执行
    static const char *acts[PET_ACT_COUNT] = {"喂食", "清洁", "抚摸"};
    for (int i = 0; i < PET_ACT_COUNT; i++) {
        act_rows[i] = label_new(acts[i], 24 + i * 64, 162, 64, LV_ALIGN_TOP_LEFT);
        lv_obj_set_style_text_align(act_rows[i], LV_TEXT_ALIGN_CENTER, 0);
    }

    // 三维状态:标签 + 进度条(颜色区分,精确数值看属性页)
    static const char *names[3] = {"饱食", "清洁", "心情"};
    static const lv_palette_t pal[3] = {LV_PALETTE_ORANGE, LV_PALETTE_BLUE, LV_PALETTE_PINK};
    for (int i = 0; i < 3; i++) {
        label_new(names[i], 14, 190 + i * 26, 48, LV_ALIGN_TOP_LEFT);
        stat_bar[i] = lv_bar_create(s_page);
        lv_obj_set_size(stat_bar[i], 158, 14);
        lv_obj_set_pos(stat_bar[i], 68, 193 + i * 26);
        lv_bar_set_range(stat_bar[i], 0, 100);
        lv_bar_set_value(stat_bar[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(stat_bar[i], lv_palette_main(pal[i]), LV_PART_INDICATOR);
    }

    p_banner = label_new("", 0, 268, UI_W, LV_ALIGN_TOP_MID);
    lv_obj_set_style_text_color(p_banner, lv_palette_main(LV_PALETTE_AMBER), 0);
    p_hint = label_new("双击属性 · 长按菜单", 0, 294, UI_W, LV_ALIGN_TOP_MID);
}

static void build_status(void)
{
    title_new("属  性", 8);
    for (int i = 0; i < 9; i++) {
        // 全局 DOT 截断:任何行超宽只显示"…",绝不换行——行距 27px 装不下第二行
        status_rows[i] = label_new("", 12, 42 + i * 27, UI_W - 24, LV_ALIGN_TOP_LEFT);
    }
    label_new("按确定返回", 0, 290, UI_W, LV_ALIGN_TOP_MID);
}

static void build_menu(void)
{
    title_new("菜  单", 8);
    // 第 3 项"声音"为开关项,文本由 refresh_menu 按当前开关状态动态填写
    static const char *items[] = {"存档", "读档", "", "返回主页", "返回标题"};
    for (int i = 0; i < 5; i++) {
        menu_rows[i] = label_new(items[i], 50, 56 + i * 34, UI_W - 100, LV_ALIGN_TOP_LEFT);
    }
    label_new("上下选择 · 确定 · 长按关闭", 0, UI_H - 40, UI_W, LV_ALIGN_TOP_MID);
}

// ---------------- 页面刷新 ----------------
static void refresh_pet(void)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "团子 · %s", pet_stage_name(s_st->stage));
    lv_label_set_text(p_title, buf);

    if ((int)s_st->stage != s_face_stage) face_rebuild(s_st->stage, s_st->mood);
    else                                  mouth_update(s_st->mood);

    lv_bar_set_value(stat_bar[0], s_st->hunger, LV_ANIM_OFF);
    lv_bar_set_value(stat_bar[1], s_st->clean, LV_ANIM_OFF);
    lv_bar_set_value(stat_bar[2], s_st->mood, LV_ANIM_OFF);

    paint_select((lv_obj_t *const *)act_rows, PET_ACT_COUNT, s_sel);
}

static void refresh_status(void)
{
    char buf[96];
    // 逐行组装:阶段/成长/年龄/三维/互动计数
    static const char *names[9] = {"阶段", "成长", "年龄", "饱食", "清洁",
                                   "心情", "喂食", "打扫", "抚摸"};
    char vals[9][40];
    // 成长显示封顶(与状态条一致):成年后继续攒,超出上限只作记录
    uint32_t need = pet_stage_need(s_st->stage);
    uint32_t grow = s_st->growth > need ? need : s_st->growth;
    const uint32_t h = s_st->age_s / 3600u;
    const uint32_t m = (s_st->age_s % 3600u) / 60u;
    const uint32_t s = s_st->age_s % 60u;
    snprintf(vals[0], sizeof(vals[0]), "%s", pet_stage_name(s_st->stage));
    snprintf(vals[1], sizeof(vals[1]), "%lu/%lu", (unsigned long)grow, (unsigned long)need);
    if (h > 0) snprintf(vals[2], sizeof(vals[2]), "%lu时%lu分", (unsigned long)h, (unsigned long)m);
    else       snprintf(vals[2], sizeof(vals[2]), "%lu分%lu秒", (unsigned long)m, (unsigned long)s);
    snprintf(vals[3], sizeof(vals[3]), "%d/100", s_st->hunger);
    snprintf(vals[4], sizeof(vals[4]), "%d/100", s_st->clean);
    snprintf(vals[5], sizeof(vals[5]), "%d/100", s_st->mood);
    snprintf(vals[6], sizeof(vals[6]), "%lu 次", (unsigned long)s_st->feed_count);
    snprintf(vals[7], sizeof(vals[7]), "%lu 次", (unsigned long)s_st->clean_count);
    snprintf(vals[8], sizeof(vals[8]), "%lu 次", (unsigned long)s_st->pat_count);

    for (int i = 0; i < 9; i++) {
        snprintf(buf, sizeof(buf), "%s: %s", names[i], vals[i]);
        fit_text(buf, sizeof(buf), UI_W - 26);   // 标签宽 216 留 2px 余量,保证单行
        if (status_rows[i]) lv_label_set_text(status_rows[i], buf);
    }
}

static void refresh_menu(void)
{
    if (menu_rows[2]) lv_label_set_text(menu_rows[2], s_sound_on ? "声音:开" : "声音:关");
    paint_select((lv_obj_t *const *)menu_rows, 5, s_sel);
}

// ---------------- 公开接口 ----------------
void pet_ui_init(void)
{
    s_root = lv_obj_create(NULL);   // LVGL 9 中 create(NULL) 即新建 screen
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x101418), 0);
    lv_screen_load(s_root);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_sel     = 0;
    s_page_id = PAGE_TITLE;
    s_page    = page_new();
    build_title();
}

// 建页面容器:显式铺满全屏并沿用深色背景,避免 lv_obj 默认的白色主题底。
// 抵消 LVGL 默认主题 card 样式:2px 灰边框/16px 内边距/圆角都会露馅。
static lv_obj_t *page_new(void)
{
    lv_obj_t *p = lv_obj_create(s_root);
    lv_obj_set_size(p, UI_W, UI_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    return p;
}

static void destroy_page(void)
{
    if (s_banner_timer) {
        lv_timer_delete(s_banner_timer);
        s_banner_timer = NULL;
    }
    face_destroy();   // 先拆形象部件(句柄归零),其余随页销毁
    if (s_page) {
        lv_obj_delete(s_page);
        s_page = NULL;
    }
    memset(act_rows, 0, sizeof(act_rows));
    memset(stat_bar, 0, sizeof(stat_bar));
    memset(status_rows, 0, sizeof(status_rows));
    memset(menu_rows, 0, sizeof(menu_rows));
    p_title = p_banner = p_hint = NULL;
}

void pet_ui_set_state(const pet_state_t *st) { s_st = st; }

void pet_ui_set_sound(bool on)
{
    s_sound_on = on;
    if (s_page_id == PAGE_MENU) refresh_menu();
}

game_page_t pet_ui_page(void) { return s_page_id; }

int pet_ui_select(void) { return s_sel; }

void pet_ui_set_select(int idx)
{
    s_sel = idx;
    if (s_page_id == PAGE_MENU) refresh_menu();
    else if (s_page_id == PAGE_PET) paint_select((lv_obj_t *const *)act_rows, PET_ACT_COUNT, s_sel);
}

void pet_ui_set_page(game_page_t page)
{
    destroy_page();
    s_page_id = page;
    s_page    = page_new();
    s_sel     = 0;

    switch (page) {
    case PAGE_TITLE:  build_title();  break;
    case PAGE_PET:    build_pet();    break;
    case PAGE_STATUS: build_status(); break;
    case PAGE_MENU:   build_menu();   break;
    }
    pet_ui_refresh();
}

static void banner_timeout_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    if (p_banner) lv_label_set_text(p_banner, "");
    s_banner_timer = NULL;
}

void pet_ui_banner(const char *text)
{
    if (s_page_id != PAGE_PET || !p_banner) return;
    if (s_banner_timer) {
        lv_timer_delete(s_banner_timer);
        s_banner_timer = NULL;
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", text);
    fit_text(buf, sizeof(buf), UI_W - 2);   // 横幅同样不允许换行
    lv_label_set_text(p_banner, buf);
    s_banner_timer = lv_timer_create(banner_timeout_cb, 1500, NULL);
    lv_timer_set_repeat_count(s_banner_timer, 1);
    lv_timer_reset(s_banner_timer);   // 立即就绪
}

void pet_ui_pet_react(void)
{
    if (s_page_id != PAGE_PET || !s_face) return;
    // 走路中先停(下次 walk 节拍从当前位置续走),防两套 y 动画打架
    lv_anim_delete(s_face, walk_x_exec_cb);
    lv_anim_delete(s_face, walk_y_exec_cb);
    if (lv_anim_get(s_face, react_exec_cb)) return;   // 已在蹦:不叠加

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_face);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 350);
    lv_anim_set_exec_cb(&a, react_exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_ready_cb(&a, react_ready_cb);
    lv_anim_start(&a);
}

void pet_ui_refresh(void)
{
    if (!s_st) return;
    switch (s_page_id) {
    case PAGE_PET:    refresh_pet();    break;
    case PAGE_STATUS: refresh_status(); break;
    case PAGE_MENU:   refresh_menu();   break;
    default: break;
    }
}
