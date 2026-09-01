// main/main.c —— 口袋宠物(AI Passport / ESP32-C3)入口。
// 职责:初始化 BSP/按键/音频/NVS → 起游戏任务 → 按键回调只投递队列(不阻塞)
//       → 游戏任务串行处理按键与每秒 tick,并在持有 LVGL 锁时刷新 UI。
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "monitor.h"
#include "pet_core.h"
#include "pet_save.h"
#include "pet_ui.h"

static const char *TAG = "pocket_pet";

#define AUTO_SAVE_PERIOD_S 60   // 每 60 秒自动落盘一次(互动/进化会即时存)

typedef struct {
    bsp_btn_t    btn;
    bsp_btn_ev_t ev;
} btn_msg_t;

static QueueHandle_t   s_btnq;
static pet_state_t     s_state;
static bool            s_in_game;       // 已进入宠物主循环
static bool            s_sound_on = true;   // 声音开关(NVS 持久化)

// ---------------------------------------------------------------------------
// 极简音效:16k/16bit 方波短音。bsp_audio 未初始化或 codec 不在位时静默失败,不阻碍游玩。
// ---------------------------------------------------------------------------
static void beep(uint32_t freq_hz, uint32_t ms_dur)
{
    if (!s_sound_on) return;   // 声音关:所有音效静默
    if (bsp_audio_set_format(16000, 16, 1) != ESP_OK) return;
    bsp_audio_set_volume(60);

    int16_t buf[128];
    const uint32_t period = 16000u / freq_hz;
    const uint32_t total  = 16000u * ms_dur / 1000u;
    uint32_t phase = 0, left = total;
    while (left > 0) {
        uint32_t n = left < (uint32_t)(sizeof(buf) / sizeof(buf[0])) ? left : (uint32_t)(sizeof(buf) / sizeof(buf[0]));
        for (uint32_t i = 0; i < n; i++) {
            buf[i] = (phase < period / 2) ? 6000 : -6000;
            if (++phase >= period) phase = 0;
        }
        if (bsp_audio_write(buf, n * sizeof(int16_t)) != ESP_OK) break;
        left -= n;
    }
}

static void beep_feed(void)   { beep(1200, 30); }
static void beep_clean(void)  { beep(800, 35);  }
static void beep_pat(void)    { beep(1600, 20); }
static void beep_full(void)   { beep(400, 40);  }
static void beep_evolve(void) { beep(900, 40); beep(1400, 80); }

// ---------------------------------------------------------------------------
// 页面流转 + 按键分发(运行于游戏任务上下文)
// ---------------------------------------------------------------------------
static void start_new_game(void)
{
    pet_reset(&s_state);
    pet_save(&s_state);          // 新档立即落盘
    s_in_game = true;
    pet_ui_set_page(PAGE_PET);
    pet_ui_banner("上下选互动 · 确定执行");
}

static void load_game(void)
{
    if (pet_load(&s_state)) {
        s_in_game = true;
        pet_ui_set_page(PAGE_PET);
        pet_ui_banner("欢迎回来,团子!");
    } else {
        start_new_game();
    }
}

// 进化瞬间:音效 + 落盘 + 重建形象 + 公告(调用方已检测到阶段变化)
static void evolve_notify(void)
{
    beep_evolve();
    pet_save(&s_state);          // 进化即落盘
    pet_ui_refresh();            // 按新阶段重建宠物形象
    pet_ui_banner(s_state.stage == PET_BABY ? "咔!蛋壳破了,团子出生!"
                                            : "团子长大啦!");
}

static void do_action(void)
{
    const pet_action_t act    = (pet_action_t)pet_ui_select();
    const pet_stage_t  before = s_state.stage;
    const char        *msg    = "";
    const pet_result_t r      = pet_do_action(&s_state, act, &msg);

    if (s_state.stage != before) {   // 互动直接触发进化
        evolve_notify();
        return;
    }
    pet_save(&s_state);              // 互动即落盘
    pet_ui_refresh();
    pet_ui_banner(msg);
    if (r == PET_R_FULL) {
        beep_full();
        return;
    }
    pet_ui_pet_react();             // 开心蹦(互动成功的动效反馈)
    switch (act) {                   // 音效随互动类型
    case PET_ACT_FEED:  beep_feed();  break;
    case PET_ACT_CLEAN: beep_clean(); break;
    default:            beep_pat();   break;
    }
}

static void enter_menu(void)
{
    pet_ui_set_select(0);
    pet_ui_set_page(PAGE_MENU);
}

