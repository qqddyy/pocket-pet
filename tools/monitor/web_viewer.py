#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESP32 屏幕监视器 · 浏览器版(配套设备端 components/dev_monitor/)

为什么是浏览器版:本机所有 Python 的 Tk 在 macOS 26.2 上崩溃
("macOS 26 (2602) or later required"),改用 stdlib http.server + 网页,
浏览器渲染无此问题。

启动: 双击 start_monitor.command,或:
  /Users/haolee/.espressif/python_env/idf5.5_py3.10_env/bin/python -B web_viewer.py
然后浏览器打开 http://127.0.0.1:8765 (会尝试自动打开)
依赖: 上述 IDF python 环境的 pyserial + pillow(缺 pillow 先
  python -m pip install pillow)。屏幕 240x320 / 三键 UP/DOWN/OK 固定。

功能:
  - 实时画面镜像(MJPEG 流)
  - 浏览模式点击画面 → 选中元素 → 改文字/颜色/位置/隐藏(设备实时生效)
  - 标注模式:画框/写字 → 导出带标注 PNG
  - 虚拟按键 ↑/↓/OK(单击/双击/长按)远程操控游戏
"""
import glob
import io
import json
import queue
import re
import signal
import struct
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial
except ImportError:
    print("缺 pyserial,请用 IDF python 环境运行")
    sys.exit(1)
try:
    from PIL import Image
except ImportError:
    print("缺 Pillow,先执行: python -m pip install pillow")
    sys.exit(1)

W, H, SCALE = 240, 320, 2
MAGIC = b"STRP"
HDR_LEN = 15
PORT_BASE = 8765

# RGB565→RGB888 查找表
LUT = bytearray(65536 * 3)
for _v in range(65536):
    _r, _g, _b = (_v >> 11) & 0x1F, (_v >> 5) & 0x3F, _v & 0x1F
    LUT[_v * 3] = _r * 255 // 31
    LUT[_v * 3 + 1] = _g * 255 // 63
    LUT[_v * 3 + 2] = _b * 255 // 31

OBJ_RE = re.compile(
    r"^MON OBJ class=(\S+) screen=(\d) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) text=(.*)$")


# ---------------------------------------------------------------------------
# 设备串口链路(后台线程):解析帧流/MON 行
# ---------------------------------------------------------------------------
class DevLink(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.ser = None
        self.port = None
        self.connected = False
        self.tx_mu = threading.Lock()
        self.rx_bytes = 0
        self.strips = 0
        self.fb = bytearray(W * H * 3)
        self.fb_mu = threading.Lock()
        self.dirty = True
        self.resp_q = queue.Queue()
        self.stop = False
        self.drain_handshake = True   # 丢弃连接握手 M 1 的应答(仅第一条)

    def send(self, line):
        with self.tx_mu:
            if self.ser is not None:
                try:
                    self.ser.write((line + "\n").encode("utf-8"))
                except Exception:
                    pass

    def find_port(self):
        ports = sorted(glob.glob("/dev/cu.usbmodem*"))
        return ports[0] if ports else None

    def run(self):
        while not self.stop:
            port = self.find_port()
            if not port:
                self.connected = False
                time.sleep(3)
                continue
            try:
                self.ser = serial.Serial(port, 115200, timeout=0.2)
                self.port = port
                self.ser.reset_input_buffer()
                self.connected = True
                self.send("M 1")
                self._pump()
            except Exception:
                pass
            finally:
                self.connected = False
                with self.tx_mu:
                    if self.ser is not None:
                        try:
                            self.ser.close()
                        except Exception:
                            pass
                        self.ser = None
                time.sleep(3)

    def _pump(self):
        buf = b""
        while not self.stop:
            data = self.ser.read(65536)
            if not data:
                continue
            self.rx_bytes += len(data)
            buf += data
            while True:
                i = buf.find(MAGIC)
                m = buf.find(b"MON ")
                if i >= 0 and (m < 0 or i < m):
                    if len(buf) < i + HDR_LEN:
                        buf = buf[i:]
                        break
                    flags = buf[i + 4]
                    plen = struct.unpack_from("<H", buf, i + 5)[0]
                    x1, y1, x2, y2 = struct.unpack_from("<4H", buf, i + 7)
                    if len(buf) < i + HDR_LEN + plen:
                        buf = buf[i:]
                        break
                    payload = buf[i + HDR_LEN:i + HDR_LEN + plen]
                    buf = buf[i + HDR_LEN + plen:]
                    self.strips += 1
                    self._paint(flags & 1, x1, y1, x2, y2, payload)
                elif m >= 0:
                    nl = buf.find(b"\n", m)
                    if nl < 0:
                        if len(buf) > 8192:
                            buf = buf[-64:]
                        break
                    line = buf[m:nl].decode("utf-8", "replace")
                    buf = buf[nl + 1:]
                    # 连接握手自发的 M 1 会先产生一条应答,不进命令队列,否则第一条
                    # 用户命令的响应配对错位。仅丢这一条——用户后续 M 0/M 1 的
                    # 应答(MON OK M 0/1)必须正常入队。
                    if line.startswith("MON OK M") and self.drain_handshake:
                        self.drain_handshake = False
                        continue
                    self.resp_q.put(line)
                else:
                    if len(buf) > 16:
                        buf = buf[-16:]
                    break

    def _paint(self, rle, x1, y1, x2, y2, payload):
        w, h = x2 - x1 + 1, y2 - y1 + 1
        need = w * h
        if rle:
            vals = []
            off = 0
            while off + 4 <= len(payload) and len(vals) < need:
                cnt, v = struct.unpack_from("<HH", payload, off)
                off += 4
                if cnt == 0:
                    break
                vals.extend([v] * cnt)
            if len(vals) != need:
                return
        else:
            if len(payload) < need * 2:
                return
            vals = struct.unpack_from("<%dH" % need, payload)
        lut = LUT
        with self.fb_mu:
            fb = self.fb
            k = 0
            for y in range(y1, y2 + 1):
                base = (y * W + x1) * 3
                for _ in range(w):
                    v = vals[k]
                    k += 1
                    p = v * 3
                    fb[base] = lut[p]
                    fb[base + 1] = lut[p + 1]
                    fb[base + 2] = lut[p + 2]
                    base += 3
            self.dirty = True

    def snapshot(self, scale=True):
        with self.fb_mu:
            img = Image.frombytes("RGB", (W, H), bytes(self.fb))
            self.dirty = False
        if scale:
            img = img.resize((W * SCALE, H * SCALE), Image.NEAREST)
        return img


LINK = DevLink()
CMD_MU = threading.Lock()
STATS = {"kbps": 0.0, "sps": 0, "last_rx": 0, "last_strips": 0}


def do_cmd(cmd):
    """发命令并等待 MON 响应(串行化,防响应串线)"""
    with CMD_MU:
        LINK.send(cmd)
        try:
            line = LINK.resp_q.get(timeout=1.5)
        except queue.Empty:
            return None
        # 非本命令的响应(理论上不会有)继续等
        for _ in range(3):
            if line.startswith("MON "):
                return line
            try:
                line = LINK.resp_q.get(timeout=0.5)
            except queue.Empty:
                return None
        return line


def stats_loop():
    while True:
        time.sleep(1.0)
        STATS["kbps"] = (LINK.rx_bytes - STATS["last_rx"]) / 1024.0
        STATS["sps"] = LINK.strips - STATS["last_strips"]
        STATS["last_rx"] = LINK.rx_bytes
        STATS["last_strips"] = LINK.strips


# ---------------------------------------------------------------------------
# HTTP 服务
# ---------------------------------------------------------------------------
INDEX_HTML = """<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<title>ESP32 · 屏幕监视器</title>
<style>
  body { font-family: -apple-system, "PingFang SC", sans-serif; background:#1e1e1e;
         color:#ddd; margin:18px; }
  .wrap { display:flex; gap:18px; }
  .panel { background:#2a2a2a; border-radius:8px; padding:12px; margin-bottom:12px; }
  .panel h3 { margin:0 0 8px; font-size:14px; color:#9fd; }
  .mirrorbox { position:relative; width:480px; height:640px;
               padding:8px; background:#111; border:3px solid #5a5a5a;
               border-radius:10px; box-sizing:content-box; }
  #mirror { display:block; width:480px; height:640px; image-rendering:pixelated;
            background:#000; border-radius:4px; }
  #overlay { position:absolute; left:8px; top:8px; pointer-events:none; }
  button { background:#3c3c3c; color:#eee; border:1px solid #555; border-radius:5px;
           padding:6px 12px; cursor:pointer; font-size:14px; }
  button:hover { background:#4c4c4c; }
  button.key { font-size:18px; padding:10px 18px; }
  input[type=text], input[type=number] { background:#1c1c1c; color:#eee;
           border:1px solid #555; border-radius:4px; padding:4px 6px; }
  .row { margin:6px 0; }
  .dim { color:#888; font-size:12px; }
  #status { margin-top:6px; font-size:13px; }
  .ok { color:#7d7; } .bad { color:#e77; }
  label { font-size:13px; }
  .radio { display:inline-block; margin:0 10px 6px 0; }
  .colorchip { display:inline-block; width:22px; height:22px; border-radius:4px;
              border:2px solid #555; cursor:pointer; margin-right:6px;
              vertical-align:middle; }
  .colorchip.sel { border-color:#fff; }
</style>
</head>
<body>
<h2 style="margin-top:0">ESP32 · 屏幕监视器</h2>
<div class="wrap">
  <div>
    <div class="mirrorbox">
      <img id="mirror" src="/stream" width="480" height="640">
      <canvas id="overlay" width="480" height="640"></canvas>
    </div>
    <div id="status">连接中...</div>
  </div>

  <div style="width:360px">
    <div class="panel">
      <h3>虚拟按键</h3>
      <button class="key" id="btnUp">↑</button>
      <button class="key" id="btnDown">↓</button>
      <button class="key" id="btnOk">OK</button>
      <div class="dim" style="margin-top:6px">OK:单击=确认 · 双击=属性 · 按住0.6s=长按</div>
    </div>

    <div class="panel">
      <h3>元素(浏览模式点击画面选中)</h3>
      <div id="selInfo" class="dim">未选中</div>
      <div class="row">文字 <input type="text" id="eText" size="22"></div>
      <div class="row">颜色 <input type="text" id="eColor" value="#FFB000" size="9">
        <input type="text" id="eColorCur" value="#FFFFFF" size="9" readonly
               style="background:#1c1c1c"></div>
      <div class="row">X <input type="number" id="eX" value="0" style="width:60px">
        Y <input type="number" id="eY" value="0" style="width:60px"></div>
      <div class="row">
        <button onclick="nudge(-8,0)">X-8</button><button onclick="nudge(8,0)">X+8</button>
        <button onclick="nudge(0,-8)">Y-8</button><button onclick="nudge(0,8)">Y+8</button>
        <button onclick="nudge(-1,0)">X-1</button><button onclick="nudge(1,0)">X+1</button>
        <button onclick="nudge(0,-1)">Y-1</button><button onclick="nudge(0,1)">Y+1</button>
      </div>
      <div class="row">
        <label><input type="checkbox" id="eHide"> 隐藏</label>
        <button onclick="applyProps()">应用</button>
        <button onclick="requery()">刷新</button>
        <button onclick="clearSel()">取消选择</button>
      </div>
    </div>

    <div class="panel">
      <h3>标注(圈出显示问题)</h3>
      <label class="radio"><input type="radio" name="mode" value="browse" checked>浏览</label>
      <label class="radio"><input type="radio" name="mode" value="rect">画框</label>
      <label class="radio"><input type="radio" name="mode" value="text">写字</label>
      <div class="row" style="margin-top:8px">
        颜色
        <span class="colorchip sel" data-c="#FF3355" style="background:#FF3355"></span>
        <span class="colorchip" data-c="#FFCC00" style="background:#FFCC00"></span>
        <span class="colorchip" data-c="#33BBFF" style="background:#33BBFF"></span>
        <span class="colorchip" data-c="#33DD66" style="background:#33DD66"></span>
        文字 <input type="text" id="annoText" value="问题" size="8">
      </div>
      <div class="row">
        <button onclick="clearAnno()">清空标注</button>
        <button onclick="exportAnno()">导出PNG(含标注)</button>
      </div>
    </div>

    <div class="panel">
      <h3>截图</h3>
      <button onclick="saveShot()">保存当前画面 PNG</button>
    </div>
  </div>
</div>

<script>
const mirror = document.getElementById('mirror');
const overlay = document.getElementById('overlay');
const octx = overlay.getContext('2d');
const selInfo = document.getElementById('selInfo');
let annos = [];      // {type:'rect',x1,y1,x2,y2,c} | {type:'text',x,y,c,t}
let annoColor = '#FF3355';
let sel = null;

// ---------- 模式切换 ----------
document.querySelectorAll('input[name=mode]').forEach(r => r.onchange = modeChanged);
document.querySelectorAll('.colorchip').forEach(ch => ch.onclick = () => {
  document.querySelectorAll('.colorchip').forEach(c => c.classList.remove('sel'));
  ch.classList.add('sel');
  annoColor = ch.dataset.c;
});
function getMode() {
  return document.querySelector('input[name=mode]:checked').value;
}
function modeChanged() {
  const m = getMode();
  mirror.style.pointerEvents = (m === 'browse') ? 'auto' : 'none';
  overlay.style.pointerEvents = (m === 'browse') ? 'none' : 'auto';
}

// ---------- 通信 ----------
async function sendCmd(cmd) {
  try {
    const r = await fetch('/cmd', {method:'POST',
      headers:{'Content-Type':'application/json'},
      body: JSON.stringify({cmd})});
    return await r.json();
  } catch (e) { return {ok:false, err:'网络错误'}; }
}

// ---------- 状态轮询 ----------
setInterval(async () => {
  try {
    const r = await fetch('/stats');
    const s = await r.json();
    const el = document.getElementById('status');
    if (s.connected) {
      el.innerHTML = '<span class="ok">● 已连接 ' + s.port +
        ' · 监视中</span> &nbsp; ' + s.kbps.toFixed(1) + ' KB/s · ' + s.sps + ' 条带/s';
    } else {
      el.innerHTML = '<span class="bad">○ 设备未连接(插入后会自动重连)</span>';
    }
  } catch (e) {}
}, 1000);

// ---------- 元素选中/编辑 ----------
mirror.addEventListener('click', async e => {
  if (getMode() !== 'browse') return;
  const x = Math.floor(e.offsetX / 2), y = Math.floor(e.offsetY / 2);
  selInfo.textContent = '查询中...';
  const r = await sendCmd('Q ' + x + ' ' + y);
  if (!r || !r.ok) { selInfo.textContent = '无响应'; return; }
  const m = r.line.match(/^MON OBJ class=(\\S+) screen=(\\d) x=(-?\\d+) y=(-?\\d+) w=(\\d+) h=(\\d+) text=([\\s\\S]*)$/);
  if (!m) { selInfo.textContent = r.line; return; }
  sel = {class:m[1], screen:m[2]==='1', x:+m[3], y:+m[4], w:+m[5], h:+m[6],
         text:m[7].replace(/\\\\n/g, '\\n')};
  selInfo.textContent = 'class=' + sel.class + (sel.screen?'(screen)':'') +
    '  ' + sel.w + 'x' + sel.h + '  @(' + sel.x + ',' + sel.y + ')';
  document.getElementById('eText').value = sel.text;
  document.getElementById('eX').value = sel.x;
  document.getElementById('eY').value = sel.y;
  document.getElementById('eHide').checked = false;
  let cur = '#FFFFFF';
  if (sel.class === 'lv_label') cur = '#FFB000';
  document.getElementById('eColor').value = cur;
  document.getElementById('eColorCur').value = cur;
  document.getElementById('eColorCur').style.background = cur;
});

async function applyProps() {
  if (!sel) { alert('先在浏览模式下点击画面选中元素'); return; }
  const t = document.getElementById('eText').value.replace(/\\n/g, '\\\\n');
  await sendCmd('S TEXT ' + t);
  let c = document.getElementById('eColor').value.replace('#','');
  if (/^[0-9a-fA-F]{6}$/.test(c)) await sendCmd('S COLOR ' + c);
  const x = +document.getElementById('eX').value, y = +document.getElementById('eY').value;
  await sendCmd('S POS ' + x + ' ' + y);
  await sendCmd('S HIDE ' + (document.getElementById('eHide').checked ? 1 : 0));
}
async function nudge(dx, dy) {
  if (!sel) return;
  const x = +document.getElementById('eX').value + dx;
  const y = +document.getElementById('eY').value + dy;
  document.getElementById('eX').value = x;
  document.getElementById('eY').value = y;
  await sendCmd('S POS ' + x + ' ' + y);
}
async function requery() {
  if (!sel) return;
  const x = +document.getElementById('eX').value, y = +document.getElementById('eY').value;
  sel = null;
  const r = await sendCmd('Q ' + x + ' ' + y);
  if (r && r.ok) {
    const m = r.line.match(/^MON OBJ class=(\\S+) screen=(\\d) x=(-?\\d+) y=(-?\\d+) w=(\\d+) h=(\\d+) text=([\\s\\S]*)$/);
    if (m) { sel = {class:m[1], screen:m[2]==='1', x:+m[3], y:+m[4], w:+m[5], h:+m[6],
                    text:m[7].replace(/\\\\n/g, '\\n')}; }
  }
}
function clearSel() {
  sel = null;
  selInfo.textContent = '未选中';
}

// ---------- 虚拟按键 ----------
document.getElementById('btnUp').onclick = () => sendCmd('B UP CLICK');
document.getElementById('btnDown').onclick = () => sendCmd('B DOWN CLICK');
let okTimer = null, okLast = 0, okPending = null, okLongFired = false;
const okBtn = document.getElementById('btnOk');
okBtn.onmousedown = () => {
  okLongFired = false;
  okTimer = setTimeout(() => { okLongFired = true; sendCmd('B OK LONG'); }, 600);
};
okBtn.onmouseup = () => {
  clearTimeout(okTimer);
  if (okLongFired) return;
  const now = Date.now();
  if (now - okLast < 350) {
    okLast = 0;
    if (okPending) { clearTimeout(okPending); okPending = null; }
    sendCmd('B OK DOUBLE');
  } else {
    okLast = now;
    if (okPending) clearTimeout(okPending);
    okPending = setTimeout(() => sendCmd('B OK CLICK'), 350);
  }
};

// ---------- 标注 ----------
function redrawAnnos(preview) {
  octx.clearRect(0, 0, 480, 640);
  const list = preview ? annos.concat([preview]) : annos;
  for (const a of list) {
    octx.strokeStyle = a.c; octx.fillStyle = a.c;
    octx.lineWidth = 3;
    if (a.type === 'rect') octx.strokeRect(a.x1, a.y1, a.x2 - a.x1, a.y2 - a.y1);
    else { octx.font = 'bold 22px sans-serif'; octx.fillText(a.t, a.x, a.y); }
  }
}
let dragStart = null;
overlay.addEventListener('mousedown', e => {
  if (getMode() !== 'rect') return;
  dragStart = {x: e.offsetX, y: e.offsetY};
});
overlay.addEventListener('mousemove', e => {
  if (!dragStart) return;
  redrawAnnos({type:'rect', x1:dragStart.x, y1:dragStart.y, x2:e.offsetX, y2:e.offsetY, c:annoColor});
});
overlay.addEventListener('mouseup', e => {
  if (!dragStart) return;
  const r = {type:'rect', x1:dragStart.x, y1:dragStart.y, x2:e.offsetX, y2:e.offsetY, c:annoColor};
  dragStart = null;
  if (Math.abs(r.x2-r.x1) > 8 && Math.abs(r.y2-r.y1) > 8) annos.push(r);
  redrawAnnos();
});
overlay.addEventListener('click', e => {
  if (getMode() !== 'text') return;
  annos.push({type:'text', x:e.offsetX, y:e.offsetY, c:annoColor,
              t: document.getElementById('annoText').value});
  redrawAnnos();
});
function clearAnno() { annos = []; redrawAnnos(); }

// ---------- 导出 ----------
function download(url, name) {
  const a = document.createElement('a');
  a.href = url; a.download = name;
  document.body.appendChild(a); a.click(); a.remove();
}
function exportAnno() {
  const c = document.createElement('canvas'); c.width = 480; c.height = 640;
  const ctx = c.getContext('2d');
  try { ctx.drawImage(mirror, 0, 0, 480, 640); } catch (e) {}
  for (const a of annos) {
    ctx.strokeStyle = a.c; ctx.fillStyle = a.c; ctx.lineWidth = 3;
    if (a.type === 'rect') ctx.strokeRect(a.x1, a.y1, a.x2 - a.x1, a.y2 - a.y1);
    else { ctx.font = 'bold 22px sans-serif'; ctx.fillText(a.t, a.x, a.y); }
  }
  download(c.toDataURL('image/png'),
    'screenshot_' + new Date().toTimeString().slice(0,8).replaceAll(':','') + '.png');
}
async function saveShot() {
  const r = await fetch('/frame');
  const blob = await r.blob();
  const url = URL.createObjectURL(blob);
  download(url, 'screen_' + new Date().toTimeString().slice(0,8).replaceAll(':','') + '.png');
  setTimeout(() => URL.revokeObjectURL(url), 5000);
}
modeChanged();
</script>
</body>
</html>
"""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass   # 静默访问日志

    def _json(self, obj, code=200):
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/index"):
            data = INDEX_HTML.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        elif self.path == "/stream":
            self._stream()
        elif self.path == "/frame":
            buf = io.BytesIO()
            LINK.snapshot().save(buf, "JPEG", quality=88)
            data = buf.getvalue()
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        elif self.path == "/stats":
            self._json({"connected": LINK.connected, "port": LINK.port,
                        "kbps": STATS["kbps"], "sps": STATS["sps"]})
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path == "/cmd":
            try:
                n = int(self.headers.get("Content-Length", 0))
                req = json.loads(self.rfile.read(n).decode("utf-8"))
                cmd = str(req.get("cmd", ""))[:200]
            except Exception:
                return self._json({"ok": False, "err": "bad request"})
            if not cmd:
                return self._json({"ok": False, "err": "empty cmd"})
            if not LINK.connected:
                return self._json({"ok": False, "err": "设备未连接"})
            line = do_cmd(cmd)
            if line is None:
                self._json({"ok": False, "err": "设备无响应"})
            else:
                self._json({"ok": line.startswith("MON OK") or
                            line.startswith("MON OBJ") or
                            line.startswith("MON HELLO"), "line": line})
        else:
            self.send_error(404)

    def _stream(self):
        self.send_response(200)
        self.send_header("Content-Type",
                         "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            while True:
                time.sleep(0.1)
                buf = io.BytesIO()
                LINK.snapshot().save(buf, "JPEG", quality=80)
                data = buf.getvalue()
                self.wfile.write(
                    b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                    + str(len(data)).encode() + b"\r\n\r\n" + data + b"\r\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    LINK.start()
    threading.Thread(target=stats_loop, daemon=True).start()

    port = None
    srv = None
    for p in range(PORT_BASE, PORT_BASE + 5):
        try:
            srv = ThreadingHTTPServer(("127.0.0.1", p), Handler)
            srv.daemon_threads = True
            port = p
            break
        except OSError:
            continue
    if srv is None:
        print("端口 %d-%d 都被占用,请检查是否已有监视器在运行" %
              (PORT_BASE, PORT_BASE + 4))
        sys.exit(1)

    url = "http://127.0.0.1:%d" % port
    print("=" * 52)
    print(" ESP32 屏幕监视器已启动")
    print(" 浏览器打开: %s  (将尝试自动打开)" % url)
    print(" 关闭: 本窗口按 Ctrl+C,或直接关掉终端窗口")
    print("=" * 52)
    threading.Timer(1.2, lambda: webbrowser.open(url)).start()

    def _bye(*_):
        LINK.send("M 0")
        time.sleep(0.2)
        sys.exit(0)

    signal.signal(signal.SIGTERM, _bye)
    signal.signal(signal.SIGINT, _bye)
    try:
        srv.serve_forever()
    except (KeyboardInterrupt, SystemExit):
        LINK.send("M 0")
    finally:
        LINK.stop = True


if __name__ == "__main__":
    main()
