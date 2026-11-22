# 好友专用采集修复与重采步骤（2026-11-21）

本次目标是补齐 MintServer 原生 GM 好友所缺的子消息和资料编码证据。修改位于 G:/EbonCore-Dumper；本轮不启动、注入或操作游戏，由用户重采。

## 已确认的问题

- 旧 run_friend_capture.bat 只在 DLL/注入器不存在时构建。源码更新后仍可能加载旧 DLL；不能用最新源码的测试结果代表旧采集进程。
- 通用配置把好友与 GameData、地图和批量同步放在同一窗口和配额中。无关调用可能用完预算，专用好友采集应隔离这些载荷。
- 旧样本只保留自定义 FName 的进程内 index/number，没有类型文本，离开该进程后不便确定 Array/Custom 参数类型。
- 既有 injected_20261121_182348_22176 窗口的目标调用和样本均为 0，只出现 ReceiveTick。这证明该窗口未观察到好友目标，不能把它解释成“字段解析失败”或承诺重采必有数据。前三项是实际代码缺陷/限制，并非该空窗口成因的已验证结论。

## 本次修改

- 启动脚本每次先构建 build-friend-capture，使用新 nte-ue5-friend.ini。该配置只保存两方向 Friend RPC 参数，关闭通用同步载荷、AES 和 F10 资源采集；自动窗口为 600 秒，仍受 128 条、单条 2 MiB、队列 16 MiB 限制。
- 两个好友 schema 必须均通过才启用专用模式。保留既有版本、地址与布局检查及原函数转发；没有引入新的 native 地址或内联代码补丁。
- 状态含 sampler_revision=friend-capture-v2、capture_scope=friends、心跳、窗口编号及两方向独立的调用、样本、解析拒绝计数。friend_diagnosis 区分无调用、只有请求、参数拒绝及保存成功；参数保存成功仍不等于包级映射或好友功能完成。
- Array/ObjectArray/Custom 参数增加 custom_type_name，从已验证 NamePool 解析，同时保留原始 index/number。无法解析时为 null，载荷裁剪仍明确标注。
- 增加 rpc_sample_capture.request 文件触发和 arm_friend_capture.bat。Python 工具等待新窗口编号确认，不把“写出请求文件”当作已开启；拒绝旧采样器和过期心跳。

## 用户重采步骤

1. 退出已加载旧 Dumper DLL 的客户端。在仓库根目录运行 run_friend_capture.bat，待构建通过并开始等待后，再从启动器打开客户端。
2. 在日志中确认 [Friend] revision=friend-capture-v2，状态文件应有两个 friend schema/native 标志和 capture_scope=friends。窗口自动开启；尽量从登录前开始，覆盖首次好友同步。
3. 完成登录，打开/刷新好友列表，并查看一个已有好友的资料。优先在确实能返回原生好友数据的环境中采集；仅连接尚未实现好友下发的 MintServer、反复查看空页面，无法凭空得到缺失的服务端响应。
4. 如需新窗口，按 F8；不方便聚焦或按键未生效时，另开普通终端运行 arm_friend_capture.bat。它自动选择最近的新版好友采集目录，也可传入目录路径：

~~~powershell
.\arm_friend_capture.bat "G:\EbonCore-Dumper\out\reflection\injected_<时间>_<PID>"
~~~

5. 操作后检查实际样本：

~~~powershell
python tools/friend_capture.py inspect
# 或显式指定本次目录：
python tools/friend_capture.py inspect "G:\EbonCore-Dumper\out\reflection\injected_<时间>_<PID>"
~~~

将本次输出目录路径交给后续分析。至少保留 game_rpc_samples.jsonl、game_rpc_sample_state.json、types.jsonl 和 objects.jsonl。首先需要实际 Client_Friend_RPC 响应，最好同时包含请求和非空好友资料。missing 不为空或 truncated_samples 非零时，需要按具体缺口再补采。

## 证据边界

采样器临时替换进程内已验证的 ProcessEvent/UFunction 槽以观察参数，不修改游戏安装文件，也不伪造响应。输出是参数对象与其 BinaryData，不是原始 UDP/IM 报文，wire_packet_bytes_captured 与 packet_mapping_verified 保持 false。ChatSDK 文本消息仍是另一条未确认的线路；本次不宣称已捕获 ChatSDK 网络流量。

## 验证

- build_native.bat build-friend-capture：11 个原生目标全部构建通过。最终 DLL SHA-256 为 ca8b88dd772d4fbf394813c482a89560a4c468fa5668a4b6502fa5259559040d。
- 设置 EBON_TEST_BUILD_DIR 指向该构建，运行 python -m unittest discover -s tests -p test_*.py -v：137 项通过（38.447 秒）。
- 单独运行 EbonFriendRpcSelfTest.exe：两方向、两入口去重和原调用保留、无关 RPC 配额隔离、文件重新开启、空采样诊断、自定义类型名、截断、错误输入及窗口过期后的心跳均通过。
- 对旧空目录运行 friend_capture.py inspect，明确报告两方向未观察到，未将 installed 或 ReceiveTick 计数当作好友样本。
- Python 工具及测试编译检查、相关跟踪文件的 git diff --check 通过。日志和合成输出位于 .work/friend-capture-fix-20261121_204527/；原有相关文件备份也在此目录。

真实客户端结果等待用户重采。本轮没有启动/注入客户端、没有采集实际账号数据，没有提交或推送；没有修改 MintServer 的业务数据或运行配置。
