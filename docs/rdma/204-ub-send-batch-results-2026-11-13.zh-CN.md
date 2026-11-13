# 204 UB SEND：批间等待对照结果与下一步

日期：2026-11-13。202 暂不可用，用户更新代码后在 204/worker1 执行。
本记录仅统计 204 同机 SDK SEND，不合并为 202 基线，也不代表 Valkey 应用性能。

## 1. 来源、配置和完整性

来源为本地 `D:\valkey\output.md` 本轮回传，SHA256：
`5ede7ec14707f9025295d8e06627167c37c815fc32e60b32cff56ed65d1ca7b2`。
批次目录为 `/home/h30086230/ub-batch-gap-KXq4k1SG`。
本次已返回完整六批终端记录及 batches.tsv；尚未回收各归档的环境、工具哈希和完整日志。
用户报告已经拉取代码，但回传未打印提交 SHA，不据此断言现场与仓库 HEAD 完全一致。
204 本轮以 h30086230 运行，202 旧记录以 root 运行；主机、用户权限、背景负载及版本差异均需保留。
设备名称相同也不能证明两机硬件和 provider 配置相同。

配置：`udmac0d1e2`、EID 1、管理地址 `141.61.17.204`、RM/CTP、poll、64 B、
单 Jetty、JFS 128、JFR 512、JFC 4096、CQ moderation 100、inline 0、`-D 30`。
每批先独立预运行一次，再正式运行一次；额外等待在整批之前，预运行和正式运行之间没有新增 10 秒等待。
因此本实验不能检验“每对打流之间都空闲 10 秒”的效果。

## 2. 六批结果

各 RUN 目录均位于 `/home/h30086230`，表中省略该前缀。
退出码为正式 server/client；失败行不参与性能汇总。

| trial | 额外等待秒 | 控制起始端口 | RUN 目录 | 双端退出 | MiB/s | Mpps |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 0 | 21720 | ub-loopback-xsmDAkQL | 0/0 | 220.44 | 3.611614 |
| 1 | 10 | 21730 | ub-loopback-ovycpx8p | 0/0 | 280.17 | 4.590280 |
| 2 | 10 | 21750 | ub-loopback-STScMPL0 | 255/255 | 排除 | 排除 |
| 2 | 0 | 21740 | ub-loopback-Yz1gw4LL | 0/0 | 221.07 | 3.621965 |
| 3 | 0 | 21760 | ub-loopback-Re7fpllF | 0/0 | 270.44 | 4.430937 |
| 3 | 10 | 21770 | ub-loopback-ROG06yu8 | 0/0 | 263.75 | 4.321284 |

独立预运行六对均为 0/0；正式共 6 次尝试，5 有效、1 无效，没有未执行批次。
外层失败批 collector_exit=1，与内部双端退出 255 是不同层次的状态。

| 分组 | 正式有效/尝试 | 成功轮带宽中位数[最小,最大] MiB/s | 成功轮 Mpps 中位数 |
| --- | --- | --- | --- |
| 0 秒 | 3/3 | 221.07[220.44,270.44] | 3.621965 |
| 10 秒 | 2/3 | 271.96[263.75,280.17] | 4.455782 |

五个成功样本的计数/消息率均对应约 15 秒有效测量窗口。
原始表头写 MB/sec；按 64 B 与 Mpps 换算，数值符合 MiB/s，沿用统一报告单位。
10 秒组性能只含成功样本，且该组存在失败；不能据此宣称等待提升吞吐或稳定性。
三次试验不足以估计长期故障率，0 秒组全过也不证明其长期更可靠。

## 3. 失败证据与结论

`ub-loopback-STScMPL0` 客户端：

```text
Failed CR status 10, tot_scnt: 15099028, tot_ccnt: 15098900.
```

服务端正式行迭代数为 0，随后报 `End test != unimport`、控制连接复位和 unimport 同步失败。
该 0 行属于失败样本，不能作为零吞吐的有效性能点，也不能仅凭它确定故障发生时间。
发送提交与完成相差 128，等于发送队列深度；不据此推断丢失消息数量或队列配置错误。

