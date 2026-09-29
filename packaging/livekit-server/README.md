# LiveKit 屏幕共享动态尺寸兼容补丁

基线：上游 LiveKit v1.13.6。patch/json 记录原文件、补丁文件、构建和测试二进制的 SHA256。本目录不含凭据或令牌。

## 修复边界

- 普通信令屏幕共享启用已有媒体尺寸解析通路；保留发送端声明的顶层尺寸，低层/备用 codec 不得缩小顶层尺寸。
- 同步主 codec 与 legacy layers 的几何；保留 SSRC、RID 和码率，不改变编码层拓扑。
- 几何改变后释放 receiver 锁，再通知当前 Track 的 Participant owner，刷新 participant/room-store；退役 Track 不再通知。
- AV1 没有携带 DD 分辨率时，当前负载解析器无法推导尺寸。仅对非 simulcast、明确单层且此前等于顶层尺寸的主 AV1 层同步 publisher 信令；不推算 SVC、多层或备用 codec。
- 普通摄像头、one-shot 原策略不变。

## 构建

在干净上游 v1.13.6 源码根目录执行 `git apply --check` 后应用补丁。固定 `GOTOOLCHAIN=go1.26.6`、`CGO_ENABLED=0`、`GOOS=linux`、`GOARCH=amd64`，执行 `go build -o livekit-server-quality ./cmd/server` 和 `go test -c -o rtc-quality.test ./pkg/rtc`。

Linux 定向测试：`./rtc-quality.test -test.run 'Test(ScreenShare(DynamicDimensions|DimensionsNotifyParticipant|AV1SignalledDimensions)|GetQualityForDimension|TrackInfo)$' -test.v -test.timeout 60s`。v6 定向测试 PASS。

## 历史 v4 维护结果

用户已授权把空闲 ECS 用作测试云服务。每次维护先在服务端签发短期 roomList 令牌查询参与者；仅总数为零才切换。配置和备份仅保存在服务端 0600 文件中，密钥不离开服务端。

2026-09-29：v4-retry1 已 ACCEPTED。实际镜像 `sha256:7e806f5b83abe356b37b1b64418eb7c57f9ad57df7fc7d2c430da5bd3f64aa78`；二进制指纹见 JSON。原容器保留为 `livekit-before-quality-20260929-v4-retry1`。

本地原镜像派生构建不访问 registry；上传二进制校验后设置 0755。候选使用原配置与 17880/17881/17882 端口。维护有 900 秒服务端 watchdog；启动或门禁失败立即恢复原容器、restart policy 和运行状态，成功 accept 解除自动回滚。

## 证据与历史

相对仓库根 `out/screen-share-quality/`：

| 证据 | 结果及边界 |
|---|---|
| s0-server-unit-v6.json | 最终 v4 Linux 定向单测 PASS |
| s0-server-v3-gate/simulcast | VP8 两轮 720→1080→1440→1080→720 PASS；H264 第二轮尺寸选层未在旧 4 秒窗口内收敛，FAIL 并回滚 |
| s0-server-v3-retry1-gate/simulcast | H264 十阶段 PASS；选层等待上限 25 秒，实际均小于 1.5 秒；不据此声称修复 H264 codec 缺陷 |
| s0-server-v3-retry1-gate/single | VP8/H264/VP9 十阶段 PASS；AV1 层元数据 FAIL，已回滚 |
| s0-server-v4-retry1-gate | AV1 十阶段及 VP8/H264 摄像头回归 PASS；ACCEPTED |

v4 新变化仅涉及明确单层 AV1 信令回退，复用 v3 不受影响的 VP8/H264 双层及 VP8/H264/VP9 单层结果。v4 首次维护因二进制缺执行位启动失败并恢复原服务，修正后使用新目录复验。更早的 v2 元数据失败和独立候选公网端口失败记录全部保留。两层 RID q/h 对应 LOW/MEDIUM，第三层 HIGH 不存在，为 N/A。

这些证据证明机制兼容，不证明复杂 2K/30 FPS 性能或长期稳定性。当前 ECS 公网带宽与简单合成图案的性能边界需单独评估。

## 历史 v5 维护结果

最终生产 API 复验暴露 v4 额外缺陷：几何元数据更新调用 StreamTrackerManager.UpdateTrackInfo，后者无条件 SetPaused(false)，实际重置流层跟踪器。active/stopped 通知交错后 LOW 可能持续不可用；失败时服务端已正确选择 spatial 0，却仍转发高层。原始证据 `s4-production-final-simulcast` 保留 FAIL，包含按字段白名单保存的 SFU 时间线。

v5 仅对屏幕共享，在 mute 状态不变时跳过 tracker pause 更新；真正 mute/unmute 和摄像头旧行为不变。新增 `pkg/sfu/screen_share_tracker_test.go`，Linux 定向回归 PASS（`s0-server-unit-v7.json`），原 RTC 测试源码与实现未变，复用 v6 证据。

