"""Render the actual formal verdict and all observed cycles; never invent PASS."""
import argparse
from pathlib import Path
import json
import re
import statistics


def read(path): return json.loads(path.read_text(encoding="utf-8-sig"))


def render(root,gate):
    result=read(root/"external-review.json")
    plan=read(root/"plan.json")
    runner=read(root/"runner-exit.json")
    if plan["mode"]!="Formal": raise ValueError("formal_report_requires_formal_run")
    counts={k:result.get("cycle_counts",{}).get(k,0) for k in
            ("PASS","FAIL","CRASH","TIMEOUT","BLOCKED","NETWORK_FAILURE")}
    complete=(result["verdict"]=="PASS_WITH_DEFERRED" and counts["PASS"]==100 and
              result.get("duration_seconds",0)>=28800 and runner["exit_code"]==0)
    lines=["# 8 小时 / 100 次生命周期验收", "",
        "状态："+("完成（保留明确 DEFERRED）" if complete else "FAIL / 未完成；不放行"),"",
        f"Run ID：`{plan['run_id']}`。证据目录：`{root.resolve()}`。",
        f"开始：{result.get('started_utc','UNKNOWN')}；结束：{result.get('finished_utc','UNKNOWN')}。",
        f"运行秒数：{result.get('duration_seconds','UNKNOWN')}；周期统计：`{json.dumps(counts)}`。",
        "", "本机产品 --debug；UIA 控件树 + Pattern。腾讯 2 vCPU / 2 GiB / 3 Mbps；固定 10 路 160×90 / 5 fps / VP8 / 40 kbps + 1 路 24 kbps 音频，simulcast off。",
        "", "| cycle | cycle ID | native session | 结果 | revision 范围 | 缺失 | 失败检查 |",
        "|---|---|---|---|---|---|---|"]
    for c in result.get("cycles",[]):
        failed=",".join(k for k,v in c.get("checks",{}).items() if v!="PASS") or c.get("reason","")
        proof=c.get("details",{}).get("revision_1_to_terminal",{})
        if not isinstance(proof,dict): proof={}
        interval=f"{proof.get('first_revision','UNKNOWN')}–{proof.get('last_revision','UNKNOWN')}"
        lines.append(f"| {c['cycle']} | {c.get('cycle_id','UNKNOWN')} | {c.get('anonymous_session_id','UNKNOWN')} | {c['verdict']} | {interval} | {proof.get('missing_revisions','UNKNOWN')} | {failed} |")
    counters={}
    last_probe={}
    probe_path=root/"process-probe.jsonl"
    if probe_path.exists():
        for line in probe_path.open(encoding="utf-8-sig"):
            last_probe=json.loads(line)
            for group,keys in (("history",("queue_drops","pending_records_dropped","write_failures","queue_peak_jobs","queue_peak_bytes")),
                               ("diagnostic",("suppressed","benchmark_suppressed","dropped_ordinary","dropped_critical","sink_failures"))):
                for key in keys:
                    if key in last_probe.get(group,{}):
                        name=group+"."+key
                        counters[name]=max(counters.get(name,0),last_probe[group][key])
    cycle_details=[c.get("details",{}) for c in result.get("cycles",[])]
    proofs=[c["revision_1_to_terminal"] for c in cycle_details if isinstance(c.get("revision_1_to_terminal"),dict)]
    network=[c["network_capacity"] for c in cycle_details if isinstance(c.get("network_capacity"),dict)]
    audio=[c["inbound_audio_continuity"] for c in cycle_details if isinstance(c.get("inbound_audio_continuity"),dict)]
    resource_peaks={}
    resource_path=root/"external-resources.jsonl"
    if resource_path.exists():
        for line in resource_path.open(encoding="utf-8-sig"):
            row=json.loads(line)
            for key in ("private_bytes","handles","threads","gpu_dedicated_bytes","gpu_shared_bytes","cpu_pct"):
                value=row.get(key)
                if value is not None:resource_peaks[key]=max(resource_peaks.get(key,value),value)
    lines += ["", "## 证据统计", "",f"诊断事件：{result.get('diagnostic_events','UNKNOWN')}；序号缺口：{result.get('diagnostic_sequence_gaps','UNKNOWN')}。",
        f"产品退出码：{result.get('exit_code','UNKNOWN')}；Windows crash：{result.get('windows_crash_events','UNKNOWN')}。",
        f"最终检查：`{json.dumps(result.get('final_checks',{}),ensure_ascii=False)}`。",
        f"遥测与日志计数峰值（明确包含有意抑制）：`{json.dumps(counters)}`。",
        f"已严格复核的终态会话 {len(proofs)}；归档修订合计 {sum(p['last_revision'] for p in proofs)}；缺失修订合计 {sum(p['missing_revisions'] for p in proofs)}；归档 segment bytes {sum(p['archived_bytes'] for p in proofs)}。统计仅涵盖有完整 proof 的会话，缺证周期不填零。",
        f"资源峰值：`{json.dumps(resource_peaks)}`；私有内存不替代额外稳态工作集。",
        f"全服务器 NIC 观察峰值 bps：{max((p['peak_bps'] for p in network if p.get('peak_bps') is not None),default='UNKNOWN')}；超 3 Mbps 样本合计：{sum(p.get('samples_above_limit',0) for p in network) if network else 'UNKNOWN'}。",
        f"独立产品下行音频最大包间隔 ms：{max((p['maximum_gap_ms'] for p in audio if p.get('maximum_gap_ms') is not None),default='UNKNOWN')}。",
        f"释放窗口资源增长：`{json.dumps(result.get('resource_growth',{}),ensure_ascii=False)}`。", "",
        "逐周期原始值、SHA-256/大小、revision 1 到终态、丢弃与写失败、Room/SFU、共享 RTP/解码、下行 WASAPI 连续性、codec/丢包/重连及配对性能详见 external-review.json 的 cycles[].details；所有采集器原始独立证据在同一 run 目录。",
        "", "## DEFERRED / NOT_RUN", "",
        *["- "+x for x in result.get("deferred",[])], "",
        "UIA 不证明视频像素或音频质量、实际 codec/RTP、物理多屏坐标、网络故障、资源长稳、慢盘、断电或真实崩溃恢复。这里的媒体/资源结论来自对应采集器；WDDM 不等于 DXGI budget，render interval p95 不等于实时回调 p99。100 次循环覆盖 8 小时总运行，不替代单个房间连续驻留 8 小时。",
        "", "## Remaining Issues", "",
        "当前执行范围未留下可自动修复的失败项；外部条件限制见上。" if complete else result.get("reason","存在未通过的门禁；保留证据继续定位，不能宣布完成。"), "",
        "## 权限", "", "UNATTENDED_EXECUTION_AUTHORIZED。具体 Permission Requests / Granted / Denied / Blockers 见 FINAL_EXTERNAL_ACCEPTANCE.md；授权不替代测试证据。", ""]
    destination=Path(__file__).resolve().parents[2]/"docs/telemetry/logging/evidence"
    rendered="\n".join(lines)
    (destination/"FINAL_8H_100_CYCLES_REPORT.md").write_text(rendered,encoding="utf-8")
    (root/"formal-report.md").write_text(rendered,encoding="utf-8")
    external=destination/"FINAL_EXTERNAL_ACCEPTANCE.md"
    text=external.read_text(encoding="utf-8")
    text=re.sub(r"状态：\*\*[^\n]*?\*\*",
                "状态：**正式限定范围验收完成；保留 DEFERRED。**" if complete else
                "状态：**正式验收 FAIL；未完成。**", text, count=1)
    text += f"\n\n## 正式运行复盘\n\nRun `{plan['run_id']}`，verdict `{result['verdict']}`，详细结果见 [100 次明细](FINAL_8H_100_CYCLES_REPORT.md)。放行证据：`{gate.resolve()}`。\n"
    if complete: text += "\nLOGGING_EXTERNAL_ACCEPTANCE_COMPLETE\n"
    external.write_text(text,encoding="utf-8")
    (root/"external-acceptance-report.md").write_text(text,encoding="utf-8")
    index=destination.parents[2]/"README.md"
    entry="正式限定范围 8 小时/100 次完成，保留 DEFERRED" if complete else "正式运行 FAIL，未完成，保留失败证据继续定位"
    content=index.read_text(encoding="utf-8")
    content=re.sub(r"^\| 日志外部验收 \|.*$",
        "| 日志外部验收 | **"+entry+"**。最新 runtime 与边界见 "
        "[外部验收](telemetry/logging/evidence/FINAL_EXTERNAL_ACCEPTANCE.md)、"
        "[8 小时/100 次](telemetry/logging/evidence/FINAL_8H_100_CYCLES_REPORT.md)。 |",
        content, flags=re.MULTILINE)
    index.write_text(content,encoding="utf-8")
    return 0 if complete else 1


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root",type=Path,required=True);parser.add_argument("--gate",type=Path,required=True)
    args=parser.parse_args();raise SystemExit(render(args.root,args.gate))
