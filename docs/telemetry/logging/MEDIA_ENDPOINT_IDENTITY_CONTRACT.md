# 媒体端点身份契约

日期：2026-09-27。适用范围：LG0 冻结的安全诊断事件与历史支持包。此处的“端点”是客户端已接受的远端媒体消费绑定：解码视频、PCM 音频或可见渲染提交。它不是 ICE candidate pair、IP:port、SFU 节点、participant SID、track SID 或用户身份；这些原值不得写入普通诊断日志或安全包。

## 身份与生命周期

- `endpoint_id` 是每次远端 native 绑定被 SessionTelemetry 接受时生成的 32 个小写十六进制字符（128-bit）的随机 ID。它只在当前 process run 内用于比对，重新绑定必须生成新 ID；同一绑定在软恢复后沿用原 ID。不把内部 `series_key`、binding serial 或原始 track ID 转成可逆输出。
- 重连 episode 开始时，按当时实际期望接收的绑定保存 `previous_endpoint_id`。只有来自当前 room generation 和 binding epoch 的媒体样本通过现有稳定窗口后，才发出 `media.endpoint.recovered`。事件中的 `endpoint_id` 来自该样本对应的当前绑定，不能由协商成功、订阅意图或 UI 状态推断。
- 恢复事件携带 `process_run_id`、`anonymous_session_id`、`operation_id`、`session_generation`、`room_generation`、`recovery_epoch`、`media_kind`、`measurement_point`、`duration_ms` 及可选 `previous_endpoint_id`。两个 ID 相等表示沿用绑定，不等表示本次恢复使用了新绑定；旧 ID 缺失表示重连开始时没有可验证的先前绑定。
- 解码视频、PCM 音频和可见渲染是不同测量点，各自拥有端点 ID。端点事件不表示对方设备身份，也不声称媒体传输曾经中断；信令恢复与媒体持续可用可以同时成立。

## 安全包完整性

- `media.recovery.milestone` 的 `expected_endpoints` 是该测量点本次 episode 中实际期望恢复的绑定数。支持包仅当同一 operation ID、session generation、recovery epoch、媒体类型和测量点的不同 `media.endpoint.recovered` ID 数恰好等于该数，且时间线没有截断或非法行、没有媒体恢复超时，才移除 `recovered_media_endpoint_identity` 缺失项。
- 旧版没有端点字段的里程碑、未知或不合法 ID、媒体超时及不完整时间线继续报告缺失。正常诊断队列丢失计数仍单独呈现；不能因缺少事件而补造端点或业务成功。
- 端点 ID 只在安全 JSONL 和用户主动导出的包内出现。源头事件为闭合字段，强制校验 32 个小写十六进制字符；不允许自由文本、raw WebRTC stats、SDP/ICE、网络地址或凭据进入该事件。

## 运行验收边界

独立测试房间中，先确认远端摄像头帧到达，再通过仅绑定 `127.0.0.1` 的专用 TCP 代理切断该客户端的 LiveKit 信令连接并短时拒连。验收必须看到真实 transport 断开、重连开始与终态、断开后重新收到媒体帧、稳定恢复事件和同一 session/epoch 下的端点 ID；脚本、媒体探针、诊断写盘均须成功。该实验只验证**信令路径的真实网络故障**，不等于 UDP/RTP 故障、跨物理设备、真实后端协议或生产 TLS 验收。
