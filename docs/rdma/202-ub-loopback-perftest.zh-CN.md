# 202 同机 UB 组件补测命令

日期：2026-11-12。按用户要求先补 202 同机测试。
用户已返回一批部分完成的现场结果，见 [样本与失败分析](202-ub-loopback-results-2026-11-12.zh-CN.md)。
本轮范围为 URMA 工具访问 UB 设备的 SDK 微基准，尚未完成五轮验收。

## 已有现场数据

用户给出的成对命令：

```bash
# master 终端 A
urma_perftest send_lat -d udmac0d1e2 --eid_idx 1 --ctp
# master 终端 B
urma_perftest send_lat -d udmac0d1e2 --eid_idx 1 --ctp -S 141.61.17.202
```

两端提示符均为 master，目标地址属于 202，按同机结果记录；仅从日志不能推断物理流量是否经过外部交换网络。
内核为 `6.6.0-145.3.34.165.oe2403sp3.aarch64`，与此前诊断内核不同；本轮 SDK、驱动、固件身份需重新采集。

| 端 | 大小 | 迭代 | min μs | max μs | median μs | avg μs | stdev μs | p99 μs | p99.9 μs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 服务端 | 2 B | 10000 | 2.29 | 9.24 | 2.33 | 2.34 | 0.16 | 2.48 | 6.22 |
| 客户端 | 2 B | 10000 | 2.29 | 10.09 | 2.33 | 2.34 | 0.17 | 2.41 | 6.48 |

配置输出为 UB / URMA_TM_RM / DUPLEX / 1 Jetty，inline 为 0。
两份报告属于一次成对试验，不是两次独立重复。原日志 p99.99/p99.999 已接近或等于最大值；
10000 个迭代不足以稳定解释极高分位，不据此报告稳定的 p99.999。
延迟先保留工具原始定义及单位，不擅自乘二或除二。

这次证明 RM/CTP/SEND 路径能收发。此前 [RC 建链失败](202-ub-platform-blockers.zh-CN.md)
和 Valkey 的 RC/RTP/OT 契约仍需独立复验。

## 先跑一组 256 B 的显式配置

先查看现场帮助：

```bash
urma_perftest send_lat -h
urma_perftest send_bw -h
```

核对 `-O/--priority`、`--share_jfr`、`--mgmt_proto`、`-p/--trans_mode`、`-P/--port`、
`-s/--size`、`-n/--iters` 和 `-D/--duration`。帮助命令的退出码不代表设备可用性。
`-O 1` 是优先级，`-p 0` 是 RM；大小写含义不同。共享 JFR 与共享 TP 是不同字段。
如现场帮助不匹配，保留帮助并调整命令，不能照搬其他版本的选项。

202 的 root 终端 A 先启动服务端：

```bash
urma_perftest send_lat -d udmac0d1e2 --eid_idx 1 --ctp \
  -O 1 --share_jfr true --mgmt_proto tcp -p 0 -P 21220 -s 256 -n 100000
```

看到 `Waiting for client to connect` 后，202 的 root 终端 B：

```bash
urma_perftest send_lat -d udmac0d1e2 --eid_idx 1 --ctp \
  -O 1 --share_jfr true --mgmt_proto tcp -p 0 -P 21220 -s 256 -n 100000 \
  -S 141.61.17.202
```

两端退出后，终端 A 跑带宽服务端：

```bash
urma_perftest send_bw -d udmac0d1e2 --eid_idx 1 --ctp \
  -O 1 --share_jfr true --mgmt_proto tcp -p 0 -P 21221 -s 256 -D 30
```

看到等待连接后，终端 B：

```bash
urma_perftest send_bw -d udmac0d1e2 --eid_idx 1 --ctp \
  -O 1 --share_jfr true --mgmt_proto tcp -p 0 -P 21221 -s 256 -D 30 \
  -S 141.61.17.202
```

两端参数保持一致。带宽用 `-D 30`，不同时传 `-n`。
显式优先级/JFR 配置作为新配置记录，不把旧的自动修正参数当作已验证与新配置完全一致。

## 批量补测并归档

将 [`ub_loopback_perftest.sh`](../../utils/ub_loopback_perftest.sh) 放到 202，例如
`/root/ub_loopback_perftest.sh`，将 [`send_perftest_report.py`](../../utils/send_perftest_report.py)
放到同一目录。只需一个 root 终端；脚本在同机顺序启动服务端和客户端。
为延续旧数据的 2 B 尺寸，本组显式指定大小：

```bash
SEND_SIZES='2 64 256 1024 4096' bash /root/ub_loopback_perftest.sh
```

