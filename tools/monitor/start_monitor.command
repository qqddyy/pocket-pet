#!/bin/bash
# 屏幕监视器启动器(浏览器版) · 设备端配套 components/dev_monitor/
# 双击本文件,或在任意终端执行: bash start_monitor.command
# 浏览器将自动打开 http://127.0.0.1:8765
cd "$(dirname "$0")"
echo "启动 ESP32 屏幕监视器(浏览器版)..."
/Users/haolee/.espressif/python_env/idf5.5_py3.10_env/bin/python -B web_viewer.py 2>&1 | tee -a web_viewer.log
echo ""
echo "viewer 已退出(输出记录在 web_viewer.log)"
read -r -p "按回车键关闭本窗口..." _
