# EbonCore-Dumper 详细使用指南

> 所有命令均在仓库根目录执行。历史实机数据和 `out/`、`build/candidate-*` 证据目录只在本地保留，不随公仓分发。新环境请先运行 `build_native.bat`；未知客户端版本仍需重新验证布局。返回 [项目首页](../README.md)。

面向 Windows x64 Unreal Engine 5 客户端的**只读反射采集 + C++ SDK 生成**工具链，当前以《异环》（`HTGame.exe`）为本机验证样本。除反射采集外，还包含 AES Pak 索引密钥搜索、Pak/IoStore 资源清单与 RPC/复制元数据导出。

采集全程只读，结果写入客户端安装目录之外；所有布局、偏移、调用入口都绑定到具体模块 SHA-256，哈希不符即拒绝。

## 主要能力

- **反射采集**：对象数组、UClass/UStruct、属性、枚举、函数参数区与局部变量区、RPC 与复制元数据。
- **两种采集路径**：注入 DLL 在反作弊初始化前于进程内采集；或在无保护环境下用外部 `ReadProcessMemory` 采集。
- **SDK 生成**：真实 C++ 声明、继承、显式填充、布尔位访问器、`sizeof`/偏移静态断言，以及 `Network.hpp` 网络表查询接口。
- **AES 索引密钥搜索**：解析 Pak footer，仅接受完整索引 SHA-1 匹配的候选；支持进程内、外部只读扫描与离线文件扫描。
- **资源容器**：`dump-res` 批量保全 loose/Pak/IoStore，覆盖脚本、地图包、表、协议描述、热更与资产依赖文件；可显式接入本地 Oodle DLL。
- **F10 资源值导出**：角色养成、抽卡、任务、文本、皮肤，以及已加载 World/Level/Actor/组件、游戏类默认值、反射脚本资产和引用关系，支持重复采集。
- **可验证性**：四个原生自测程序、Python 回归测试与全量 SDK 编译检查。

## 环境要求

| 项目 | 要求 |
| --- | --- |
| 系统 | Windows 10/11 x64 |
| 编译器 | MSVC x64（VS 2022/2026 生成工具，C++17） |
| Python | 3.11+，标准库即可跑主流程 |
| 可选依赖 | `pycryptodome`（加密 Pak 索引）、`capstone`（`verify_sdk_runtime.py`），见 [requirements.txt](../requirements.txt) |

`build_native.bat` 支持 `EBON_VCVARS` 指定工具链，否则通过 VS Installer 的 `vswhere` 自动定位 x64 C++ 工具链；也支持已经初始化的 x64 Native Tools 终端。

## 快速开始

```powershell
.\build_native.bat          # 构建全部 10 个原生目标到 build\
.\run_inject.bat            # 按提示从启动器启动游戏；采集结束后自动生成 C++ SDK
```

产物默认落在 `out/reflection/injected_<时间>_<PID>\` 与 `out/sdk/cpp_injected_<时间>_<PID>\`。

## 使用方法

### 1. 构建

```powershell
.\build_native.bat              # 输出到 build\
.\build_native.bat build-aes    # 独立输出目录，AES 采集流程使用
```

| 产物 | 用途 |
| --- | --- |
| `EbonDumper.exe` | 外部只读采集（按 PID 读取） |
| `EbonDumper.dll` | 进程内采集 DLL，导出 `EbonDumpW` |
| `EbonLogConsole.exe` | 独立日志终端，需与 DLL 同目录 |
| `EbonDebugInject.exe` | 反作弊初始化前的早期注入器 |
| `EbonReflectionSelfTest.exe` | 反射引擎合成校验（预算、超时、两种字段布局） |
| `EbonDllSelfTest.exe` | 在独立合成宿主中验证真实 DLL 的启动/发现/采集/超时 |
| `EbonResourceSelfTest.exe` | 表行/配置值、稀疏容器、中文文本与资源采集预算校验 |
| `EbonAesScan.exe` | 外部只读进程内存扫描 |
| `EbonAesFinder.exe` | 离线镜像/文件扫描 |
| `EbonAesSelfTest.exe` | AES/SHA-1 向量与边界自测 |

### 2. 注入采集（推荐，反作弊接管前）

```powershell
python tools/ebon_cli.py inject-and-dump `
  --dump-out out/reflection/<新目录> `
  --sdk-out out/sdk/<新目录>
