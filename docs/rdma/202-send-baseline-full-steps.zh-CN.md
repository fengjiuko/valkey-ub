# 202 同机 RDMA / UB SEND：拉取、打流、终端回传完整步骤

适用日期：2026-11-12。使用 202/master 的 root 终端，不需要启动或重编译 Valkey。
本组为组件层基础性能：单通道、inline 0、2/64/256/1024/4096 B，延迟和单向带宽各五轮。
两后端顺序测试，不同时打流。现场结果尚未补齐，不预先宣称稳定性或收益。

## 1. 终端 A：从用户仓库取得新工具

新建独立目录，不覆盖已有源码、旧结果或其他用户的仓库：

```bash
set -euo pipefail
umask 077
SEND_TOOLS=$(mktemp -d /root/valkey-send-tools-XXXXXXXX)
git clone --single-branch --branch work/rdma-perf-baseline \
  https://github.com/fengjiuko/valkey-ub.git "$SEND_TOOLS"
cd "$SEND_TOOLS"
git rev-parse HEAD
SEND_LOGS=$(mktemp -d /root/send-session-XXXXXXXX)
git rev-parse HEAD > "$SEND_LOGS/tool-commit.txt"
printf 'SEND_TOOLS=%s\nSEND_LOGS=%s\n' "$SEND_TOOLS" "$SEND_LOGS"
test -r utils/ub_loopback_perftest.sh
test -r utils/rdma_send_loopback_perftest.sh
test -r utils/send_perftest_report.py
python3 -m unittest discover -s tests/rdma -p test_send_perftest_report.py -v
```

`git rev-parse HEAD` 应与本次提供的远端提交相符；若后来分支又有更新，应记录实际提交。
三个工具已在同一 utils 目录，不用再手动复制到 /root。下面继续使用终端 A 的变量。

## 2. 终端 A：核对环境和工具选项

```bash
{
  hostname
  date -Iseconds
  uname -a
  id
  ulimit -l
  python3 --version
  command -v urma_perftest ib_send_lat ib_send_bw timeout stdbuf
  rpm -q umdk-urma-tools umdk-urma-lib umdk-urma-devel perftest libibverbs
  ip -br address
  ibv_devinfo -d rocep40s0f0 -v
} 2>&1 | tee "$SEND_LOGS/preflight.log"

# 部分版本的帮助命令返回非零；保留帮助，不以该返回码验收硬件。
urma_perftest send_lat -h > "$SEND_LOGS/urma-lat-help.log" 2>&1 || true
urma_perftest send_bw -h > "$SEND_LOGS/urma-bw-help.log" 2>&1 || true
ib_send_lat --help > "$SEND_LOGS/rdma-lat-help.log" 2>&1 || true
ib_send_bw --help > "$SEND_LOGS/rdma-bw-help.log" 2>&1 || true
cat "$SEND_LOGS/urma-lat-help.log"
cat "$SEND_LOGS/urma-bw-help.log"
```

继续条件：当前仍为 202/master，存在 `udmac0d1e2` 的可用 EID 1，以及
`rocep40s0f0/1`、本机 IPv4 `192.168.17.202`；RoCE 端口为 ACTIVE。
缺工具、包版本/设备变化时先保留并处理该项。此流程不安装软件、改网卡或停止已有服务。

URMA 参数必须按本机帮助核对：`-O 1` 为优先级，`-p 0` 为 RM，`-P` 为 TCP 控制端口。
本组保留已跑通的 CTP 和 EID 1，不换成此前失败的 RC/RTP/OT 组合，不额外添加 tp_aware。
`--share_jfr true` 为共享接收队列，与共享 TP 不是同一个字段。

## 3. 两终端：先复验 URMA 最小收发

原来成功的核心命令是：

```bash
# 服务端
urma_perftest send_lat -d udmac0d1e2 --eid_idx 1 --ctp
# 客户端：需在另一个202终端，在服务端等待连接时运行
urma_perftest send_lat -d udmac0d1e2 --eid_idx 1 --ctp -S 141.61.17.202
```

正式采集前先采用下列显式配置复验 256 B。两条命令不能在同一个前台终端顺序粘贴，
必须先 A 等待连接，再在 B 发起连接。

终端 A，服务端：

