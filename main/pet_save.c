// main/pet_save.c —— 存档实现:nvs 命名空间 "pocket_pet",单键存整段状态 blob。
// 自带 magic + 版本 + 简单校验和,读到损坏/旧版时视为无存档(安全回退新档)。
#include "pet_save.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "pet_save";

#define SAVE_NAMESPACE "pocket_pet"
#define SAVE_KEY       "state"
#define SAVE_MAGIC     0x9EC0DE01u
#define SAVE_VERSION   1u

typedef struct {
    uint32_t       magic;
    uint8_t        version;
    uint8_t        reserved[3];
    uint16_t       checksum;    // 逐字节异或,防 NVS 写坏/掉电半写
    pet_state_t    state;
} save_record_t;

static uint16_t calc_checksum(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t c = 0;
    for (size_t i = 0; i < len; i++) c = (uint16_t)(c ^ p[i]);
    return c;
}

// NVS 简短 API 封装:开命名空间,失败返回 0。
static nvs_handle_t open_nvs(void)
{
    nvs_handle_t h = 0;
    esp_err_t e = nvs_open(SAVE_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open 失败: %s", esp_err_to_name(e));
        return 0;
    }
    return h;
}

bool pet_save(const pet_state_t *st)
{
    nvs_handle_t h = open_nvs();
    if (!h) return false;

    save_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic   = SAVE_MAGIC;
    rec.version = SAVE_VERSION;
    rec.state   = *st;
    rec.checksum = calc_checksum(&rec.state, sizeof(rec.state));

    esp_err_t e = nvs_set_blob(h, SAVE_KEY, &rec, sizeof(rec));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob 失败: %s", esp_err_to_name(e));
        nvs_close(h);
        return false;
    }
    e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit 失败: %s", esp_err_to_name(e));
        return false;
    }
    return true;
}

bool pet_load(pet_state_t *st)
{
    nvs_handle_t h = open_nvs();
    if (!h) return false;

    save_record_t rec;
    size_t len = sizeof(rec);
    esp_err_t e = nvs_get_blob(h, SAVE_KEY, &rec, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof(rec)) {
        if (e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs_get_blob 失败: %s", esp_err_to_name(e));
        }
        return false;
    }
    if (rec.magic != SAVE_MAGIC || rec.version != SAVE_VERSION ||
        rec.checksum != calc_checksum(&rec.state, sizeof(rec.state))) {
        ESP_LOGW(TAG, "存档校验失败(magic/version/checksum),视为无存档");
        return false;
    }
    *st = rec.state;
    return true;
}

bool pet_has_save(void)
{
    nvs_handle_t h = open_nvs();
    if (!h) return false;
    // NVS 里同一 key 只允许一种实体类型("state" 存的是 blob),探测必须用 blob 形式。
    size_t len = 0;
    esp_err_t e = nvs_get_blob(h, SAVE_KEY, NULL, &len);
    nvs_close(h);
    return e == ESP_OK;
}

// ---------------------------------------------------------------------------
// 声音开关:独立 u8 键。设置项与游戏进度分家——清档/读档不影响声音设置。
// ---------------------------------------------------------------------------
bool pet_sound_load(void)
{
    nvs_handle_t h = open_nvs();
    if (!h) return true;   // 打不开就默认开
    uint8_t v = 1;
    esp_err_t e = nvs_get_u8(h, "sound", &v);
    nvs_close(h);
    if (e != ESP_OK) return true;   // 无记录默认开
    return v != 0;
}

void pet_sound_save(bool on)
{
    nvs_handle_t h = open_nvs();
    if (!h) return;
    if (nvs_set_u8(h, "sound", on ? 1 : 0) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}