```

- 注入器以 1 ms 轮询目标进程，确认模块数达到 `--min-modules`（默认 24）且 ACE 尚未装载后，用 `CreateRemoteThread` + `LoadLibraryW` 注入。
- 注入成功后由 DLL 拉起独立 `EbonLogConsole.exe` 显示彩色日志，并追加写入 `out/ebon_inject.log`；关闭终端不影响游戏。
- 采集线程等待注册稳定后采集，写出终态后 Python 侧再生成 SDK。`--no-sdk` 只保留 JSONL。
- 错失窗口会提示 `SKIP: ACE already loaded, window missed` 并安全跳过。
- `--capture-delay 0..300` 可延后采集，用于等待复制表初始化。

### 3. 外部只读采集（无保护/离线环境）

```powershell
Get-Process HTGame | Select-Object Id,ProcessName
python tools/ebon_cli.py capture-sdk `
  --exe build/EbonDumper.exe --pid <PID> `
  --profile profiles/nte-ue5.ini `
  --dump-out out/reflection/<新目录> --sdk-out out/sdk/<新目录>
```

### 4. 生成 SDK / 补采网络快照

```powershell
# 从已验证的完整快照生成
python tools/ebon_cli.py generate-sdk --dump out/reflection/<目录> --out out/sdk/<新目录>

# 布局来自完整快照，网络表来自同哈希的场景快照
python tools/ebon_cli.py generate-sdk --dump out/reflection/<完整快照> `
  --network-capture out/reflection/<完整快照>/network_snapshot_1 --out out/sdk/<新目录>
```

首次采集后 15 分钟内进入场景，可触发同进程补采：

```powershell
.\tools\request_network_capture.ps1 -Capture "<本次采集目录>"
python -m tools.ebon.runtime_network --capture "<本次采集目录>/network_snapshot_1"
```

2026-11-15 实机场景补采并逐字节重验得到 **21 个完整布局、1,522 条复制句柄**，已覆盖玩家状态、四个角色类、HTAbilitySystemComponent、GameState 和 WorldDataLayers。完整指这些具体 Owner 的 Parents/Cmds 全部覆盖；不代表所有已加载类或数据包兼容性已验收。结果见 `out/reports/network_layouts_20261115_004927/network_coverage.md`。

新版按地址范围去重，按实际步长保存完整 Parents/Cmds，并登记数组等容器的内部字段；仍保留 32,768 节点、65,536 队列、20 秒和 16 MiB 上限，输出具体预算原因。构建与验证记录见 [复制布局补采修复](NETWORK_LAYOUT_CAPTURE_2026-11-15.md)。使用下列命令启动：

```powershell
$Capture = Join-Path (Get-Location) ("out\reflection\network_layouts_" + (Get-Date -Format "yyyyMMdd_HHmmss"))

python tools/ebon_cli.py inject-and-dump `
  --injector build/EbonDebugInject.exe `
  --dumper build/EbonDumper.dll `
  --profile profiles/nte-ue5.ini --dump-out $Capture `
  --log out/network_layouts_capture.log --seconds 900 --no-sdk
