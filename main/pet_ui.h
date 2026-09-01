// main/pet_ui.h —— 口袋宠物 LVGL 界面层。
// 约定:所有函数必须在持有 bsp_lvgl_lock 的前提下调用(由 app 任务统一加锁)。
#pragma once

#include "pet_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAGE_TITLE = 0,
    PAGE_PET,      // 宠物主页:形象 + 三维状态条 + 互动选择
    PAGE_STATUS,   // 属性页:阶段/成长/年龄/计数明细
    PAGE_MENU,     // 菜单:存档/读档/声音/返回
} game_page_t;

void pet_ui_init(void);                  // 根屏初始化;之后默认停在标题页
void pet_ui_set_page(game_page_t page);  // 切换页面(销毁上一页对象,重建)
game_page_t pet_ui_page(void);
void pet_ui_refresh(void);               // 用当前状态刷新活动页动态内容
void pet_ui_set_state(const pet_state_t *st); // 绑定游戏状态指针(只读)
void pet_ui_set_select(int idx);         // 互动/菜单项高亮
int  pet_ui_select(void);
void pet_ui_banner(const char *text);    // 宠物页瞬态横幅(互动反馈/进化公告)
void pet_ui_pet_react(void);             // 互动成功动效:开心蹦一下(仅宠物页有效)
void pet_ui_set_sound(bool on);          // 声音开关(菜单页"声音:开/关"项显示用)

#ifdef __cplusplus
}
#endif
