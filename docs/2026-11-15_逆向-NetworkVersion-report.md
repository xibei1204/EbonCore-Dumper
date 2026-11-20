# EbonCore UE NetworkVersion 确认报告

## 结论与范围

**[V] 匹配构建的 UE Local NetworkVersion 为 `1039291632`（`0x3DF254F0`）。**

本任务修复 Dumper 未采集 UE 原生握手校验版本的问题。用户同意本次实机补采并自行从启动器运行客户端。分析与采集保持只读；不伪造版本、不调用游戏版本计算函数、不修改保护机制。范围与回退记录见 [主记录](NETWORK_VERSION_2026-11-15.md)。

## Evidence → Finding → Path

| Finding / 分级 | 证据 | 位置 / 结论 | 状态 |
| --- | --- | --- | --- |
| F-NV-001 / n/a_re | E-NV-001～003、006 | getter RVA `0x16414a0`；缓存标志 `0x10291560`，值 `0x10291564`。旧文件名字符串及业务 ProtocolVersion 不是它。 | [V] 当前客户端代码 |
| F-NV-002 / n/a_re | E-NV-004、006 | Hello 的调用 RVA `0x60a75b0` 指向 getter；结果按 4 字节进入控制消息序列化。 | [V] 代码消费者链 |
| F-NV-003 / n/a_re | E-NV-007、008 | 实机缓存两次都是 `01000000f054f23d`；有效标志为 1，UInt32 LE 为 1039291632。 | [V] 实际只读观测与离线重验 |
| F-NV-004 / n/a_re | E-NV-009～011 | 自动回归、SDK 元数据、Network.hpp 静态断言及编译运行一致。 | [V] 本地工具验证 |

P-NV-001（callflow）：Hello → `GetLocalNetworkVersion` → 已初始化缓存早退 / 允许委托的计算路径 → 4 字节版本字段。Dumper 读取该缓存及代码证据 → Python 校验 SHA-256、ASLR、RIP 目标、双读字节和值 → SDK `std::optional<std::uint32_t>`。

证据清单与时间线集中于 [主记录](NETWORK_VERSION_2026-11-15.md)，其中 E-NV-001～012 具有实际本地文件来源。

## 版本与字节证据

- 原始 EXE SHA-256：`64ed3cae0b5fc61f3b56cdd933b4807cb23f0ec7913590db60d1a8642de2dfec`。
- 解码镜像 SHA-256：`94e38635dcc1168ea3d01ba4c85fdbfc99db3cb4abadc332f8d18b7f597aa3ef`。
- 实机模块基址：`0x140000000`，所有目标仍以 RVA 绑定，不假定未来启动基址。
- 实机证据：`out/reflection/network_version_live_20261115_022646/network_version.json`。
- 源码：`native/sdk/ebon_network_version.hpp`、`tools/ebon/network_version.py`；绑定在 `profiles/nte-ue5.ini` 和资源 profile。
- 新版 DLL：`build/candidate-network-version-final/EbonDumper.dll`。

## 可复现校验

```powershell
python tools/ebon_cli.py verify-network-version `
  --capture out/reflection/network_version_live_20261115_022646
$env:EBON_TEST_BUILD_DIR = (Resolve-Path 'build/candidate-network-version-final').Path
python -m unittest discover -s tests -p 'test_*.py' -v
```

本次全量 97 项通过。SDK 为 `out/sdk/CppSDK_NetworkVersion_20261115`；`Network.hpp` 编译使用 `/std:c++17 /bigobj /W4 /WX`，程序退出码 0。原有 SDK 不被覆盖。

## 尚未证明的事情

- 这不是远端服务端版本，也不证明官方服务器允许某个自建服务端。
- 代码链与真实缓存不是网络包往返验收；`packet_mapping_verified=false` 保留。
- 客户端更新、项目版本/NetCL/自定义版本或委托改变后需要重新捕获，不能固定使用本报告数值。
- 缓存未初始化或证据不符时输出空值，不通过执行引擎函数来强行制造观测。

审查：当前会话自审与实际工具验证，非多人签名。Git 提交、自动清理未执行；保留现有用户文件与游戏进程。
