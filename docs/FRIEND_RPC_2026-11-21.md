# 好友 RPC 与子消息结构（2026-11-21）

好友重采请使用新版[专用采集步骤](FRIEND_CAPTURE_FIX_2026-11-21.md)：启动脚本每次重建，运行时标识为 friend-capture-v2，默认好友专用 600 秒窗口。下文 180 秒窗口和旧 build-friend-rpc 路径为历史记录，不代表新版采集结果。

## 结论

- 反射记录确认 /Script/HTGame.HTFriendInfo 的大小为 0x270（624 字节），但没有导出字段。SDK 中的 Pad_0000[0x270] 只是保长占位，不能当成已还原的好友字段布局。
- 好友 RPC 的 InMessage 类型是 FHottaRpcMessage。其进程内结构、参数类型标签和二进制载荷容器均有静态反射证据。
- 同版本运行时原始缓存已重验：Client_Friend_RPC 的 FieldNetIndex 为 162（0xA2），Server_Friend_RPC 为 494（0x1EE）。此前引用的 NetworkLayouts SDK 没有覆盖到这张缓存，其 null 不能代表所有现存证据。
- 同版本解码镜像已找到实际序列化实现：HottaRpcMessage 没有 native NetSerialize，普通 RPC 走默认 FRepLayout；其数组长度是 16 位，不能套用基类归档 helper 的 32 位长度。
- 包头、当前会话所选路径、具体好友子消息与参数语义仍须实际收发样本对应验证；packet_mapping_verified 保持 false。

## 证据范围

结论绑定于 profiles/nte-ue5.ini 所列 HTGame.exe SHA-256：64ed3cae0b5fc61f3b56cdd933b4807cb23f0ec7913590db60d1a8642de2dfec。字段布局来源仍为同哈希反射快照 out/reflection/network_layouts_20261115_004927。索引补用成功父反射 out/reflection/injected_20261114_195359_36276 与其 network_snapshot_1 的原始网络内存；序列化代码来自 out/HTGame_unpacked.exe，SHA-256 为 94e38635dcc1168ea3d01ba4c85fdbfc99db3cb4abadc332f8d18b7f597aa3ef。输入只读，核验报告输出到独立 out/reports 目录；代码与缓存证据不冒充抓包。

## 好友对象

快照中的 /Script/HTGame.HTFriendInfo 记录为 size=624、fields=[]。SDK 对应声明 FHTFriendInfo 大小 0x270，内容只有 std::byte Pad_0000[0x270]。因此目前只确认类型尺寸；字段名、偏移、数组维度和语义均未观察到。

当前可见的外围引用是 FPlaystationSocialParams.m_FriendInfo：偏移 0x80，大小 0x270；外围结构大小为 0x2F0。其余反射字段如下：

| 偏移 | 字段 | 反射类型 / 大小 |
| ---: | --- | --- |
| 0x00 | AccountId | FString / 0x10 |
| 0x10 | OnlineId | FString / 0x10 |
| 0x20 | AvatarUrl | FString / 0x10 |
| 0x30 | AboutMe | FString / 0x10 |
| 0x40 | bIsOfficiallyVerified | bool / 1 |
| 0x44 | SDKuid | int32 / 4 |
| 0x48 | ServerId | FString / 0x10 |
| 0x58 | RoleId | FString / 0x10 |
| 0x68 | RoleName | FString / 0x10 |
| 0x78 | bOnline | bool / 1 |
| 0x79 | SocialType | 枚举底层 uint8 / 1 |
| 0x7A | bHasRoleInServer | bool / 1 |
| 0x80 | m_FriendInfo | FHTFriendInfo / 0x270 |

FHTFriendInfo 出现在平台社交包装结构中；现有静态证据没有证明它就是 HTPlayerState 好友 RPC 的子消息对象。

同一反射快照还包含字段可见的 HTGame.ChatFriendInfo，大小 0x48。它是单独的聊天联系人结构，不能与不透明的 HTFriendInfo 混用，也没有证据表明它就是 Friend_RPC 的 BinaryData 内容：

