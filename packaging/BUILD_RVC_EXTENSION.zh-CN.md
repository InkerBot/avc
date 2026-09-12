# AVC RVC 扩展安装包构建指南

本文说明如何构建独立的 Windows x64 RVC 扩展安装包。主 AVC 安装包无需包含 RVC；需要变声功能的用户单独安装本扩展包即可。

当前完整 CPU 扩展包包含：

- `avc-rvc.dll`；
- ONNX Runtime 1.24.4 CPU 运行库；
- 独立 CPython 3.10.20；
- PyTorch 2.12.1 CPU、ONNX、FAISS、NumPy、SciPy 等固定版本依赖；
- ContentVec v1/v2 与 RMVPE 基础模型；
- `.pth` 和可选 `.index` 到 `.avcrvc` 的离线转换器；
- RVC 与 ONNX Runtime 的许可证和第三方声明。

当前暂存体积约 1.37 GiB，LZMA2 压缩后的安装包约 433 MiB。依赖版本变化后体积可能不同。

## 1. 安装布局

扩展安装器采用当前用户安装，不请求管理员权限：

```text
%LOCALAPPDATA%/avc/
├─ extensions/
│  ├─ avc-rvc.dll
│  └─ lib/
│     ├─ onnxruntime.dll
│     └─ onnxruntime_providers_shared.dll
└─ rvc-converter/
   ├─ avc-rvc-convert.exe
   ├─ convert_pth.py
   ├─ base/
   ├─ python/
   └─ site-packages/
```

安装器把当前用户的 `AVC_RVC_CONVERTER_ROOT` 设置为 `%LOCALAPPDATA%\avc\rvc-converter`。如果安装前已有自定义值，卸载扩展时会安全恢复；如果用户在安装后手动修改了该变量，卸载器不会覆盖用户的新值。

用户导入的 `.avcrvc` 模型保存在 AVC 数据目录中，不属于安装器文件，因此卸载扩展不会删除用户模型。

## 2. 环境要求

需要：

- Windows 10/11 x64；
- Visual Studio C++、Windows SDK 和 CMake；
- Inno Setup 7；
- vcpkg，并按[项目构建说明](../README.md)设置 `VCPKG_ROOT`；
- 首次配置和构建时可以访问 GitHub、Hugging Face、PyTorch 与 PyPI。

主 UI 不需要为扩展包重新构建，但 RVC 扩展必须和目标 AVC 版本使用相同源码和插件 ABI 构建。

## 3. 配置完整 CPU 版本

在仓库根目录运行：

```powershell
cmake -S . -B build-vcpkg-rvc-installer -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
  -DAVC_BUILD_TESTS=OFF `
  -DAVC_BUILD_RVC_EXT=ON `
  -DAVC_BUILD_QWEN_LIVETRANSLATE_EXT=OFF `
  -DAVC_RVC_ENABLE_CUDA=OFF `
  -DAVC_RVC_BUNDLE_ONNXRUNTIME=ON `
  -DAVC_RVC_BUNDLE_CONVERTER=ON `
  -DAVC_BUNDLE_USBIP_DRIVER_INSTALLER=OFF
```

配置阶段会下载并校验固定版本的 ONNX Runtime、CPython、RVC 转换源码和基础模型。

不要复用主程序的构建目录来切换 RVC/CUDA 选项。使用独立的 `build-vcpkg-rvc-installer` 可以避免 CMake 缓存和主安装包产物互相影响。

## 4. 构建扩展和转换器

```powershell
cmake --build build-vcpkg-rvc-installer `
  --config Release `
  --target avc_rvc `
  --parallel
```

首次构建会安装固定版本的 PyTorch、ONNX、FAISS 等转换依赖，下载和文件展开可能需要数分钟。

## 5. 生成扩展暂存目录

只安装 CMake 的 `rvc` 组件：

```powershell
cmake --install build-vcpkg-rvc-installer `
  --config Release `
  --component rvc `
  --prefix "C:\完整路径\avc\dist\rvc-extension-stage-0.1.0"
```

确认存在：

```text
dist/rvc-extension-stage-0.1.0/bin/extensions/avc-rvc.dll
dist/rvc-extension-stage-0.1.0/bin/extensions/lib/onnxruntime.dll
dist/rvc-extension-stage-0.1.0/libexec/avc/rvc-converter/avc-rvc-convert.exe
dist/rvc-extension-stage-0.1.0/libexec/avc/rvc-converter/base/contentvec-v1.onnx
dist/rvc-extension-stage-0.1.0/libexec/avc/rvc-converter/base/contentvec-v2.onnx
dist/rvc-extension-stage-0.1.0/libexec/avc/rvc-converter/base/rmvpe.onnx
```

