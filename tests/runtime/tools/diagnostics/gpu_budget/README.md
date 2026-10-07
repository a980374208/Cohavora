# 产品进程内 GPU 预算旁证

`QueryVideoMemoryInfo` 返回调用进程的预算和用量。外部探针只能测自己；产品
预算由 `--debug` 下的 `PilotDiagnosticsProbe` 独立采集线程读取，使用显式
`LIVEKIT_UIA_GPU_BUDGET_PROBE=1` 开关，每秒一次。普通调用默认不开启。
采集器初始化时用临时 D3D12 device 读取每个硬件适配器的实际 node 数，
该 device 不提交 GPU 工作，在首个预算快照前释放。逐 node 查询 local/nonlocal
组，记录 node 数的查询 HRESULT、API HRESULT、
Budget、CurrentUsage、AvailableForReservation、CurrentReservation 和查询耗时。
无效结果保留 null/UNAVAILABLE；适配器分别报告，不累加可能的别名。
节点发现失败、节点组缺失或 DXGI factory 已因适配器变化过期时不宣称完整。

独立工程仅用于 API 可用性核查，其 PID 不得当作产品 PID：

```powershell
cmake -S tests/runtime/tools/diagnostics/gpu_budget -B out/build/product-gpu-budget -G "Visual Studio 18 2026" -A x64
cmake --build out/build/product-gpu-budget --config RelWithDebInfo
out/build/product-gpu-budget/RelWithDebInfo/product_gpu_budget_probe.exe out/new-gpu-capability.jsonl
```

原 B14 普通 PILOT 和 Formal 编排现在均启用产品预算采集，并将采集器条件、
helper、分析器和证据自测纳入执行指纹。外部复核增加
`dxgi_node0_budget_coverage` 与 `dxgi_all_nodes_budget_coverage`：绑定实际
RelWithDebInfo 产品 PID、run 和 cycle，
逐周期核对入会至导出（含释放观察）的完整字段与最大 2 秒采样间隔。
缺失、跨进程、跨周期或采样断档均 FAIL；WDDM 资源增长门保持独立。

单独诊断使用 `-GpuBudgetDiagnostic`。该开关只允许 Pilot，记录
diagnostic-only marker，由控制器只执行两个完整生命周期；不进入三轮或正式。
原固定负载、PCM 200 ms、3 Mbps 和所有资源门保持。运行结束后：

```powershell
python -B tests/runtime/tools/product_acceptance/analyze_product_gpu_budget.py --root <新运行目录/pilot-01>
python -B -m unittest discover -s tests/runtime/selftests -p test_product_gpu_budget_evidence.py -v
```

判定器核对产品 PID、run、连续序号、每生命周期入会至导出窗口的 1 Hz 覆盖
（最大观察间隔 2 秒）和完整字段，生成单独 `gpu-budget-review.json`。
`GPU_BUDGET_SAMPLING_COMPLETE` 仅表示报告 scope 内的采样完整，不关闭 GPU queue、
WGC ownership、正式 8 小时增长／释放或音视频质量门。历史 node-0 数据仍按
旧 scope 解析；不得替代新的 all-nodes 门，
也不改变原 PILOT／外部报告中已经发生的 FAIL/DEFERRED。

完整 B14 continuation 使用 `--require-full-media-gpu` 生成正式放行门，
Formal 启动还核对 `full_media_gpu_ready=true`。预算采集完整不足以关闭
GPU queue 或实际选中 WGC 后的 ownership/release；
这些采集仍缺失时，即使三份限定 PILOT PASS，也不能生成完整正式 READY。

## 独立 GPU 释放对照

`product_gpu_budget_probe.exe <新输出文件> --release-control` 在同一进程
依次观测 device 创建前、真实 copy/fence 完成、device 释放后三阶段；各三次。
`PROCESS_NODE` 在无存活 device 时可能返回 0xC000000D，保持 UNAVAILABLE。
进程级和适配器级 raw 统计也保存；固定 NodeCount 或恢复到基线的系统内存
不能替代 packet 排空或 context 释放证明。该模式不运行产品。

`product_gpu_trace` 用任务专属 ETW session 记录 DxgKrnl device/context、
queue/DMA packet 相关 17 个事件 ID。读取 ETL 使用 ProcessTrace 归一化
FILETIME，保留原始时间戳和零丢失统计；不将 tracerpt 的显示时区当作原始时钟。
独立对照命令（输出目录必须不存在）：

```powershell
cmake --build out/build/product-gpu-budget --config RelWithDebInfo --target product_gpu_budget_probe product_gpu_trace
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests/runtime/tools/diagnostics/gpu_budget/invoke_gpu_release_control.ps1 -OutputDirectory out/new-gpu-release-control -Probe out/build/product-gpu-budget/RelWithDebInfo/product_gpu_budget_probe.exe -TraceTool out/build/product-gpu-budget/RelWithDebInfo/product_gpu_trace.exe
```

`product_acceptance/product_gpu_etw.py` 按 payload PID→device→context→packet
关联所有权；事件 header PID 不代表 GPU 工作 owner。提交／完成、device／
context 创建与销毁必须配对，抢占不清除 pending；缺完成、超时、abort、
重用、未知 schema、事件／缓冲丢失和超出 trace 的释放窗口均拒绝。
hardware-queue 450/451 的 schema 尚未接入，观测到时拒绝完整证明。

目前只验证独立短对照，真实产品接入及长时存储限制仍未完成。全机事件
流量可能很大；128 MiB 采集上限仅适用于此短对照，不得直接用于 B14
完整 PILOT 或 8 小时正式运行。独立 PASS 不计产品三轮资格。
