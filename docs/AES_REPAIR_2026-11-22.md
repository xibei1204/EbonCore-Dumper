# AES Key Dump 修复（2026-11-22）

已实机导出真实 Key：PID 28608，8266 毫秒，`verified`。结果见 `out/aes-live-verified-20261122-28608/aes_key.txt`；已独立确认十二个加密 IoStore 目录的 647,010 个文件条目。下面保留此前失败记录及最终修复依据。

随后已补齐 Pak：恢复 NTE v12 的主索引位置编码，同一份 Key 已通过全部十四个 Pak 的完整索引 SHA-1 校验，完整枚举与实际文件提取也已验证。详见 [Pak 完整修复](PAK_REPAIR_2026-11-22.md)。文中此前 Pak 未通过的记录属于修复前状态。

## 发现的问题

- `run_injector.bat` 固定使用 `build-aes`，该目录的 DLL 和日志终端仍是 2026-11-14 的版本，早于源码中的 F9 改动。用该目录运行原有 AES 回归时，热键用例因缺失 `aes_hotkey_state.json` 失败。
- `out/reflection/gameplay_20261121_001153/aes/aes_state.json` 为 `timed_out`，`phase=text`、`sweeps=1`、`bytes_scanned=3002482804`。旧算法先扫描整个地址空间的文本候选，然后才搜索二进制候选；这次扫描期限结束前没有进入二进制阶段。
- 重复 F9 会生成 `aes_2`、`aes_3`，但 `wait-aes` 固定读取 `aes/aes_state.json`，会把首次扫描的旧终态当成后续扫描结果。
- 终端输入设置保留了虚拟终端输入和 Quick Edit，可能让功能键转为转义序列或让选择操作暂停终端。

## 修复

- `tools/ensure_native_build.ps1` 检查采集与 AES 工具的修改时间；`run_injector.bat` 在注入前自动重建缺失或早于原生源码的 `build-aes` 产物。
- 进程扫描每轮枚举所有可读区，优先模块可写区、私有可写区、其他私有区、模块只读区，最后检查映射区。同类区按大小排序，保留大区、相邻区重叠及失败页回退。
- 先对每个内存块一起检查 ASCII/UTF-16 十六进制和对齐 raw，覆盖全部区域后再做非对齐 raw 搜索。`aes_unaligned=0` 仍关闭非对齐 raw 搜索。此顺序由下述实际超时后的覆盖修复调整，初版的逐块三种编码搜索没有保证后续区域能在期限内被检查。
- `wait-aes` 每次轮询跟随本次采集监听器的 `last_output`，对尚未写出的最新状态继续等待，并继续校验 `verified` 状态与密钥文件一致。
- AES 热键监听器状态改为原子发布；终端关闭虚拟终端输入和 Quick Edit，保留原有 F9 请求路径。
- 只有候选解密后的完整 Pak 索引 SHA-1 匹配时才写 `aes_key.txt`；验收规则未放宽。

## 使用

运行 `run_injector.bat`，按提示启动客户端，在本次 Dumper 终端按 F9。成功时最新的 `aes` 或 `aes_N` 内同时出现 `aes_state.json` 的 `verified` 终态和 `aes_key.txt`。

当前游戏进程未运行；真实客户端密钥是否成功抓取，需要以新采集的 `verified` 结果验收。

## 验证

