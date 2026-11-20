# AES 抓取改为终端内 F9 触发（2026-11-20）

## 变更

旧行为：DLL 在模块哈希与输出目录校验通过后立刻启动 AES 后台线程，扫描窗口固定从注入时刻起算（默认 120 秒），与游戏何时加载 Pak 索引无关。

新行为：`aes_hotkey=1`（默认）时注入后只登记监听器，不扫描。按键来源限定为**本次采集的 Dumper 终端窗口**：

1. `EbonLogConsole` 打开自身控制台输入句柄（`CONIN$`），保留 `ENABLE_WINDOW_INPUT`，去掉 `ENABLE_PROCESSED_INPUT`/`ENABLE_LINE_INPUT`/`ENABLE_ECHO_INPUT`，让单次 F9 以普通 `KEY_EVENT` 到达。
2. 收到 F9（`wVirtualKeyCode=VK_F9`）后在采集目录写 `aes_hunt.request`；Ctrl+C 在关闭处理后仍按退出处理。
3. 游戏进程内的监听线程轮询该请求文件，取走后调用原有 `ebon_hunter` 后台扫描。

因此游戏进程不再调用 `GetAsyncKeyState`，键盘状态不进入游戏侧判定；其他窗口、其他进程以及游戏自身按 F9 都不会产生请求。

## 设计取舍

早期实现用"游戏内 `GetAsyncKeyState` + 前台窗口 PID 等于日志查看器 PID"来限定来源，本机验证时发现该判定依赖前台窗口归属：终端由 Windows Terminal/ConPTY 承载时，前台窗口属于宿主进程，PID 与查看器不符，热键会静默失效。改为由终端读取自己的输入队列后，判断依据变成"这个控制台是否收到了按键"，与终端宿主无关，也不再需要前台 PID 猜测。

## 相关文件

- `native/dumper/EbonLogConsole.cpp`：终端侧输入捕获、请求文件写入、Ctrl+C 退出。
- `native/dumper/EbonDumper.cpp`：`startAesHotkey` 监听线程（`aes_hotkey_state.json` 状态、请求消费、运行中拒绝重复触发、首次写 `aes/` 后续写 `aes_N/`）；日志查看器改为在采集目录确定且创建之后启动，并把目录传给终端。
- `native/common/ebon_console.hpp`：`startViewer` 增加 `--request-dir` 参数。
- `native/sdk/EbonDllSelfTest.cpp`：新增 `aes_hotkey` 宿主模式（`aes_hotkey=1`，用请求文件触发）；其余模式固定 `aes_hotkey=0` 以保持原有"注入即扫"的时序断言。
- `tools/ebon_cli.py`：`wait-aes` 在扫描目录尚未出现时读取 `aes_hotkey_state.json`，打印"到终端按 F9"或"没有终端，写 `aes_hunt.request`"，不再静默空等。
- `tools/request_aes_hunt.ps1`：脚本触发，等价于按 F9。
- `profiles/nte-ue5.ini`、`profiles/nte-ue5-resource-export.ini`：增加 `aes_hotkey=1`。
- `run_injector.bat`、`README.md`、`docs/USAGE.md`：操作说明。

## 配置

| 键 | 默认 | 含义 |
| --- | --- | --- |
| `aes_hunt` | 1 | 0 时完全不启动，写 `aes/aes_state.json` 的 `disabled` |
| `aes_hotkey` | 1 | 1 时等待终端 F9；0 时注入即扫（无终端场景与自动化） |
| `aes_timeout_seconds` / `aes_retry_ms` / `aes_unaligned` / `aes_pak` | 不变 | 扫描本身的行为 |

## 状态文件

`<采集目录>/aes_hotkey_state.json`：

```json
{"format":"EbonAesHotkey-v1","status":"armed","key":"F9","terminal_pid":1234,
 "terminal_available":true,"presses":0,"runs":0,"running":false,"last_output":""}
```

`status` 只会是 `armed` 或 `running`；`presses` 统计被接受的 F9 请求，`runs` 统计实际启动的扫描数，`last_output` 指向最近一次 `/aes` 或 `aes_N`。扫描本身的终态仍在各自的 `aes_state.json`。

## 验证

本机（Windows x64，MSVC x64）在本轮完成后：

- `build_native.bat build\candidate-aes-f9`：10 个原生目标全部构建通过，无警告输出。
- Python 全量回归 **102 项通过**（`EBON_TEST_BUILD_DIR=build/candidate-aes-f9`，27 秒），其中新增：
  - `test_dll_hotkey_listener_waits_for_the_request_file`：真实 DLL 在合成宿主中保持 `armed`，收到 `aes_hunt.request` 后才进入 `scanning`，最终 `timed_out`，且不产生 `aes_key.txt`。
  - `test_hotkey_armed_capture_reports_the_listener` / `test_hotkey_without_terminal_points_at_the_request_file`：`wait-aes` 的提示分支。
- 端到端热键链路（真实 DLL + 真实 Dumper 终端进程）：向终端控制台输入队列写入 `VK_F9` 的 `KEY_EVENT` 后，终端写出 `aes_hunt.request`，游戏侧日志出现 `AES key hunt started from the dumper terminal`，`aes_hotkey_state.json` 变为 `presses=1 runs=1 running=true`，`aes/aes_state.json` 进入 `scanning`；扫描运行中再次写入 F9 时日志为 `AES key hunt already running; ignoring F9`，`runs` 保持 1、未产生第二个 `aes_*` 目录。
- `EbonDllSelfTest` 的 `aes` / `aes_error` 原有两条用例在 `aes_hotkey=0` 下继续通过，注入即扫的独立性断言未被削弱。

## 未验证 / 边界

- **真机游戏进程内的 F9 按键未验收**。上面的端到端链路是在本机合成宿主上完成的：按键以控制台输入记录注入，覆盖"终端输入 → 请求文件 → 扫描启动"这一段，不覆盖物理键盘与终端窗口焦点本身（属操作系统行为）。
- 终端窗口未启动（`console=0`）时 F9 不存在，只能用请求文件；监听器会在日志中明确提示。
- 密钥仍然只通过 `aes_key.txt` 与 `aes_state.json` 的 `key` 字段落地，日志不打印密钥，这一点未改动。
- 真实客户端的加密 Pak 索引抓取仍以本次 `aes*/aes_state.json` 的 `verified` 终态为准，不以监听器状态代替。
