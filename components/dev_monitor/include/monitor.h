// components/dev_monitor/include/monitor.h —— 开发用屏幕监视器(设备端)。
// Mac 端配套: tools/monitor/(web_viewer.py 浏览器版,零游戏依赖,即拷即用)。
// 能力:实时画面串流 / 点击查询+修改元素 / 虚拟按键注入。协议见 src/monitor.c 头注。
//
// ★ 复用到新项目(AI Passport,240x320,三键 UP/DOWN/OK):
//   ① 拷 components/dev_monitor/ 整目录 + tools/monitor/ 整目录
//   ② main 组件 CMakeLists 的 REQUIRES 加 dev_monitor
//   ③ app_main 里 bsp_lvgl_init() 成功后调 monitor_init(按键注入回调),
//      回调把虚拟按键投进本项目的按键队列即可(参考 pocket-xiuxian main.c)
//   Mac 端:双击 tools/monitor/start_monitor.command → 自动开浏览器
//   http://127.0.0.1:8765(依赖 IDF python 环境的 pyserial+pillow,
//   详细协议见 src/monitor.c 头注)。设备端常驻零开销,默认不串流,
//   viewer 连上才开(M 1),拔线自动停流不影响游戏。
#pragma once

#include "bsp_button.h"

// 初始化:接管 LVGL flush + 起命令读取任务。
// inject_btn:虚拟按键注入回调(由 main 提供,直投游戏按键队列)。
// 必须在 bsp_lvgl_init() 成功之后调用;重复调用无副作用。
void monitor_init(void (*inject_btn)(bsp_btn_t btn, bsp_btn_ev_t ev));