- MSVC x64 构建 `build-aes-fix` 的 12 个原生目标通过。已将其中 12 个 EXE/DLL 复制到 `build` 与 `build-aes`，逐文件 SHA-256 一致。
- `EBON_TEST_BUILD_DIR=build-aes-fix` 下全量回归 146 项通过，47.095 秒；输出为 `.work/aes-repair-tests.log`。
- 新增实际 DLL 验证用例：先确认热键监听器存在且扫描目录尚未创建，再把随机 32 字节 Key 放入非对齐私有内存，写请求触发扫描；结果为 `verified`，`encoding=raw`、`source=in_process_memory`，状态中的 Key 与 `aes_key.txt` 一致。
- 调度用例验证较小私有内存在大映射区之前被搜索，并验证首块内的非对齐 raw Key 在读取耗时的后续块之前被验证。
- 重复扫描回归验证 `aes_2` 的运行状态覆盖首次 `aes` 的旧终态，以及最新状态尚未发布时继续等待。
- `.work/verify_aes_console.py build-aes-fix` 在独立隐藏控制台中预置虚拟终端输入与 Quick Edit，再启动实际 `EbonLogConsole`；输入模式从 1015 改为 432，F9 输入记录生成 `aes_hunt.request`，退出后原输入模式恢复。该测试覆盖控制台输入队列，不覆盖物理键盘和窗口焦点。
- `ensure_native_build.ps1` 在产物更新后直接通过，不再启动重复构建。

## 实际超时后的继续修复

收到实际采集 `out/reflection/injected_20261122_152036_18372/aes/aes_state.json`：`pid=27144`、`status=timed_out`、`success=false`、`elapsed_ms=120062`、`phase=raw_unaligned`、`sweeps=1`、`bytes_scanned=881609052`、`candidates=778204402`、`read_failures=108516`。热键状态为 `presses=1 runs=1`，日志记录 15:21:28 启动、15:23:28 停止，证明 F9 链路已经触发，真实密钥仍未抓取成功。

- 每块做完整非对齐穷举会消耗约七倍于对齐候选的额外验证工作，把后续区域拖到超时。现改为先覆盖全域文本/对齐候选，随后再检查非对齐候选；增加后续区域对齐 Key 的覆盖回归。
- 原进程内读取直接复制枚举时的地址。现复用 `InProcessReader.copy`，在每次读取前查询当前页面状态，再保留异常处理以应对查询后的竞争；不读取 `PAGE_GUARD` 页面。新增测试在枚举后的区域中设置保护页，确认保护标志保留、后续页 Key 仍可验证。
- 状态新增 `completed_sweeps`、`bytes_unread`、`regions`、`region_index`、`range_address`、`range_size`、`range_type`、`range_protection`。旧状态没有失败地址，不能把 108,516 次失败归因到某个区域，也不能据此判断 Key 是否已加载。
- 本地旧解包镜像的连续 raw/文本搜索结束于 `exhausted`，13.5 秒；常见立即数写入窗口复核也未找到匹配。两项只描述旧镜像上已检查的形式，不证明当前运行时不存在 Key。

本次真实采集未成功。初版的 146 项回归证明测试链路通过，不能替代真实客户端 `verified` 结果。继续修复版仍需在新客户端进程中验收。

继续修复版构建目录为 `build-aes-coverage-fix`，12 个原生目标通过；147 项回归通过，43.560 秒，输出 `.work/aes-coverage-tests.log`。已将 12 个 EXE/DLL 更新到 `build`、`build-aes`，逐文件 SHA-256 一致。已运行中的旧 DLL 不会随磁盘文件更新，需要重新启动客户端采集以加载新版。

## 第二次实机结果与验证规则修复

`out/reflection/injected_20261122_153811_7580/aes/aes_state.json` 仍为 `timed_out`：120047 毫秒、`sweeps=2`、`completed_sweeps=1`、2606088101 个读取字节、12679 次失败，证明至少一次遍历完成，仍没有已验证 Key。随后同采集的 `aes_2` 也在 120078 毫秒后超时，`completed_sweeps=0`，停止于私有可写区 `0x1d8b68000`。两次都仅验证 `pakchunk5`。

检查当前安装的六个加密 Pak footer，GUID 均为零，索引长度分别为 179376、992、5648、363920、256、160 字节。原采样按零 GUID 合并为最小索引，实际只验证 `pakchunk5`；未指定 GUID 不能证明所有容器使用同一 Key。

使用上一版真实工具复现了两项错误拒绝：

