# 独立 ChatSDK 客户端静态逆向记录

## 结论

该客户端包含独立聊天 SDK 接入，文本私聊、群聊和频道文本都有明确的 ChatSDK API 与 UE 包装入口。它不是 HTPlayerState 好友 RPC 的另一种名字：对象模型与调用接口不同。静态证据支持 ChatSDK 有独立 IM 网络实现；还不足以确认每次 UI 操作的运行时加载分支、实际服务器地址、IM 消息线路编码或反垃圾请求的完整交互。

## 版本与证据范围

以下证据绑定本机 Windows x64 客户端：

| 文件 | SHA-256 | 证据 |
|---|---|---|
| HTGame.exe | 64ed3cae0b5fc61f3b56cdd933b4807cb23f0ec7913590db60d1a8642de2dfec | 现有配置档和 2026-11-21 反射快照一致 |
| WPChatSDK_64.dll | 4805881bc725a73bf07fd39cab7b146b4c6f76546c870d65c6fbd4d57e4a8627 | 游戏目录中的 SDK 导出与 PE 导入表 |
| WMCommunityImpl_64.dll | 71c0d24b70694c37dd63df99df59cc9800d527f35656eb22663355b71d9fed76 | 启动器目录中的实现层导出、导入表和字符串 |

两个 Chat DLL 的 Authenticode 签名均验证为 Perfect World。反射数据来自 out/reflection/injected_20261121_111221_21112，绑定相同 HTGame.exe 哈希。二进制输入保持只读；分析数据库和提取文本放在 .work/chatsdk/。本轮没有运行客户端，也没有抓取线上流量。

## 调用链

已证实的静态组件分三层：

1. UE 反射中有 /Script/ChatSDK.WMChatSDKSubsystem，其 CommunitySDK 字段指向 ChatCommunitySDK；HTGame.ChatSDKWrapper 暴露登录、发送、历史消息和接收回调。
2. WPChatSDK_64.dll 有 947 个命名导出，包括 ChatSDK_CreateApiClient、ChatSDK_DestroyApiClient、IChat_Init/Login/SendChatMessage/SendGroupMessage/SendChannelMessage、消息构造与访问接口、ICommunity_* 接口族和客服 IWMKefu_* 接口。PE 导入表只列 KERNEL32，其中包括 LoadLibraryW 和 GetProcAddress。
3. WMCommunityImpl_64.dll 导出 _CreateCommunityApiClient、_DestroyCommunityApi、_InitAndSetRecvCallback、_SendCall、_SetConfigInfo、_SetInfo、_SetCommunityTracker 和 _UnInitAll。

游戏二进制含 PcCommunitySdk.cpp、CommunitySDKPCAdapter.cpp、ChatSDKWrapper.cpp 等源文件标记，并同时包含 WPChatSDK_64.dll、WMCommunityImpl_64.dll、ChatSDK_CreateApiClient 和 _CreateCommunityApiClient 字符串及加载失败日志。这支持两者属于同一 SDK 接入体系，但不能单凭字符串确定具体运行时一定按 WPChatSDK → WMCommunityImpl 顺序加载，或选择哪条动态加载分支。

WMCommunityImpl_64.dll 导入 Winsock、Crypt32、Bcrypt 等系统接口，并包含 libcurl/OpenSSL 风格的 HTTP、TLS、HTTP/2 字符串。静态证据支持“SDK 有自己的网络/IM 实现”这一判断，但还不能据此确定 IM 长连接具体使用 TCP、WebSocket 或其他协议。

## 登录与发消息模型

ChatCommunitySDK.Init 的反射参数是 AppID、AppKey 和初始化回调。Login 接收 GameUserInfo，其字段依次为 uid、Token、RoleId、ServerId、RoleName、ServerName、Avatar、GameExt。HTGame.ChatSDKWrapper.LoginWithUserInfo 接收 World、HTPlayerState、Avatar 和 GameExt，并有单独的登录成功/失败回调。

文本发送入口明确分为：