```bash
set +e
timeout -k 5s 120s urma_perftest send_lat \
  -d udmac0d1e2 --eid_idx 1 --ctp -O 1 --share_jfr true \
  --mgmt_proto tcp -p 0 -P 21210 -s 256 -n 10000 \
  2>&1 | tee "$SEND_LOGS/urma-smoke-server.log"
smoke_rc=${PIPESTATUS[0]}
set -e
printf '%s\n' "$smoke_rc" > "$SEND_LOGS/urma-smoke-server.exit"
printf 'server_exit=%s\n' "$smoke_rc"
test "$smoke_rc" -eq 0
```

出现 `Waiting for client to connect` 后，202 的终端 B 执行：

```bash
timeout -k 5s 120s urma_perftest send_lat \
  -d udmac0d1e2 --eid_idx 1 --ctp -O 1 --share_jfr true \
  --mgmt_proto tcp -p 0 -P 21210 -s 256 -n 10000 \
  -S 141.61.17.202
echo "client_exit=$?"
```

两边退出 0 且有延迟行再继续；失败时保留双端输出，不进入“全通过”统计。
后面的批量脚本只需终端 A，自动启动同机双方并保存双方退出码。

## 4. 终端 A：选择 202 自己的 RoCE v2 GID

```bash
RDMA_DEVICE=rocep40s0f0
RDMA_IB_PORT=1
rdma_gid_root=/sys/class/infiniband/$RDMA_DEVICE/ports/$RDMA_IB_PORT
for entry in "$rdma_gid_root"/gids/*; do
  idx=${entry##*/}
  printf 'index=%s gid=%s type=%s netdev=%s\n' "$idx" \
    "$(cat "$entry")" \
    "$(cat "$rdma_gid_root/gid_attrs/types/$idx")" \
    "$(cat "$rdma_gid_root/gid_attrs/ndevs/$idx")"
done
read -r -p '输入202本机 IPv4 RoCE v2 GID 的数字索引: ' RDMA_GID
[[ $RDMA_GID =~ ^[0-9]+$ ]]
printf 'RDMA_GID=%s\n' "$RDMA_GID" | tee "$SEND_LOGS/selected-gid.txt"
```

选择地址为 `0000:0000:0000:0000:0000:ffff:c0a8:11ca`（192.168.17.202）、
类型 RoCE v2、关联测试网口的条目。不要沿用 204 的索引 3，也不要把 UB EID 1 当作 GID 1。
采集脚本会再次验证本地 GID、地址及状态。

## 5. 终端 A：跑 RDMA SEND 五轮基础组

```bash
cd "$SEND_TOOLS"
set +e
RDMA_DEVICE=rocep40s0f0 RDMA_IB_PORT=1 RDMA_GID="$RDMA_GID" \
RDMA_SERVER=192.168.17.202 RDMA_ROUNDS=5 RDMA_OUTPUT_PARENT=/root \
  bash utils/rdma_send_loopback_perftest.sh 2>&1 | tee "$SEND_LOGS/rdma-console.log"
rdma_rc=${PIPESTATUS[0]}
set -e
printf 'RDMA collector_exit=%s\n' "$rdma_rc"
RDMA_RUN=$(sed -n 's/^RUN=//p' "$SEND_LOGS/rdma-console.log")
printf 'RDMA_RUN=%s\n' "$RDMA_RUN"
```

底层分别调用 `ib_send_lat` 和 `ib_send_bw`：verbs RC、inline 0、单 QP，
延迟 100000 次/轮，带宽 TX 128/RX 512/CQ moderation 100。
带宽 `-D 29 -f 7` 目标有效计时约 15 秒。成功完成应有 50 个正式有效样本，另有 10 对预运行。
脚本打印每轮结果、最后的延迟/带宽汇总和 ARCHIVE 路径；失败时保留部分汇总和错误。

## 6. 终端 A：跑 UB SEND 五轮基础组

确认 RDMA 进程已经退出，再执行：