- 同为零 GUID 的两个 Pak 使用不同 Key，扫描输入中的 Key 能验证较大 Pak，旧算法只保留较小 Pak，错误返回 `exhausted`。
- Pak 对原始索引计算 SHA-1，AES 对齐尾部复制索引开头的非零字节，旧验证只接受零填充，正确 Key 也返回 `exhausted`。

修复后为每个零 GUID 容器保留独立样本，显式非零 GUID 仍选择较小样本；解密索引在完整长度及最多 15 字节对齐填充前尝试 SHA-1，不按填充值提前拒绝。大型样本在完整哈希前先检查 FString 范围、NUL 终止符和挂载点开头的控制字符，减少错误候选的哈希成本，UTF-8/UTF-16 长路径有独立回归。Python Pak 读取器同步支持非零 AES 填充，原始未加密索引的规则保持不变。

这两个错误拒绝已由独立合成样本复现，不证明真实客户端恰好使用不同容器 Key 或非零填充。新算法对旧解包镜像的六样本扫描仍为 `exhausted`，23 秒；实际 Key 尚未取得。

验证修复版输出 `build-aes-validation-fix`：12 个原生目标成功构建，155 项全量回归通过，43.027 秒，日志 `.work/aes-validation-tests-final.log`。首次全量运行曾有一项好友重置确认时序测试超时；该用例独立复测通过，全量重跑也全部通过，相关实现未在本次 AES 修复中修改。已将测试过的 12 个 EXE/DLL 更新至 `build` 和 `build-aes`，逐文件 SHA-256 一致。实际客户端仍以新版采集的 `verified` 状态验收。

## 第三次实机结果与模块证据

`out/reflection/injected_20261122_161933_10900/aes/aes_state.json` 已列出六个 Pak，但仍为 `timed_out`：120250 毫秒、4289197800 个读取字节、342571925 个候选、`sweeps=1`、`completed_sweeps=0`、`phase=raw_aligned`，停止于 `0x1e2b06000` 的私有可写区。原排序把所有私有区放在模块只读区之前；这次世界中的堆未扫完，模块只读区尚未参与这一轮。

- 模块只读数据区提前至私有可写区之前，执行代码区保留在后面，避免数 GiB 堆阻塞常量区域。已有模块可写区仍最先检查。
- 增加失败证据：`aes_snapshot_module=1`（进程内默认开启，标准 AES profiles 显式开启）在超时后采集主模块当前可读字节；每次使用 1 MiB 缓冲，失败读降为 4 KiB 页，读取预算 15 秒，模块最多 512 MiB。
- `module_memory.bin` 使用内存 RVA 布局；`module_memory.json` 的 `captured_ranges`、`copied_bytes`、`unread_bytes` 区分真实读到的字节和零占位。它是内存镜像证据，不能称为完整解包 PE。
- 快照结束前状态为 `collecting_evidence`，完成后才发布 `timed_out`。`module_snapshot` 指向元数据；失败写入 `evidence_error`。快照不产生 `aes_key.txt`，也不将超时改为成功。

这轮仍未取得真实 Key。模块快照用于下一步基于当前代码和覆盖范围定位 Key 生成/解密路径，避免持续依靠旧镜像和超时计数推测。

此版最初输出 `build-aes-image-fix`，12 个原生目标构建成功。AES 的 32 项测试和容器读取的 7 项测试通过（`.work/aes-image-focused-tests.log`、`.work/aes-image-container-tests.log`），覆盖只读模块 Key 先于大堆扫描、快照读取空洞以及实际 DLL 超时快照输出。首次全量 162 项中有 2 项 SDK 测试失败：`test_failed_f5_capture_can_retry_without_reinjecting` 缺失 `dump_state.failed_1.json`，`test_unloading_blueprint_flags_are_not_exported_as_rpc_ids` 报 `types.jsonl[4] reports incomplete type metadata`；记录为 `.work/aes-image-tests.log`。