| 偏移 | 字段 | 反射类型 / 大小 |
| ---: | --- | --- |
| 0x00 | ChatChannelType | EChatChannelType / 1 |
| 0x08 | ChatToID | int64 / 8 |
| 0x10 | ChatToName | FString / 0x10 |
| 0x20 | ChatToHeadIcon | FString / 0x10 |
| 0x30 | FromMsgSeq | FString / 0x10 |
| 0x40 | bNeedGetMsgFromCloud | bool / 1 |

EChatChannelType 的观测值为 None=0、Friend=1、Team=2。

## 好友 RPC 参数

反射快照与 SDK 参数声明确认：

| 函数 | 方向 | 可靠 | 参数区 |
| --- | --- | --- | --- |
| /Script/HTGame.HTPlayerState.Server_Friend_RPC | server | 是 | InMessage: FHottaRpcMessage，偏移 0x0，大小 0x20 |
| /Script/HTGame.HTPlayerState.Client_Friend_RPC | client | 是 | InMessage: FHottaRpcMessage，偏移 0x0，大小 0x20 |

原 NetworkLayouts SDK 的两个函数仍记录 rpc_id=0、field_net_index=null；反射 RPCId 不等于线上 FieldNetIndex。补查 out/sdk/CppSDK_WireMappings/network_metadata.json 及其原始缓存后，确认以下实际运行时索引：

| 函数 | FieldNetIndex | Checksum | 原始缓存行地址 |
| --- | ---: | --- | --- |
| Client_Friend_RPC | 162 / 0xA2 | 0x32C4DB72 | 0x4AFB0CA0 |
| Server_Friend_RPC | 494 / 0x1EE | 0x61E70043 | 0x4AFB2BC0 |

缓存属于 HTPlayerState，地址 0x1A253CDA0，FieldsBase=28，元素步长 24。两条原始行分别为 89a039def47f0000a200000072dbc432000000000000bc44 与 f9c13edef47f0000ee0100004300e7610020a54400007b43。前 8 字节关联当前捕获中的 UFunction，随后为索引与 checksum；不能把此内存记录解释成线上包头。

tools/verify_friend_rpc_evidence.py 调用现有 checked_runtime_mapping，对缓存头、父类、函数指针、索引与保存内存逐字节重验；本次核验覆盖 43 张缓存、1,649 条索引。network_snapshot_1 的全局反射有 22 项错误，未被冒充为成功完整 SDK；工具使用成功父快照交叉核对两个目标的声明，报告保留此限制。out/reflection/focused_live_f10_32768_20261114_231312/network_snapshot_1 的独立观测也得到相同索引与 checksum。

## 子消息的进程内布局

以下是 SDK 与反射类型记录确认的进程内结构，不是序列化字节格式：

| 类型 | 大小 | 偏移 | 成员 |
| --- | ---: | ---: | --- |
| FHottaRpcMessage | 0x20 | 0x00 | 基类 FParameterWrapperArray，大小 0x18 |
| FParameterWrapperArray | 0x18 | 0x00 | ParameterWrappers: TArray<FParameterWrapper>，大小 0x10 |
| FParameterWrapperArray |  | 0x10 | ReadParameterIndex: int32 |
| FHottaRpcMessage |  | 0x18 | Type: int32 |
| FParameterWrapper | 0x20 | 0x00 | TypeInfo: FParameterTypeInfo，大小 0x0C |
| FParameterWrapper |  | 0x0C | 对齐填充，0x04 |
| FParameterWrapper |  | 0x10 | BinaryData: TArray<uint8>，大小 0x10 |
| FParameterTypeInfo | 0x0C | 0x00 | Type: EParameterType，反射元素宽度 1 字节 |
| FParameterTypeInfo |  | 0x04 | CustomTypeName: FName，大小 0x08 |

