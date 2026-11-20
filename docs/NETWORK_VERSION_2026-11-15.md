# UE Local NetworkVersion 恢复与采集

## 当前确认值

**[V] 此版本客户端的 Local NetworkVersion = `1039291632`（`0x3DF254F0`）。**

2026-11-15 用户同意实机补采后，`out/reflection/network_version_live_20261115_022646/network_version.json` 保存了实际 getter/Hello 指令及两次一致的缓存字节 `01000000f054f23d`。原生读取不执行版本计算、不更改游戏缓存；Python 独立重验导出的值、代码形状、RIP 目标、模块哈希和 ASLR base。

此值是**本次匹配构建的本地连接校验值**，不是 EngineNetworkProtocolVersion=42，也不是远端服务端版本。包往返、完整服务器兼容性仍未验证。

## 范围

- M-NV-001：用当前同哈希客户端的解码镜像追踪 `GetLocalNetworkVersion`、CRC、缓存及 Hello 序列化来源。
- M-NV-002：只读读取经过代码校验的缓存；未初始化、哈希不符或读不稳定时保留 `null`，不执行引擎函数、不修改版本。
- M-NV-003：保存可重验字节证据，CLI 等待实机结果，SDK 导出可选 UInt32。
- M-NV-004：合成负例与真实客户端补采分开验证。用户已同意实机补采。
- 不包含：服务端版本伪造、兼容性绕过、握手发送、保护层修改、游戏账号操作。
- 退出标准：来源与消费者链核对、原生/Python 回归通过、真实缓存值与 SDK 重验一致；实机前不得宣布已确认具体数值。

## 证据清点

| ID | 来源 | 结论 |
| --- | --- | --- |
| E-NV-001 | `out/research/network_version_ida.txt`、`find_network_version.py` | [V] 旧脚本查字符串后才等待分析，且没有找到关键 UTF-16 格式串；源码文件名不是数值证据。 |
| E-NV-002 | `out/HTGame_unpacked.exe`（SHA-256 `94e38635dcc1168ea3d01ba4c85fdbfc99db3cb4abadc332f8d18b7f597aa3ef`）、`out/research/network-version-scout.json` | [V] RIP 相对引用定位真正函数；原始安装 EXE SHA-256 为 profile 中的 `64ed3cae0b5fc61f3b56cdd933b4807cb23f0ec7913590db60d1a8642de2dfec`。 |
| E-NV-003 | 当前镜像反汇编 | [V] getter RVA `0x16414a0`；缓存有效标志 RVA `0x10291560`；UInt32 校验值 RVA `0x10291564`。缓存路径先于委托路径返回。 |
| E-NV-004 | 当前镜像反汇编 | [V] Hello 调用 RVA `0x60a75b0` 调用相同 getter，EAX 存入栈变量，随后按 4 字节序列化；相邻有 Endianness、EncryptionToken、RuntimeFeatures 字段。业务 `FHTBalatroTransportChunk.ProtocolVersion` 不在这条链。 |
| E-NV-005 | 既有 `network_memory.json/bin` 和本机 `UEMinidump.dmp` 的有界检查 | [V] 没有保存对应全局缓存范围；本机 HT 日志为非明文，不能从搜索结果声称不存在版本。 |
| E-NV-006 | `out/research/network-version-static.json` | [V] 原始/解码镜像布局与 SHA-256、getter/Hello RIP 目标和完整代码段记录；仅代码证据，不伪装成运行时数值。 |
| E-NV-007 | `out/reflection/network_version_live_20261115_022646/network_version.json` | [V] `observed_cached`，值 1039291632；模块哈希与 profile 相符，缓存两次为 `01000000f054f23d`。 |
| E-NV-008 | 同次 `dump_state.json` 与 `verify-network-version` 实际运行 | [V] 成功父反射快照：105,399 Objects、71,874 Types、193,600 Properties；本地版本字节重验通过。整体反射仍有 optional unresolved，不影响独立版本证据。 |
| E-NV-009 | `out/verification/network-version-full-tests.log` | [V] 最终候选构建全量 97 项回归通过（47.976 秒）。 |
| E-NV-010 | `out/sdk/CppSDK_NetworkVersion_20261115/Network.hpp`、`sdk_manifest.json` | [V] SDK 导出实际 `LocalNetworkVersion=1039291632`、`network_version_verified=true`；不替换原有 SDK。 |
| E-NV-011 | `out/verification/network-version/compile.bat` 与 `CompileNetworkVersion.cpp/.exe` | [V] MSVC `/std:c++17 /bigobj /W4 /WX` 编译、静态断言和程序运行退出码 0；该检查针对 Network.hpp，不冒称本轮全量 SDK ABI / 官方服务器验收。 |
| E-NV-012 | `out/verification/network-version-build.log`、`network-version-live.log`、`network-version-hashes.json` | 构建、实际补采命令输出与候选/证据/SDK 文件哈希；不是 Git 提交或外部签名。 |

## 工具与推断边界

