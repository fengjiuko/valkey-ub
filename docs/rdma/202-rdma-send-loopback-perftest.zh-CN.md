# 202 同机 RDMA SEND 组件性能补测

日期：2026-11-12。补齐与 [URMA/UB SEND 数据](202-ub-loopback-results-2026-11-12.zh-CN.md)
对应的 RoCE/verbs SEND 微基准，供 Valkey 传输路径评估。现场尚未运行本轮测试。
Valkey 的应用命令是 SET/GET 等；此处 SEND 是底层 verbs 操作，工具是 ib_send_lat/ib_send_bw，
结果不标作 Valkey 命令 QPS，也不需要重编译或启动 Valkey。

## 1. 在 202 root 终端核对设备、GID、工具

```bash
hostname
uname -a
command -v ib_send_lat ib_send_bw ibv_devinfo
rpm -q perftest libibverbs
ip -br address
ibv_devinfo -d rocep40s0f0 -v
ib_send_lat --help
ib_send_bw --help
```

既有 202 记录为 `rocep40s0f0/1`、网口 `enp40s0f0np0`、IPv4 `192.168.17.202`，
本次仍以实际盘点为准。工具缺失时保留输出，按现场软件管理流程准备，不自动安装。

列出本机 GID 的类型和关联接口：

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
```

选择 `RoCE v2`、关联测试网口、地址为 IPv4-mapped `192.168.17.202` 的项；
完整十六进制通常为 `0000:0000:0000:0000:0000:ffff:c0a8:11ca`。
**不要直接沿用 204 的 GID 3**，UB EID index 1 也不等于 RoCE GID index 1。

## 2. 先验证一对 256 B 延迟

两端均在 202，分别将 RDMA_GID 设置成上一步核对的数字；以下赋值占位符必须替换。
若设备名/地址改变，两端同步修改。先在终端 A：

```bash
RDMA_GID=填写202实际RoCEv2索引
timeout -k 5s 120s ib_send_lat -d rocep40s0f0 -i 1 -x "$RDMA_GID" \
  -c RC -I 0 -r 512 -t 1 -s 256 -n 100000 -p 21400
echo "server_exit=$?"
```

看到等待连接后，终端 B：

```bash
RDMA_GID=填写202实际RoCEv2索引
timeout -k 5s 120s ib_send_lat -d rocep40s0f0 -i 1 -x "$RDMA_GID" \
  -c RC -I 0 -r 512 -t 1 -s 256 -n 100000 -p 21400 192.168.17.202
echo "client_exit=$?"
```

客户端地址是位置参数；perftest 的 `-p` 是 TCP 控制端口，`-i` 是 RDMA 设备端口，
与 urma_perftest 的同名字母含义不同。两边必须正常结束，无 CQE 错误/超时。
本组用 TCP 交换 QP 信息，verbs RC 传数据，不传 `-R` 切换 RDMA CM 建 QP。
因此该建链方式也与 Valkey 使用 librdmacm 的完整路径有区别。

## 3. 验证一对 256 B 单向带宽

两端保留各自的 RDMA_GID。终端 A：

```bash
timeout -k 5s 120s ib_send_bw -d rocep40s0f0 -i 1 -x "$RDMA_GID" \
  -c RC -I 0 -q 1 -r 512 -t 128 -Q 100 -s 256 -N -D 29 -f 7 -p 21401
echo "server_exit=$?"
```

等待连接后，终端 B：

```bash
timeout -k 5s 120s ib_send_bw -d rocep40s0f0 -i 1 -x "$RDMA_GID" \
  -c RC -I 0 -q 1 -r 512 -t 128 -Q 100 -s 256 -N -D 29 -f 7 -p 21401 \
  192.168.17.202
