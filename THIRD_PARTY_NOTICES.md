# 第三方依赖与参考

项目源码采用 GPL-3.0；以下第三方内容保留各自许可。此列表不授予游戏资产、客户端二进制或第三方服务的使用权。

| 项目 | 用途 | 分发方式 / 许可 |
| --- | --- | --- |
| [CUE4Parse](https://github.com/FabianFG/CUE4Parse) | Pak / IoStore 格式阅读参考；来源注释见 `tools/ebon/containers.py` | Apache-2.0；不捆绑其代码或二进制。 |
| [ooz](https://github.com/powzix/ooz) | 本地 Oodle 流解码；源码位于 `third_party/ooz`，版本与校验和见 `SOURCE.json` | GPL-3.0-or-later；保留原版权与许可头，完整许可证见 `COPYING`。 |
| [PyCryptodome](https://github.com/Legrandin/pycryptodome) | 可选 AES 解密依赖 | 独立安装；遵循其 BSD-2-Clause 与公有领域组成部分的许可。 |
| [Capstone](https://github.com/capstone-engine/capstone) | 可选 x64 指令特征核对 | 独立安装；BSD-3-Clause，组件许可见上游。 |
| [pefile](https://github.com/erocarrera/pefile) | 可选 NetworkVersion PE 布局 / 代码映射核对 | 独立安装；MIT；不执行客户端代码。 |
| [actions/checkout](https://github.com/actions/checkout)、[actions/setup-python](https://github.com/actions/setup-python) | GitHub Actions CI | MIT；按固定提交引用，不捆绑源码。 |

`EbonOodleDecode.exe` 由捆绑的 ooz 源码构建，在独立进程中解码。使用者也可通过 `--oodle-dll` 提供本地兼容 DLL；该 DLL 不随源码仓库分发。Windows / Visual Studio / Unreal Engine 以及目标客户端遵循各自条款。
