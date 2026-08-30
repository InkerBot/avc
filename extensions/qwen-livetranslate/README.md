# AVC Qwen 语音扩展

此扩展通过阿里云百炼提供三个可独立使用的实时节点：

- `qwen-livetranslate.stt`：`audio → transcript`，默认使用
  `qwen3-asr-flash-realtime`；
- `qwen-livetranslate.tts`：`text → audio`，默认使用
  `qwen3-tts-flash-realtime`；
- `qwen-livetranslate.interpreter`：输入原声，同时输出译文、源语言识别文本与
  24 kHz 翻译语音，使用 `qwen3.5-livetranslate-flash-realtime`。

模型返回的 PCM 会自动重采样到 AVC 引擎采样率。音频回调只执行采样率转换、
固定内存快照和 SPSC 队列读写。WSS、TLS、JSON、Base64、重连和 `session.finish`
都在独立线程执行，不会在实时音频线程上访问网络。

## 构建

默认静态编译固定版本的 libcurl；Windows 使用 Schannel，Linux 使用 OpenSSL：

```sh
cmake -S . -B build-qwen -DAVC_BUILD_QWEN_LIVETRANSLATE_EXT=ON
cmake --build build-qwen --config RelWithDebInfo --target avc_qwen_livetranslate
```

Linux 打包环境也可设置 `-DAVC_QWEN_USE_SYSTEM_CURL=ON`，但系统 libcurl 必须不低于
7.86 且启用了 WebSocket 与 TLS。安装组件名为 `qwen-livetranslate`。

## 密钥与地址

在 AVC 的“扩展设置”中选择“Qwen 语音”，填写 API Key、服务地域与 Workspace ID，
然后点击“保存并重启引擎”。API Key 输入框默认隐藏字符，但按用户选择会以明文写入 AVC
扩展设置文件：

- Windows：`%APPDATA%\avc\extensions.json`；
- Linux：`${XDG_CONFIG_HOME:-~/.config}/avc/extensions.json`。

扩展不读取环境变量。请限制设置文件的访问权限，不要将其提交到版本控制或随诊断包发送。
北京地域使用 `cn-beijing`，新加坡地域使用 `ap-southeast-1`，且 API Key、Workspace ID 与
地域必须匹配。推荐填写 Workspace ID；未填写时使用阿里云仍兼容的旧域名。STT、TTS 和
同声传译分别有模型名与完整 WSS 地址覆盖项。为防止 Bearer Token 泄漏，覆盖地址必须使用
`wss://` 且主机位于 `aliyuncs.com`。

## 使用

安装并启用扩展后，可加载“Qwen 同声传译（麦克风到扬声器）”预设，或手工连接：

```text
Capture → qwen-livetranslate.interpreter → Playback
```

也可以加载“Qwen 语音识别并重读”预设，验证两个新节点：

```text
Capture → qwen-livetranslate.stt → qwen-livetranslate.tts → Playback
```

`stt` 会把实时修订和最终识别结果写成 AVC 分段文本帧。`tts` 对分段文本只合成最终版本，
避免把尚会修正的 ASR 草稿重复朗读；普通文本输入则会在内容变化时合成一次。TTS 输出支持
语种、语速、音量、语调、音色和抖动缓冲设置。`instructions` 仅在把 TTS 模型改为
`qwen3-tts-instruct-flash-realtime` 时使用。

仓库中的 `example-graph.json` 是同声传译拓扑的可直接加载版本，
`example-speech-graph.json` 是 STT → TTS 拓扑。枚举参数在图文件中保存为索引；
默认的 `target_language: 1` 是英语，`source_language: 0` 是自动检测。

节点支持 60 种目标语言。模型仅为其中 29 种生成语音；选择其余语种时节点自动使用纯文本
模式并保持音频输出静音。`voice_clone` 支持 `once`、`always` 和预先复刻的 `preset`；
`preset` 模式需要在 `voice` 中填写 `qwen-translate-vc-...` 音色 ID。

停止节点、重载图或关闭 AVC 时，扩展会先发送 `session.finish`，最多等待 5 秒接收
`session.finished`，以避免丢失最后一段翻译。

协议依据：

- <https://help.aliyun.com/zh/model-studio/qwen-asr-realtime-interaction-process>
- <https://help.aliyun.com/zh/model-studio/interactive-process-of-qwen-tts-realtime-synthesis>
- <https://help.aliyun.com/zh/model-studio/qwen3-5-livetranslate-flash-realtime>