static void handle_button(const btn_msg_t *m)
{
    const game_page_t pg = pet_ui_page();

    // 双击/长按的页面语义
    if (m->ev == BSP_BTN_LONG) {
        if (pg == PAGE_PET || pg == PAGE_STATUS || pg == PAGE_MENU) {
            enter_menu();
            return;
        }
        if (pg == PAGE_TITLE) return;
    }
    if (m->ev == BSP_BTN_DOUBLE) {
        if (pg == PAGE_PET) {
            pet_ui_set_page(PAGE_STATUS);
            return;
        }
    }

    // 上下键:移动选择(宠物页 3 个互动,菜单页 5 项)
    if (m->btn == BSP_BTN_UP || m->btn == BSP_BTN_DOWN) {
        if (m->ev != BSP_BTN_CLICK) return;
        int count = 0;
        if (pg == PAGE_PET)   count = PET_ACT_COUNT;
        else if (pg == PAGE_MENU) count = 5;
        if (count == 0) return;
        int s = pet_ui_select();
        s += (m->btn == BSP_BTN_UP) ? -1 : 1;
        if (s < 0) s = count - 1;
        if (s >= count) s = 0;
        pet_ui_set_select(s);
        return;
    }

    if (m->ev != BSP_BTN_CLICK) return;   // 其余只处理单击

    switch (pg) {
    case PAGE_TITLE:
        load_game();
        break;
    case PAGE_PET:
        do_action();
        break;
    case PAGE_STATUS:
        pet_ui_set_page(PAGE_PET);
        break;
    case PAGE_MENU:
        switch ((int)pet_ui_select()) {
        case 0:  pet_save(&s_state); pet_ui_set_page(PAGE_PET); pet_ui_banner("已存档"); break;
        case 1:  load_game(); break;
        case 2:  // 声音开关:切换即存,留在菜单页看效果
            s_sound_on = !s_sound_on;
            pet_sound_save(s_sound_on);
            pet_ui_set_sound(s_sound_on);
            break;
        case 3:  pet_ui_set_page(PAGE_PET); break;
        default: s_in_game = false; pet_ui_set_page(PAGE_TITLE); break;
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 按键回调:仅入队,绝不阻塞(button 组件的定时器任务)
// ---------------------------------------------------------------------------
static void on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    btn_msg_t m = { .btn = btn, .ev = ev };
    xQueueSend(s_btnq, &m, 0);   // button 回调运行在组件任务,非 ISR;队列满直接丢弃本次
}

// 监视器虚拟按键注入:与实体按键同路(直投队列)
static void inject_button(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    btn_msg_t m = { .btn = btn, .ev = ev };
    xQueueSend(s_btnq, &m, 0);
}

// 每秒 tick:状态衰减/被动成长/进化判定 + 周期自动存档,全部在 LVGL 锁内刷新
static void do_every_second(void)
{
    if (!s_in_game) return;

    const pet_stage_t before = s_state.stage;
    pet_tick(&s_state);
    if (s_state.stage != before) {   // 挂机被动成长触发进化
        evolve_notify();
        return;
    }
    pet_ui_refresh();

    // 周期自动存档
    static uint32_t s_autosave_s;
    if (++s_autosave_s >= AUTO_SAVE_PERIOD_S) {
        s_autosave_s = 0;
        pet_save(&s_state);
    }
}

static void game_task(void *arg)
{
    (void)arg;

    // UI 初始化:在 LVGL 锁内完成,之后绝不持锁阻塞(否则会饿死 lvgl_port 的渲染任务)
    bsp_lvgl_lock(portMAX_DELAY);
    pet_ui_init();
    pet_ui_set_state(&s_state);
    pet_ui_set_sound(s_sound_on);   // 菜单页"声音:开/关"显示与实际一致
    bsp_lvgl_unlock();

    TickType_t last_tick = xTaskGetTickCount();
    for (;;) {
        // 按键:队列最多等 100ms(不带锁),有事件才进锁处理
        btn_msg_t m;
        if (xQueueReceive(s_btnq, &m, pdMS_TO_TICKS(100)) == pdTRUE) {
            bsp_lvgl_lock(portMAX_DELAY);
            handle_button(&m);
            bsp_lvgl_unlock();
        }

        // 每秒 tick:衰减/成长/进化/自动存档(同样只在锁内刷新)
        TickType_t now = xTaskGetTickCount();
        if (now - last_tick >= pdMS_TO_TICKS(1000)) {
            last_tick = now;
            bsp_lvgl_lock(portMAX_DELAY);
            do_every_second();
            bsp_lvgl_unlock();
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_display_init());
    bsp_display_backlight(100);

    // NVS:存档容器。失败不致命,只是本局无法持久化
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_LOGI(TAG, "NVS 就绪");
    s_sound_on = pet_sound_load();   // 声音设置(独立键,默认开)

    // 音频:codec 不在位(无 ES8311/接线问题)时仅降级为无声,不影响游戏
    if (bsp_audio_init() == ESP_OK) {
        ESP_LOGI(TAG, "音频就绪");
    } else {
        ESP_LOGW(TAG, "音频不可用,本局静音");
    }

    lv_display_t *disp = bsp_lvgl_init();
    if (disp == NULL) {
        ESP_LOGE(TAG, "LVGL 初始化失败");
        return;
    }
    (void)disp;

    s_btnq = xQueueCreate(8, sizeof(btn_msg_t));
    if (!s_btnq) {
        ESP_LOGE(TAG, "按键队列创建失败");
        return;
    }
    ESP_ERROR_CHECK(bsp_button_init(on_button, NULL));

    monitor_init(inject_button);   // 开发用:Mac 监视器(viewer.py)配套,默认不串流

    xTaskCreate(game_task, "game", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "口袋宠物启动完成,等待操作");
}
