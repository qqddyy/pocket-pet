// components/dev_monitor/src/monitor.c —— 开发用实时屏幕监视器(设备端)。
//
// 原理:接管 LVGL flush 回调——转发行为与 esp_lvgl_port 原版完全一致
// (小端→大端原地字节交换 + esp_lcd_panel_draw_bitmap(x1,y1,x2+1,y2+1));
// 绝不调 lv_disp_flush_ready(SPI on_color_trans_done 负责,IDF5.5 下
// LVGL_PORT_HANDLE_FLUSH_READY=1)。监视开启时额外把扫描带 RLE 压缩后
// 经 USB CDC(stdout)串流给 Mac 查看器。
//
// 协议:
//  设备→Mac 二进制帧:"STRP"(4B)|flags(1B,bit0=RLE)|payload_len(u16LE)
//                    |x1 y1 x2 y2(4×u16LE)|payload
//    raw 载荷 = w×h 个 RGB565 小端(LVGL 原生,交换前采样);
//    RLE 载荷 = (u16 count,u16 rgb565) 序列,按大小取小者。
//  设备→Mac ASCII 行:"MON ..."(\n 结尾)。
//  Mac→设备 ASCII 行(LF 结尾):
//    M 1/0      开/关监视(开=静默 ESP_LOG,关=恢复 INFO)
//    H          握手 → MON HELLO 1
//    B <UP|DOWN|OK> <CLICK|DOUBLE|LONG>   注入虚拟按键
//    Q <x> <y>  查询坐标处最顶层对象 → MON OBJ class=.. screen=.. x=.. y=.. w=.. h=.. text=..
//               (text 为行尾自由字段,\n 转义为 "\\n")
//    S TEXT <行尾原文>   改选中对象文字(仅 label)
//    S COLOR <rrggbb>    label→text_color,否则 bg_color
//    S POS <x> <y>       绝对屏幕坐标 → 父相对坐标移动(screen 不可移动)
//    S HIDE <0|1>        隐藏/显示
//    S REFRESH           失效重绘选中对象
//  选中模型:设备存 s_sel_obj;每次 S 先验证指针仍在活动 screen 树内
//  (防页面切换悬垂),失效回 MON ERR STALE。修改均为运行时效果,不持久化。
#include "monitor.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bsp_display.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
// LVGL 9.5 对象结构体/class 名是"私有头"——dev 工具需要 obj->class_p->name 做
// 命中报告,此处显式引入(升级 LVGL 时注意同步)。
#include "core/lv_obj_class_private.h"
#include "core/lv_obj_private.h"

static const char *TAG = "monitor";

// esp_lvgl_port 显示缓冲 = 240×20 像素,单次 flush 面积上限即此值
#define STRIP_MAX_PIX (240 * 20)
#define RLE_BUF_SIZE   (STRIP_MAX_PIX * 4)   // RLE 最坏 4B/像素

static uint8_t  s_rle[RLE_BUF_SIZE];

static bool              s_on;
static bool              s_inited;
static lv_obj_t         *s_sel_obj;
static void            (*s_inject_btn)(bsp_btn_t, bsp_btn_ev_t);
static SemaphoreHandle_t s_tx_mu;   // 串行化 stdout 写(flush 任务 vs reader 任务)

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }

// 统一出口:互斥保证"头+载荷"两段写不与 MON 响应行交错。
// ★ 必须直接走驱动 API:VFS 驱动模式的 write 在 TX ring 满时会"短写",
//   fwrite 不重试即静默丢字节(实测:连续多条带猛灌超过 ring 容量时
//   必现坏帧)。驱动 API 循环重试直到全部入队。
// 主机消失(TX ring 持续满 ~5s)则放弃并返回 false——调用方关 s_on 自愈,
// 防止 LVGL flush 任务被无限阻塞导致设备屏幕冻结。
static bool mon_write(const uint8_t *p, size_t n)
{
    int timeouts = 0;
    while (n > 0) {
        int w = usb_serial_jtag_write_bytes(p, n, pdMS_TO_TICKS(1000));
        if (w <= 0) {
            if (++timeouts >= 5) return false;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        timeouts = 0;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static void reply(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n > sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n] = '\n';

    xSemaphoreTake(s_tx_mu, portMAX_DELAY);
    mon_write((const uint8_t *)buf, (size_t)n + 1);
    xSemaphoreGive(s_tx_mu);
}

// ---------------------------------------------------------------------------
// flush 接管:dump(可选)+ 原样转发
// ---------------------------------------------------------------------------
static void monitor_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)disp;
    const size_t npix = lv_area_get_size(area);

    if (s_on && npix && npix <= STRIP_MAX_PIX) {
        // RLE 编码:(count, rgb565) 对
        const uint16_t *src = (const uint16_t *)px_map;
        uint32_t rlen = 0;
        for (uint32_t i = 0; i < npix; ) {
            const uint16_t v = src[i];
            uint32_t run = 1;
            while (i + run < npix && src[i + run] == v && run < 0xFFFF) run++;
            put16(s_rle + rlen, (uint16_t)run);
            put16(s_rle + rlen + 2, v);
            rlen += 4;
            i += run;
        }
        const size_t raw_len = npix * 2;
        const bool   use_rle = rlen < raw_len;

        uint8_t hdr[15];
        memcpy(hdr, "STRP", 4);
        hdr[4] = use_rle ? 1 : 0;
        put16(hdr + 5, (uint16_t)(use_rle ? rlen : raw_len));
        put16(hdr + 7, (uint16_t)area->x1);
        put16(hdr + 9, (uint16_t)area->y1);
        put16(hdr + 11, (uint16_t)area->x2);
        put16(hdr + 13, (uint16_t)area->y2);

        xSemaphoreTake(s_tx_mu, portMAX_DELAY);
        const bool ok = mon_write(hdr, sizeof(hdr)) &&
                        mon_write(use_rle ? s_rle : px_map, use_rle ? rlen : raw_len);
        xSemaphoreGive(s_tx_mu);
        if (!ok) s_on = false;   // 主机不在(~5s 写不动):自动停流防冻屏,M 1 可重开
    }

    // 原样转发 esp_lvgl_port 行为:RGB565 字节交换 + 送面板(不调 flush_ready)
    uint16_t *p16 = (uint16_t *)px_map;
    for (size_t i = 0; i < npix; i++) {
        p16[i] = (uint16_t)((p16[i] >> 8) | (p16[i] << 8));
    }
    esp_lcd_panel_draw_bitmap(bsp_display_panel(), area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
}

// ---------------------------------------------------------------------------
// 对象树操作:命中测试 / 悬垂校验 / 属性修改
// ---------------------------------------------------------------------------
static lv_obj_t *hit_test(lv_obj_t *parent, int32_t x, int32_t y)
{
    // 逆序遍历:后创建的子对象画在上层
    const int32_t cnt = (int32_t)lv_obj_get_child_count(parent);
    for (int32_t i = cnt - 1; i >= 0; i--) {
        lv_obj_t *child = lv_obj_get_child(parent, i);
        if (!child || lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
        lv_obj_t *hit = hit_test(child, x, y);
        if (hit) return hit;
        lv_area_t a;
        lv_obj_get_coords(child, &a);
        if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) return child;
    }
    return NULL;
}

static bool obj_alive(lv_obj_t *obj, lv_obj_t *root)
{
    if (root == obj) return true;
    const uint32_t cnt = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < cnt; i++) {
        if (obj_alive(obj, lv_obj_get_child(root, i))) return true;
    }
    return false;
}

static bool sel_valid(void)
{
    return s_sel_obj != NULL && obj_alive(s_sel_obj, lv_screen_active());
}

static bool sel_is_label(void)
{
    return s_sel_obj && s_sel_obj->class_p && s_sel_obj->class_p->name &&
           strcmp(s_sel_obj->class_p->name, "lv_label") == 0;
}
// text 里的 '\n' 转义成 "\\n",避免破坏行协议
static void escape_text(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < cap; p++) {
        if (*p == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else dst[o++] = *p;
    }
    dst[o] = 0;
}