```

请先运行命令，再从启动器开游戏；首次反射采集完成后进入可操作场景，保持游戏运行并执行 `.\tools\request_network_capture.ps1 -Capture $Capture`。生成逐类覆盖报告：

```powershell
python -m tools.ebon.network_coverage --capture "$Capture/network_snapshot_1" --out out/reports/<新目录>
```

报告逐表核对 Parents/Cmds 的实际与声明数量，并列出目标继承链中各个 Owner 类的状态。同哈希客户端的 UE `LocalNetworkVersion` 已于 2026-11-15 实机确认：`1039291632`（`0x3DF254F0`）；新采集器保存 getter/Hello/缓存字节并在 SDK 生成时重验。`FHTBalatroTransportChunk.ProtocolVersion` 仍不能代替连接握手版本。见 [版本确认与使用](NETWORK_VERSION_2026-11-15.md)。

SDK 目录主要内容：`SDK.hpp`、`Types.hpp`、`Enums.hpp`、`Basic.hpp`、`Forward.hpp`、`Parameters.hpp`、`Functions.hpp`、`Network.hpp`、`Runtime.cpp`、`Packages/*.hpp`、`type_map.json`、`CompileCheck.cpp` 与 `CMakeLists.txt`，未确认项记录在 `unresolved.json`，覆盖范围见 `sdk_manifest.json`。

### 5. AES 索引密钥搜索

```powershell
.\run_injector.bat    # 注入采集 + 等待本次 AES 结果

# 等价命令行
python tools/ebon_cli.py inject-and-dump --injector build-aes/EbonDebugInject.exe `
  --dumper build-aes/EbonDumper.dll --no-sdk --wait-aes

# 独立只读扫描 / 离线扫描
build-aes\EbonAesScan.exe   --pid <PID> --pak "<Pak 文件或目录>" --out out/aes/<新目录> --seconds 120
build-aes\EbonAesFinder.exe --file <镜像文件> --pak "<Pak 文件或目录>" --out out/aes/<新目录>
python tools/ebon_cli.py wait-aes --capture out/reflection/<本次目录> --seconds 180
```

结果在**本次采集目录**的 `aes/`：`aes_state.json` 记录阶段、候选与失败原因；只有 `status=verified` 且 `success=true` 才写出 `aes_key.txt`，密钥不进入日志。退出码 `0` 已验证 / `1` 错误 / `2` 结束仍未找到。行为由 `profiles/nte-ue5.ini` 的 `aes_hunt`、`aes_timeout_seconds`、`aes_retry_ms`、`aes_unaligned`、`aes_pak` 控制。

### 6. F10 导出资源配置值

快捷键误判修复版位于 `build/candidate-focused-runtime-f10-logfix`。它会接受游戏窗口或 PID 配对的 EbonCore 日志窗口为前台，并读取按键事件边沿。为了在大场景导出更多值，先用独立资源预算 profile 启动注入器（必须在启动游戏前运行命令）：

```powershell
$Capture = Join-Path (Get-Location) ("out\reflection\resource_f10_" + (Get-Date -Format "yyyyMMdd_HHmmss"))
python tools\ebon_cli.py inject-and-dump `
  --injector build/candidate-focused-runtime-f10-logfix/EbonDebugInject.exe `
  --dumper build/candidate-focused-runtime-f10-logfix/EbonDumper.dll `
  --profile profiles/nte-ue5-resource-export.ini --dump-out $Capture `
  --log out/ebon_inject_f10.log --seconds 900 --no-sdk
```

等待日志显示 **`F10 resource dump ready`**。游戏窗口或该进程对应的 EbonCore 日志窗口前台时按 **F10**。若想用文件请求，另一个 PowerShell 窗口运行：

```powershell
.\tools\request_resource_capture.ps1 -Capture $Capture
```

每次生成：

```text
out/reflection/<本次采集>/resources/res_N/
├─ resource_state.json         # 成功/失败、条目数、未解析项、分类覆盖
├─ resource_manifest.jsonl     # 资源对象 → 数据文件 → 分类
├─ data/                      # DataTable 实际行值、DataAsset 配置、文本候选
├─ characters/                # 角色、等级、突破、觉醒、装备、技能
├─ gacha/                     # 卡池、保底、消耗、概率相关配置
├─ activities_systems/        # 活动与其他系统配置
├─ story_quests/              # 剧情、对话、任务配置
├─ localization/              # 文本资源索引及 text_values.jsonl 字符串汇总
├─ character_skins/           # 时装、外观、染色、装饰、皮肤配置
├─ maps/                      # 已加载 World/Level/Actor/组件与流式关卡反射值
├─ scripts/                   # 已加载反射脚本资产（不读取 Lua VM，不反编译）
├─ items_economy/              # 道具、奖励、掉落、货币配置
├─ combat_ai/                  # 战斗、AI、行为树等反射配置
├─ protocols/                 # 协议/网络命名配置
├─ blueprints/                # 蓝图/状态树命名资产
├─ other_assets/              # 其他资源，不再归入活动系统
├─ references.jsonl           # 来源字段 → 对象引用 / 反射软路径
├─ reflection/                # 本次重新采集的对象与字段结构
└─ unresolved.jsonl           # 未解析字段与失败原因（最多记录前 1,000 项详情）
```

每个分类下的 `resources.jsonl` **仅保存索引，不嵌入正文**；正文是该分类 `data/` 下的独立 JSONL 文件。例如 `gacha/resources.jsonl` 的 `file="gacha/data/DataTable/Avatar/AvatarDataTable.jsonl"` 指向按 Unreal 对象路径命名的正文，路径相对于 `res_N` 根目录。`/Game/` 映射到 `data/` 内容根，`/Engine/`、`/Script/` 等包保留各自根目录；子对象名附加 outer 路径，避免相互覆盖。根目录正文和分类副本独立写入，不共享可修改的硬链接。旧编号 dump 可用 `export-resource-bodies` 重导；分类依据对象、类型及字段命名，允许一个资源属于多个分类。配置对象同时包含默认值（`is_default=true`）和当前实例，包括 `/Engine/Transient` 下的抽卡管理器。独立副本计入 `resource_max_output_mb` 输出预算。

已有 dump 可在不修改原件的情况下生成分类正文文件：

```powershell
$BodiesOut = 'out/resources/bodies_' + (Get-Date -Format 'yyyyMMdd_HHmmss')
python tools/ebon_cli.py export-resource-bodies `
  --capture 'out/reflection/network_layouts_20261115_004927/resources/res_1' `
  --out $BodiesOut
```

`export-resource-bodies` 会检查正文 JSONL 和对象身份，保留原字节及 SHA-256；索引继续只保存路径和元数据。失败采集中已经写出但尚未进入索引的有效正文，放入 `other_assets/data/`，并标记 `partial=true`、`indexed_in_source=false`。状态写入 `resource_bodies_state.json`，保留原采集的失败、未解析和默认对象信息，**不补造缺失内容**。退出码：0 无未解决项，1 正文已导出但原采集不完整/仍有未解析项，2 导出失败或没有正文。默认预算：100,000 个正文副本、单文件 512 MiB、正文合计 4 GiB、元数据 64 MiB、1,800 秒。

采集当前已加载资源。进入地图、角色、抽卡、活动或任务界面后可以再次按 F10 补采，每次创建新目录。地图输出是场景对象与组件的反射值（包括可反射坐标及引用），不是完整 `.umap` 或全地图碰撞/导航。Lua VM、未加载的 World Partition cell 不在此路径的覆盖内；二进制资源与磁盘脚本走下节 `dump-res`。

`FString` 输出实际字符串；`FText` 与原生 `StringTable` 在尚未确认内部文本语义时输出原始值和字符串候选，明确标注 `_unresolved` / `inferred_candidates`，不能视为完整译文。详见 [资源导出实现与边界](RESOURCE_DUMP_2026-11-14.md)。

也可以通过文件请求触发同一采集入口：

```powershell
.\tools\request_resource_capture.ps1 -Capture '<本次采集目录>'
```

`profiles/nte-ue5.ini` 中 `resource_hotkey=0` 可关闭资源监听。默认监听持续到游戏退出，网络补采的 15 分钟窗口不影响 F10。

默认还导出 `/Script/HTGame` 和游戏蓝图类型的已加载 UObject 实例（管理器、配置及其他游戏状态）；`resource_game_objects=0` 可关闭这部分扩展实例，不影响场景、表和 DataAsset。两轮共用资源预算，原有表/资产优先。

### 7. 资源清单与 Pak 导出

```powershell
$env:EBON_AES_KEY = '<64 位十六进制密钥>'   # 索引加密时才需要

python tools/ebon_cli.py inventory-resources `
  --client '<客户端 Content 目录>' --out out/resources/<新目录>

python tools/ebon_cli.py extract-pak `
  --container '<...>\Paks\pakchunk0-Windows.pak' --out out/resources/<新目录> `
  --include '*.uasset' --include '*.uexp'
```

`extract-pak` 默认上限 1,000 个文件、单文件 512 MiB、总量 2 GiB。没有兼容解压器时，Oodle 条目逐项报告为未导出；可用 `--oodle-dll` 显式提供可信本地解码 DLL，`--oodle-sha256` 在加载前校验文件哈希。IoStore chunk 是原始块，不重建 cooked package。

### 8. 批量 Dump 地图、Lua、热更与私服研究资源

```powershell
# 全客户端磁盘资源；AES 索引加密时沿用 EBON_AES_KEY 环境变量
$ResOut = 'out/resources/res_' + (Get-Date -Format 'yyyyMMdd_HHmmss')
python tools/ebon_cli.py dump-res `
  --client 'C:\Program Files\Neverness To Everness\Client\WindowsNoEditor\HT\Content' `
  --out $ResOut
```

默认保全 `.lua/.luac/.luajit/.as/.js`、`.umap/.uasset/.uexp/.ubulk/.uptnl`、配置/文本、协议描述及 `.hpw` 等 RuntimeData 原文件。`.hpw` 不等于 Lua/热更：本机样本有混淆媒体头部线索，格式仍待完整验证。不要只过滤 `.umap`：地图的 sidecar、外置 Actor 和引用资产同样需要。`--all` 补收未知后缀（不可与 `--include` 同用）。

| 参数 | 默认 | 用途 |
| --- | --- | --- |
| `--capture` | 无 | 合并指定的成功 F10 `resources/res_N` 原始快照；可重复 |
| `--include` | 资源后缀集合 | 自定义路径 glob；可重复；会过滤无法命名的 chunk |
| `--max-files` | 100,000 | 全局最多尝试读取的文件/chunk；失败读取也计入 |
| `--max-file-mb` / `--max-total-mb` | 512 / 32,768 | 单文件 / 全局 payload 预算 |
| `--max-manifest-mb` | 512 | 清单和分类索引的独立预算 |
| `--timeout-seconds` | 1,800 | 扫描和读取总期限 |
| `--oodle-dll` / `--oodle-sha256` | 无 | 显式加载本地可信 Windows x64 DLL；记录哈希，不自动下载 |

样本历史盘点超过 750,000 个 chunk，默认预算不能覆盖全部。大批量运行前确认空间，按需要提高上述上限。触及文件/大小预算仍记录后续条目的 `blocked` 状态；超时或清单预算耗尽写出 `failed`。

产物包含 `resource_bundle_state.json`、`resource_files.jsonl`、`unresolved.jsonl`、分类索引，以及 `sources/<来源ID>/files/...` 或 `sources/<来源ID>/chunks/*.bin`。保留补丁和基础包的不同版本，不猜测挂载优先级，也不互相覆盖。

`success=true` 仅表示取得可保全产物。退出码 **0** 表示本次范围内无未解决项，**1** 表示保全了部分文件但仍有缺项，**2** 表示无可用资源或失败。IoStore 输出带 `raw_iostore_chunk`，Lua 字节码不反编译，`.hpw` 未确认格式；不能把这些文件视为可直接运行的完整私服。详见 [res 扩展说明](RES_EXPANSION_2026-11-15.md)。

### 9. 测试

```powershell
python -m unittest discover -s tests -p 'test_*.py' -v

$env:EBON_TEST_BUILD_DIR = (Resolve-Path build).Path   # 让 DLL 宿主测试指向已构建产物
build\EbonReflectionSelfTest.exe
build\EbonAesSelfTest.exe
build\EbonResourceSelfTest.exe
```

## 项目结构

```
EbonCore-Dumper/
├─ build_native.bat               # MSVC x64 构建全部原生目标
├─ run_inject.bat                 # 注入采集 → 等终态 → 生成 C++ SDK
├─ run_inject.ps1                 # 同上的参数化版本
├─ run_injector.bat               # AES 抓取流程（走 build-aes\）
├─ requirements.txt               # 可选 Python 依赖
├─ profiles/
│  ├─ nte-ue5.ini                 # 版本绑定：模块哈希、反射偏移、采集/AES 参数
│  └─ nte-sdk-runtime.json        # ProcessEvent 入口与模块哈希证据
├─ native/
│  ├─ common/                     # ebon_common.hpp（配置/哈希/内存工具）、ebon_console.hpp（日志终端）
│  ├─ dumper/                     # EbonDumper.cpp（EXE/DLL 共用）、EbonLogConsole.cpp、
│  │                              #   ebon_aes_hunter.hpp（DLL 内搜索线程）、ebon_aes_scan.hpp（footer 解析与校验）
│  ├─ injector/                   # EbonDebugInject.cpp（早期注入器）
│  ├─ sdk/                        # ebon_reflection.hpp（反射引擎）、ebon_network_runtime.hpp（运行时网络解码）、
│  │                              #   EbonReflectionSelfTest.cpp、EbonDllSelfTest.cpp
│  └─ aes/                        # aes_cli.hpp + aes_finder / aes_mem_scanner / test_aes
├─ tools/
│  ├─ ebon_cli.py                 # 统一命令行入口
│  ├─ ebon/                       # containers（Pak/Utoc）、cpp_sdk（C++ 生成）、sdk_codegen（旧格式）、
│  │                              #   network_codegen、runtime_mapping、runtime_network
│  ├─ verify_sdk_runtime.py       # ProcessEvent 入口交叉核对
│  ├─ request_network_capture.ps1 # 触发同进程网络快照补采
│  └─ request_resource_capture.ps1 # 触发同进程 F10 资源采集
├─ tests/                         # Python 单元/集成测试
├─ docs/                          # 各轮迭代记录与证据说明
└─ out/                           # 运行产物：reflection / sdk / resources / aes
```

## 产物与状态约定

- 输出目录必须是新建或空目录；拒绝写入客户端安装目录，也不默认覆盖已有文件。
- `dump_state.json` 在采集开始时即创建，记录 `phase`、进度、错误与候选；只有 `success=true` 且状态为 `complete` 或 `complete_with_unresolved` 才能用于 SDK 生成。
- 失败的采集会保留已校验的元数据供诊断，但不会被当作成功结果。
- 注入器输出 `DONE` 只代表 DLL 加载成功，采集结果以本次目录的状态文件为准。
- AES 的 `timed_out`/`exhausted`/`disabled`/`error` 都不代表抓取成功。

## 版本绑定

`profiles/nte-ue5.ini` 绑定 `HTGame.exe` 的 SHA-256（`exe_sha256`），并给出 NamePool、对象数组与 FProperty/FField 的偏移；RVA 置 `0` 时走有预算和超时的自动发现，找不到唯一候选即写入失败状态。客户端更新后必须重新确认哈希与布局，未知版本不会猜测调用入口。`profiles/nte-sdk-runtime.json` 记录 `ProcessEvent` 入口的虚表项与反汇编特征，`tools/verify_sdk_runtime.py` 可离线复现该核对。

## 已知边界

- 反射只覆盖当次已加载的对象，不代表资源、协议或服务端功能完整。
- 不附带 Oodle DLL；可显式接入本地兼容解码器，真实目标块兼容性需要验证。尚不重建 IoStore cooked package。
- AES 结果只证明状态中 `verified_sample` 指向的 Pak/GUID，不覆盖其他容器或自定义加密。
- SDK 的成员偏移与尺寸经编译断言校验；未采集到原生对齐值的类型使用显式填充，不等同于完整原生 ABI 还原。
- 654 个复用父类尾部填充的字段通过 `Get_字段()` 访问器暴露，38 个少见语义字段仍列为未确认。
- 网络映射当前为 `observed_partial`、`packet_mapping_verified=false`，RPCId 与 FieldNetIndex、RepIndex 与 FRepLayout handle 不可互换。

## 文档

- [DUMPER_FIX_2026-11-14.md](DUMPER_FIX_2026-11-14.md)：采集器根因与修复验收
- [CPP_SDK_2026-11-14.md](CPP_SDK_2026-11-14.md)：C++ SDK 交付与调用入口证据
- [NETWORK_METADATA_2026-11-14.md](NETWORK_METADATA_2026-11-14.md)：RPC 与复制元数据
- [RUNTIME_MAPPING_2026-11-14.md](RUNTIME_MAPPING_2026-11-14.md)：运行时网络映射
- [AES_HUNTER_2026-11-14.md](AES_HUNTER_2026-11-14.md)：AES 抓取实现与验证
- [RESOURCE_DUMP_2026-11-14.md](RESOURCE_DUMP_2026-11-14.md)：F10 资源值导出、分类与文本限制
- [RES_EXPANSION_2026-11-15.md](RES_EXPANSION_2026-11-15.md)：场景反射、批量脚本/地图/热更保全、预算与验收边界
- [RESOURCE_BODIES_2026-11-15.md](RESOURCE_BODIES_2026-11-15.md)：索引与独立正文文件、旧 dump 保全

## 免责声明

本项目采用 [GPL-3.0](../LICENSE)，是**学习、技术研究与自验证参考**工具，按「现状」提供，不附带任何明示或默示担保。以下为第三方授权及使用风险提醒，不对 GPL 许可增加限制。

- 代码中的路径、哈希、偏移与样本记录来自作者本机的已安装文件，仅作为方法与工程结构的记录。
- 请在自己拥有或已获许可的环境中运行；使用本项目进行的任何操作及其后果由使用者自行承担。
- 项目不构成对任何第三方软件、服务或数据的访问建议，也不对由此产生的可用性、完整性或合规性负责。
- 复制、修改和分发本项目的许可条件以 GPL-3.0 为准；第三方软件、资产及数据仍遵循各自权利与许可。
