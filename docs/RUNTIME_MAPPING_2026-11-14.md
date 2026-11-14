# 实际运行时网络映射

## 结果

完整布局来源：`out/reflection/injected_20261114_195359_36276`。

用户确认进入可操作场景后，同进程补采：`out/reflection/injected_20261114_195359_36276/network_snapshot_1`。

解码并逐字节复核后：

- 43 个 FClassNetCache，1649 个字段网络索引，其中 1203 个属于 RPC。
- 8 个 FRepLayout，301 个非 Return 复制命令句柄；这 8 个布局的 Parents/Cmds 均完整覆盖。
- 覆盖 PlayerController、GameplayAttributeSet、HTInventoryComponent、GameExperienceComponent、HTTimerClockComponent、当前场景类和两个武器类。
- manifest 状态 `observed_partial`；数据包往返尚未验收，`packet_mapping_verified=false`。未覆盖类、动态数组的实例路径以及 Iris 协议不由上述数量推断。

SDK 输出：`out/sdk/CppSDK_WireMappings`。原有 `out/sdk/CppSDK` 的 Network.hpp、网络清单和 manifest 网络段同步更新，保留其原布局来源。

## 为什么旧探测器漏检

旧探测器错误地要求缓存以裸 UClass 指针开头，并用 16 字节 FieldCache 步长。实际捕获中的类标识是弱引用（对象索引与 serial），字段记录步长为 24 字节（包含 incompatible 标志）。FRepParentCmd 的命令起止字段在该版本偏移 28/30，早期假设是 24/26。

本轮修正这些假设，并使用真实对象数组、继承链、字段地址和原始记录交叉验证。引擎参考定义仅用于提出结构候选；实际结果来自此客户端的运行时字节，不把其他引擎版本源码当作当前行为证明。

## 证据验证

NetDriver 根来自本次捕获的活动网络对象。有限指针图只读扫描后保存 memory.bin 与块索引；未调用引擎函数去人为创建缓存。扫描的节点、队列、时间和总字节数均有上限，达到边界会记录 budget_exhausted。

缓存校验：弱类索引对应捕获对象、serial 非零、字段所属类一致、父缓存类与继承关系一致、FieldNetIndex 连续并从实际 FieldsBase 开始，记录地址和 checksum 可对照内存。

复制布局校验：实际 Owner 指针匹配类；Parents 指向已采集的属性；每个父命令的静态数组元素、命令起止范围、条件与原始字节一致；每个非 Return Cmd 的字段、ParentIndex、RelativeHandle、内存偏移、动态数组结束索引均校验。嵌套字段保留顶层 parent_path 和 scope_cmd_index，不能用 RepIndex 替代句柄。

离线输出 runtime_mapping.json 还会在 SDK 生成时重新核对 network_memory.bin。只有实际捕获的同哈希内存能提升状态。伪造非零 ID、修改原始记录或复制其他版本映射都被拒绝。

## 文件和接口

- `native/sdk/ebon_network_runtime.hpp`：运行时缓存和复制布局解码、有限扫描与完整记录保存。
- `tools/ebon/runtime_network.py`：对保存的内存执行可重复的离线解码。
- `tools/ebon/runtime_mapping.py`：根据同一捕获重验实际地址和原始字节。
- `Network.hpp`：RuntimeFieldIndices、RuntimeRepHandles、FindFieldIndex、FindRepHandle。
- `network_metadata.json`：完整记录、上下文、原始字节、覆盖数量与未确认项。
- `sdk_manifest.json`：索引与句柄观测数量、observed_partial、packet_mapping_verified=false。

## 实例

先前局部捕获已实测 PlayerController 中 Actor.bReplicateMovement 的 RelativeHandle 为 1、bHidden 为 2。其 RepIndex 为 0、1，两者不能混用。HTGuitarGamePlayComponent.Multicast_GuitarAudioType 的 FClassNetCache FieldNetIndex 实测为 2，并有对应 checksum；该编号应在所属类/缓存上下文中使用。

## 验证范围

原生自测覆盖实际弱引用缓存形状、正确/错误索引、复制父命令范围和错误 ParentIndex；实际 DLL 合成宿主测试通过。Python 测试包含原始字节、索引或句柄被篡改后的拒绝、错误模块哈希拒绝、自动离线解码及旧捕获保持 not_observed。全量 SDK 使用 MSVC /bigobj /std:c++17 /W4 /WX 编译链接检查。

这些验证支持“实际运行时映射已部分恢复”，不声称已完成数据包发送/接收验收或能据此实现整个服务器。

