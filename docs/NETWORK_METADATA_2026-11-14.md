# RPC 与复制元数据补充

后续运行时映射已取得，当前结果见 [RUNTIME_MAPPING_2026-11-14.md](RUNTIME_MAPPING_2026-11-14.md)。本文保留早期静态元数据阶段的证据与限制，后文“未完成”状态已被后续实测部分更新。

## 已实现

采集器导出 UFunction RPCId/RPCResponseId；从 FunctionFlags 识别 Server/Client/Multicast、Reliable；导出 FProperty RepIndex、BlueprintReplicationCondition 和 RepNotify。偏移由 profile 管理，未知版本可置为 -1。

类复制表发现逐条核对属性地址、所属类继承链、静态数组索引和 `RepIndex + ArrayIndex == table position`。类 NetFields 表核对实际 RPC 地址及继承归属，输出内存位置和表头偏移。没有将它们重命名为线协议编号。

SDK 新增 Network.hpp 与 network_metadata.json。C++ 提供 RPC/复制属性查询、类表记录及 `RepIndexValidated`。JSON 额外保存布局偏移、活动网络对象和诊断候选。network_evidence.json 保存相关字段、函数和类的原始字节，避免靠偏移猜测解释结果。

## 实际观测

- 模块哈希保持 `64ed3cae0b5fc61f3b56cdd933b4807cb23f0ec7913590db60d1a8642de2dfec`。
- 启动快照 `out/reflection/injected_20261114_183018_32316`：完整反射采集成功，1490 个 RPC、1051 个复制属性；复制索引尚未初始化为可验证类表，RPC service ID 均为 0。
- 用户确认进入可操作场景后，触发同进程 `network_snapshot_1`；观测到实际 IpNetDriver、46 张 ClassReps、28 张 NetFields、453 个类表交叉验证的属性 RepIndex、462 个与实际函数对应的 RepNotify。
- ClassReps 表头实测位于 UClass+0xf0，NetFields 表头位于 UClass+0x100。Actor 表有 11 项，bReplicateMovement、bHidden、bTearOff、bCanBeDamaged 等索引依次从 0 开始。
- 场景中部分正在卸载的动态 Blueprint 元数据未通过完整布局校验。网络生成器排除布局失败、方向标志互相冲突或与验收布局冲突的 RPC，避免把垃圾值中的 FUNC_Net 位和随机 RPCId 导出成协议。
- 反射布局来自完整启动快照；网络表来自单独的场景快照。manifest 记录网络快照 `reflection_capture_success=false` 和 75 项错误，未将该快照伪装为成功。
- `network_snapshot_1` 的 runtime_cache_candidates 为空。仅以 NetDriver 为根的有限指针图搜索不能证明目标结构不存在。

## 未完成的部分

真正的 FClassNetCache FieldNetIndex、FRepLayout 命令/父索引/Handle，以及 Iris 复制句柄尚未取得。SDK 的对应字段保留未知值，`HasObservedWireMapping=false`。现有 RPCId 和 RepIndex 不能补替这些值。

BlueprintReplicationCondition 也不是原生 GetLifetimeReplicatedProps 的最终条件；LifetimeCondition 保持未知。RepNotify 先解码 FName，再核对本类或祖先的函数路径，指针误读或不存在的函数名会被剔除并保留诊断。

## 验证与使用

原生自测覆盖 RPC ID 非零/零、属性索引为零、类表指针归属与连续索引校验、错位候选拒绝、FClassNetCache 结构候选的索引连续性；Python 覆盖旧数据缺字段、raw/JSON一致性、拒绝伪造线协议编号、场景快照和布局来源分开，以及损坏 Blueprint 排除。

启动器支持 --capture-delay（0..300 秒）。首次采集完成后，DLL 默认保留 900 秒网络补采监听；tools/request_network_capture.ps1 写入请求标记即可在同一进程取下一份快照，补采重新加载哈希保持一致的 profile。

输出目录：out/sdk/CppSDK_Replication。全量编译日志：build/replication-sdk-compile.log。确认状态和覆盖范围请读 SDK 的 sdk_manifest.json 与 network_metadata.json。

原有 `out/sdk/CppSDK` 也已补入同一 Network.hpp/网络清单，并追加 SDK 总入口；其已验证的布局文件保持原采集来源。网络记录经稳定性过滤后为 1495 个 RPC、1060 个复制属性，剔除 11 条不稳定记录。全量 `/W4 /WX` 编译验证日志：`build/canonical-network-check.log`。

最终源码及原生构建保存在 `build/candidate-network-release`，已通过原生回归及实际 DLL 合成宿主测试。当前游戏仍加载前一版带按需补采的 DLL，未尝试在游戏运行中替换它；最后构建额外把 NetDriver 指针图诊断限制为最多 5 秒，下次退出游戏后可重新运行 build_native.bat 更新默认构建。