状态 10 的既有枚举解释及两种主要假设见
[202 诊断记录](202-ub-send-rnr-diagnostic-2026-11-13.zh-CN.md#2-已确认与尚未确认)。
204 现场头文件与安装包版本仍需核对。缺少双端统一时间线，不能断言接收停止与 CR 错误的跨进程先后。

本次支持以下判断：

- 相同错误现象在另一主机 204 上复现，调查范围不能只限于 202；不证明两机根因完全相同。
- 整批之前额外等待 10 秒未消除故障，不支持把这项等待直接作为修复方案。
- 仍不能区分运行中接收进度不足、duration 停止时序，以及环境/provider 等因素。
- 202 的两次固定 1 亿次成功与 204 的 duration 结果跨主机，不能构成只改变停止模式的对照。

## 4. 下一步：先回收现场，再做 204 固定次数对照

### 4.1 同一终端回收现有证据，无需重跑

使用本轮同一普通用户；所有输出同时保存在新证据目录中：

```bash
cd /home/h30086230/valkey-ub || exit 1
UB204_EVIDENCE=$(mktemp -d "$HOME/ub204-evidence-XXXXXXXX")
{
  hostname; id; date -Iseconds; uname -a
  git rev-parse HEAD
  git status --short
  cat "$HOME/ub-batch-gap-KXq4k1SG/batches.tsv"
  for name in xsmDAkQL ovycpx8p STScMPL0 Yz1gw4LL Re7fpllF ROG06yu8; do
    run="$HOME/ub-loopback-$name"
    printf '\nRUN=%s\n' "$run"
    cat "$run/environment.log" "$run/tool.sha256"
    cat "$run/measured-send_bw-64-r1-command.txt"
    cat "$run/results.tsv"
  done
  for side in server client; do
    printf '\nFAILED_SIDE=%s\n' "$side"
    cat "$HOME/ub-loopback-STScMPL0/measured-send_bw-64-r1-$side.log"
    cat "$HOME/ub-loopback-STScMPL0/measured-send_bw-64-r1-$side.exit"
  done
  grep -R -n -A 24 --include=urma_opcode.h \
    'typedef enum urma_cr_status' /usr/include 2>/dev/null || true
} 2>&1 | tee "$UB204_EVIDENCE/existing-evidence.txt"
printf 'EVIDENCE=%s\n' "$UB204_EVIDENCE"
```

既有归档也应保留。内核/provider 日志需按 environment.log 中本轮时间窗口由有权限的账户回收；
当前用户若无权限，不能把空日志解释为没有驱动错误。

### 4.2 204 终端 A：固定次数服务端

保持上述队列、模式、尺寸和用户身份，只将 duration 改为 `-n 100000000 -N`。
先做一对，双方成功后再安排预定重复；不要与其他压测并发。
本次没有增加独立预运行，和 duration 批次的预运行历史也需记录，不能只凭单次结果定根因。

```bash
UB204_COUNT=$(mktemp -d "$HOME/ub204-count64-XXXXXXXX")
printf 'COUNT_RUN=%s\n' "$UB204_COUNT"
date -Iseconds > "$UB204_COUNT/start.txt"
set +e
timeout --signal=TERM --kill-after=5s 180s stdbuf -oL -eL \
  urma_perftest send_bw -d udmac0d1e2 --eid_idx 1 --ctp \
  -O 1 --share_jfr true --mgmt_proto tcp -p 0 -P 21840 \
  -s 64 -I 0 -R 512 -J 1 -T 128 -C 4096 -Q 100 \
  -n 100000000 -N 2>&1 | tee "$UB204_COUNT/server.log"
server_rc=${PIPESTATUS[0]}
printf '%s\n' "$server_rc" > "$UB204_COUNT/server.exit"
printf 'server_exit=%s\nCOUNT_RUN=%s\n' "$server_rc" "$UB204_COUNT"
date -Iseconds
```

### 4.3 204 终端 B：看到服务端等待后启动客户端

将第一行替换为终端 A 打印的完整 COUNT_RUN 路径，两个终端用同一用户和目录。

```bash
UB204_COUNT='填写终端A打印的COUNT_RUN完整路径'
test -d "$UB204_COUNT" || { printf '请先填写实际 COUNT_RUN 路径\n'; exit 1; }
set +e
timeout --signal=TERM --kill-after=5s 180s stdbuf -oL -eL \
  urma_perftest send_bw -d udmac0d1e2 --eid_idx 1 --ctp \
  -O 1 --share_jfr true --mgmt_proto tcp -p 0 -P 21840 \
  -s 64 -I 0 -R 512 -J 1 -T 128 -C 4096 -Q 100 \
  -n 100000000 -N -S 141.61.17.204 2>&1 | tee "$UB204_COUNT/client.log"
client_rc=${PIPESTATUS[0]}
printf '%s\n' "$client_rc" > "$UB204_COUNT/client.exit"
printf 'client_exit=%s\nCOUNT_RUN=%s\n' "$client_rc" "$UB204_COUNT"
date -Iseconds
```

需要返回现有证据块、两个计数终端完整输出及退出码。超时或非零均保留为失败，
不把重试成功替换原失败；固定次数与 duration 分组报告。
若计数也报状态 10，说明问题不局限于 duration；若通过，下一步预定重复并核验同版本 duration 实现，
尚不能凭一对通过确认停止机制就是根因。

## 5. 当前 UB 数据边界

202 已有五尺寸延迟各五轮（25/25）以及独立 1024 B duration 带宽单点；
还有 64 B 固定 1 亿次成功记录和完整矩阵中保留的失败。
204 新增本记录的 64 B duration 六批诊断（5/6 有效）。
204 尚未补齐自身五尺寸延迟和带宽五轮矩阵、同机 RDMA 对照及 Valkey 原生 UB 性能。
若后续报告以 204 为正式比较环境，需在其上分别建立 RDMA/UB 基线；
不能将 202 延迟、204 带宽和 202 RDMA 拼成同一主机的完整比较结果。