反射枚举 EParameterType 的观测值：None=0、Bool=1、AnsiChar=2、WideChar=3、Int8=4、Int16=5、Int32=6、Int64=7、UInt8=8、UInt16=9、UInt32=10、UInt64=11、Float=12、Double=13、String=14、Name=15、Text=16、Vector=17、Vector2D=18、Vector4=19、IntVector=20、TwoVectors=21、Rotator=22、Transform=23、Matrix=24、DateTime=25、Timespan=26、IntPoint=27、IntRect=28、Guid=29、NetworkGUID=30、Enum=31、Nullptr=32、AnsiString=33、WideString=34、Object=35、Array=36、ObjectArray=37、Custom=38。

这些值是反射类型标签；没有证据表明它们以相同宽度直接出现在网络包中。BinaryData 证明参数包装器持有字节数组，但未说明这些字节由哪种对象序列化器编码。

反射还导出 MDWrapperLibrary.MakeParameterTypeInfo、MakeParameterWrapper、MakeParameterWrapperArray 三个构造函数；它们的参数记录只含对应的 ReturnValue。这证明存在包装对象的构造入口，不能据此推出线上序列化过程。

## 已确认的序列化代码路径

tools/verify_friend_rpc_image.py 只读核对原始/解码镜像的固定 SHA-256、PE 布局、字符串引用、类型注册和 ops 虚表，并保存函数原始机器码、摘要与反汇编。完整代码证据包含以下链条：

| 证据 | RVA | 结论 |
| --- | --- | --- |
| HottaRpcMessage 注册与 ops | 0x6BC9780 → 0x6BE9140 → 0x6BC97C0；虚表 0xD1021F8 | Serialize / NetSerialize 槽均为返回 false 的 0x14AC640 |
| PrepareCppStructOps | 0x179B2E0 | capabilities 的 0x40 位控制 StructFlags 的 0x400 位；Hotta 返回 0x2004，Wrapper/WrapperArray 返回 0x204C |
| NetSerializeStruct | 0x4223610 | 有 native 位才调用 ops+0x50；否则构建默认 FRepLayout |
| 默认结构展开 | 0x47BEFB0、0x47BF9D0 | 包括父类，排除 RepSkip，按成员偏移排序；ReadParameterIndex 未设置 RepSkip |
| 普通 RPC 参数接收 | 0x47EB330 | 非回放路径对非 Bool 顶层参数读 1 个 presence bit；不是每个结构成员各读一位 |
| 默认动态数组接收 | 0x47F7F80 | 读 uint16 元素数量，再逐元素序列化 |
| IntProperty NetSerializeItem | 0x186D770 → 0x17A2E40 | 固定序列化 4 字节，未使用 packed 整数编码 |
| ParameterWrapper native NetSerialize | 0x6BCA800 → 0x6BDC130 | 读 1 字节类型标签；特殊类型分支见下 |
| 字节数组归档 | 0x155C340 | int32 字节长度后跟载荷；网络分支拒绝负长度和超过 16 MiB 的长度 |

普通默认 RPC 参数路径的逻辑顺序为：顶层 InMessage 的 presence bit，随后 ParameterWrappers 的 uint16 数量及各元素，再处理各占 32 位的 ReadParameterIndex 和 Type。presence 为零则参数保持默认值。这是匹配二进制的静态接收规则，尚未证明当前会话使用普通非回放路径，更没有给出外层 ActorChannel/Bunch 头、可靠序号、字段导出或加密格式。它也不保证从 UDP/TCP 包的字节零开始就能解析。

包装器有以下已确认的分支：

- 所有标签先经过 1 字节序列化。
- 标签 36、37、38（Array、ObjectArray、Custom）额外调用 archive 的 FName 虚方法处理 CustomTypeName。FName 的线路表示依赖该 archive，不能发送进程内的 name index/number 代替。
- Object=35 和 ObjectArray=37 在存在 PackageMap 且 archive 设置对应网络标志时走对象映射；不能把本地 UObject 指针或 BinaryData 直接发出去。
- 其余普通分支通过 0x155C340 处理 BinaryData。FText=16 在网络读取后还会清理本地载荷，参数内存采样因此未必保留其原始网络正文。
- 标量 archive 在 byte-swapping 标志置位时走 0x1674940 的 bswap 路径；不能脱离会话端序直接宣称所有捕获均为固定小端。

