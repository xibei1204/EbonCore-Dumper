# res 扩展：地图、脚本与离线资源

## 范围与验收

- M-001：F10 保留原表/资产导出，增加 World、Level、流式关卡、Actor、组件、游戏配置与资产的反射值及对象引用边。`/Script/HTGame` 与游戏蓝图类型的 UObject 默认同时保留 CDO 和实例，实例可用 `resource_game_objects=0` 关闭。只读、不加载资产、不执行脚本。
- M-002：`dump-res` 批量处理 loose/Pak/IoStore，保留原路径、来源版本与 SHA-256；脚本、地图、配置、协议描述、热更与资产依赖文件不靠当前是否加载来筛选。
- M-003：合成原生测试、Python 回归与实际安装目录只读盘点。实机 F10、加密资源解包与私服验收分开记录。
- 排除：官方服务端数据库/逻辑、网络重定向、保护绕过、猜测 `.hpw` 格式、Lua 反编译、IoStore cooked package 重建。
- 退出标准：构建通过；新增路径、分类、字节完整性、预算、失败状态、来源冲突与输出隔离测试通过；未覆盖项明确记录。不宣称“所有私服数据齐全”。

## 证据与工具

| ID | 来源 | 结论 |
| --- | --- | --- |
| E-001 | `native/dumper/ebon_resource_dump.hpp` 原实现 | [V] 原 F10 仅选择 DataTable、StringTable、DataAsset。 |
| E-002 | `out/resources/installed-20261114/resource_inventory_state.json` | [V] 历史盘点 30 个容器、751,654 个 IoStore chunk；加密目录未读，不证明文件名覆盖。 |
| E-003 | `out/reflection/focused_live_f10_32768_20261114_231312/types.jsonl` | [V] World、Level、SceneComponent、Vector/Rotator/Transform 布局已有反射证据；不硬编码坐标布局。 |
| E-004 | 安装目录 `HT/Content/RuntimeData` 只读列举 | [V] 本次发现 5 个 `.hpw` 文件；[T] 内容和 Lua 关系未确认。 |
| G-001 | https://github.com/FabianFG/CUE4Parse （Apache-2.0） | 查阅 README、OodleHelper；成熟包解析库，但依赖 .NET/目标版本适配，不将原始 chunk 伪装为 uasset。当前 Python 容器读取器复用已有实现，包重建留给成熟工具。未安装或执行该库。 |
| E-006 | `out/verification/res-expanded-runtime-data/resource_bundle_state.json` 与 `resource_files.jsonl` | [V] 本次实际保全 5 个安装目录 `.hpw`，36,797,762 字节，原文件和导出文件的 SHA-256 均与清单交叉核对一致。5 个格式未解析项仍保留，退出码 1。 |
| E-008 | `out/verification/res-expanded-hpw-headers.json` | [V] 文件头 XOR `0xff` 探测可见 `isom/iso2/avc1/mp41/mvhd`，顶层 box 类型仍被改写；[H] 本机 `.hpw` 很可能是混淆媒体而非 Lua。未做全文件解析/播放验证，不导出成假 Lua。 |

G-001 额外查阅版本：CUE4Parse `v1.2.2` / commit `920dc1682278d70ee738383b6fbd3a1e9b7523ea` 的 `Compression/OodleHelper.cs`。没有下载 DLL。新增 `tools/ebon/oodle.py` 是可选 ABI 适配器，仅在用户显式指定 DLL 时加载；记录 SHA-256，可先校验期望哈希。自动测试使用模拟导出函数/注入解压回调，**不是实际 Oodle 兼容性证据**。

## 待验证

| 项目 | 原因 | 下一步 |
| --- | --- | --- |
| 实机 F10 场景/脚本对象 | 合成宿主不能证明真实场景覆盖 | 新 DLL 重启后进入地图、任务与相关界面，多次 F10。 |
| 加密 Pak/IoStore 和 Oodle | 密钥和兼容解压器需显式提供 | 用已验证密钥及本地可信解压器运行 `dump-res`，检查逐项清单。 |
| 全地图/World Partition/导航碰撞 | 客户端只加载部分 cell；raw chunk 不是可用地图 | 离线索引、包重建及场景采集交叉验证。 |
| Lua/热更语义 | `.hpw` 未解析，当前快照没有确认的 Lua 资产类型 | 原文件保全，后续基于实际格式取证。 |

## BF-RES-001 / 验证记录

