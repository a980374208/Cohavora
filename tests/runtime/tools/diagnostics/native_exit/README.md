# 原生退出诊断采样

`run_publisher_exit_capture.py` 和 `run_full_publisher_exit_capture.py` 是 Linux
上的独立历史输入诊断入口，分别使用 120 秒和 660 秒 SDK 预算；API 清理单独限时
30 秒。它们不运行产品 UI，不授予 PILOT 资格或正式长稳信用。

## 冻结夹具与来源

`fixtures/` 保存按原始字节冻结的项目自有历史源码，供诊断部署和离线自测使用。
局部 `.gitattributes` 禁用这些 Python 文件的换行转换，确保 Windows 干净检出后
SHA256 仍与冻结值一致。不要把历史夹具当作当前实现，也不要在此修改生命周期行为。

| 文件 | SHA256 | 来源 |
|---|---|---|
| `fixtures/product_pilot_load.py` | `5113c47cff213ef8161238d4e20eff9c1d19fb1e43e3d35cb44bd8f827e5d373` | 本仓库提交 `732a7bac46d4e24dd2fde39692faa87e4d7d74e5` 中的 `tests/runtime/tools/product_acceptance/product_pilot_load.py` |
| `fixtures/subscriber.py` | `288007e11f5bf0cc05a055b6c9a92759236a567126adf3dff368e4b32cdc82d0` | 原生退出诊断 run `5f791acb096f454184890364996f5e74` 的任务自有订阅夹具；原始证据副本保留 |

两个入口只接受以上发布端 SHA；当前发布端的 capture 排空修复不会自动进入历史
重放。使用当前发布端需要另行定义、冻结和验证输入，历史观察不能作为当前源码验收。
订阅端阶段包装器默认要求以上订阅夹具 SHA，不增加 GC 或 FFI disposal 调用。

## 部署依赖与执行边界

将发布端和订阅端夹具复制到显式冻结的任务 bundle，保留文件名
`product_pilot_load.py`、`subscriber.py`。同一目录还须部署
`../../product_acceptance/product_pilot_scheduler.py`、`product_pilot_timing.py`
和 `product_pilot_video_counter.py`；调度策略来自同领域
`product_pilot_scheduler_policy.json`。三个 Python 依赖必须同时列入计划的 `files`
指纹清单，入口会拒绝遗漏。这里只声明依赖，不复制或修改外部 SDK。
若订阅端另放一个目录，该目录中的 scheduler 和 video-counter 副本也必须分别
列入清单；入口不以发布端目录的指纹代替订阅端实际加载的文件。

阶段包装器、API helper，以及完整入口的 launcher/observer 必须按各自计划中的
字段一起部署。完整入口支持 `full-exit-660-two-240` 和
`full-exit-all-660-two-240`；后者同时观测发布端及两个订阅端。
独立 SDK 环境、六项 SDK 文件指纹、原生 observer `.so`、目标配置、服务地址和
任务独占房间均为显式运行前提，不包含在夹具中。计划 SHA 必须先于解析校验，
旧证据和冻结计划不由本次整理改写。

SDK 采样使用外层 GNU timeout；`--cleanup` 随后用同一入口和计划单独执行 API-only 清理：

```sh
timeout -k 2s 120s python -B run_publisher_exit_capture.py --plan PLAN --plan-sha256 SHA256
timeout -k 2s 30s python -B run_publisher_exit_capture.py --plan PLAN --plan-sha256 SHA256 --cleanup
timeout -k 2s 660s python -B run_full_publisher_exit_capture.py --plan PLAN --plan-sha256 SHA256
timeout -k 2s 30s python -B run_full_publisher_exit_capture.py --plan PLAN --plan-sha256 SHA256 --cleanup
```

原始 stderr、映射和 pthread trace 留在任务采集目录，不向正式验收授予信用。
本组交付采样控制器及阶段校验，未交付 PXTRACE1 完整 trace 安全导出链；该能力
为 `DEFERRED`。历史 `out` 下 exporter 不属于入口的运行依赖，不能据此宣称完整
后处理可用。Rust 回溯字段解析和 NXENTRY2 标量解码是已提交的独立诊断模块；
空 trace、正常退出或 `NON_REPRODUCED` 均不能证明 panic 根因或修复。

离线自测使用 `tests/runtime/selftests/` 下的三个对应测试，不读取历史 `out`：
`test_publisher_exit_capture.py`、`test_full_publisher_exit_capture.py` 和
`test_subscriber_exit_phases.py`。自测不加载 SDK、原生 observer 或访问服务；
Linux 真实采样、媒体和清理仍需单独执行，未运行时保持 `NOT_RUN`。
