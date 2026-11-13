# 204：只补 UB，失败继续，末尾单独复测

日期：2026-11-13。按用户新要求调整采集顺序：不测 RDMA，先执行完整 UB 计划，
单条失败后保存现场并执行下一条，首轮结束后对失败条目各复测一次。
这项调整用于收集覆盖面和重复性证据，不表示既有状态 10 已修复。

## 1. 数量与参数

| 项目 | 首轮计划 |
| --- | --- |
| SEND 延迟 | 2/64/256/1024/4096 B，各 5 轮，共 25 条；每轮 100000 次 |
| SEND 带宽 | 相同尺寸各 5 轮，共 25 条；duration 参数 30 秒，按输出核对有效窗口 |
| 合计 | 50 条计划任务，每条由一对服务端/客户端执行 |
| 末尾复测 | 首轮失败 F 条，则只复测这 F 条，各一次；最多再执行 50 条 |

设备 `udmac0d1e2`、EID 1、管理地址 `141.61.17.204`；RM/CTP、poll、inline 0。
带宽单 Jetty、TX128/RX512/JFC4096/CQ100；延迟单 Jetty、TX1/RX512/JFC512。
没有额外人为批间等待，不改变重试次数、绑定 CPU 或设备配置。

新增入口是 `utils/ub_loopback_batch.py`，仅用 Python 3 标准库，调用既有
`ub_loopback_perftest.sh`，原采集器遇错退出的默认行为保持不变。
新入口每条使用独立采集目录、独立进程和控制端口，先预运行一次，再正式运行一次。
因此首轮最多有 50 对预运行；复测每条也有自己的预运行，不计入正式指标。
它与旧采集器“先为各尺寸预运行、再连续正式五轮”的执行安排不同，报告需分组保存。

预运行失败时，该条正式打流不启动，条目记为无效后继续；末尾复测整个条目。
单条预运行或正式打流的工具超时均由原采集器限制为 120 秒，服务端就绪等待最多约 20 秒。
清理、报告及归档完成后才进入下一条，不并发打流。
用例错误不中断；目录无法创建、文件无法写入等使证据无法保存的基础设施错误仍会停止。

## 2. 204 执行命令

使用此前运行测试的 h30086230 用户。确认当前没有其他压测后，在一个终端执行即可。
先更新分支；有本地修改时先保留，不使用 reset 或强制覆盖：

```bash
cd /home/h30086230/valkey-ub || exit 1
git status --short
git switch work/rdma-perf-baseline || exit 1
git pull --ff-only origin work/rdma-perf-baseline || exit 1
git rev-parse HEAD
```

启动完整 UB 计划，终端与日志同时输出每条结果，末尾自动打印两阶段汇总并打包：

```bash
cd /home/h30086230/valkey-ub || exit 1
UB_BATCH_CONSOLE=$(mktemp "$HOME/ub204-batch-console-XXXXXXXX.log")
set +e
UB_DEVICE=udmac0d1e2 UB_EID=1 UB_SERVER=141.61.17.204 \
UB_OUTPUT_PARENT="$HOME" UB_BW_SECONDS=30 \
SEND_MODE=poll SEND_LAT_ITERS=100000 \
SEND_CHANNELS=1 SEND_TX_DEPTH=128 SEND_RX_DEPTH=512 SEND_CQ_MOD=100 \
  python3 utils/ub_loopback_batch.py \
    --sizes 2 64 256 1024 4096 --ops send_lat send_bw --rounds 5 --port 22000 \
  2>&1 | tee "$UB_BATCH_CONSOLE"
ub_batch_rc=${PIPESTATUS[0]}
printf 'BATCH_EXIT=%s\nCONSOLE=%s\n' "$ub_batch_rc" "$UB_BATCH_CONSOLE"
```

最多预留控制端口 22000..22199，每个计划条目预留两个新端口，即使失败也不复用。
默认末尾复测一次，无需再启动命令；不会无限重试。
`--no-retry` 可仅执行首轮；`--ops send_bw` 可用于后续独立带宽计划，勿与本次全量结果混合。
尺寸、操作、轮次、起始端口以新入口的命令行参数为准，不能用 SEND_SIZES/SEND_OPS/SEND_ROUNDS/UB_PORT 替换。
其他设备、队列、有效时间参数仍使用上述环境变量。

## 3. 终端判读和回收文件

输出 `BATCH_ROOT=...` 和 `BATCH_ARCHIVE=...tar.gz`。需要回收最终总归档和外层 CONSOLE 文件即可。
总归档包含以下材料，以及所有子采集目录/归档：

| 路径（相对 BATCH_ROOT） | 内容 |
| --- | --- |
| `plan.tsv` | 首轮 50 条逻辑任务，包括原始轮次 |
| `attempts.tsv` | 两阶段每条的端口、采集器退出、RUN 路径、有效性与错误 |
| `failed-initial.tsv` | 首轮失败清单，成功复测后也不删除 |
| `initial/summary.txt`、`initial/metrics.csv` | 只汇总首轮，不以复测替换失败 |
| `retry/summary.txt`、`retry/metrics.csv` | 只汇总失败项复测，保留原逻辑轮次 |
| `initial/`、`retry/` 下各 console.log、ub-loopback-* | 每条预运行和正式打流的完整命令、双端日志、退出码、环境、工具哈希 |
| `tool.sha256` 和三个采集源码快照 | 本轮批量入口、原采集器、报告器身份 |
| `FINISHED` | 首轮和安排的复测阶段已执行结束，不代表所有测试通过 |

阶段汇总的 `CASES valid/invalid/attempted/planned/unattempted` 按逻辑任务统计。
预运行失败也算一条已尝试的无效任务；正式工具实际是否启动以对应子目录 results.tsv 为准。
失败项不贡献性能指标。两阶段中位数分别计算，不合并为“补齐后的全通过五轮”。
复测发生在首轮之后，负载和时间可能变化，不能只凭复测成功断言错误原因。

退出码 0 表示首轮所有条目有效；退出码 1 表示首轮曾有失败，即使复测全部成功也保留此状态。
可结合 FINISHED 和两份 summary 判断“计划执行结束”与“全部有效”，不要只凭退出码认定中途停止。
若复测仍失败，先返回该条双端日志和退出码，不额外循环刷成功数。

## 4. 软件验证边界

本地单元及模拟工具检查覆盖首轮失败后继续、预运行未启动正式用例、非零退出附带数值行、
无效结果表头、仅失败项末尾复测一次、独立端口、原轮次与首轮失败保留。
软件检查不是 204 硬件通过证据，现场结果需按归档核验。
