# 202 UB 带宽状态 10：失败证据与固定次数诊断

日期：2026-11-13。当前阶段由补齐基础矩阵转为故障定位，暂不重复执行整组 duration 带宽。
本记录不修改 SDK、驱动或采集器，也不宣称故障已经修复。

**后续结果：** 下述 64 B 固定 100000000 次对照已完成，双方退出 0。
客户端 104.48 MiB/s、1.711870 Mpps，服务端 104.48 MiB/s、1.711875 Mpps。
按客户端计数/消息率推算约 58.4 秒。当前继续执行第 5 节的批间间隔对照，
不以这一个成功样本认定 duration 问题或缓冲资源问题已经解决。

## 1. 本次结果

现场目录：`/root/ub-loopback-pgmHKa7Q`。
终端记录：`/root/ub-bw-followup-7w1MtLJ9/console.log`。
归档：`/root/ub-loopback-pgmHKa7Q.tar.gz`。
来源：本地 `D:\valkey\output.md` 的失败回传版本（随后已更新为计数模式结果），SHA256：
`ab20d6d9b091b164cf4f387b962ca8533c1b48355fc9d53d85b38e5e6c4d31bc`。

| 项目 | 证据 |
| --- | --- |
| 路径 | 202 同机，udmac0d1e2，EID 1，RM/CTP，单 Jetty，poll |
| 队列 | JFC 4096、JFS 128、JFR 512、CQ moderation 100、inline 0 |
| 独立预运行 | 2/64/256/1024/4096 B 共 5 对，双端全部退出 0 |
| 正式 2 B | 双端退出 0，53219600 次，6.77 MiB/s、3.547967 Mpps，约 15 秒有效计时 |
| 正式 64 B | 双端退出 255，客户端无有效带宽行；首个关键错误为 CR status 10 |
| 正式总体 | valid=1、invalid=1、attempted=2、planned=25、unattempted=23 |

服务端 64 B 打印了 67.25 的带宽数字，但这一对最终失败，仍须排除。
新失败不影响此前延迟组 25/25 通过的记录，也不证明应用层或持续带宽通过。

## 2. 已确认与尚未确认

客户端错误为：

```text
Failed CR status 10, tot_scnt: 41835928, tot_ccnt: 41835800.
```

