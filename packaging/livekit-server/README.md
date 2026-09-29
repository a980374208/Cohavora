# LiveKit Server 屏幕共享补丁

补丁目标为上游 LiveKit Server `v1.13.6`。源码来源、工具链和各产品文件的输入／输出 SHA-256 见 `screen-share-dimensions-v1.13.6.json`。

## 行为

- 屏幕共享的媒体尺寸变化通过参与者通知边界同步房间元数据。
- 保留发布端信令中的完整屏幕尺寸，低层或备用 codec 的尺寸不会缩小轨道整体尺寸；AV1 单个完整尺寸层可使用明确的信令几何信息。
- 屏幕共享仅更新元数据时保留活动 tracker；实际 mute/unmute 继续更新暂停状态。
- consolidated publisher transport 未显式启用 BWE interceptor 时使用 send-side BWE；双 PC、显式 interceptor 和禁用配置保持原策略。
- simulcast 使用 RID 对应的 spatial 层，SVC 使用包内 spatial；VideoSizes 保留稀疏层索引。

## 应用与构建

在干净上游 `v1.13.6` 源码根目录执行：

```sh
git apply --check /path/to/screen-share-dimensions-v1.13.6.patch
git apply /path/to/screen-share-dimensions-v1.13.6.patch
GOTOOLCHAIN=go1.26.6 CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -o livekit-server ./cmd/server
```

部署时使用实际构建的可执行文件，并按所使用的 LiveKit Server 配置管理进程和容器。