```bash
cd "$SEND_TOOLS"
set +e
UB_DEVICE=udmac0d1e2 UB_EID=1 UB_SERVER=141.61.17.202 UB_OUTPUT_PARENT=/root \
UB_BW_SECONDS=30 SEND_PROFILE=baseline SEND_MODE=poll \
SEND_SIZES='2 64 256 1024 4096' SEND_OPS='send_lat send_bw' \
SEND_ROUNDS=5 SEND_LAT_ITERS=100000 SEND_CHANNELS=1 \
SEND_TX_DEPTH=128 SEND_RX_DEPTH=512 SEND_CQ_MOD=100 \
  bash utils/ub_loopback_perftest.sh 2>&1 | tee "$SEND_LOGS/ub-console.log"
ub_rc=${PIPESTATUS[0]}
set -e
printf 'UB collector_exit=%s\n' "$ub_rc"
UB_RUN=$(sed -n 's/^RUN=//p' "$SEND_LOGS/ub-console.log")
printf 'UB_RUN=%s\n' "$UB_RUN"
```

此命令显式固定旧数据的 2 B，避免新 UB 脚本默认 8 B 导致维度不一致。
实际 URMA 带宽参数是下列成对命令，供核查；批量执行期间不要另外同时运行它们：

```bash
# 服务端示例，256 B
urma_perftest send_bw -d udmac0d1e2 --eid_idx 1 --ctp \
  -O 1 --share_jfr true --mgmt_proto tcp -p 0 -P 21211 \
  -s 256 -I 0 -R 512 -D 30 -J 1 -T 128 -C 4096 -Q 100
# 客户端示例，在另一个202终端执行
urma_perftest send_bw -d udmac0d1e2 --eid_idx 1 --ctp \
  -O 1 --share_jfr true --mgmt_proto tcp -p 0 -P 21211 \
  -s 256 -I 0 -R 512 -D 30 -J 1 -T 128 -C 4096 -Q 100 -S 141.61.17.202
```

本批 UB `-D 30` 成功样本实际计时约 15 秒，不标成 30 秒正式计时。
CTP SEND 本组最大 4096 B，不混入 64 KiB 或换 TP 的结果。
已有 4 KiB 第 2 轮 RNR 失败尚未得到修复证据；再次失败时脚本会停止并打印汇总。
保留该失败，不通过加重试或丢弃轮次来制造五轮全成功。

## 7. 终端 A：同时返回两份终端数据

若两个采集器均打印了 RUN 目录，可直接运行：

```bash
test -n "$RDMA_RUN"
test -n "$UB_RUN"
test -d "$RDMA_RUN"
test -d "$UB_RUN"
python3 "$SEND_TOOLS/utils/send_perftest_report.py" --write \
  "$RDMA_RUN" "$UB_RUN" | tee "$SEND_LOGS/rdma-ub-summary.txt"
printf 'RDMA_ARCHIVE=%s.tar.gz\nUB_ARCHIVE=%s.tar.gz\nSESSION=%s\n' \
  "$RDMA_RUN" "$UB_RUN" "$SEND_LOGS"
```

看每个后端的 `MEASURED valid/invalid/planned/unattempted`、LAT、BW 和 INVALID 行。
汇总为成功轮次的 `中位数[最小值,最大值]`；单位为 μs、MiB/s、Mpps。
任意一端非零或日志不合格的样本不提供性能指标。COMPARE 不自动生成提升倍数。
本组 RC 与 RM/CTP、设备和完成机制仍有差异；同机数据不能代表物理链路带宽或 Valkey QPS。

两个 tar.gz 已包含 summary.txt、metrics.csv 和原始日志。
将终端的两份汇总文本返回即可先比较；完整复核时再回传双方采集归档和 SESSION 记录。
若 shell 已退出，重新从各 console.log 的 RUN 行取得路径即可，不依赖旧 shell 变量。

## 8. 已有旧 UB 目录不必重跑即可显示

```bash
python3 "$SEND_TOOLS/utils/send_perftest_report.py" --write /root/ub-loopback-RAjXduzL
```

旧目录没有完整计划/配置文件时，相应字段显示 legacy/unknown，不猜配置。
原始失败分析见 [202 UB 部分结果](202-ub-loopback-results-2026-11-12.zh-CN.md)，
基础项目清单见 [基础性能状态](send-basic-performance-status.zh-CN.md)。

本地交付验证覆盖 Bash 语法、报告解析测试及模拟成对流程；真实设备、性能、
GID 和有效时长仍以本次现场输出为准。