## 6. 更新版本

打开 `packaging\rvc-extension.iss`，同步修改：

```ini
#define ExtensionVersion "0.1.0"
#define StageDir "..\dist\rvc-extension-stage-0.1.0"
VersionInfoVersion=0.1.0.0
VersionInfoProductVersion=0.1.0.0
```

不要修改 `AppId=AVC.Extension.RVC`，否则 Windows 会把新版本视为另一个产品，无法正确升级或卸载旧版本。

扩展源码中的插件版本也应同步更新：

```cpp
avc::sdk::Plugin plugin("rvc", "RVC voice conversion", "0.1.0");
```

## 7. 编译扩展安装包

```powershell
& "C:\Program Files\Inno Setup 7\ISCC.exe" `
  "packaging\rvc-extension.iss"
```

生成：

```text
dist\installer\AVC-RVC-Extension-0.1.0-x64-setup.exe
```

压缩 3 万多个文件可能需要数分钟。构建服务器上可以使用 `--quiet-progress` 减少日志：

```powershell
& "C:\Program Files\Inno Setup 7\ISCC.exe" `
  --quiet-progress `
  "packaging\rvc-extension.iss"
```

## 8. 自检

### 验证转换运行时

```powershell
$root = (Resolve-Path `
  "dist\rvc-extension-stage-0.1.0\libexec\avc\rvc-converter").Path

$env:PYTHONHOME = Join-Path $root "python"
$env:PYTHONPATH = (Join-Path $root "site-packages") + ";" + $root
$env:PYTHONNOUSERSITE = "1"
$env:PYTHONDONTWRITEBYTECODE = "1"

& (Join-Path $root "python\python.exe") -s -c `
  "import torch, onnx, faiss, numpy, scipy; print(torch.__version__)"

& (Join-Path $root "avc-rvc-convert.exe") --help
```

### 验证 AVC 能加载扩展

```powershell
$env:AVC_RVC_CONVERTER_ROOT = (Resolve-Path `
  "dist\rvc-extension-stage-0.1.0\libexec\avc\rvc-converter").Path

& "build-windows\Release\avc.exe" `
  "--extension-dir=$((Resolve-Path `
    'dist\rvc-extension-stage-0.1.0\bin\extensions').Path)" `
  --list-extensions
```

输出应包含：

```text
extension 'rvc' 0.1.0 loaded
node  rvc.voice_conversion
```

### 校验最终安装包

```powershell
Get-FileHash -Algorithm SHA256 `
  "dist\installer\AVC-RVC-Extension-0.1.0-x64-setup.exe"

Get-AuthenticodeSignature `
  "dist\installer\AVC-RVC-Extension-0.1.0-x64-setup.exe"
```

## 9. 安装测试

正式发布前在干净的 Windows 10/11 x64 虚拟机验证：

1. 安装匹配版本的 AVC 主程序；
2. 完整退出 AVC；
3. 安装 RVC 扩展包；
4. 重新启动 AVC，确认扩展面板显示 RVC；
5. 导入一个受支持的 RVC v1/v2 F0 `.pth`，可选附带标准 L2 `IndexIVFFlat` `.index`；
6. 确认转换完成并生成 `.avcrvc`；
7. 创建 `rvc.voice_conversion` 节点并进行音频测试；
8. 覆盖升级同一扩展；
9. 卸载扩展，确认 AVC 和用户模型仍保留。

安装扩展后必须完整退出并重新启动 AVC。只使用“强制重启引擎”不足以让已经运行的 AVC 父进程重新读取新的环境变量。

## 10. CUDA 版本

默认包是兼容性更好的 CPU 版本。构建 CUDA 版时修改配置：

```powershell
-DAVC_RVC_ENABLE_CUDA=ON
```

需要 CUDA 13 ONNX Runtime 包时再增加：

```powershell
-DAVC_RVC_ONNXRUNTIME_CUDA13=ON
```

CUDA 安装包应使用不同的输出文件名，并明确标注所需 NVIDIA 驱动/CUDA 兼容性，避免用户误装。

## 11. 签名与发布

发布前建议依次签署：

1. `avc-rvc.dll`；
2. `avc-rvc-convert.exe`；
3. 最终 RVC 扩展安装程序。

ONNX Runtime DLL 已保留微软上游签名，不要修改或重新签署。没有代码签名证书时可以内部测试，但公开下载可能显示 SmartScreen“未知发布者”。