- 问题：场景对象被资源选择器排除；单 Pak 导出无法保全多来源资源与 loose 热更。
- 根因：只枚举三种资源基类；离线命令没有跨容器统一入口。
- 最小修复：扩展反射选择器与引用索引；独立离线模块、可选解压适配器及 CLI。
- 验证：见下方最终验证；实机验收仍待执行。
- 审查：当前会话自审，非多人签名。
- Git 快照：未执行（用户未要求提交；基线源码均为未跟踪文件）。
- 回退：仅恢复 `.work/res-baseline-20261115-003551/` 中本轮改动文件的预改备份；不要 `git reset` 或删除用户输出目录。新增离线模块没有预改版本，按文件清单单独处理。

## BF-RES-002：干净反射清单误用容错门槛

- E-005：首轮 `out/verification/res-expanded-python-tests.log`：72 项运行，DLL 重复资源请求测试失败；`fresh reflection did not produce a validated resource inventory`。
- [V] 根因：候选二进制的 `captureResources` 重试循环允许 `reflectionClean`，循环后却无条件要求 `resourceInventoryUsable()` 的 1,000 对象/类型/属性门槛，导致合成清单即使完全校验成功仍被拒绝。
- 当前源码：只有不完整的 live inventory 才应用大清单容错门槛；完整采集继续依赖 `Reflection::collect()` 的全局/属性校验。未降低容错门槛。
- 涉及文件：`native/dumper/EbonDumper.cpp`。回退源同 BF-RES-001。
- 验证：[V] 最终构建和同一 DLL 宿主重复请求回归通过；首次失败记录保留，不把它记作通过。
- 审查：当前会话自审；Git 提交未执行。

## 使用与输出判读

本轮新版原生产物位于 `build/candidate-res-expanded-final/`，保留原 `build/` 二进制。新 DLL 需要重启客户端后重新加载；已有进程不会自动切换。

```powershell
python tools/ebon_cli.py inject-and-dump `
  --injector build/candidate-res-expanded-final/EbonDebugInject.exe `
  --dumper build/candidate-res-expanded-final/EbonDumper.dll --no-sdk
```

运行后按原流程启动游戏，在每个相关场景/界面按 F10。此命令为操作说明，本轮未对真实游戏进程执行。

```powershell
$ResOut = 'out/resources/res_' + (Get-Date -Format 'yyyyMMdd_HHmmss')
python tools/ebon_cli.py dump-res `
  --client 'C:\Program Files\Neverness To Everness\Client\WindowsNoEditor\HT\Content' `
  --out $ResOut
```

- 密钥只从 `EBON_AES_KEY` 或 `--key-env` 指定的环境变量读取，不写入日志。没有密钥时逐容器记录未解析项。
- Oodle 需要可信本地兼容 DLL；加载 DLL 会在 Python 进程执行其代码，默认不加载。可指定 `--oodle-dll`、`--oodle-sha256`，单 Pak 入口也支持。
- `sources/<来源ID>/files/` 保留 loose/Pak/capture 原路径与文件内容；基础包、补丁、不同 F10 快照不相互覆盖。由 `resource_files.jsonl` 关联来源、逻辑路径、输出路径及 SHA-256。
- IoStore 输出始终是 `sources/<来源ID>/chunks/<索引>_<chunkID>.bin`。目录名仅作为 `aliases` 保留；不以 `.umap/.uasset` 名称输出不可直接加载的原始 chunk。
- F10 输出是当前对象的反射值，`is_default` 区分 CDO 和实例；`outer`、`RootComponent`、组件坐标、`references.jsonl` 用于关联。组件坐标是相对值时，不自行推算世界坐标。
- `--capture` 可重复，参数必须指向成功的 F10 `resources/res_N` 目录。原快照含未解析值时，合并结果继续保留这一状态。
- 默认预算 100,000 次文件/chunk 读取、512 MiB 单文件、32 GiB payload、512 MiB 清单、1,800 秒。样本 chunk 总数超过默认读取预算；先确认磁盘容量再提高上限。`blocked`、`error`、`filtered`、`deleted` 与 `extracted` 分开记，不伪造成功条目。
- 退出码：0 = 本次选择范围内无未解决项；1 = 已保全部分内容但存在缺项；2 = 失败或没有取得产物。`success=true` 不代表私服资源完整，`server_resources_complete` 始终为 false。

### 格式线索纠正（C-RES-001）

初始按目录/扩展名把 `.hpw` 当作可能热更资源；E-008 的媒体头部线索使这一推断不再可靠。现在只将它称为 RuntimeData 原文件，并保持 `opaque_hpw`；本机暂未取得真实 Lua 文件的证据。导出器支持 Lua 源文件/字节码的能力由合成测试证明，不能代替本机 Lua 资源命中的证据。

