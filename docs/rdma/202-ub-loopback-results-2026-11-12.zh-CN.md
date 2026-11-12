# 202 同机 UB SEND 部分结果与 RNR 排查

日期：2026-11-12。证据来自用户粘贴的现场日志，远端目录 `/root/ub-loopback-RAjXduzL`；
本地尚未取得完整归档和该批环境文件，不声称已复核双方全部原始记录。

## 完成范围

| 样本类别 | 成功 | 失败 | 尚未执行 |
| --- | --- | --- | --- |
| 独立预运行 | 10 | 0 | 0 |
| 正式延迟 | 9 | 0 | 16 |
| 正式带宽 | 9 | 1 | 15 |
| 正式合计 | 18 | 1 | 31 |

`ok=28` 包含 10 对 warmup，不是 28 对正式样本。成功轮次先作为部分结果保留；
整批未达到各配置五次重复，不写 COMPLETE，也不以重复重跑抹掉已发生的失败。

## 已返回的客户端正式样本

单位：延迟 μs；带宽列保留日志数字，解释为 MiB/s 的依据见下一节。
两张表按相同大小/轮次并列整理，不表示延迟和带宽是在同一进程内测量。

| 大小 B | 轮次 | t_median | t_avg | p99 | p99.9 | BW average 原数值 | MsgRate Mpps |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 2 | 1 | 1.37 | 1.41 | 1.72 | 2.88 | 8.45 | 4.430106 |
| 2 | 2 | 1.40 | 1.42 | 1.52 | 2.91 | 8.22 | 4.309823 |
| 64 | 1 | 1.38 | 1.41 | 1.57 | 2.96 | 280.89 | 4.602052 |
| 64 | 2 | 1.36 | 1.38 | 1.46 | 2.57 | 236.61 | 3.876593 |
| 256 | 1 | 1.44 | 1.47 | 1.70 | 2.61 | 1119.92 | 4.587180 |
| 256 | 2 | 1.45 | 1.47 | 1.62 | 2.74 | 1063.94 | 4.357887 |
| 1024 | 1 | 1.45 | 1.48 | 1.81 | 2.63 | 4315.80 | 4.419377 |
| 1024 | 2 | 1.44 | 1.47 | 1.60 | 3.18 | 4248.81 | 4.350780 |
| 4096 | 1 | 1.54 | 1.55 | 1.66 | 3.31 | 16896.53 | 4.325511 |

4 KiB 第 2 轮带宽失败，客户端无数字；同大小第 2 轮延迟尚未执行。
该失败轮服务端虽有 `BW average=13645.60`，不能纳入有效带宽统计。
其他成功样本是否受相同机制影响，仍需完整日志和补测确认，不将它们称为稳定极限。

## 单位与时间窗口核对

日志表头为 `MB/sec`，但九个成功样本均满足（允许输出舍入）：

```text
BW 数值 ≈ MsgRate[Mpps] × 1000000 × bytes ÷ 1048576
有效计时秒数 ≈ iterations ÷ (MsgRate[Mpps] × 1000000) ≈ 15 秒
```

因此本批带宽按二进制 MiB/s 解释，与公开参考头文件中的单位常量一致。
4 KiB 成功轮次为 16896.53 MiB/s，约 16.50 GiB/s、141.74 Gbit/s 应用消息有效载荷吞吐。
这是同机设备路径结果，不能当作外部线缆/交换网络带宽。
`BW peak=0.00` 在 duration/no-peak 模式下不代表峰值带宽实际为零。