期间工作区的 SDK/原生代码又有更新，启动检查重建了 `build-aes`。对最终安装产物重跑全量 **163 项通过，53.631 秒**（`.work/aes-image-installed-tests.log`），再将该目录的 12 个 EXE/DLL 同步到 `build`，逐文件 SHA-256 一致。上述首次失败仍保留在日志中；最终已安装产物的验证结果为全量通过。实际游戏 Key 尚未取得。

## 指令密钥与 IoStore：真实客户端成功

本次继续分析 `injected_20261122_165347_20368/aes/module_memory.bin`，并读取当前 HTGame 进程 PID 28608。AES 分派槽仍指向 RVA `0x15da2e0`；当前函数字节与快照一致，SHA-1 为 `eef79d8cc429e8c8cfcffb36599bb7f847ec384f`。

真实密钥委托位于 RVA `0x10290f40`，执行桩 RVA `0x14acdf0`，绑定回调 RVA `0x8b0ca10`。该回调从 RVA `0x8b0ca2b` 起，用八条 32 位栈写入指令构造 Key，再复制给调用者；它不会在模块中保存连续的 32 字节密钥。运行时零 GUID 密钥注册表中的值与回调重建值相同。

旧验证器只读取 `.pak`。这份真实 Key 无法解开已安装 Pak 的索引，但可以解开 `Paks` 与 `PatchPaks` 中十二个加密 IoStore 目录。因此调整 Key 搜索与验证：

- 模块代码首先进入 `instruction_keys`，重建 256 字节窗口内 RBP/RSP 栈槽的八段 DWORD，结果编码为 `immediate32`；跨 1 MiB 读取边界保留 255 字节重叠。候选仍须通过容器验证。
- 样本目录同时发现 `.pak` 与 `.utoc`。IoStore v3-v8 按头部大小、种子/溢出表、压缩块表、方法表和签名表定位加密目录；显式 GUID 分别保留 Pak/IoStore 样本。
- `verification_method=pak_index_sha1` 保留完整索引摘要校验。IoStore 没有目录摘要，使用 `io_store_directory_structure` 验证整个目录、文件和字符串图：边界、终止符、连通性、循环、名字、块引用及不超过 15 字节的尾部对齐。该方法不声称签名或资源块哈希已验证。

真实结果：

- 离线快照：`out/aes-recovered-20261122/aes_state.json`，`verified`，625 毫秒，编码 `immediate32`。
- 当前游戏进程直接只读扫描：`out/aes-live-verified-20261122-28608/aes_state.json`，`verified`，8266 毫秒，零读取失败，Key 来源地址 `0x148b0ca2b`。密钥位于同目录 `aes_key.txt`。
- 独立 Python 容器读取器复核所有十二个加密 IoStore 目录；详细计数记录在该结果目录的 `container_validation.json`。

这份 Key 尚未通过现有 Pak 索引的标准 AES-ECB/SHA-1 验证，不匹配原因仍未确认。已验证的实际用途是 IoStore，不能把目录成功解释成所有 Pak 均已解密。

验证与安装：新增五项 AES 回归覆盖跨块且逆序的 RBP/RSP 指令常量、签名/无签名 IoStore、错误 Key、目录循环、无效块引用、异常尾部、混合 Pak/IoStore 样本及 DLL F9 请求链路。真实游戏验收使用外部只读扫描；DLL F9 请求链路在合成宿主中验收。

全量 **176 项通过，75.139 秒**（`.work/aes-iostore-installed-tests.log`）。期间默认 `build` 又有 SDK 工作更新，保留该目录的新产物，对其 AES **37 项通过，17.199 秒**（`.work/aes-default-tests.log`），并在 625 毫秒内重建出与实机相同的 Key（`out/aes-default-verified-20261122`）。将默认目录的八个 AES/采集相关程序同步到 `build-aes`，逐文件 SHA-256 相同；记录在 `.work/aes-installed-programs.json`。启动构建检查退出 0，无需再重建。未将候选目录较旧的 SDK 测试程序覆盖到默认目录。
