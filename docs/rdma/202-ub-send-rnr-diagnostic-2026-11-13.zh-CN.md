# 202 UB 带宽状态 10：失败证据与固定次数诊断

日期：2026-11-13。当前阶段由补齐基础矩阵转为故障定位，暂不重复执行整组 duration 带宽。
本记录不修改 SDK、驱动或采集器，也不宣称故障已经修复。

## 1. 本次结果

现场目录：`/root/ub-loopback-pgmHKa7Q`。
终端记录：`/root/ub-bw-followup-7w1MtLJ9/console.log`。
归档：`/root/ub-loopback-pgmHKa7Q.tar.gz`。
来源：本地 `D:\valkey\output.md` 本次版本，SHA256：
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