此前说明把 `-D 30` 写成“30 秒正式测量”不准确，现已修正。
公开参考实现将 duration 分为预热、正式计时、后段运行，正式计时约占一半；
本批九个样本的迭代数/消息率独立支持约 15 秒。
参考源码见 [计时状态机及带宽计算](https://github.com/openeuler-mirror/umdk/blob/master/src/urma/tools/urma_perftest/perftest_run_test.c)
及 [单位/时间常量](https://github.com/openeuler-mirror/umdk/blob/master/src/urma/tools/urma_perftest/perftest_parameters.h)。
公开参考不代表已核验现场安装包的完整源码。

## 第 2 轮 4 KiB 失败的证据链

客户端首次关键错误为 `Failed CR status 10`，提交数 83214528、完成数 83214400，差 128，
与输出的 JFS depth 128 相符。该差值表明有未完成发送，不足以证明 JFS 配置错误。
官方 API 的状态枚举中，10 对应 `URMA_CR_RNR_RETRY_CNT_EXC_ERR`，含义为接收方
JFR 无缓冲导致 RNR 重试超限；应再用现场安装头文件核对枚举。
参见 [openEuler 24.03 LTS SP3 URMA API 的 urma_cr_status_t](https://docs.openeuler.org/zh/docs/24.03_LTS_SP3/unifiedbus/unifiedbus/urma/URMA%20API%20Guide.ch.html)。

服务端已打印结果，进入 End test 同步；客户端退出数据循环后进入 unimport 路径，
双方收到不同阶段字符串，随后控制连接复位。现有证据更支持数据面异常后阶段分叉，
不能把最后的 errno 104 直接定为最初根因，也不能认定只是清理失败而保留该轮服务端数值。
两端日志没有统一逐行时间戳，不能从粘贴顺序严格断定两个进程事件的绝对先后。

待验证的主要假设为：

1. 持续运行中接收进度/补充 RQE 不及时，导致可用接收缓冲耗尽。
2. duration 两端独立计时或调度差异，使接收端先结束接收循环，而发送端仍发送。

第 2 项与“服务端已出结果、客户端尚有 128 笔在途”相容，仍只是推断。
目前不同时修改信用、队列深度、CQ moderation、绑核或重试次数；这些会改变实验条件。

## 下一步：只改变停止条件的诊断对照

保留旧目录。先确认本机定义和原始命令：

```bash
cat /root/ub-loopback-RAjXduzL/measured-send_bw-4096-r2-command.txt
rpm -q umdk-urma-tools umdk-urma-lib umdk-urma-devel
grep -R -n -A 22 --include=urma_opcode.h 'typedef enum urma_cr_status' /usr/include 2>/dev/null
```

下面参数对应现场输出的 RM/CTP、JFC 4096、JFR 512、JFS 128、CQ moderation 100。
将 duration 模式换成 `-n 10000000 -N`，`-N` 保持不计算 peak，与原 duration 行为一致。
固定次数不具备相同稳态时长，只用于诊断，不能替换旧 duration 样本或混合汇总。

202 root 终端 A：建立新目录，记录开始时间，启动服务端。

```bash
UB_DIAG=$(mktemp -d /root/ub-rnr-countcheck-XXXXXXXX)
printf 'UB_DIAG=%s\n' "$UB_DIAG"
date -Iseconds > "$UB_DIAG/start.txt"
set +e
timeout --signal=TERM --kill-after=5s 120s stdbuf -oL -eL \
  urma_perftest send_bw -d udmac0d1e2 --eid_idx 1 --ctp \
  -O 1 --share_jfr true --mgmt_proto tcp -p 0 -P 21320 \
  -C 4096 -R 512 -T 128 -Q 100 -s 4096 -n 10000000 -N \
  > "$UB_DIAG/server.log" 2>&1
rc=$?
printf '%s\n' "$rc" > "$UB_DIAG/server.exit"
date -Iseconds > "$UB_DIAG/server-end.txt"
cat "$UB_DIAG/server.log"
printf 'server_exit=%s\n' "$rc"
```

终端 B 将 UB_DIAG 改为 A 打印的新目录。先查看日志，确认有等待连接提示，再运行客户端。
因为 A 将输出写入文件，A 的终端不会实时显示等待提示。

```bash
UB_DIAG=/root/ub-rnr-countcheck-填写实际后缀
cat "$UB_DIAG/server.log"
```

确认等待提示后，在 B 执行：

```bash
set +e
timeout --signal=TERM --kill-after=5s 120s stdbuf -oL -eL \
  urma_perftest send_bw -d udmac0d1e2 --eid_idx 1 --ctp \
  -O 1 --share_jfr true --mgmt_proto tcp -p 0 -P 21320 \
  -C 4096 -R 512 -T 128 -Q 100 -s 4096 -n 10000000 -N \
  -S 141.61.17.202 > "$UB_DIAG/client.log" 2>&1
rc=$?
printf '%s\n' "$rc" > "$UB_DIAG/client.exit"
date -Iseconds > "$UB_DIAG/client-end.txt"
cat "$UB_DIAG/client.log"
printf 'client_exit=%s\n' "$rc"
```

两端退出后，在保留 UB_DIAG 变量的终端读回结果并保存时间范围内内核日志：

```bash
cat "$UB_DIAG/server.exit" "$UB_DIAG/client.exit"
journalctl -k --since "$(cat "$UB_DIAG/start.txt")" --no-pager > "$UB_DIAG/kernel.log"
tar -czf "$UB_DIAG.tar.gz" -C "$(dirname "$UB_DIAG")" "$(basename "$UB_DIAG")"
printf 'ARCHIVE=%s.tar.gz\n' "$UB_DIAG"
```

kernel.log 可能包含并行任务，按设备、时段和进程关联，不能把其他任务错误算作本用例。
固定次数若仍出现状态 10，说明它不只发生在 duration 的结束路径；
若成功，只说明这个较短样本通过，仍需同条件重复及较长固定次数与 duration 对照。
之后才按证据决定是否单独增加 credit 或调整调度，不把一次成功当作根因已修复。
完整五轮基线在诊断后新建独立批次，保留这次失败记录。

## 与附件的历史数据如何比较

用户附件中的旧命令为 Valkey over RDMA：Value 256 B、threads 16、连接 128、Pipeline 384、
每操作 200000 请求，目标 `192.168.17.204:6379`。最终行分别为：

| 应用操作 | 吞吐 requests/s | p50 ms |
| --- | --- | --- |
| SET | 396952.38 | 16.311 |
| GET | 397741.53 | 20.527 |

这些是应用层短压测结果；命令不含显式预热和多轮时长配置，当前附件也不足以核验
GET 的填充/命中情况。日志末尾的 27921 keys 属于另一功能服务日志，不能证明该 benchmark 的预填充。
附件不是已记录待回收的 `rdma-perf-client-szdmcbh1.tar.gz` 完整受控基线。

当前可分别报告“Valkey over RDMA 的历史应用结果”和“URMA/UB 的同机 SDK 部分结果”，
二者的协议工作、并发、Pipeline、拓扑和计时均不同，不计算性能提升倍数。
要得到组件层同口径比较，需补同机 RDMA SEND 微基准；要得到应用层同口径比较，
需原生 URMA 的等价 Valkey 客户端及功能准入，现有工具仍未具备该性能路径。