最终覆盖计数：Parents 171/171，Cmds 310/310，其中 301 个有句柄的非 Return 命令。43 个缓存中的记录分别按所属类保留，RPC 可直接查询的 FieldNetIndex 为 1203 条。原 SDK 的 `RuntimeMappingExample.cpp` 编译运行返回 0，全量编译记录在 `build/canonical-runtime-compile.log`，新 SDK 全量编译记录在 `build/wire-sdk-compile.log`。

目录中旧的 CppSDK_Replication 及更早 trial 输出是历史结果，其 not_observed 状态未用于冒充本轮观测。当前结果请查看 out/sdk/CppSDK 或 out/sdk/CppSDK_WireMappings。场景快照仍有 22 项动态蓝图布局错误；完整 SDK 声明来自成功的启动快照，映射记录则按原始内存和有效类型单独验收。

最终定向回归：以 `EBON_TEST_BUILD_DIR=build/candidate-wire-complete` 运行 runtime_mapping、network_codegen、cpp_sdk 和 dll_capture，共 17 项通过。全量自动发现曾同时纳入并行新增的 AES 测试，因该任务的测试/构建处于变更中出现失败与超时；不将该轮称为全仓测试通过。默认 build 下的部分合成宿主/编译测试也出现过超时，以指定已验收构建后的定向复测结果为本任务验证依据。

## 最新实机复核与剩余缺口

较早的 `injected_20261114_195359_36276/network_snapshot_1` 曾观察到 43 个 `FClassNetCache`，包括 `HTPlayerState` 的 854 条、`HTGameState` 的 20 条和 `WorldDataLayers` 的 5 条字段索引；它们仍不是相应类的完整 `FRepLayout`。

最新实机目录 `out/reflection/focused_live_20261114_220120/network_snapshot_1` 的模块哈希匹配目标版本。离线解码得到 9 个完整布局，新增 `/Game/Blueprints/Weapon/Player/Weapon_Player_Adler.Weapon_Player_Adler_C`；没有解出任何 `FClassNetCache`。扫描包含 98 个目标根，但在 2,048 个节点处达到上限（`budget_exhausted=true`），保存约 1.51 MB 网络内存。因此玩家状态、角色、技能/属性组件、GameState 和 WorldDataLayers 的布局仍属未捕获，不能据此断言这些类没有复制布局。全局反射快照因动态蓝图变化标记失败；逐记录交叉验证通过的 9 个网络布局仍可离线重放，不能据此声称全局快照完整。

针对这次预算截断，扫描上限已提高到 8,192 个访问节点、32,768 个排队节点；单次总读内存仍不超过 16 MiB，扫描时限为 20 秒，每个明确根最多读取 64 KiB。新版在 `build/candidate-focused-runtime-8192`，未替换当前进程加载的 DLL。候选版通过完整 MSVC 构建、2,100 节点预算回归、反射自测、DLL 合成宿主、AES-NI、资源自测，以及 10 项网络映射/代码生成 Python 测试。仍需用新版本重启同哈希客户端并再次补采，才能知道增加预算是否找到目标布局。

有效 `NetworkVersion` 仍未确认。镜像中的 `NetworkVersion.cpp` 文本是引擎源码路径；`ProtocolVersion` 属于 `FHTBalatroTransportChunk` 结构。IDA 字符串交叉引用没有给出可认定的网络版本数值。需要握手字节或确认版本计算函数及其输入，才能交付有效值。

## 22:46 第二轮实机补采与后续候选版

实机采集目录为 `out/reflection/focused_live_8192_20261114_224241/network_snapshot_1`。扫描根增加到 123，保存网络内存约 6.32 MB，仍触及 8,192 节点上限且 `budget_exhausted=true`；FClassNetCache 仍为 0。离线记录含 10 个完整布局，其中玩家武器新增 Adler 和 Haniel，目标玩家状态、角色、技能/属性组件、GameState、WorldDataLayers 布局仍未命中。包内原始 `rep_layouts` 有 17 项，离线交叉验证保留 10 项；SDK 只应使用离线重验结果。

候选版已构建于 `build/candidate-focused-runtime-f10-32768`：访问节点上限 32,768、排队上限 65,536，仍受 20 秒/16 MiB 限制；预算自测使用 10,000 个根节点。资源导出热键、日志提示和文档已从 F11 切换为 F10。完整 MSVC 构建、原生反射/DLL/AES/资源自测及 11 项网络映射/资源 Python 测试通过。当前已构建但尚未实机验证；需用该候选版重启同哈希客户端才能验证新预算与 F10。