echo "client_exit=$?"
```

`-D 29 -f 7` 目标为 7 秒前段、15 秒计时、7 秒后段；用正式迭代数和消息率核算现场有效计时。
不因两个工具均有 `-D` 就假定参数相同意味着正式计时相同。
`-N` 不计算峰值，统计 BW average；不同时传 `-n`。

## 4. 批量五轮与归档

将 [`rdma_send_loopback_perftest.sh`](../../utils/rdma_send_loopback_perftest.sh)
上传到 202 的 `/root/rdma_send_loopback_perftest.sh`，同时将
[`send_perftest_report.py`](../../utils/send_perftest_report.py) 放到同一目录。
只需一个终端自动成对运行：

```bash
RDMA_GID=填写202实际RoCEv2索引
RDMA_GID="$RDMA_GID" bash /root/rdma_send_loopback_perftest.sh
```

| 项目 | 固定配置 |
| --- | --- |
| 主机/数据路径 | 202 同机，原生 RoCE 设备，verbs RC SEND/RECV |
| 大小 | 2、64、256、1024、4096 B，与 UB 相同 |
| 延迟 | tx depth 1，rx depth 512，inline 0，100000 次，5 轮 |
| 带宽 | 单 QP，tx depth 128，rx depth 512，CQ moderation 100，inline 0，单向 |
| 带宽计时 | 每轮 `-D 29 -f 7`，目标有效计时 15 秒，每大小 5 轮 |
| 独立预运行 | 每大小每操作先固定 10000 次，标 warmup，排除正式汇总 |
| CPU | 系统默认调度，不绑核、不改频率、不加实时优先级 |
| 失败处理 | 等待就绪最多 20 秒，每进程最多 120 秒，失败停止，保留完整证据 |

脚本检查 GID 类型、地址、ACTIVE/Ethernet 状态及关联接口的本机 IPv4；默认使用真实
`/sys/class/infiniband`。`RDMA_SYSFS_ROOT` 仅用于本地模拟验证，不用于真实性能采集；
归档会记录实际 sysfs 根路径。
默认从 TCP 21420 端口开始，每对使用独立端口；不要与其他批次同时使用这些端口。
预计约十几分钟，实际以工具耗时为准。

输出 `RUN=...`、`ARCHIVE=...tar.gz`。归档包含设备环境、二进制/脚本 SHA256、
选择的 GID、帮助、实际命令、双方日志和退出码、时间及 `results.tsv`。
每轮打印原始结果；结束或失败时自动显示终端汇总，另存 summary.txt 和 metrics.csv。
两种后端和历史目录的汇总方法见 [终端输出与基础性能清单](send-basic-performance-status.zh-CN.md)。
仅正式 50 对全成功且客户端均有数据行时写 COMPLETE；失败有 FAILED.txt。
COMPLETE 不代表载荷逐字节正确性或无资源泄漏，错误轮次和 warmup 均不进入汇总。

客户端为主报告端；保留 min/max/typical/avg/p99 等工具实际提供的列。
不同工具的分位定义、样本尾部处理可能有差异；不从没有提供的列推算分位。
带宽原始单位和消息率一起保留；MB/sec 是否为 MiB/s 必须按现场数值/版本确认。
未开启 `--report_gbits`，避免与旧 UB 表中的字节速率单位混淆。
这组不采稳态 CPU 百分比，不能声称双端 CPU 收益。

## 5. 对比边界

RDMA 采用 RC、UB 使用 RM/CTP，两套硬件、provider、完成队列容量、接收队列共享方式
和计时细节仍有差异；这里只尽量匹配负载与队列参数，不声称 API 成本完全隔离。
不把 URMA priority 1 对应成 verbs service level 1，也不凭参数名称强行匹配 QoS。
同机 HCA 回环不能代表物理链路/交换网络带宽。

RDMA 完成后，与 UB 各配置成功样本逐行对照 median/avg/p99、BW average、Mpps 和错误。
UB 目前只有一到两轮成功且 4 KiB 有失败；未补齐前可报告观察值和稳定性限制，
不发布稳定提升百分比。旧 Valkey SET/GET 保持应用层单独报告。

工具依据：[linux-rdma/perftest 官方说明](https://github.com/linux-rdma/perftest/blob/master/README)
及 [参数实现](https://github.com/linux-rdma/perftest/blob/master/src/perftest_parameters.c)。
公开参考未被当作现场 perftest 4.5 安装包的已验证完整源码；以现场帮助和输出复核。

## 本地检查记录

Git Bash 语法和帮助入口检查通过。模拟工具验证了 10 对预运行及 50 对正式用例的
命令参数、日志配对、退出码和归档；客户端退出 7、缺少结果行、GID 类型错误均被拒绝。
另验证“打印了结果行后非零退出”仍失败；在临时副本削弱退出码判断后，负例能检出错误完成。
模拟环境替换了设备 sysfs、ip 和 stdbuf，不代表真实硬件收发、有效时长或性能数值已经验证。
