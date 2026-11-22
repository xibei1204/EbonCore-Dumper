# NTE Pak 索引与资源提取修复

同一份实机 AES Key 已通过安装目录全部 **14 个 Pak** 的完整主索引与目录索引 SHA-1 校验。已完整枚举 **115,826 个有效条目、222 个删除条目**，从每个 Pak 实际提取资源，累计 **25 个样本**覆盖 Oodle、未压缩、加密与未加密数据。

## 格式与校验

| 差异 | 实现与证据 |
| --- | --- |
| NTE v12 footer 保存实际 `IndexOffset + 1` | 客户端 RVA `0x27f475e` 增加 1，`0x27f47b8` 减少 1。先尝试标准位置，再尝试减 1；只有完整 SHA-1 匹配才选择变体。 |
| 完整条目的压缩块位置做异或编码 | 客户端 RVA `0x27c5440` / `0x27c5447` 使用 `0x76D6DA`。识别 NTE 变体后，在添加条目基址前解码。压缩后的索引条目通过长度重建位置。 |
| 加密压缩条目的 Size 可包含 AES 填充 | 校验逻辑压缩长度或逐块对齐后的物理长度，同时检查实际填充不越过主索引。 |
| 补丁目录包含删除标记 | `INT_MIN` 位置记录为删除路径；它计入声明数量，提取清单只返回有效资源。 |

先前多个索引首块出现十五个相同字节，是从真实 AES 块偏移一字节读取造成的。修复恢复了正确的块边界，AES 算法与 Key 无需更改。

原生扫描器在同一容器样本内部保留两种候选位置，保持标准 Pak 的优先次序与样本数量。状态保存 `serialized_index_offset`、实际 `index_offset` 与 `index_offset_adjustment`；NTE 样本成功时使用 `verification_method=pak_index_sha1`。Python 读取器同时验证主索引和完整目录索引，输出 `format=nte_v12`、`index_verified`、`directory_complete` 与删除数量。

## 本地解压

`build_native.bat` 构建 `EbonOodleDecode.exe`，使用 `third_party/ooz` 中的 GPL-3.0-or-later 源码，固定上游提交 `05038060aa68f9187ae9923b2388ca8db40e58d1`。源码、版权、完整许可证与原始文件校验和一并保存。原始程序 main 在编译时改名，实际入口只执行有长度限制的解码。

Python 自动使用该本地程序，解码器每次通过独立子进程运行；输入和输出最多 512 MiB，解码缓冲保留上游要求的 64 字节空间，进程超时 30 秒。长度不匹配、解码失败或进程退出均成为可见错误。显式 `--oodle-dll` 继续使用已有 DLL 适配器。

```powershell
.\build_native.bat
$env:EBON_AES_KEY = (Get-Content '<本次已验证的 aes_key.txt>' -Raw).Trim()
python tools/ebon_cli.py extract-pak `
  --container 'C:\Program Files\Neverness To Everness\Client\WindowsNoEditor\HT\Content\Paks\pakchunk5-Windows.pak' `
  --include '*.stinfo' --out 'out/resources/pak5_verified'
```

## 验证证据

- `out/pak-verified-20261122/verification.json`：十四个 Pak 的索引摘要、完整目录计数、实际提取文件的路径、长度与 SHA-256，以及解码器来源。
- `out/pak-native-aes-20261122/aes_state.json`：原生扫描器从当前模块快照恢复 Key，完整 Pak 索引 SHA-1 验证成功，实际位置 24197、序列化位置 24198、调整值 -1。
- 回归覆盖标准/NTE v12、完整索引哈希、错误 Key、损坏哈希、异或块位置、两种压缩大小约定、填充越界、删除记录和本地 Oodle 进程错误。

另一个 Runtime/SDK 会话结束后完成联合验收：13 个原生目标构建成功，185 项测试全部通过（60.976 秒）。`build`、`build-aes` 中十三个程序与测试产物逐文件 SHA-256 一致。使用默认命令行从 pakchunk5 提取两个 `.stinfo` 文件，共 421,056 字节，状态为 `complete`、错误与未解决项均为空；见 `out/pak-cli-verified-20261122/pak_extraction_state.json`。

提取结果是容器中的原始文件内容。`verification.json` 的输出 SHA-256 用于保存与复核该内容，不代表已经反序列化所有 Unreal 类型，也不代表 IoStore cooked package 已重建。客户端资产、Key 与本地验证输出继续保留在被 Git 排除的 `out/` 中。