| 目标 | UE 包装方法 | 关键参数 |
|---|---|---|
| 单聊 | ChatSDKWrapper.SendTextMessage | Text、To、Ext |
| 群聊 | ChatSDKWrapper.SendGroupTextMessage | Text、To、AtAll、AtList、Ext |
| 频道 | ChatSDKWrapper.SendChannelTextMessage | Text、ChannelId、AtAll、AtList、Ext |

此外有 SendCommonTextMessage，多一个 LogInfo 参数；底层 ChatCommunitySDK 对应 SendChatMessage、SendGroupMessage、SendChannelMessage。这些方法返回 ChatSDKSendMsgResult 或 SendMsgResult，含成功标记、ClientMsgId、状态码和描述。接收侧有单聊、群聊、频道、通知消息和会话/群/频道事件回调，也支持本地与历史消息列表、会话、未读数、已读标记和资源下载。

SDK 消息对象 ChatSDKMessageData（256 字节）是 UE 进程内布局，不是已确认的 wire schema：ClientMsgId 在 0x00，MsgId 在 0x10，MsgSeq 在 0x18，MsgTime 在 0x28，From 在 0x38，AtUserList 在 0x50，MsgType 在 0x61；各消息体对象引用位于 0x68–0xA8，EventMsg 在 0xB0，Ext 在 0xC0，SendStatus 在 0xD0，LocalExt 在 0xD8，bIsAuditFailed 在 0xE8，LogInfo 在 0xF0。类型包括 Text、Emoji、Image、Location、Audio、Video、File、Event、Pass、Custom 和 RichText。

HTGame.HTChatSDKMessageData（272 字节）是游戏侧镜像：包含会话/目标字段，并在偏移 0xE8 放置 InstancedStruct Msg；HTGame.ChatMsgInfo.SDKMsgContent 又嵌入这份结构。这说明游戏聊天视图数据能承载 SDK 消息模型，但单凭布局不能推出序列化格式。

## 与游戏内 RPC 的区别

同一反射快照仍有 HTPlayerState.Server_Friend_RPC / Client_Friend_RPC，二者参数都是 FHottaRpcMessage；已有好友 RPC 记录也确认其线上 FieldNetIndex 未知。ChatSDK 侧则有独立初始化、登录、消息发送和异步回调 API，函数元数据没有 RPC ID。现有证据没有证明好友 RPC 的 BinaryData 是 ChatSDK 正文，也没有证明旧聊天 RPC 是当前文本消息的承载通道。

游戏二进制还含 Antispam_ChatSDKReq、Antispam_ChatSDKRes、AHottaGameState::OnChatSDKRes 和 ChatSDKHttpParams 字符串/类型线索。这可能表示游戏服务端参与消息预审或反垃圾流程；关联函数、载荷和它与 SDK 发送的先后顺序尚未确认，因此不能把“独立 IM 传输”扩大成“聊天全流程完全不经过游戏服务端”。

## 尚未确认

- EnableChatSDK 是运行时开关；静态导出和反射证明接入存在，但本轮没有执行客户端来验证实际启用状态或 UI 入口覆盖率。
- WMCommunityImpl 中出现 /im/user/getToken/v2、/im/user/getToken、new/im/token、群管理路径以及 channelToken/channelAppId/channelAppKey 等字符串。它们是端点候选，不是已观察到的请求；实际域名、端口、所选版本、签名/加密、报文 framing 和 JSON/二进制编码都未确认。
- 启动器配置有 channelAppId 键；本轮没有发现对应的 ChatSDK/region/AppKey 配置键，也没有把 channelAppId 与 ChatCommunitySDK.Init(AppID, AppKey) 的实参来源关联起来。配置值和凭据值不纳入报告。
- 未取得与同一客户端版本对应的 ChatSDK 收发样本，因此不能给出消息编号、字段字节序、长度规则或服务端响应格式。
- SDK 本地缓存/数据库的实际路径、生命周期和离线重试规则仍未知。

下一步最有价值的证据是同版本运行时对 Init/Login/SendTextMessage 的实参与回调记录，以及一次发送文本时对齐的游戏侧反垃圾请求和 ChatSDK 侧网络记录。这样才能确认开关、鉴权来源、传输端点与实际消息格式。