- 复用本机 `pefile 2024.8.26`（MIT）、`Capstone 5.0.9`（BSD-3-Clause），`.work/network-version/scout.py` 和 `tools/verify_network_version_image.py` 只读执行；静态核对不执行客户端镜像。原生探针不需要这两个 Python 依赖。
- 引擎参考：https://github.com/chenyong2github/UnrealEngine/blob/5.3/Engine/Source/Runtime/Core/Private/Misc/NetworkVersion.cpp 。仅解释候选机制；实际 RVA/指令来自 E-NV-002。
- 本版本不是旧五字段公式。前缀是 `%s %s, NetCL: %d`，随后按当前容器顺序添加 `, %s: %d`。CRC 函数 RVA `0x1529e90` 对每个 UTF-16 码元处理低、高字节，再处理两个零字节（4 字节语义），不能直接用 UTF-16 的 zlib CRC 替代。
- 自定义网络版本默认注册的 Engine 值为 42、Game 值为 0；项目版本、NetCL、注册顺序和委托仍可改变，**不据此捏造最终校验值**。

## BF-NV-001

- 问题：Dumper 无 UE 握手版本值采集，文档长期保留未确认项。
- 根因：原探针缺 UTF-16/RIP 相对调用链识别；反射清单不覆盖全局原生缓存；未区分计算输入、缓存及业务 ProtocolVersion。
- 修复：独立只读缓存探针，完整模块哈希绑定 + getter/Hello 字节验证；保存双读缓存证据并在 SDK 生成时重验。
- 预改副本：`.work/network-version-baseline-20261115-020914/`。原有未提交文档改动保留，不覆盖。
- 验证与实机状态：[V] 已取得 E-NV-007 / E-NV-008 实机值并独立重验，97 项回归通过；SDK 状态在下方单独记录。
- Git 提交：未执行（用户未要求）；审查为当前会话自审，非多人签名。
- 回退：只恢复本轮改动文件的预改副本；不重置工作区或清除已有 dump。

## 使用

```powershell
python tools/ebon_cli.py inject-and-dump `
  --injector build/candidate-network-version-final/EbonDebugInject.exe `
  --dumper build/candidate-network-version-final/EbonDumper.dll `
  --profile profiles/nte-ue5.ini --no-sdk --wait-network-version
```

先运行，再从自己的启动器启动客户端；进入场景即可，不需 F10。DLL 启动快照写完后，最多 900 秒每秒观察一次缓存。`--network-version-wait-seconds` 默认 600 秒；CLI 未观测/未初始化时不能以版本确认成功退出。

```powershell
python tools/ebon_cli.py verify-network-version `
  --capture out/reflection/network_version_live_20261115_022646
python tools/ebon_cli.py generate-sdk `
  --dump out/reflection/network_version_live_20261115_022646 `
  --out out/sdk/CppSDK_NetworkVersion_20261115
```

`network_version.json` 与 SDK 的 `network_metadata.json` / `sdk_manifest.json` 包含来源和验证状态；`SDK::Network::LocalNetworkVersion` 是 `std::optional<std::uint32_t>`。只有重验通过才含值；旧 dump 或未初始化缓存为 `std::nullopt`，不会回填本报告数值。

外部只读模式可使用 `EbonDumper.exe --pid <PID> --profile profiles/nte-ue5.ini --out <新目录> --network-version-only`，仅采版本，不冒充反射快照。受保护环境中读取失败时明确失败，不尝试更换或绕过保护。

## 验证边界与时间线

1. 原有未提交文档改动保全到 BF-NV-001 的预改目录。
2. UTF-16/RIP 字符串索引、完整 getter / CRC / Hello 反汇编及原始文件哈希交叉核对通过。
3. 首次定向运行因 PowerShell 构建路径变量错误引用旧默认 EXE，原生 JSON 用例失败；没有把该轮记作通过。改用明确构建目录后，29 项定向回归通过。
4. 最终 `build/candidate-network-version-final/` 全部 10 个原生目标构建通过，无该轮新增编译警告；全量 97 项回归通过。
5. 用户启动同版本客户端，真实缓存观测到 1039291632，独立 CLI 重验通过（E-NV-007 / E-NV-008）。
6. SDK 生成到 `out/sdk/CppSDK_NetworkVersion_20261115`，版本元数据含实机值与来源；仅 Network.hpp 的 C++ 编译、静态断言和程序运行通过，不把它当作本轮全量 SDK 或官方服务器验收。

自审：没有强制调用引擎方法、没有写入游戏内存、没有用业务 ProtocolVersion 或固定数字兜底。未知版本和证据不一致被拒绝。数字 0 / 0xffffffff 都可以是实际 UInt32，但必须具有已初始化缓存和代码证据。完整网络映射的 `packet_mapping_verified` 仍为 false。

## 状态

- 实现：getter / Hello / 双读缓存采集、原始字节保全、离线重验、CLI 等待及 SDK 可选值导出已覆盖。
- 自动测试：[V] 97 项全量回归通过，包含未初始化、不一致字节、篡改值、错误哈希/ASLR/RIP 目标、0 / UInt32 最大值、旧 dump 不造值及原生 JSON 重验。
- 实机：[V] 用户启动同版本客户端，真实缓存值与离线 / SDK / C++ 断言一致（E-NV-007～011）。
- 尚未执行：网络包往返、服务端版本/兼容性验证、其他客户端版本。不能把本地版本确认扩展为全服务器实现或所有协议验证。
- 原始 `build/`、原有 SDK 与用户文档改动保留；没有提交、没有自动清理、没有停止用户游戏进程。
