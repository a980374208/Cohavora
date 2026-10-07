# 独立麦克风音源旁证

此工程不修改产品或主工程构建。通过 shared WASAPI 采样产品默认录音角色
`eConsole`（不可用才回退 `eMultimedia`），也支持显式录音端点。
PCM 仅在内存处理，每秒输出能量、100 ms 区块、997 Hz 匹配、设备位置和收包时序；
不保存人声、PCM 或设备原始 ID，不更改默认设备、静音和音量。

```powershell
cmake -S tests/runtime/tools/media/microphone_input -B out/build/product-microphone-input -G "Visual Studio 18 2026" -A x64
cmake --build out/build/product-microphone-input --config RelWithDebInfo
ctest --test-dir out/build/product-microphone-input -C RelWithDebInfo --output-on-failure
python -B tests/runtime/tools/media/microphone_input/probe_microphone_input.py --executable out/build/product-microphone-input/RelWithDebInfo/product_microphone_input.exe --output out/microphone-new-baseline --mode baseline --seconds 45
```

`baseline` 仅观察连续非静音条件，不能证明已知测试音源；`coupled` 同步向默认
输出提交低幅度 997 Hz 信号并核对麦克风能否实际接收，输出静音或 0 音量时明确失败。
`external-tone` 用于手机/独立扬声器持续播放 997 Hz 的真实物理输入。
已有证据目录拒绝覆盖。判定条件在启动前写入并冻结 `conditions.json`：
稳态至少 30 秒，每个 100 ms 区块 AC RMS ≥0.005、峰值 <0.95；已知源还需
997 Hz 能量比例 ≥0.30；帧覆盖率 ≥98%，包间隔 ≤200 ms，设备位置无缺失，
稳态无 discontinuity/timestamp error。采集起始 2 秒保留在证据中，另标为稳定等待。

```powershell
python -B tests/runtime/tools/media/microphone_input/probe_microphone_input.py --executable out/build/product-microphone-input/RelWithDebInfo/product_microphone_input.exe --output out/microphone-new-external --mode external-tone --seconds 45
```

物理测试音源必须从预检持续到受控产品对照结束，并保持同一端点；不得仅凭提交
播放缓冲或低于门限的环境噪声认定输入有效。独立原始录音旁证不证明产品
AEC/ANS/AGC 后的信号、实际协商 DTX、RTP 或远端 PCM 连续性。
所有结果 `diagnostic_only=true`、`release_eligible=false`、资格积分 0。
音源确认后才可使用原 B14 固定媒体负载/200 ms/3 Mbps 门做独立受控诊断。

`microphone_signal_metrics` 使用确定性合成数据验证静音、DC、噪声、低能量、
错误频率、削波及三种采样率，不能替代真实设备采集。
