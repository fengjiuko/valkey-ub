# RDMA / UB 基础性能与终端输出

日期：2026-11-12。区分“工具具备采集能力”和“现场已有有效结果”。

## 目前已有

| 测试 | 采集能力 | 已返回的现场证据 |
| --- | --- | --- |
| UB 同机 SEND 延迟 | 各大小、重复轮次、客户端原始分布列 | 2/64/256/1024 B 各 2 轮，4096 B 1 轮；正式共 9 个成功样本 |
| UB 同机 SEND 带宽/消息率 | 单向 BW average、Mpps、错误 | 同上共 9 个成功样本；4096 B 第 2 轮状态 10、双端退出 255 |
| RDMA 同机 SEND 延迟 | 脚本已提供，100000 次/轮 | 尚未收到现场结果，不能填数 |
| RDMA 同机 SEND 带宽/消息率 | 脚本已提供，目标有效计时约 15 秒 | 尚未收到现场结果，不能填数 |
| 每轮及运行结束终端汇总 | 两脚本共用报告器，支持旧目录重读 | 软件检查不等于新增硬件结果 |
| Valkey over RDMA SET/GET | 历史应用压测记录 | SET 396952.38 requests/s、p50 16.311 ms；GET 397741.53 requests/s、p50 20.527 ms |

UB 正式样本共 18 对成功、1 对失败、31 对未执行；另有 10 对成功预运行。
旧应用压测与 SDK SEND 的协议、并发及拓扑不同，分别报告。
原生 URMA/UB 上的 Valkey 应用层性能仍未具备有效测试结果。

## 结束后自动显示什么

将以下三个文件放在 202 的同一目录，例如 `/root`：

- `ub_loopback_perftest.sh`
- `rdma_send_loopback_perftest.sh`
- `send_perftest_report.py`

依赖已安装的 Python 3 标准库，不安装第三方 Python 包。使用新版脚本时三个文件一起上传。
每对正式用例结束时打印标签、双端退出码及客户端原始结果行；运行结束或失败停止后，打印：

1. backend、传输模式、同机拓扑及已记录配置。
2. 正式样本 valid/invalid/attempted、计划数量及尚未执行数量（新目录含 plan.tsv）。
3. 每大小的延迟 median、avg、p99、p99.9，以及带宽 MiB/s、Mpps；工具没有提供的字段显示 `-`。
4. 指标的跨成功轮次 `中位数[最小值,最大值]`、有效/尝试次数、失败用例及原因。
5. 单通道带宽以迭代数/消息率推算的有效计时，供现场复核时间口径。

预运行排除；任意一端非零、退出码证据不一致、日志缺失或结果无法解析的样本不进入性能汇总。
即使失败端日志中打印了性能数字，也不作为有效结果。
报告器另外写出 `summary.txt` 和逐轮 `metrics.csv`，连同原始日志一起归档。
采集器 COMPLETE 表示打流步骤走完；还必须查看报告 valid/invalid 数量，不能只看 COMPLETE。
跨轮 p99 的中位数不是合并样本的总体 p99。

## 基础组运行方式

先核对 202 的实际 RoCE v2 GID，具体方法见 [RDMA 操作说明](202-rdma-send-loopback-perftest.zh-CN.md)。
不要把下面占位符原样执行：

```bash
RDMA_GID=填写本机实际数字
RDMA_GID="$RDMA_GID" RDMA_ROUNDS=5 bash /root/rdma_send_loopback_perftest.sh
```

RDMA 结束后，在空闲时间运行 UB，不同时打流：

```bash
SEND_PROFILE=baseline SEND_SIZES='2 64 256 1024 4096' \
SEND_OPS='send_lat send_bw' SEND_ROUNDS=5 SEND_LAT_ITERS=100000 \
SEND_CHANNELS=1 SEND_TX_DEPTH=128 SEND_RX_DEPTH=512 SEND_CQ_MOD=100 \
UB_BW_SECONDS=30 bash /root/ub_loopback_perftest.sh
```

显式固定尺寸，避免现有 UB 脚本默认尺寸中 8 B 与旧记录的 2 B 混淆。
UB 的 `-D 30` 与 RDMA 的 `-D 29 -f 7` 均需按输出核算，目标有效计时约 15 秒。
UB 4 KiB 故障尚未解决；如再次失败，保留失败及部分汇总，按 [RNR 排查](202-ub-loopback-results-2026-11-12.zh-CN.md)继续。

## 旧数据也可直接在终端重读

无需重跑就可汇总原 UB 目录：

```bash
python3 /root/send_perftest_report.py --write /root/ub-loopback-RAjXduzL
```

RDMA 运行后，将下一段路径替换为其实际打印的 RUN 目录，就能一次显示两份数据：

```bash
python3 /root/send_perftest_report.py --write \
  /root/rdma-send-loopback-填写实际后缀 \
  /root/ub-loopback-RAjXduzL
```

命令读取解压后的目录，不直接读取 tar.gz。旧目录没有 plan.tsv/settings.tsv 时，
无法证明的配置和计划数量显示 unknown/legacy，不凭文件夹名猜出完整环境。
原始带宽的 MB/MiB/Gbit 单位结合消息率核验后归一为 MiB/s，保留原单位解释。
两份报告并列显示，不自动生成提升百分比。

## 优先补什么

| 优先级 | 工作 | 验收 |
| --- | --- | --- |
| 当前基础组 | RDMA 与 UB 相同大小、单通道、inline 0、相同负载参数的 SEND 延迟和单向带宽 | 各配置 5 次有效重复，完整双方日志及错误；UB 失败如实单列，不能只挑成功轮次 |
| 当前基础组 | 确认有效窗口、单位、设备/驱动/版本和同机拓扑 | 原始日志、环境、哈希和配置齐备，不混入双机数据 |
| 当前基础组 | 定位并复验 UB 4 KiB RNR | 固定次数与 duration 对照，未证实前不宣称已解决或稳定峰值 |
| 后续补充 | 双端进程 CPU/RSS，必要时主机 irq/softirq | 与正式窗口对齐，不能由吞吐反推 CPU |
| 后续扩展 | 1/多通道、不同深度、双向带宽、不同调度、双机 | 每次单独分组并重复，基础组完成后再扩矩阵 |
| 独立测试层次 | RDMA READ/WRITE/atomic 或 Valkey SET/GET | 按实际业务需要安排；SEND 数据不替代这些操作 |

当前基础组首先回答“小消息延迟、单通道有效吞吐、重复波动及是否持续出错”。
同机回环、RC 与 RM/CTP、两套设备的差异必须保留在结论中；不据此直接归因于 API，
也不把底层 SEND 消息率叫作 Valkey QPS。