v5 空闲检查参与者为 0；`s0-server-v5-gate` 全部门禁 PASS：正式 Room API 的 VP8/H264 双层各十阶段、VP8/H264/VP9/AV1 单层各十阶段，以及 VP8/H264 摄像头各三阶段。已 ACCEPTED。当前镜像 `sha256:a2b1d1e19210e8cb004f5573625deacb4e7d35e7f3b4928ef8fa7485246aaa8e`，二进制 `3f0ee7b542e22fca74fad46ae7c17856eae04bd6ef9ff5e17d1f176f1533b56a`。

v5 维护前容器保留为 `livekit-before-quality-20260929-v5`（v4），更早原版容器 `livekit-before-quality-20260929-v4-retry1` 仍保留。维护脚本当前记录 v5，重复执行会拒绝覆盖旧 evidence。完整源码补丁已再次对干净 v1.13.6 `git apply --check` PASS。

新增定向测试命令：`./sfu-quality-v5.test -test.run '^TestScreenShareMetadataPreservesActiveTracker$' -test.v -test.timeout 60s`。新构建时须同时编译 `go test -c -o sfu-quality.test ./pkg/sfu`，测试二进制指纹不与 RTC 测试混同。

## v6 拥塞反馈修复

4K/30 生成窗口在 v5 长稳第 170 秒出现连续 10 秒无新解码帧（`s4-generated-window-final-soak-r3`，FAIL 保留）。主层仍编码约 30 FPS，接收从约 24～28 FPS 逐渐下降。源码核对发现 consolidated single-PC 协商 Transport-CC，但默认 RemoteBWE 只消费 REMB，对 TWCC 的处理为空；对端采用 TWCC 时默认估算器缺少有效反馈。受控限带宽能够降层并恢复，证明选择控制通路本身可用。

v6 在 consolidated publisher transport 未显式启用 BWE interceptor 时使用 send-side BWE。双 PC 保持原策略；显式 interceptor 与 Enabled=false 不覆盖；不修改共享配置。新增 `TestConsolidatedTransportCongestionFeedback`，Linux 定向 PASS 见 `s0-server-unit-v8.json`。源码 patch 在干净上游 apply --check PASS。

v6 二进制 SHA256 `1f5355e04a70f73564c3aeab1c80c85f0edd10135a61202b3e2a9271d3fea74f`。维护门禁和 30 分钟负载结果以 `s0-server-v6-gate` 和实施报告的最终记录为准，候选构建及单测不等同长稳通过。

v6 维护前活动参与者为 0，完整门禁已 PASS 并 ACCEPTED，保留镜像 `sha256:caa93c01c9b18218eb84581f56721cea73fe5530d200066624ecd7e2e081bfc6`。VP8/H264 双层、四 codec 单层、普通视频、受控 1000 bps 限制及解除恢复均通过。上一版本容器保留为 `livekit-before-quality-20260929-v6`。`s0-server-v6-gate/provenance-note.json` 区分本轮实际可执行文件构建指纹与运行期间工作树探针诊断修改的指纹，不把二者混同。

权限边界：额外读取服务端配置白名单字段的尝试被自动审批拒绝，未执行；本修复依据本地服务端源码和获准的独立测试房间日志统计，不依赖该次配置读取。

## v7 当前维护结果

v7-retry2 空闲检查为 0，完整门禁 PASS 并 ACCEPTED（out/screen-share-quality/s0-server-v7-retry2-gate）。当前镜像 `sha256:37f99d42ec76fe8747295f3cde4d411a78cfc0e7d52019f313a53a05f3d624ce`，二进制 `8d9e645d2854e76298fc4110ed0f5149180f8e3a8c58777763d5956064f7c014`。v6 原容器保留为 `livekit-before-quality-20260929-v7-retry2`；维护脚本重复执行拒绝覆盖本轮状态。

新增修复：simulcast 保留 RID-derived spatial，只有 SVC 才用包内 spatial；VideoSizes 保留稀疏层索引，不在首个空槽截断。Linux 定向三项 PASS 见 s0-server-unit-v9.json，测试二进制及源码指纹见 JSON；干净上游 apply --check PASS。

主 VP9/备用 VP8 各双层十阶段、高低层实际解码及元数据 PASS；VP8/H264 双层各十阶段、四 codec 单层各十阶段、普通视频各三阶段和弱带宽恢复 PASS。低层/主层为 LOW/MEDIUM，第三层不存在，N/A。旧 v7 与 retry1 的低层失败均自动回滚 v6，证据保留；客户端修正 VP9 多 RID 模式、编码适配及 Dynacast MIME 名称后，本轮才通过，不将旧失败改写。

新增测试命令：`./sfu-quality-v7.test -test.run 'Test(SimulcastPacketSpatialKeepsRID|VideoSizesKeepsSparseLayerIdentity|ScreenShareMetadataPreservesActiveTracker)$' -test.v -test.timeout 60s`。原 RTC/BWE 实现未变，复用对应 Linux 结果。
