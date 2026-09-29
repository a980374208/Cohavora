# 测试执行分层

耗时回归始终注册在 CTest 中；无筛选执行和 `full-debug` preset 均包含它们。
快速入口只改变选择范围，不缩短等待、不放宽断言，也不表示完整回归通过。

| 入口 | 范围 |
| --- | --- |
| `ctest --preset quick-debug` | 排除 `DURATION_REGRESSION` 的开发回归 |
| `ctest --preset duration-debug` | 仅执行 `DURATION_REGRESSION` |
| `ctest --preset full-debug` | 当前配置注册的全部测试，包括耗时回归 |
| `ctest --preset core-debug` | CORE 回归；不包含独立耗时组 |

前三个 preset 使用 `windows-vs2026-dev` 的构建目录；已有 `build-debug` 可直接使用：

```powershell
ctest --test-dir build-debug -C Debug --output-on-failure -LE DURATION_REGRESSION
ctest --test-dir build-debug -C Debug --output-on-failure -L DURATION_REGRESSION --no-tests=error
```

## 耗时回归的必跑条件

| 测试 | 保留的契约 | 相关改动必须覆盖 |
| --- | --- | --- |
| `diagnostic_qt_blocked_ui_test` | UI 不处理事件 10 秒时，3000 条记录仍写入成功，UI 队列丢失可见 | diagnostic writer/mirror、Qt 日志队列、背压或丢弃统计 |
| `telemetry_throughput_test` | 约 50 MiB 历史数据、320 次提交、跨越 15 秒维护周期，验证零拒绝和零丢失 | telemetry history 的队列、缓存、维护、持久化或提交节奏 |

相关改动执行对应耗时用例；同时触及两者时执行整个耗时组。Snapshot/RC/Release
执行完整回归，不必再重复执行耗时组。吞吐测试的约 18.8 秒提交时间表保持不变。
Qt bridge 的其余过滤、隐私、缓存和生命周期检查仍由 `diagnostic_qt_bridge_test` 执行。

真实设备、服务和交互桌面继续遵循各自的 opt-in 开关；耗时组是本地合成回归，
其 PASS 不代表真实媒体或长稳验收。
