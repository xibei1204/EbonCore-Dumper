# F5 游戏内 SDK 采集与正文发布修复

用户日志的模块 SHA-256 校验通过，F5 请求也已消费。采集期间对象数从 388,682 增至 430,520，前两轮出现蓝图父类缺失、非法 UStruct 大小及属性链错误，第三轮在类型遍历阶段耗尽原有 180 秒时限。原实现缓存整个轮次的元数据页，早期 UObject 头可能与稍后读取的 FProperty 页来自不同生命周期；少量坏类型还会让整个轮次重来。

- F5 使用 600 秒的独立预算。按类型刷新缓存，复核对象数组身份、销毁/替换标志和读取前后类型头；只保留完整验证的类型，并递归剔除其无效基类和所属类型的依赖。默认剔除率上限 5%，全局校验及上限失败仍保留失败状态。
- 启动 CLI 读取 `capture_timeout_seconds`，避免继续使用原来的五分钟上限提前报告失败。
- `dump_state.json` 和 `sdk_manifest.json` 中的 `excluded_types` 明确列出遗漏，成功代表已验证的类型集合，不代表整个游戏全部类型。
- 初次失败保留 `dump_state.failed_N.json` 并重新等待 F5；无需重新注入即可重试。
- `EbonLogConsole.exe` 启动隐藏的 `tools/watch_sdk.py`。它在每个成功的 F5 快照后生成 C++ SDK，默认正文目标为 `G:\EbonCore-Dumper\out\sdk\CppSDK`，不依赖 AES 专用启动脚本是否已经退出。
- `tools/ebon/sdk_publish.py` 在临时目录生成完整代码，成功后替换固定目标，并保留 `CppSDK.previous_*` 备份。生成失败不会覆盖现有 SDK。
- 反射快照、SDK 正文及生成状态分开保留：`sdk_hotkey_state.json` 描述采集，`sdk_body_state.json` 描述正文生成。

验证覆盖：在缓存建立后模拟对象销毁、无效父类和超出类型大小的属性；验证其依赖被剔除且剔除率上限仍生效；真实 DLL 的 F5 等待、重复、失败重试；真实日志终端启动独立 Python 生成器并两次发布到同一正文目录。

游戏内验收仍应使用重新启动采集后的新版 DLL：按 F5 后观察 `F5 C++ SDK body ready`，检查固定目录的 `sdk_manifest.json` 中 `source_capture` 指向本次成功快照，并查看 `excluded_types`。构建和合成宿主验证不能代替新版本的游戏内复测。