// "\\n" 还原为 '\n'
static void unescape_text(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] == 'n') { *w++ = '\n'; r++; }
        else *w++ = *r;
    }
    *w = 0;
}

static void cmd_query(int x, int y)
{
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t *hit = hit_test(lv_screen_active(), x, y);
    s_sel_obj = hit ? hit : lv_screen_active();

    lv_area_t a;
    lv_obj_get_coords(s_sel_obj, &a);
    const char *cls = (s_sel_obj->class_p && s_sel_obj->class_p->name) ?
                      s_sel_obj->class_p->name : "?";
    const bool is_screen = lv_obj_get_parent(s_sel_obj) == NULL;

    char text[128];
    text[0] = 0;
    if (sel_is_label()) {
        const char *t = lv_label_get_text(s_sel_obj);
        escape_text(text, sizeof(text), t ? t : "");
    }
    reply("MON OBJ class=%s screen=%d x=%d y=%d w=%d h=%d text=%s", cls, is_screen,
          (int)a.x1, (int)a.y1, (int)lv_area_get_width(&a), (int)lv_area_get_height(&a), text);
}

static void cmd_set_text(const char *text)
{
    if (!sel_valid()) { reply("MON ERR STALE"); return; }
    if (!sel_is_label()) { reply("MON ERR NOTLABEL"); return; }
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", text);
    unescape_text(buf);
    lv_label_set_text(s_sel_obj, buf);
    reply("MON OK TEXT");
}

static void cmd_set_color(uint32_t rgb)
{
    if (!sel_valid()) { reply("MON ERR STALE"); return; }
    const lv_color_t c = lv_color_hex(rgb & 0xFFFFFF);
    if (sel_is_label()) lv_obj_set_style_text_color(s_sel_obj, c, 0);
    else                lv_obj_set_style_bg_color(s_sel_obj, c, 0);
    lv_obj_invalidate(s_sel_obj);
    reply("MON OK COLOR");
}

static void cmd_set_pos(int x, int y)
{
    if (!sel_valid()) { reply("MON ERR STALE"); return; }
    lv_obj_t *parent = lv_obj_get_parent(s_sel_obj);
    if (!parent) { reply("MON ERR SCREEN"); return; }
    lv_area_t pa;
    lv_obj_get_coords(parent, &pa);
    // lv_obj_set_pos 坐标系 = 父内容区(扣除边框+内边距)
    const int32_t off_x = lv_obj_get_style_border_width(parent, 0) + lv_obj_get_style_pad_left(parent, 0);
    const int32_t off_y = lv_obj_get_style_border_width(parent, 0) + lv_obj_get_style_pad_top(parent, 0);
    lv_obj_set_pos(s_sel_obj, x - pa.x1 - off_x, y - pa.y1 - off_y);
    reply("MON OK POS");
}