基类 ParameterWrapperArray 的 helper 0x6BDC7C0 使用 int32 count（上限 1024），且不处理 ReadParameterIndex。这个 helper 由基类自身的 Serialize/NetSerialize 调用。派生 HottaRpcMessage 的 ops 能力不同，完整好友 RPC 不能照搬该 helper。

## 可重现核验与补采

机器可读规格见 [friend_rpc.protocol.spec.json](friend_rpc.protocol.spec.json)。本轮报告为 out/reports/friend_rpc_evidence_20261121.json 与 out/reports/friend_rpc_image_verified_20261121.json；前者核验真实缓存，后者核验固定镜像代码，均没有将包级验证置为 true。

运行时索引重验（报告必须是源采集目录以外的新文件）：

~~~powershell
python tools/verify_friend_rpc_evidence.py --reflection out/reflection/injected_20261114_195359_36276 --network out/reflection/injected_20261114_195359_36276/network_snapshot_1 --expected-sha256 64ed3cae0b5fc61f3b56cdd933b4807cb23f0ec7913590db60d1a8642de2dfec --out out/reports/friend_rpc_evidence_new.json
python tools/verify_friend_rpc_image.py --image out/HTGame_unpacked.exe --module-image 'C:/Program Files/Neverness To Everness/Client/WindowsNoEditor/HT/Binaries/Win64/HTGame.exe' --out out/reports/friend_rpc_image_new.json
~~~

新版采样器同时观察 Client_Friend_RPC / Server_Friend_RPC 的 ProcessEvent 与 native 分发路径，验证继承布局后记录 message_type、包装器类型与 payload hex、方向、模块哈希和 GetTickCount64 时间；保留原调用并去重。使用 run_friend_capture.bat，已加载旧 DLL 的客户端须先退出并重新启动。状态必须出现 friend_client_schema_verified / friend_server_schema_verified；native 两项安装状态分别记录，不能仅凭 installed 推断好友入口成功。等待 armed 后打开或刷新好友列表，过期后按 F8 重开窗口。

采样记录仍标 wire_packet_bytes_captured=false，它们是参数对象。真正的网卡包另用 tools/capture_friend_packets.ps1，在管理员 PowerShell 中传入当前游戏 PID、新输出目录与 5–120 秒窗口。脚本校验客户端哈希、拒绝已有 PktMon 会话/筛选器和跨进程共享端口，按游戏当前独占端口过滤，自动停止并只清理自建筛选器，保留 ETL 与 pcapng；启动之后新开的端口不在覆盖范围。当前普通终端调用 PktMon 返回拒绝访问，不能据此声称已抓包。

## 尚待包级验证

FieldNetIndex 的内存观测缺口已关闭。仍需将同次好友操作的原始包/bit 区间与采样实参对齐，确认会话路径、实际消息 Type 与参数顺序、Custom 的内部 schema、FName/PackageMap 和外层 framing。拿到原始加密包或非零采样计数本身都不能把 packet_mapping_verified 改为 true。

## 本轮验证记录

- MSVC 完整构建 11 个目标成功，最终产物在 build-friend-rpc-final；当前实机加载的是此前通过定向测试的 build-friend-rpc 产物，两者不混称同一加载版本。
- 全仓 Python 125 项通过，日志 out/verification/friend-rpc-final-tests.log。此前构建期间工作区同步采样源码继续变化，旧构建对新测试曾有 3 项不匹配；重新构建后全部通过，未通过删除新测试规避。
- 好友合成宿主验证两方向、ProcessEvent/native 路径与去重、原调用与结果保留，以及错误布局/哈希/frame/count/预算拒绝。合成参数不是线上样本。
- 新实机目录 out/reflection/injected_20261121_182348_22176 的两个好友 schema 与 native 安装状态均为 true；该轮 180 秒窗口结束时 window_samples=0、window_target_calls=0。因此仅验收安装，没有验收好友子消息或原始包。
- 当前普通终端无法访问 PktMon；桌面控制也不能向管理员权限日志窗口发送 F8。后续须在有权限的终端抓包，并重新开启窗口后触发好友操作。
