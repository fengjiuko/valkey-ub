# 2026-11-13 UB 独立测试进展与下一步带宽补测

## 1. 本轮结论

- **UB 延迟独立组已完成 25/25 个有效正式样本**：2、64、256、1024、4096 B，各五轮，每轮 100000 次；双端退出码均为 0。
- **1024 B 带宽独立复测通过 1/1**：3492.52 MiB/s、3.576337 Mpps，有效计时约 15 秒。
- 下一步只跑 UB 带宽五个尺寸各五轮，目标 **25 个有效正式样本**，延迟和 RDMA 暂不重跑。
- 单次带宽通过不证明此前退出 255 或历史 RNR 已修复；SDK 全矩阵稳定性和 Valkey 应用准入仍未完成。

本轮结果改变了之前“UB 延迟只有单轮”的证据状态，但不覆盖或删除之前失败记录。
本次没有修改采集脚本或 Valkey 传输实现。

## 2. 来源与核验

| 用例 | 现场目录 | 正式结果 |
| --- | --- | --- |
| UB 延迟独立五轮 | `/root/ub-loopback-L1j7FHbt` | valid=25、invalid=0、planned=25 |
| UB 1024 B 带宽单点 | `/root/ub-loopback-GCeYcttb` | valid=1、invalid=0、planned=1 |
| 历史 RDMA 基线 | `/root/rdma-send-loopback-UF9iIygF` | 五个尺寸延迟/带宽各五轮，50 个有效样本 |

本次来源为本地 `D:\valkey\output.md`，SHA256：
`f041cfdae791be2b8033613d80a525e7d75ae61ca1b5da012e5c9d967b46ce38`。

已逐条核对本次 25 个正式延迟标签、双端退出码、100000 次迭代和尺寸/轮次完整性，
并从逐轮数值重算 median、avg、p99、p99.9 的跨轮中位数及最小/最大值，与终端汇总一致。
带宽的消息率/尺寸与 MiB/s 相符，迭代数除以消息率约为 15 秒。

本次仍未回收完整归档中的环境和版本记录。旧 RDMA 来自 11 月 12 日，
新 UB 来自 11 月 13 日的回传材料；不是同时间窗交替进行的配对实验。

## 3. 本轮 UB 延迟

单位为 μs；各格为“跨轮中位数[跨轮最小值,跨轮最大值]”，不是所有请求的最小/最大值。

| 消息大小 B | 有效轮次 | median | avg | p99 | p99.9 |
| --- | --- | --- | --- | --- | --- |
| 2 | 5/5 | 1.470[1.370,1.510] | 1.500[1.400,1.540] | 1.680[1.590,1.710] | 2.930[2.630,3.180] |
| 64 | 5/5 | 1.460[1.450,1.470] | 1.500[1.480,1.500] | 1.680[1.660,1.690] | 2.860[2.620,3.290] |
| 256 | 5/5 | 1.500[1.470,1.510] | 1.530[1.510,1.550] | 1.730[1.680,1.750] | 3.050[2.700,3.390] |
| 1024 | 5/5 | 1.530[1.510,1.550] | 1.560[1.530,1.590] | 1.750[1.720,1.790] | 2.900[2.740,3.430] |
| 4096 | 5/5 | 1.690[1.560,1.720] | 1.710[1.590,1.730] | 1.900[1.790,1.940] | 3.150[3.050,3.670] |

p99/p99.9 的跨轮中位数不是合并原始样本的总体分位数。
虽然全部延迟用例通过，这个闭环发送深度为 1 的结果不证明持续带宽用例或多连接业务不会失败。

## 4. 与已有 RDMA 五轮基线的观察比较

两边均为各自五轮的指标中位数。下降比例按
`(RDMA median - UB median) / RDMA median` 计算，描述跨批次数值差异，不是同环境因果收益。

| 大小 B | RDMA median μs | 新 UB median μs | median 观察下降 | RDMA p99 μs | 新 UB p99 μs |
| --- | --- | --- | --- | --- | --- |
| 2 | 1.670 | 1.470 | 12.0% | 1.700 | 1.680 |
| 64 | 1.890 | 1.460 | 22.8% | 2.090 | 1.680 |
| 256 | 2.020 | 1.500 | 25.7% | 2.220 | 1.730 |
| 1024 | 2.230 | 1.530 | 31.4% | 2.420 | 1.750 |
| 4096 | 2.590 | 1.690 | 34.7% | 2.780 | 1.900 |

本轮 UB 五个尺寸的 median 和 p99 观察值均低于已有 RDMA 基线，
median 的观察下降为约 12.0%～34.7%。
这与旧 UB 基础组的延迟表现不同，提示需要核验跨批次的环境、版本、调度和背景负载。
目前可以报告五轮结果及其波动，不把差异全部归因于 API 或硬件，也不推算 Valkey QPS 收益。