依赖 Bash、coreutils、tar、Python 3 标准库和已安装的 `urma_perftest`，不安装软件或修改系统配置。
默认端口从 21220 开始，每对进程使用不同端口；与手动命令分开运行，避免端口占用。
本轮设置如下：

| 项目 | 设置 |
| --- | --- |
| 设备 / EID / 客户端目标 | udmac0d1e2 / 1 / 141.61.17.202 |
| 传输 | RM / CTP / SEND / 共享 JFR / 优先级 1 |
| 大小 | 2、64、256、1024、4096 B |
| 延迟 | 每大小每轮 100000 次，5 轮 |
| 带宽 | 每大小每轮 `-D 30`，5 轮，单向；本批成功样本有效计时约 15 秒 |
| 预运行 | 每大小先独立运行 10000 次延迟及 5 秒带宽，单独标为 warmup，排除汇总 |
| 调度 | 默认调度，不绑核；保留单 Jetty 和工具默认队列深度/轮询方式 |
| 故障处理 | 就绪等待最多 20 秒，每进程 120 秒超时，失败停止并保留证据 |

25 对正式带宽用例的 `-D` 参数之和为 750 秒，不代表 750 秒正式计时。
本批按迭代数/消息率核算，每对成功带宽用例有效计时约 15 秒；按此口径完成 25 对为约 375 秒正式计时。
参考源码以 duration 的约四分之一预热、二分之一计时、四分之一后段运行；整数秒和调度会影响总时长。
整批约需十几分钟，包含预运行、非计时阶段、延迟和初始化开销。
独立预运行不等于原 Valkey 基线的进程内 5 秒预热，本组不声称两者测量窗口等价。
当前公开 UMDK 源码对 CTP/SEND 的消息上限为 4096 B，因此本轮不加入 64 KiB；
最终限制以现场版本帮助、错误及配套源码为准。

脚本打印 `RUN=...` 和 `ARCHIVE=...tar.gz`。每轮保存完整双方输出、实际命令、退出码，
另存环境、工具/脚本 SHA256、帮助和 `results.tsv`。`results.tsv` 是执行索引，性能数字保留在原始日志。
新版每轮打印原始结果，结束或失败时显示终端汇总，另存 summary.txt 和 metrics.csv。
RDMA/UB 联合查看及基础测试缺项见 [终端输出说明](send-basic-performance-status.zh-CN.md)。
仅所有正式轮次双端退出 0 且客户端有结果行时写 `COMPLETE`；失败有 `FAILED.txt`。
`COMPLETE` 表示打流流程完成，不代替载荷逐字节校验、顺序验证或 Valkey 功能验收。
脚本退出时只清理它自己启动的进程；失败后按打印路径保留归档。

可回传本轮 `ub-loopback-*.tar.gz`，连同旧结果一起分析。不要把 warmup 样本或失败轮次合入性能汇总。
比较先固定客户端为主报告端，服务端用于核验；各配置报告 5 轮的指标中位数及最小/最大值。
各轮 p99 的中位数不是合并样本的总体 p99。
带宽保留工具原始表头；本批 `MB/sec` 数值经消息率核算与 MiB/s 一致，见结果分析。
换算 Gbit/s 时注明单位依据；消息率不等于 Valkey QPS。
脚本不采集稳态 CPU 百分比，因此后续不能据此补造双端 CPU 成本。

## 后续对比口径

UMDK 是软件开发套件，UB 是互联体系；应比较明确的应用/工具路径和配置。
收到旧数据后，先核对测试层次、同机/双机、API、操作、尺寸、队列、迭代/时间、单位、
内核/SDK/驱动和调度，再建立匹配行。若旧数据来自 Valkey SET/GET，和这组 SDK SEND 数据分表展示。
现有 Valkey benchmark 尚无原生 `--urma`；本轮不产生应用级 UB QPS 或加速比例。

参数参考：[UMDK 官方镜像的工具参数源码](https://github.com/openeuler-mirror/umdk/blob/master/src/urma/tools/urma_perftest/perftest_parameters.c)。
这是公开参考，未声称与现场安装包一致。

## 交付前本地检查

2026-11-12：Git Bash 语法与帮助入口检查通过。用模拟 perftest 完成单轮矩阵
（10 对预运行、10 对正式运行），检查双方日志、索引及归档；注入客户端退出 7
和“退出 0 但无结果行”，均返回失败并保留 FAILED，未写 COMPLETE。
本地模拟替换了 stdbuf 包装，不验证 Linux provider、真实数据收发、带宽持续时间或性能数值。
