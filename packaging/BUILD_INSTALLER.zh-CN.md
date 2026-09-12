# AVC Windows 安装包构建指南

本文说明如何在 Windows x64 上构建 AVC Release 版本，并使用 Inno Setup 7 生成可分发的安装程序。

当前安装包包含：

- `avc.exe` 主程序及内嵌 Web UI；
- `avc-qwen-livetranslate.dll` 实时翻译扩展；
- 官方签名的 `usbip-win2` 0.9.7.7 驱动安装程序；
- Microsoft Visual C++ x64 Redistributable；
- Microsoft WebView2 Evergreen Bootstrapper；
- 第三方许可证和中文、英文安装向导。

RVC 不放入主安装包，而是作为独立的可选扩展发布。完整构建流程见
[AVC RVC 扩展安装包构建指南](BUILD_RVC_EXTENSION.zh-CN.md)。

USB/IP 驱动不会在安装 AVC 时自动执行。用户应在 AVC 的“音频设备 → USB/IP 驱动”界面中安装，以便在 USB Hub 被短暂重置前看到警告并保存工作。

## 1. 环境要求

准备以下工具：

- Windows 10/11 x64；
- Visual Studio，安装“使用 C++ 的桌面开发”、CMake 和 Windows SDK；
- Node.js 与 pnpm；
- vcpkg，并按[项目构建说明](../README.md)设置 `VCPKG_ROOT`；
- [Inno Setup 7](https://jrsoftware.org/isinfo.php)；
- 首次配置和下载依赖时可访问互联网。

确认工具可用：

```powershell
cmake --version
node --version
pnpm --version
& "C:\Program Files\Inno Setup 7\ISCC.exe" --version
```

如果工具没有加入 `PATH`，可直接使用其绝对路径。Visual Studio 自带的 CMake 通常位于：

```text
C:\Program Files\Microsoft Visual Studio\<版本>\<版本类型>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

## 2. 构建 Web UI

从仓库根目录执行：

```powershell
Set-Location ui
pnpm install --frozen-lockfile
pnpm run build
Set-Location ..
```

构建结果位于 `ui\dist`。CMake 会把这里的 HTML、CSS 和 JavaScript 嵌入 `avc.exe`，因此每次修改 UI 后都应先执行本步骤。

仓库中的 `ui\pnpm-workspace.yaml` 应明确允许 Vite 所需的 `esbuild` 安装脚本：

```yaml
allowBuilds:
  esbuild: true
```

## 3. 配置并构建 Release

建议为安装包使用独立构建目录，避免混入 Debug 或测试产物：

```powershell
cmake --preset windows-package `
  -DAVC_BUILD_TESTS=OFF `
  -DAVC_BUILD_QWEN_LIVETRANSLATE_EXT=ON `
  -DAVC_BUNDLE_USBIP_DRIVER_INSTALLER=ON

cmake --build build/windows-package `
  --config Release `
  --target avc avc_qwen_livetranslate `
  --parallel
```

`AVC_BUNDLE_USBIP_DRIVER_INSTALLER=ON` 会下载官方 `USBip-0.9.7.7-x64.exe`，在配置阶段验证固定 SHA-256，并在安装阶段将其放入 `bin\drivers`。

此预设使用 vcpkg manifest 和 `x64-windows-static-md`。首次迁移时不要复用原有的 FetchContent 构建目录。

## 4. 生成安装暂存目录

安装器脚本当前使用版本 `0.1.0`，对应的暂存目录为 `dist\avc-stage-0.1.0`：

```powershell
cmake --install build/windows-package `
  --config Release `
  --prefix "C:\完整路径\avc\dist\avc-stage-0.1.0"
```

完成后至少应存在：

```text
dist/avc-stage-0.1.0/
├─ bin/
│  ├─ avc.exe
│  ├─ drivers/
│  │  └─ USBip-0.9.7.7-x64.exe
│  └─ extensions/
│     └─ avc-qwen-livetranslate.dll
└─ share/avc/licenses/
```

`drivers` 和 `extensions` 必须保持在 `avc.exe` 旁边的相对位置。最终用户安装包不需要包含 `include` 目录；Inno Setup 脚本只收集 `bin` 和许可证。

## 5. 准备 Microsoft 运行库

创建目录并下载官方安装程序：

```powershell
New-Item -ItemType Directory -Force packaging\redist

Invoke-WebRequest `
  -Uri "https://aka.ms/vc14/vc_redist.x64.exe" `
  -OutFile "packaging\redist\vc_redist.x64.exe"

Invoke-WebRequest `
  -Uri "https://go.microsoft.com/fwlink/p/?LinkId=2124703" `
  -OutFile "packaging\redist\MicrosoftEdgeWebview2Setup.exe"
```

下载来源：

- [最新版 Microsoft Visual C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist)；
- [Microsoft WebView2 Runtime 下载和部署说明](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution)。

必须检查两份文件的 Microsoft Authenticode 签名：

```powershell
Get-ChildItem packaging\redist\*.exe | ForEach-Object {
  Get-AuthenticodeSignature -LiteralPath $_.FullName |
    Select-Object Path, Status, SignerCertificate
}
```

所有文件的 `Status` 都应为 `Valid`。不要使用签名无效或来源不明的运行库。

当前方案包含的是 WebView2 在线引导程序。如果目标电脑没有 WebView2 Runtime，安装时需要联网。要制作完全离线的安装包，请在微软下载页面取得 `MicrosoftEdgeWebView2RuntimeInstallerX64.exe`，并同步修改 `packaging\avc.iss` 中 `[Files]` 和 `[Run]` 的文件名。

## 6. 更新安装包版本

发布新版本前，打开 `packaging\avc.iss` 并同步修改：

```ini
#define AppVersion "0.1.0"
#define StageDir "..\dist\avc-stage-0.1.0"
VersionInfoVersion=0.1.0.0
VersionInfoProductVersion=0.1.0.0
```

暂存目录名称必须与第 4 步的 `--prefix` 一致。

不要随版本修改 `AppId=AVC.AudioGraph.Desktop`。保持固定 `AppId` 才能让新版安装程序识别并升级旧版本。

## 7. 编译安装程序

在仓库根目录运行：

```powershell
& "C:\Program Files\Inno Setup 7\ISCC.exe" "packaging\avc.iss"
```

成功后输出：

```text
dist\installer\AVC-0.1.0-x64-setup.exe
```

也可以在 Inno Setup Compiler 中打开 `packaging\avc.iss`，然后选择 Compile。

## 8. 校验产物

计算安装包校验值：

```powershell
Get-Item dist\installer\AVC-0.1.0-x64-setup.exe |
  Select-Object FullName, Length, VersionInfo

Get-FileHash `
  -Algorithm SHA256 `
  dist\installer\AVC-0.1.0-x64-setup.exe

Get-AuthenticodeSignature `
  dist\installer\AVC-0.1.0-x64-setup.exe
```

正式发布前，至少在一台干净的 Windows 10/11 x64 虚拟机中验证：

1. 安装、启动、卸载均成功；
2. 没有 WebView2 Runtime 时能够完成安装；
3. Qwen 实时翻译扩展能够被发现和加载；
4. UI 能够启动 USB/IP 驱动安装程序；
5. 驱动安装后能够创建虚拟麦克风或虚拟扬声器；
6. UI 的“强制重启引擎”功能正常；
7. 同版本重装和新版本覆盖升级均正常；
8. 静默安装不会自动启动 AVC：

```powershell
.\AVC-0.1.0-x64-setup.exe /VERYSILENT /NORESTART
```

不要在有重要 USB 音频、视频或存储任务运行时测试 USB/IP 驱动安装，因为上游安装程序会短暂重置 USB 3 Hub。

## 9. 代码签名

未签名的安装包可以内部测试，但公开下载时可能出现 Windows SmartScreen 警告。正式发布建议使用受信任的代码签名证书：

1. 在 Inno Setup 编译前签署 `avc.exe` 和自有扩展 DLL；
2. 编译安装包；
3. 再签署最终的 `AVC-<版本>-x64-setup.exe`；
4. 使用 RFC 3161 时间戳服务，确保签名在证书到期后仍可验证。

示例命令中的证书指纹和时间戳地址需要替换为实际值：

```powershell
signtool sign /fd SHA256 /td SHA256 `
  /tr "<RFC3161 时间戳 URL>" `
  /sha1 "<证书指纹>" `
  "dist\installer\AVC-0.1.0-x64-setup.exe"
```

不要修改或重新签署 USB/IP、VC++ Redistributable 和 WebView2 安装程序；它们应保留上游厂商签名。

## 10. 常见问题

### 找不到 CMake 或 ISCC

使用工具的绝对路径，或者重新打开终端让安装程序更新后的 `PATH` 生效。

### `ERR_PNPM_IGNORED_BUILDS: esbuild`

确认 `ui\pnpm-workspace.yaml` 中有：

```yaml
allowBuilds:
  esbuild: true
```

然后重新运行 `pnpm install --frozen-lockfile` 和 `pnpm run build`。

### Inno Setup 报找不到源文件

依次确认：

- `StageDir` 与 CMake 安装前缀一致；
- `packaging\redist` 中的两个 Microsoft 安装程序存在；
- `bin\drivers` 和 `bin\extensions` 已由 CMake 安装。

### 安装包里没有 USB/IP 驱动

确认是 Windows x64 构建，并在 CMake 配置时设置：

```text
AVC_BUNDLE_USBIP_DRIVER_INSTALLER=ON
```

如果修改过选项，建议使用新的构建目录重新配置，避免旧 CMake 缓存干扰。

### 安装后 WebView2 仍不可用

在线引导程序需要访问微软下载服务。离线或受限网络环境应改用 x64 Evergreen Standalone Installer。

### 安装包显示“未知发布者”

这是安装包没有 Authenticode 代码签名造成的。需要使用可信代码签名证书签署最终 EXE，重新压缩文件或仅计算 SHA-256 不能消除该提示。

## 11. 快速命令清单

版本和路径已经在 `packaging\avc.iss` 中配置好、运行库也已准备好时，完整流程可简化为：

```powershell
Set-Location ui
pnpm install --frozen-lockfile
pnpm run build
Set-Location ..

cmake --preset windows-package `
  -DAVC_BUILD_TESTS=OFF `
  -DAVC_BUILD_QWEN_LIVETRANSLATE_EXT=ON `
  -DAVC_BUNDLE_USBIP_DRIVER_INSTALLER=ON

cmake --build build/windows-package `
  --config Release `
  --target avc avc_qwen_livetranslate `
  --parallel

cmake --install build/windows-package `
  --config Release `
  --prefix "C:\完整路径\avc\dist\avc-stage-0.1.0"

& "C:\Program Files\Inno Setup 7\ISCC.exe" "packaging\avc.iss"
```