## 调用路径（P-RES-001）

1. `ebon_cli.main` → `dump_resources` → `ResourceBundle.run`：校验输出隔离与预算。
2. loose / F10 capture → `emit`；Pak → `PakFile.entries/read_entry` → `emit`；IoStore → `UtocFile.directories/chunks/read_chunk` → `emit`。
3. `emit` → 独占发布文件 → SHA-256 / 类型标记 → 分类索引 / 逐项清单 → 终态。
4. F10 → `captureResources` → 新反射清单 → `resources::Dumper` → 表/资产优先，再取场景与游戏 UObject → 引用索引。

## 验证时间线

1. 预改备份与哈希写入 `.work/res-baseline-20261115-003551/`，没有 Git 提交。
2. 首版 10 个原生目标构建通过；合成表/场景/脚本资产自测、反射自测与 AES 向量测试通过。
3. 实际 `.hpw` 保全与 SHA-256 交叉验证通过（E-006）。
4. 首轮全量回归暴露 BF-RES-002；失败日志原样保留（E-005）。
5. 当前源码离线相关 27 项回归通过；最终原生构建与全量回归待追加。

## BF-RES-003：连续资源请求状态删除竞态

- E-007：`out/verification/res-expanded-final-python-tests.log`：78 项运行，连续请求测试的第一轮完成，第二轮报 `resource output already used`；独立保存的 DLL 宿主同样用例当次通过，符合时序敏感表现。
- [V] 代码根因：`captureResources` 删除自己的 `discovering` 状态后再进入 `Dumper.run`，但忽略 `DeleteFileW` 失败。Windows 轮询读者不共享删除时，状态文件仍存在，触发防覆盖判断。
- 修复：显式交接“本次采集已拥有初始化状态”给 `run(true)`，不再删除状态文件；已有 manifest、issues、references 或 data 仍拒绝覆盖。F10 子目录必须独占创建，目录碰撞时不写入原有状态。
- 涉及文件：`native/dumper/EbonDumper.cpp`、`ebon_resource_dump.hpp`、`native/sdk/EbonResourceSelfTest.cpp`。
- 验证：[V] 初始化状态交接、已完成资源拒绝覆盖自测通过，并真实持有不共享删除的 Windows 文件句柄复现 `ERROR_SHARING_VIOLATION`；同一 DLL 连续请求及最后全量回归通过。失败日志保留。
- 审查：当前会话自审；Git 快照未执行；回退源同 BF-RES-001。

## 最终验证（等待实机验收）

| ID | 项目 | 结果 / 证据 |
| --- | --- | --- |
| E-009 | 10 个 MSVC x64 / C++17 原生目标 | [V] 构建通过，`build/candidate-res-expanded-final/`；构建日志 `out/verification/res-expanded-build.log`。 |
| E-010 | 最终原生资源自测 | [V] `out/verification/res-expanded-verified-native/`；场景坐标、游戏 UObject、Lua 反射资产、引用边、初始化状态交接、真实删除共享拒绝及防覆盖均通过；129 个 JSON/JSONL 记录解析通过。 |
| E-011 | DLL、资源、离线导出与 AES 失败项针对性复测 | [V] 22 项通过，`out/verification/res-expanded-focused-final-tests.log`。 |
| E-012 | 最后一轮全量 Python 回归 | [V] 79 项通过，22.091 秒，`out/verification/res-expanded-final-rerun-python-tests.log`。 |
| E-013 | 本轮代码/二进制哈希 | `out/verification/res-expanded-artifact-hashes.json`，便于确认使用正确 DLL；不是 Git 提交或多人签名。 |

原生反射自测与 AES-NI 向量自测也在最终候选构建上实际执行并通过。Python 修改文件已用 `ast.parse` 校验。原 `build/` 产物未替换。

审计保留：E-005、E-007 的失败日志不删除。其后 `res-expanded-verified-python-tests.log` 曾出现 AES 独立线程宿主 10 秒未结束；资源连续请求当时已通过。该 AES 用例针对性复测和 E-012 全量复测均通过，**超时根因仍为 [T] 未定位，不声称已修复 AES**。

三轴状态：实现与声明范围内自动测试已覆盖；真实客户端 F10、真实加密 Pak/IoStore/Oodle、Lua 命中与私服验收均未执行。客户端更新、未加载 cell、原生不可反射数据与 `.hpw` 媒体格式仍在待验证清单。没有 Git 提交，没有自动清理用户输出。
