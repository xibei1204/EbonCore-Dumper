# F8 PlayerState 采样器绑定修复

2026-11-21 的失败快照 injected_20261121_165459_1592 显示 no_verified_playerstate_vtable，样本文件为空。原实现只接受 PlayerState 的虚表第 76 项等于根 UObject 事件入口 0x18d4d20，失败状态只打印一次英文代码。

本地 out/HTGame_unpacked.exe 的静态反汇编显示派生入口 RVA 0x3e6cb90：

- 0x3e6cb9f 检查 [rdx+0xb0] 的 0x400 标志。
- 0x3e6cba9、0x3e6cbac、0x3e6cbaf 分别保存参数、函数和对象。
- 0x3e6cbf4、0x3e6cbf7、0x3e6cbfa 恢复三个参数。
- 0x3e6cbfd 调用根入口 0x18d4d20。

上述派生入口地址还出现在映像的数据区虚表中。此次静态核对使用本地 Python/Capstone；不是 REA 或实机调用证明。本次本地解包映像 SHA-256 为 94e38635dcc1168ea3d01ba4c85fdbfc99db3cb4abadc332f8d18b7f597aa3ef；根入口的既有核对见 profiles/nte-sdk-runtime.json。运行时仍需用实际 PlayerState 虚表项确认派生入口。

专用配置现在分别绑定根入口和派生入口。安装时保留客户端哈希、根虚表项和参数结构校验，只修改实际目标与配置完全匹配的 PlayerState 虚表项；调用结束仍转发原派生函数。状态文件新增 installed、根/派生入口和逐对象 binding_diagnostics。反射还会保存 Actor、HottaPlayerState、HTPlayerState 的默认对象虚表，方便下次实机核对。

隔离进程回归覆盖拒绝未配置的派生入口、拒绝错误根绑定、安装共享虚表、保留原函数及三个参数、未开启窗口不采样、只记录目标函数、重复开启窗口和状态日志。日志窗口标题持续显示 F8 状态。

这些验证不等于目标客户端已采样成功。运行中的旧 DLL 不会自动更新；需重启客户端并通过根目录 run_gameplay_capture.bat 加载当前 build-gameplay-observed-sync 产物，再以本次实际状态和样本为准。

## 第二次修复：已安装但窗口没有样本

实机快照 injected_20261121_173339_10668 确认虚表安装已成功：两个 PlayerState 虚表均被监听；17:36:52 与 17:37:35 开启的两次 30 秒窗口均未保存样本。之后 F10 成功导出资源。旧状态缺少入口调用与解析拒绝计数，无法单凭零样本确定是目标没有调用、分发路径不同还是解析失败。

本次补充的静态证据来自同一 SHA-256 的本地解包映像：

- UFunction::Invoke 的 RVA 0x1798c93 调用 UFunction 的 +0xd8 函数指针；0x1798c89 设置 FFrame +0x90 的当前 native 函数。
- FFrame 构造函数 0x18d1fb0 将 Node、Object、Code、Locals 分别写入 +0x10、+0x18、+0x20、+0x28。
- StepExplicitProperty 的 0x18d5d0a 从 FFrame +0x28 取得参数基址，再加反射字段偏移。
- 真实反射给出两个 native 入口 RVA：ClientGameDataRPC 为 0x99e84c0；OnRecChange_MiniMapFogRec 为 0x9a0e450。两个 thunk 均检查 FFrame +0x20 选择字节码/编译参数路径。

新增采样只在客户端哈希、反射 native RVA 和实际函数指针全部匹配时安装。调用原 native 函数前，验证 Node、Object、当前 native 函数和 Code=0，再按 Locals 指针读取已验证的参数布局。字节码调用只计数并拒绝当作参数块读取。原函数、FFrame 和返回值指针按原样转发。虚表入口和 native 入口对同一调用去重。

专用配置自动开启 180 秒窗口，默认最多 128 条样本，仍保留 16 MiB 队列和每条 2 MiB payload 预算。F8 可重新开启窗口。状态增加两个入口调用数、目标命中、参数/帧拒绝，以及有上限的事件函数计数。自动采样不能恢复安装之前的调用；本次修复仍需实机验证，不能据隔离回归宣称已捕获完整地图或 GameDataFirstSync 线路包。

## 第三次修复：目标范围与实际同步路径不符

实机采集 injected_20261121_181237_12720 在 18:17:35—18:20:35 的窗口中记录 4210 次 PlayerState ProcessEvent 调用，其中存在 ClientCommonFunRPC、四次 ClientDataLayerRPC、SendReplicatedObjectPropertyArrayToClient 以及 ServerMapIconTransfer、OnFinishTransfered。目标 native 调用、参数拒绝、帧拒绝和保存样本均为 0。证据表明监听工作正常，但原先选定的两个目标在该窗口中没有出现；没有证据将空样本归因为参数解析失败。

此次增加三个实际出现的同步入口的参数保存，均需匹配反射布局；沿用已观察到的 PlayerState ProcessEvent 入口，不增加未验证的 native 入口。两个 HottaRpcMessage 同步的 Type 位于偏移 24，区别于 GameDataRPC 的 InMsgID；ClientCommonFunRPC 还具有偏移 32 的目标 Actor。批量对象属性采用 96 字节容器、32 字节属性、FName 名称、TArray 字节 payload 和 bCompressed。继承关系、元素大小、偏移和压缩掩码均需验证。

批量样本按调用共用 payload 预算，最多保存 128 个容器、2048 个属性。数组头与读取后的数组头不一致则拒绝；预算裁剪标记 truncated。压缩 payload 保持原样，不猜测解码。样本和状态明确保留 map_coverage_verified=false，captured_by_kind 区分实际来源。新增路径不是已确认的地图协议；后续必须按真实样本内容判断是否包含 MiniMapFogRecord。

隔离回归专门模拟原先目标不再调用、仅三个新增入口调用的窗口，校验能够生成三类真实命名样本、保留 message type/actor/property payload/压缩标记，并验证错误布局、损坏数组、预算裁剪和 FName 解析。不能用合成样本代替客户端验收。
