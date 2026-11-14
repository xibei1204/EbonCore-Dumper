# 贡献指南

## 开发与验证

在 Windows x64 上安装 Visual Studio C++ 工具链和 Python 3.11+，然后在仓库根目录执行：

```powershell
cmd /c build_native.bat
python -m pip install -r requirements.txt
$env:EBON_TEST_BUILD_DIR = (Resolve-Path build).Path
python -m unittest discover -s tests -p 'test_*.py' -v
build\EbonReflectionSelfTest.exe
build\EbonAesSelfTest.exe
build\EbonResourceSelfTest.exe
```

如需保留原构建，运行 `cmd /c "build_native.bat build-dev"`，并把 `EBON_TEST_BUILD_DIR` 改为 `build-dev` 的绝对路径。原生集成测试使用独立合成宿主，不启动游戏。

可选依赖不是主流程的硬要求，但完整 AES 测试需要 `pycryptodome`；CI 安装 `requirements.txt` 以避免这一部分因缺依赖跳过。实际客户端容器测试在未安装样本客户端时会明确跳过。

## 提交要求

- 聚焦一个问题；保持周围代码的命名、格式与布局验证规则。
- 修改资源 / 网络布局时保留版本绑定、预算、错误状态及未解析标记，并添加合成回归测试。
- 不将 CDO 默认值、未加载对象或字符串候选宣称为真实活动配置、完整资源或完整译文。
- 记录实际执行的构建、测试及未验证项，不把本地合成测试等同于真实客户端验收。
- 提交消息使用 Conventional Commits，例如 `fix: preserve separate resource bodies`。

## 不应提交的内容

不要提交实际密钥、账号凭据、访问令牌、个人信息、客户端二进制/资产、内存 dump、采集快照、生成的 SDK、第三方解压 DLL 或本地日志。测试数据应为合成样本；忽略规则并不能代替暂存内容检查。

历史一次性探针和个人指令文件保留本地，不属于公开源码。文档引用的 `out/` 证据路径仅用于说明验证过程，原始产物不随仓库提供。

贡献代码按项目 [GPL-3.0](LICENSE) 许可提交；引入第三方代码时必须保留原版权/许可并更新 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