按 [openEuler 24.03 LTS SP3 的 urma_cr_status_t 定义](https://docs.openeuler.org/zh/docs/24.03_LTS_SP3/unifiedbus/unifiedbus/urma/URMA%20API%20Guide.ch.html)，
状态 10 对应 `URMA_CR_RNR_RETRY_CNT_EXC_ERR`：接收端 JFR 缺少可用缓冲，RNR 重试超限。
现场安装头文件应进一步核对该枚举。
提交与完成计数相差 128，等于 JFS depth；这是尚有在途请求的证据，不能直接解释成深度配置错误。

服务端进入 End test 同步，客户端在失败后进入 unimport 路径，双方阶段字符串不一致，
随后控制连接复位。应优先定位 CR 错误及接收进度，不把最后的 errno 104 当作唯一根因。
双端没有统一的逐行时间戳，暂不能严格确定跨进程事件的绝对先后。
服务端正式窗口计数与客户端全程提交计数也不能直接相减来估算丢包。

目前仍有两种主要解释：

1. 运行中接收进度或 RQE 补充不及时，导致接收资源阶段性耗尽。
2. duration 两端的结束时序发生偏差，接收循环已停止而发送端仍在提交，继而触发 RNR。

公开 [perftest 参考实现](https://github.com/openeuler-mirror/umdk/blob/master/src/urma/tools/urma_perftest/perftest_run_test.c)
包含定时状态切换及按 END_STATE 结束收发循环的逻辑，因此第二项值得对照；
它仍是假设，参考主线源码也不能替代现场安装包版本的核验。

## 3. 最小对照：64 B、固定次数

目标是测试失败是否依赖 duration 的结束机制。保持 RM/CTP、单 Jetty、优先级、队列和 CQ 参数，
将 `-D 30` 替换为 `-n 100000000 -N`。`-N` 继续关闭 peak 计算，与原 duration 模式一致。
新 TCP 控制端口为 21640，两端保持一致。

固定次数不是修复措施，其性能数字与 duration 基线分组保存。
100000000 次在公开参考头文件的允许范围内；仍以现场版本接受的参数为准。
参见 [迭代范围定义](https://github.com/openeuler-mirror/umdk/blob/master/src/urma/tools/urma_perftest/perftest_parameters.h)。

### 3.1 终端 A：收集已有失败命令和版本

在 202 的 root 终端：

```bash
cd /home/h30086230/valkey-ub
cat /root/ub-loopback-pgmHKa7Q/measured-send_bw-64-r1-command.txt
cat /root/ub-loopback-pgmHKa7Q/environment.log
cat /root/ub-loopback-pgmHKa7Q/tool.sha256
grep -R -n -A 22 --include=urma_opcode.h 'typedef enum urma_cr_status' /usr/include 2>/dev/null
```

随后启动固定次数服务端：

```bash
UB_DIAG_SERVER_LOG=$(mktemp /root/ub-rnr64-server-XXXXXXXX.log)
date -Iseconds
set +e
timeout --signal=TERM --kill-after=5s 180s stdbuf -oL -eL \
  urma_perftest send_bw -d udmac0d1e2 --eid_idx 1 --ctp \
  -O 1 --share_jfr true --mgmt_proto tcp -p 0 -P 21640 \
  -s 64 -I 0 -R 512 -J 1 -T 128 -C 4096 -Q 100 \
  -n 100000000 -N 2>&1 | tee "$UB_DIAG_SERVER_LOG"
server_rc=${PIPESTATUS[0]}
printf 'server_exit=%s\nSERVER_LOG=%s\n' "$server_rc" "$UB_DIAG_SERVER_LOG"
printf '%s\n' "$server_rc" > "$UB_DIAG_SERVER_LOG.exit"
date -Iseconds
```

### 3.2 终端 B：看到服务端等待连接后启动

```bash
cd /home/h30086230/valkey-ub
UB_DIAG_CLIENT_LOG=$(mktemp /root/ub-rnr64-client-XXXXXXXX.log)
date -Iseconds
set +e
timeout --signal=TERM --kill-after=5s 180s stdbuf -oL -eL \
  urma_perftest send_bw -d udmac0d1e2 --eid_idx 1 --ctp \
  -O 1 --share_jfr true --mgmt_proto tcp -p 0 -P 21640 \
  -s 64 -I 0 -R 512 -J 1 -T 128 -C 4096 -Q 100 \
  -n 100000000 -N -S 141.61.17.202 2>&1 | tee "$UB_DIAG_CLIENT_LOG"
client_rc=${PIPESTATUS[0]}
printf 'client_exit=%s\nCLIENT_LOG=%s\n' "$client_rc" "$UB_DIAG_CLIENT_LOG"
printf '%s\n' "$client_rc" > "$UB_DIAG_CLIENT_LOG.exit"
date -Iseconds
```

不要把服务端和客户端在同一前台终端顺序执行。超时 180 秒时按超时记录，不当作完成。
运行时不同时调整队列、credit、重试次数或绑核，也不与其他压测并发。

## 4. 如何解释下一次结果

| 对照结果 | 能支持的判断 | 后续 |
| --- | --- | --- |
| 双端退出 0，固定计数完成 | 此次长计数样本通过，duration 结束机制值得重点复核 | 重复同一计数用例，并与同参数 duration 对照；仍不能单次定根因 |
| 仍出现 CR status 10 | 错误不只发生在 duration 的结束路径 | 检查接收补充、调度、设备/provider 及错误时内核记录；按单变量定位 |
| 固定次数超时或其他错误 | 无法判为成功 | 保留双方完整日志、时间和状态，继续按实际错误定位 |

请返回两个终端的完整输出、server/client_exit 及日志路径，并保留旧失败归档。
在这些证据到位前不再次跑完整五轮矩阵，不根据服务端提前打印的数据填补失败样本。

## 5. 固定次数通过后的分批对照

### 5.1 证据更新与假设

固定次数成功记录来自工作区 output.md 的后续版本，SHA256：
`4eaeafd2da6f14d909d3f2cfe48dbd42317929e9922e7bac5bc9e8724d4c675f`。
双方各完成 100000000 次迭代，客户端按消息率推算运行约 58.4 秒；
它比此前 duration 用例的总运行时间参数更长，累计发送量也更多。

这削弱了“只要持续时间更长或累计量更多就必然耗尽固定总量缓冲”的解释，
但没有排除运行中瞬时接收不足、跨进程资源回收延迟，或 duration 的结束时序问题。
两次试验的间隔、背景负载和停止机制都可能影响结果，尚不能从一次通过确定根因。

现有采集器每一对用例都会启动新服务端/客户端进程并 wait 双方结束，
不是把所有尺寸放在一个长期复用的进程里。进程退出后 provider/驱动资源是否及时回收，
仍需配套设备计数和日志核验；主机空闲内存不能直接证明 JFR/RQE 是否充足。
RX depth=512 是接收队列容量，不是整场只能接收 512 条消息，运行中需要持续补充接收请求。

因此分批是合理的定位手段，先比较“有无额外间隔”，不直接把加等待当作修复。

### 5.2 两组只改变批间额外等待

- 两组都只测 64 B，使用原 duration 30 参数、TX128/RX512/CQ100、单 Jetty、inline 0。
- 每批调用一次现有采集器，正式轮次为 1；因此每批具有相同的独立预运行、初始化和归档过程。
- 安排三轮配对试验，额外等待分别为 0 秒和 10 秒，配对顺序交替，以减少固定执行先后的影响。
- “0 秒”只表示不额外等待，仍包括进程初始化、报告和归档开销；它并非原整组测试的完全复现。
- 总计最多 6 个正式诊断样本。所有失败保留，不重试凑成功数，不与原五轮基线合并。

在 202/root 的工具仓库目录执行以下一段；不需要再开第二个终端：

```bash
cd /home/h30086230/valkey-ub
UB_BATCH_ROOT=$(mktemp -d /root/ub-batch-gap-XXXXXXXX)
printf 'trial\tgap_seconds\tcontrol_port\tcollector_exit\trun\n' > "$UB_BATCH_ROOT/batches.tsv"
set +e

for trial in 1 2 3; do
  if (( trial % 2 )); then gaps=(0 10); else gaps=(10 0); fi
  for gap in "${gaps[@]}"; do
    log="$UB_BATCH_ROOT/trial-${trial}-gap-${gap}.log"
    port=$((21720 + 20 * (trial - 1) + gap))
    printf 'BATCH trial=%s gap_seconds=%s\n' "$trial" "$gap"
    sleep "$gap"
    UB_DEVICE=udmac0d1e2 UB_EID=1 UB_SERVER=141.61.17.202 \
    UB_OUTPUT_PARENT=/root UB_PORT="$port" UB_BW_SECONDS=30 \
    SEND_PROFILE=baseline SEND_MODE=poll \
    SEND_SIZES=64 SEND_OPS=send_bw SEND_ROUNDS=1 \
    SEND_CHANNELS=1 SEND_TX_DEPTH=128 SEND_RX_DEPTH=512 SEND_CQ_MOD=100 \
      bash utils/ub_loopback_perftest.sh 2>&1 | tee "$log"
    rc=${PIPESTATUS[0]}
    run=$(sed -n 's/^RUN=//p' "$log")
    printf '%s\t%s\t%s\t%s\t%s\n' "$trial" "$gap" "$port" "$rc" "$run" >> "$UB_BATCH_ROOT/batches.tsv"
  done
done

cat "$UB_BATCH_ROOT/batches.tsv"
printf 'BATCH_ROOT=%s\n' "$UB_BATCH_ROOT"
```

每个采集器在失败时仍会停止自身用例并归档；外层循环按预定计划启动下一批独立试验。
这用于比较重复结果，不是自动重试并抹去失败。批次使用新目录，保持旧档案。
每批使用独立的起始控制端口，避免端口复用造成的控制面错误干扰判断；
若仍出现 bind 或资源创建错误，作为该批实际失败单列。
两组可能仍有背景负载及时间顺序影响，三次也不是严格显著性检验。

### 5.3 结果判读

| 观察 | 支持的下一步 |
| --- | --- |
| 有间隔的批次明显更容易通过 | 继续核验资源回收/调度、背景负载和队列计数；支持采用间隔的工作假设，不能单凭此证明缓冲泄漏 |
| 两组 duration 都失败，而固定次数重复通过 | 进一步聚焦 duration 的停止/同步机制，匹配现场工具版本复核实现 |
| 两组 duration 都通过 | 说明本次未复现失败；保留历史失败，后续再测试多尺寸连续切换及更长观察窗口 |
| 计数模式后续也报状态 10 | 转向运行中接收补充、provider/驱动和负载压力，停止把原因仅限于 duration |

请返回完整终端输出及 batches.tsv。延迟 25/25 已完成，当前不必随分批诊断重跑。