static void cmd_set_hide(int hide)
{
    if (!sel_valid()) { reply("MON ERR STALE"); return; }
    if (hide) lv_obj_add_flag(s_sel_obj, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_remove_flag(s_sel_obj, LV_OBJ_FLAG_HIDDEN);
    reply("MON OK HIDE");
}

// ---------------------------------------------------------------------------
// 命令读取任务
// ---------------------------------------------------------------------------
static bool parse_btn(const char *s, bsp_btn_t *btn, bsp_btn_ev_t *ev)
{
    char bn[16], en[16];
    if (sscanf(s, "%15s %15s", bn, en) != 2) return false;
    if      (strcmp(bn, "UP") == 0)   *btn = BSP_BTN_UP;
    else if (strcmp(bn, "DOWN") == 0) *btn = BSP_BTN_DOWN;
    else if (strcmp(bn, "OK") == 0)   *btn = BSP_BTN_OK;
    else return false;
    if      (strcmp(en, "CLICK") == 0)  *ev = BSP_BTN_CLICK;
    else if (strcmp(en, "DOUBLE") == 0) *ev = BSP_BTN_DOUBLE;
    else if (strcmp(en, "LONG") == 0)   *ev = BSP_BTN_LONG;
    else return false;
    return true;
}

static void reader_task(void *arg)
{
    (void)arg;
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n == 0) continue;

        if (strncmp(line, "M ", 2) == 0) {
            const bool on = line[2] == '1';
            esp_log_level_set("*", on ? ESP_LOG_NONE : ESP_LOG_INFO);
            if (on) {
                bsp_lvgl_lock(portMAX_DELAY);
                s_on = true;   // 先开再失效,确保首帧不丢
                lv_obj_invalidate(lv_screen_active());
                bsp_lvgl_unlock();
            } else {
                s_on = false;
            }
            reply("MON OK M %d", on);
        } else if (strcmp(line, "H") == 0) {
            reply("MON HELLO 1");
        } else if (strncmp(line, "B ", 2) == 0) {
            bsp_btn_t btn;
            bsp_btn_ev_t ev;
            if (parse_btn(line + 2, &btn, &ev) && s_inject_btn) {
                s_inject_btn(btn, ev);
                reply("MON OK B");
            } else {
                reply("MON ERR BTN");
            }
        } else if (strncmp(line, "Q ", 2) == 0) {
            int x, y;
            if (sscanf(line + 2, "%d %d", &x, &y) == 2) {
                bsp_lvgl_lock(portMAX_DELAY);
                cmd_query(x, y);
                bsp_lvgl_unlock();
            }
        } else if (strncmp(line, "S ", 2) == 0) {
            const char *rest = line + 2;
            if (strncmp(rest, "TEXT ", 5) == 0) {
                bsp_lvgl_lock(portMAX_DELAY);
                cmd_set_text(rest + 5);
                bsp_lvgl_unlock();
            } else if (strncmp(rest, "COLOR ", 6) == 0) {
                unsigned rgbu;
                if (sscanf(rest + 6, "%x", &rgbu) == 1) {
                    bsp_lvgl_lock(portMAX_DELAY);
                    cmd_set_color((uint32_t)rgbu);
                    bsp_lvgl_unlock();
                }
            } else if (strncmp(rest, "POS ", 4) == 0) {
                int x, y;
                if (sscanf(rest + 4, "%d %d", &x, &y) == 2) {
                    bsp_lvgl_lock(portMAX_DELAY);
                    cmd_set_pos(x, y);
                    bsp_lvgl_unlock();
                }
            } else if (strncmp(rest, "HIDE ", 5) == 0) {
                int h;
                if (sscanf(rest + 5, "%d", &h) == 1) {
                    bsp_lvgl_lock(portMAX_DELAY);
                    cmd_set_hide(h);
                    bsp_lvgl_unlock();
                }
            } else if (strncmp(rest, "REFRESH", 7) == 0) {
                bsp_lvgl_lock(portMAX_DELAY);
                if (sel_valid()) lv_obj_invalidate(s_sel_obj);
                bsp_lvgl_unlock();
                reply("MON OK REFRESH");
            }
        }
    }
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------
void monitor_init(void (*inject_btn)(bsp_btn_t, bsp_btn_ev_t))
{
    if (s_inited) return;
    s_inited = true;
    s_inject_btn = inject_btn;
    s_tx_mu = xSemaphoreCreateMutex();
    if (!s_tx_mu) {
        ESP_LOGE(TAG, "互斥量创建失败");
        return;
    }

    // 控制台切到中断驱动模式:stdin 可阻塞读,stdout 走环形缓冲(吞吐更高)
    usb_serial_jtag_driver_config_t drv = {
        .tx_buffer_size = 16384,
        .rx_buffer_size = 1024,
    };
    if (usb_serial_jtag_driver_install(&drv) != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag 驱动安装失败,虚拟按键/命令不可用(画面串流仍走 stdout)");
        return;
    }
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);

    // 接管 flush(LVGL 锁内替换;转发行为与 esp_lvgl_port 一致)
    bsp_lvgl_lock(portMAX_DELAY);
    lv_display_set_flush_cb(lv_display_get_default(), monitor_flush_cb);
    bsp_lvgl_unlock();

    xTaskCreate(reader_task, "mon_rd", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "监视器就绪(默认关闭,viewer 连接后 M 1 开启)");
}