## 5. 1024 B 带宽复测的意义

| 项目 | 数值 |
| --- | --- |
| 双端退出码 | server=0，client=0 |
| 正式迭代数 | 53645200 |
| BW average | 3492.52 MiB/s |
| 消息率 | 3.576337 Mpps |
| 推算有效计时 | 约 15 秒 |
| 有效正式重复 | 1 次 |

此前 UB 基础组在 1024 B 带宽处失败；本次同类参数单点运行成功，说明该失败没有在这一次复测中出现。
当前证据不能区分间歇性平台问题、运行负载变化或 duration 结束同步等可能原因。

历史 RDMA 1024 B 带宽五轮中位数为 5519.25 MiB/s；本次 UB 单点为其约 63.3%。
这一组仍是五轮对单轮、跨批次比较，不是完整 UB 带宽基线。
本次日志中的 peak=0.00 不用于判断峰值能力。

## 6. 下一步：只跑 UB 带宽五轮

在 202/master root 的工具仓库目录执行，确认 `utils/ub_loopback_perftest.sh`
和 `utils/send_perftest_report.py` 存在。下面参数保持本次记录的 RM/CTP、TX128、RX512、CQ100，
不同时调整信用、重试次数或绑核，不和其他打流进程并发运行。

```bash
test -r utils/ub_loopback_perftest.sh
test -r utils/send_perftest_report.py

UB_BW_LOGS=$(mktemp -d /root/ub-bw-followup-XXXXXXXX)
git rev-parse HEAD > "$UB_BW_LOGS/tool-commit.txt"

set +e
UB_DEVICE=udmac0d1e2 UB_EID=1 UB_SERVER=141.61.17.202 \
UB_OUTPUT_PARENT=/root UB_PORT=21520 UB_BW_SECONDS=30 \
SEND_PROFILE=baseline SEND_MODE=poll \
SEND_SIZES='2 64 256 1024 4096' SEND_OPS=send_bw SEND_ROUNDS=5 \
SEND_CHANNELS=1 SEND_TX_DEPTH=128 SEND_RX_DEPTH=512 SEND_CQ_MOD=100 \
  bash utils/ub_loopback_perftest.sh 2>&1 | tee "$UB_BW_LOGS/console.log"
ub_bw_rc=${PIPESTATUS[0]}
printf 'UB_BW collector_exit=%s\n' "$ub_bw_rc"

UB_BW_RUN=$(sed -n 's/^RUN=//p' "$UB_BW_LOGS/console.log")
printf 'UB_BW_RUN=%s\nCONSOLE=%s\n' "$UB_BW_RUN" "$UB_BW_LOGS/console.log"

if [[ -n $UB_BW_RUN && -d $UB_BW_RUN ]]; then
  python3 utils/send_perftest_report.py --write \
    /root/ub-loopback-L1j7FHbt "$UB_BW_RUN" \
    | tee "$UB_BW_LOGS/latency-and-bandwidth.txt"
fi
```

每次运行使用新目录。预运行为各大小一次独立用例，排除正式汇总；
正式带宽仍使用 `-D 30`，按本次工具口径预计有效计时约 15 秒，需要再次核算。
脚本会在首个错误时停止，打印错误及部分汇总，并保存归档。

完成条件为带宽汇总中 `valid=25 invalid=0 attempted=25 planned=25 unattempted=0`，
并且五个大小均显示 `5/5`。本次 1024 B 单点不拼接成新批次的第六轮，也不删掉旧失败。
如重新失败，先返回 console.log 尾部、失败标签和 ARCHIVE 路径，按原始双端日志定位，不自动扩大调参矩阵。

## 7. 后续分支

| 下一轮结果 | 后续动作 |
| --- | --- |
| 带宽 25/25 有效且计时/单位核算通过 | 回收本轮延迟与带宽归档，更新五轮对比，复核版本/环境，随后评估双机与 CPU/RSS |
| 任一大小失败 | 保留部分结果和失败，读取该对的 command、server/client log 和 exit；按时间关联必要的内核记录 |
| 退出 0 但汇总解析无效 | 先核对原始表头、单位和输出格式，不把解析失败当作性能通过 |
| 组件基础组补齐 | 按原报告的 RC/RTP/OT 或 RM/CTP 路线核验应用传输契约，不直接进入 Valkey 性能发布 |

本次只推进 SDK/组件基础组；现有 Valkey URMA 适配的 RC/RTP/OT 契约、原生性能客户端、
完整回复计时及生命周期准入仍按 [适配方案](202-rdma-ub-send-comparison-2026-11-12.zh-CN.md)执行。
