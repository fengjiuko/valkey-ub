# 202 → 204 RDMA 性能基线复现

本说明固定已完成双机功能验证的源码提交，提供独立性能实例、预填充、重复测量和
两端 CPU 采集入口。采集器只依赖 Python 3 标准库，入口是
[`utils/rdma_baseline.py`](../../utils/rdma_baseline.py)。

## 已有证据与待测范围

源码提交：`aff2d3350e57e1c222d28743b486a940b0ac992c`。
根据用户提供的双机日志，202 `192.168.17.202` → 204 `192.168.17.204:16380`
的 RDMA PING、单客户端和 4 线程逐值读写校验已通过：

| 功能检查 | 日志结果 |
| --- | --- |
| 单客户端 | 6758 个 KV，退出码 0 |
| 4 线程 | 21163 个 KV，退出码 0 |
| 合计 | 27921 个 KV，与服务端最终 Key 数一致 |
| BGSAVE | 两次均有 `DB saved on disk` 和后台保存成功记录 |

快照完成不代表最终全部写入已被快照覆盖，也没有验证重启恢复。
上述日志是功能证据；本文件没有新的双机性能结果，也不据此宣称 UB 性能收益。
原始运行产物保留在两端用户目录，不放入源码仓库。

## 首组负载

| 项目 | 固定值 |
| --- | --- |
| 路径 | 物理 RoCE RDMA，202 → 204 `192.168.17.204:16381` |
| 设备 / 网卡 | `rocep40s0f0` / `enp40s0f0np0` |
| 服务端 | 独立数据目录；I/O 线程 1；TCP、URMA、集群、AOF、自动快照关闭 |
| 客户端 | 工作线程 1，连接 32，Pipeline 1 |
| 数据 | 10000 个 Key，Value 256 字节，随机种子 20261110 |
| 操作 | SET 和 GET 分别测量；不是混合比例测试 |
| 每样本 | 进程内预热 5 秒，正式测量 30 秒 |
| 重复 | SET/GET 各 5 轮，交替先后顺序；总计约 6～7 分钟 |
| CPU | 操作系统默认调度，不绑核；逐秒记录进程 CPU 和 RSS |

实例仅用于获准的隔离测试网络；测试服务无认证，RDMA 只绑定指定测试网 IP。
采集器不加载设备、不安装依赖、不修改 MTU/PFC/ECN/CPU 频率。
本组使用默认调度，后续绑核试验应作为独立配置，比较时保留相同口径。
两端固件版本不同，应结合归档的设备信息报告。

## 取得工具

在两台主机分别克隆到新目录，保留现有源码和运行实例：

```bash
git clone --single-branch --branch work/rdma-perf-baseline \
  https://github.com/fengjiuko/valkey-ub.git "$HOME/valkey-rdma-baseline-tools"
cd "$HOME/valkey-rdma-baseline-tools"
git rev-parse HEAD
```

记录工具提交 SHA；复现实验时固定工具版本。工具分支从实际功能测试的提交建立，
采集器和工具测试的变更不会自动进入被测二进制。

## 准备匹配二进制

两端若已有此次验证的独立源码树，可以直接复用，并检查上层的 `source-sha.txt`。
204 的现有树示例为 `~/valkey-validation/rdma-20261111-032212-57179/source`；
202 为 `~/valkey-validation/rdma-cross-client.M6uyNa/source`。

重新复现时，两端分别在工具仓库根目录执行以下 Bash，归档只取固定提交：

```bash
set -euo pipefail
baseline_sha=aff2d3350e57e1c222d28743b486a940b0ac992c
mkdir -p "$HOME/valkey-validation"
baseline_root=$(mktemp -d "$HOME/valkey-validation/rdma-perf-build.XXXXXX")
mkdir "$baseline_root/source"
printf '%s\n' "$baseline_sha" > "$baseline_root/source-sha.txt"
git archive "$baseline_sha" | tar -x -C "$baseline_root/source"
cd "$baseline_root/source"
make -j4 BUILD_RDMA=yes BUILD_URMA=no MALLOC=libc \
  2>&1 | tee "$baseline_root/build.log"
make -C tests/rdma 2>&1 | tee "$baseline_root/test-build.log"
printf 'SOURCE=%s/source\n' "$baseline_root"
```

构建需要编译器、常规 Valkey 构建依赖和 RDMA 库/头文件。保留实际失败日志，按
主机包源补齐依赖。本采集器不调用同机 `runtest-rdma`，也不自动安装软件。
归档构建的 `--version` 可能显示 `sha=00000000`，提交身份以源码归档记录和
二进制 SHA256 为依据。`source-sha.txt` 是记录，不是对任意修改源码的完整性证明。

## 启动和采集

在 204 的专用终端，`--source` 填该机实际构建路径：

