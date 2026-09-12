# AVC

AVC 使用 CMake 构建 C++20 音频引擎，使用 pnpm/Vite 构建内嵌编辑器。
C++ 通用依赖由 **vcpkg manifest 模式**管理，版本由 `vcpkg.json` 的
`builtin-baseline` 固定。首次 CMake 配置会自动安装所需依赖。

## Windows x64

需要 Visual Studio 2022 或更新版本的“使用 C++ 的桌面开发”、Windows SDK、
CMake 3.24+ 以及 Node.js/pnpm（Visual Studio 2026 需要 CMake 4.2+）。
在提供这些工具的 Developer PowerShell 中，从仓库根目录执行以下命令。

先准备 vcpkg。如果已有独立安装或 Visual Studio 自带的 vcpkg，只需将
`VCPKG_ROOT` 指向包含 `scripts/buildsystems/vcpkg.cmake` 的目录：

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
& C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
$env:VCPKG_ROOT = 'C:\dev\vcpkg'
```

构建编辑器、程序并运行测试：

```powershell
pnpm --dir ui install --frozen-lockfile
pnpm --dir ui run build
cmake --preset windows-x64
cmake --build --preset windows-release --parallel
ctest --preset windows-release
```

程序位于 `build\windows-x64\Release\avc.exe`。调试构建使用
`windows-debug` 构建/测试预设。Windows 预设使用 `x64-windows-static-md`：
第三方库静态链接，MSVC CRT 使用 `/MD`（Debug 为 `/MDd`），与安装包中的
Visual C++ Redistributable 配套。

Windows 预设让 CMake 自动选择已安装的 Visual Studio 生成器；也可在首次配置时显式指定：

```powershell
cmake --preset windows-x64 -G "Visual Studio 18 2026"
```

启用 Qwen 语音扩展会自动安装带 TLS/WebSocket 支持的 curl；关闭测试后无需安装 GTest：

```powershell
cmake --preset windows-x64 -DAVC_BUILD_QWEN_LIVETRANSLATE_EXT=ON -DAVC_BUILD_TESTS=OFF
cmake --build --preset windows-release --parallel
```

开发时可加 `-DAVC_BUNDLE_USBIP_DRIVER_INSTALLER=OFF` 跳过驱动安装包下载。
可发布的主安装包使用 `windows-package` 预设，详见
[Windows 安装包构建指南](packaging/BUILD_INSTALLER.zh-CN.md)。

## Linux x64

需要 C++20 编译器、CMake 3.24+、Ninja、Git、curl、zip/unzip、tar、pkg-config、
PipeWire 开发包以及 Node.js/pnpm。PipeWire 是系统音频服务，开发库通过
`pkg-config` 从系统获取。例如 Debian/Ubuntu 可安装 `build-essential cmake
ninja-build git curl zip unzip tar pkg-config libpipewire-0.3-dev`。

```sh
git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
"$HOME/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
export VCPKG_ROOT="$HOME/vcpkg"
pnpm --dir ui install --frozen-lockfile
pnpm --dir ui run build
cmake --preset linux-x64
cmake --build --preset linux-release --parallel
ctest --preset linux-release
```

程序位于 `build/linux-x64/Release/avc`。调试使用 `linux-debug` 构建/测试预设。

## 自定义构建与依赖

不使用预设时，必须显式传入 vcpkg toolchain；例如 PowerShell：

```powershell
cmake -S . -B build/custom -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build build/custom --config Release --parallel
ctest --test-dir build/custom -C Release --output-on-failure
```

迁移已有构建时使用新的目录，不要复用旧的 FetchContent 构建缓存。更换生成器、
toolchain 或 triplet 也应使用新目录。机器专属路径可放在已忽略的
`CMakeUserPresets.json` 中；无需执行 `vcpkg integrate install`。

| 依赖 | 来源 / 启用条件 |
| --- | --- |
| spdlog、fmt、nlohmann-json、cpp-httplib | vcpkg；始终启用，HTTP 库不启用可选压缩/TLS |
| miniaudio、WebView2 SDK | vcpkg；仅 Windows |
| GTest | vcpkg `tests` feature；`AVC_BUILD_TESTS=ON`（默认） |
| curl | vcpkg `qwen-livetranslate` feature；`AVC_BUILD_QWEN_LIVETRANSLATE_EXT=ON` |
| PipeWire | Linux 系统开发包 |
| ONNX Runtime、RVC 转换环境与基础模型 | RVC 扩展现有固定版本下载；保留 CPU/CUDA 包选择和 `ONNXRUNTIME_ROOT` |
| usbip-win2 安装程序 | Windows x64 现有下载及 SHA-256 校验 |

CMake 在 `project()` 前根据构建选项选择 manifest features，无需手工安装 GTest 或 curl。
curl 在 Windows 使用 Schannel，在 Linux 使用 OpenSSL。依赖许可证在
`cmake --install` 时一起安装；动态 Windows triplet 的运行时 DLL 也会复制到安装目录。

RVC 的完整配置和独立发布流程见 [RVC 扩展说明](extensions/rvc/README.md) 和
[RVC 安装包构建指南](packaging/BUILD_RVC_EXTENSION.zh-CN.md)。
vcpkg 接入方式参见 [官方 CMake 集成文档](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration)。
