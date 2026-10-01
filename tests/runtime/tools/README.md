# Runtime 工具领域目录

| 目录 | 内容 | 主要入口 |
|---|---|---|
| `product_acceptance/` | 产品验收、采集、证据校验、云端传输及配套配置 | `invoke_product_external.ps1`、`verify_product_acceptance.py` |
| `meeting/` | 会议长稳、渲染探针、fake peer、低带宽发布与资源采样 | `meeting_soak.py`、`meeting_render_probe.py` |
| `screen_capture/` | WGC 长稳、屏幕共享质量采集与分析 | `run_wgc_soak.py`、`invoke_screen_share_quality_probe.py` |
| `desktop/` | 产品 UIA 监督、低频快照及 PowerShell worker | `product_uia_retest.py`、`uia_snapshot.py`、`e2ee_password_uia_worker.ps1` |
| `media/` | 双端媒体矩阵与 E2EE 互操作驱动 | `invoke_e2e_media_matrix.ps1`、`invoke_e2ee_interop.py`、`invoke_e2ee_product.py` |
| `diagnostics/` | 诊断场景统一参数入口 | `invoke_diagnostic_probe.ps1` |

入口继续支持按文件路径直接执行，CLI 参数和运行条件保持不变。使用仓库根目录作为工作目录，
具体命令与执行边界见 [runtime README](../README.md)、[UIA README](../../uia/README.md)
和 [编排 README](../orchestration/README.md)。

同领域 Python 模块及配套 JSON / worker 保持同目录；`desktop/product_uia_retest.py` 显式依赖
`meeting/meeting_soak.py`，屏幕质量驱动显式依赖 `product_acceptance/product_aliyun_transport.py`。
跨领域入口只添加所需目录，不扫描或自动加入全部领域目录。

远端部署的 `product_pilot_remote.py` 与 `product_pilot_context.py` 仍须一起复制，保持原来的远端文件名。
离线自测在 `../selftests/`，真实桌面自测在 `../desktop_checks/`；工具目录不注册新的自动测试。

```powershell
python -B -m unittest discover -s tests/runtime/selftests -p "test_*.py" -v
```

工具运行涉及服务、设备、桌面、采集或长稳时仍需满足原有显式执行条件；离线自测通过不代表 runtime 验收通过。