```bash
python3 -u "$HOME/valkey-rdma-baseline-tools/utils/rdma_baseline.py" server \
  --source /实际/204/source
```

等出现 `READY: 192.168.17.204:16381`，保持终端运行。在 202：

```bash
python3 -u "$HOME/valkey-rdma-baseline-tools/utils/rdma_baseline.py" client \
  --source /实际/202/source
```

客户端检查端口、I/O 线程、持久化配置及空数据库，不清空已有数据。随后顺序写满
全部 Key，用 Lua 检查每个 Value 长度，再开始随机读写。每轮检查服务端 run_id，
拒绝存在 miss、淘汰、错误回复或快照计数变化的样本。

看到客户端 `COMPLETE` 后，在 204 性能服务终端按一次 Ctrl+C。脚本只停止自己
启动的性能服务；此前端口 16380 的功能实例不会被它停止。
两端均打印 `ARCHIVE=/home/.../rdma-perf-<role>-....tar.gz`，将两个归档取回分析。
客户端 `COMPLETE` 文件只表示 10 个客户端样本均有效，完整报告还需检查服务端
CPU 日志和两端设备/环境证据。失败时查看 `FAILED.txt`，保留归档，不能补齐不存在的数据。

## 在 202 上做同机测试

`--host` 同时控制服务端 RDMA 绑定地址、客户端目标和路由查询；默认仍为
`192.168.17.204`。204 尚未准备好时，可以在 202 开两个终端，都指定
`--host 192.168.17.202`。复用先前已构建的 202 源码树，在工具仓库根目录执行：

```bash
# 202 终端 A：保持运行，等出现 READY: 192.168.17.202:16381。
python3 -u utils/rdma_baseline.py server \
  --source "$HOME/valkey-validation/rdma-cross-client.M6uyNa/source" \
  --host 192.168.17.202
```

```bash
# 202 终端 B：完成后，在终端 A 按 Ctrl+C，保留两份归档。
python3 -u utils/rdma_baseline.py client \
  --source "$HOME/valkey-validation/rdma-cross-client.M6uyNa/source" \
  --host 192.168.17.202
```

源码目录若不同，替换 `--source`，其上层必须有记录固定 SHA 的 `source-sha.txt`。
该参数指向被测二进制所在源码树，与采集脚本所在工具仓库是两个概念。
使用 `python3 -u` 实时打印输出；`python3 -i` 会在结束或参数错误后进入 `>>>`
交互模式，此时输入 `exit()` 退出。

此模式的客户端与服务端共享同一主机资源，只作为同机测试结果展示；不能作为
202 → 204 跨机性能数据，也不能据此作 UB/RDMA 性能结论。

## 输出和展示口径

- `results.csv`：每轮 QPS、请求数、测量时间和 avg/min/p50/p95/p99/max 延迟。
- `summary.json`：SET 和 GET 各 5 轮的中位数与最小/最大值。分位数汇总是“各轮
  分位数的中位数”，不是合并直方图后的总体分位数。
- 完整 benchmark 报告、每轮命令与退出码、INFO 前后快照及服务器日志。
- 两端 CPU 逐秒 JSONL：进程累计 user/system ticks、RSS、整机 CPU ticks，
  `100%` 表示一个逻辑 CPU；服务端包含进程内全部线程，不包含采集器自身。
- 两端脚本/二进制哈希、可读的 Make 设置、CPU/NUMA/允许 CPU 列表、设备信息及
  前后设备计数。缺失查询工具时会记录错误，不将缺失项解释为硬件正常。

CPU 原始数据包含预热。使用 `rounds.jsonl` 的窗口和每轮 `TIME` 探测对齐两端时钟。
`approx_measurement_start_ns` 是进程启动加 5 秒的估算，有进程初始化和逐秒采样误差；
分析应选窗口内部的稳态样本，避开预热及结束边界，不声称精确的 30 秒 CPU 积分。
QPS 和延迟由 benchmark 自己的正式测量窗口产生。

首组只回答上述配置下的表现，不代表最大吞吐；客户端瓶颈、默认调度和背景负载
仍可能影响数据。UB/URMA 对比需要等价的发生器、负载、计时及统计口径；
`--rdma` 不能直接替代原生 URMA 客户端。原生硬件 RoCE、RoUB 和原生 URMA
应分别标注，不把 API 或硬件差异隐去。

## 软件检查与硬件验证边界

在工具仓库根目录运行不需要 RDMA 设备的软件测试：

```bash
python3 tests/rdma/test_baseline.py
python3 utils/rdma_baseline.py --help
```

本地软件检查覆盖报告解析、无效样本拒绝、子进程退出与超时清理等行为；
不能替代 Linux `/proc` 现场采样、真实双机运行及性能数据验收。
