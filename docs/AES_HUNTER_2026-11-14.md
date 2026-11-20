# AES 抓取改进与验证（2026-11-14）

本轮在原项目内完善 AES 抓取代码。新版产物位于 `build-aes/`，`run_injector.bat` 明确使用这套程序；默认 `build/EbonLogConsole.exe` 被现有会话占用，默认目录重建返回 LNK1104，因此没有以关闭现有日志窗口的方式替换旧产物。

## 实现

- DLL、外部只读扫描器和离线文件扫描器共用 `native/dumper/ebon_aes_scan.hpp`，移除了旧扫描器的重复实现。
- 从标准 Pak v8/v10/v11/v12 footer 读取加密标记、GUID、索引位置、长度和 SHA-1，按 GUID 选较小的验证样本；不再写死 pakchunk5、24198、160 字节及某个 SHA-1。
- 搜索原始 32 字节、ASCII 十六进制、UTF-16 十六进制；分块保留最长候选所需的边界重叠，覆盖非对齐位置和相邻可读区域，不再跳过大于 128 MiB 的区域。读取失败降到页粒度。
- 仅完整索引 SHA-1 匹配才产生密钥文件；支持索引零填充前后哈希。第一次验证成功即结束，结果只证明记录的样本/GUID，不证明其他容器密钥相同。
- DLL 在模块和输出路径校验后启动独立线程，并为线程持有 DLL 引用。反射完成后 AES 可继续重扫，默认 120 秒超时、轮次间隔 2 秒。（启动时机已于 2026-11-20 改为终端内 F9 触发，见 [AES_HOTKEY_2026-11-20.md](AES_HOTKEY_2026-11-20.md)；扫描本身的行为未变。）
- 本次采集的 `aes/aes_state.json` 原子记录状态、扫描阶段、目标 PID、字节/候选数量、页读取失败、样本及错误。初始化失败也有终态；Windows 短暂文件共享冲突会重试提交。
- 仅已验证结果写入同目录 `aes_key.txt`。日志不打印密钥，非空结果目录被拒绝；启动脚本不强制关闭游戏，也不根据旧的全局密钥文件判断本次成功。
- 新增 `wait-aes`、`inject-and-dump --wait-aes`；命令行等待会检查终态和密钥文件的一致性。原有大型镜像文件得到保留，新启动流程不再无条件覆盖该文件。

## 验证

`build_native.bat build-aes` 成功完成，包含 DLL、反射宿主、注入器及两个 AES 命令行工具和 AES 自测程序。

```powershell
$env:EBON_TEST_BUILD_DIR = (Resolve-Path build-aes).Path
python -m unittest discover -s tests -p 'test_*.py' -v
```

最终 **55 项测试通过**（21.523 秒）。完整输出为 `.work/aes-final-tests.log`。

新增测试覆盖 NIST AES-256 与 SHA-1 向量、动态索引长度、支持的 Pak 版本、填充哈希、跨块/非对齐/末尾候选、文本密钥、错误密钥和损坏哈希、footer 边界、GUID 样本选择、旧输出保护、超过 128 MiB 区域、失败页后的继续扫描、Windows 临时读取锁、延迟出现密钥后的真实外部进程重扫，以及实际 DLL 的独立后台超时和初始化失败状态。

测试中发现原子状态提交会遇到读取方未释放的 Windows 文件共享锁，已增加提交重试，并让测试宿主在等待前关闭读取句柄。

## 真实客户端状态

本次读取安装目录下实际 Pak footer，成功识别 6 个加密 Pak；最小索引仍是 pakchunk5 的 160 字节索引。实际外部进程扫描在 OpenProcess 返回 Win32=5 后明确失败，证据为 `out/aes-live-20261114-201038/aes_state.json`。最终复查时 HTGame 已不在运行，因此没有对新版 DLL 做真实游戏注入验收。

**本轮未取得或验证真实游戏 AES 密钥。** 实际 DLL 合成宿主测试与真实客户端成功抓取是两项独立结论。下一次客户端启动时可运行 `run_injector.bat`，以该次 `aes/aes_state.json` 的 `verified` 终态为准。
