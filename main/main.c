// main/main.c —— 口袋宠物(AI Passport / ESP32-C3)骨架占位入口。
// 骨架阶段只验证 BSP/LVGL/按键/监视器链路;游戏逻辑在 feature 分支开发。
#include <stdio.h>

#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "monitor.h"

static const char *TAG = "pocket_pet";

// 按键回调:骨架阶段仅打日志(运行在 button 组件任务,不做重活)
static void on_button(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    ESP_LOGI(TAG, "按键 btn=%d ev=%d", (int)btn, (int)ev);
}

// 监视器虚拟按键注入:骨架阶段与实体按键同路
static void inject_button(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    ESP_LOGI(TAG, "虚拟按键 btn=%d ev=%d", (int)btn, (int)ev);
}

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_display_init());
    bsp_display_backlight(100);

    // NVS:后续游戏存档容器。失败不致命,只是无法持久化
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_LOGI(TAG, "NVS 就绪");

    lv_display_t *disp = bsp_lvgl_init();
    if (disp == NULL) {
        ESP_LOGE(TAG, "LVGL 初始化失败");
        return;
    }

    // 占位标题页(骨架用 ASCII,中文标题随字体一起进 feature 分支)
    bsp_lvgl_lock(portMAX_DELAY);
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    lv_label_set_text(label, "pocket-pet");
    bsp_lvgl_unlock();

    ESP_ERROR_CHECK(bsp_button_init(on_button, NULL));
    monitor_init(inject_button);   // 开发用:Mac 监视器(viewer.py)配套

    ESP_LOGI(TAG, "口袋宠物骨架启动完成");
}
