# 202/204 环境恢复后的测试操作手册

更新日期：2026-11-11。此前两台环境暂不可用；现 202 已恢复，用户报告同机 RDMA
采集完成，归档待审阅。204 当前可用性待确认，下面双机性能及 UB 功能步骤仍待执行。
已知双机 RDMA 功能证据和固定负载见
[性能基线复现说明](202-204-performance-baseline.zh-CN.md)。
202 的 UB 查询结果及下一步见 [排序能力诊断](202-ub-order-diagnostic.zh-CN.md)。
现场命令由用户执行；本地软件检查不替代硬件测试。
后续性能口径和可并行的软件工作见
[RDMA / 原生 URMA 评估改进方案](rdma-urma-performance-comparison-design.zh-CN.md)。
该方案中的统一 URMA 发生器尚未实现；当前可先准备 TCP/RDMA 对照，原生 URMA
仍需通过平台建链和应用功能准入。

## 进度与执行顺序

| 阶段 | 当前状态 | 恢复后的动作 |
| --- | --- | --- |
| 固定源码构建、204 同机功能/压力测试 | 已有通过日志 | 检查旧目录和二进制是否仍可复用 |
| 202 → 204 功能测试 | 已有通过日志，合计 27921 个 KV | 环境有变化时在专用实例复验 |
| BGSAVE 完成 | 已有两次成功记录 | 不据此声称最终全量快照或重启恢复通过 |
| 202 同机性能采集 | 用户报告完成，两份归档待回收审阅 | 审阅每轮数据和两端 CPU |
| 202 → 204 性能基线 | 待执行 | 首组 SET/GET 各 5 个有效样本 |
| UB 功能与等价性能比较 | 202 原生 UB 设备枚举成功；报告 rc_order=0 与适配器 OT 要求不匹配 | 先执行独立排序资源诊断，再做收发功能及性能准入 |

推荐顺序：更新工具 → 两端盘点 → 确认匹配二进制 → 必要的功能复验 →
双机性能采集 → 回收两端归档 → 验收并汇总 → 扩展负载或进入 UB 对比。
202 同机采集可单独先做，结果必须标注同机，不能替代双机基线。

## 1. 更新工具，固定本轮版本

沿用此前的工具目录；若工具在 `~/valkey-ub`，将下面目录替换为该路径。
在两台主机分别执行：

```bash
cd "$HOME/valkey-rdma-baseline-tools"
git status --short
git fetch origin work/rdma-perf-baseline
git switch work/rdma-perf-baseline
git pull --ff-only origin work/rdma-perf-baseline
git rev-parse HEAD
python3 utils/rdma_baseline.py --help
```

有本地修改时先保留，不使用 reset 或强制覆盖。记录两端工具 SHA，确认相同后再跑。
新环境尚无工具目录时，按主文档“取得工具”克隆。
被测源码固定为 `aff2d3350e57e1c222d28743b486a940b0ac992c`；工具 SHA 与被测源码
SHA 分开记录。工具新增采集/文档代码，不代表现场二进制已重新构建。

## 2. 复核两端环境

在 202、204 各运行一份，保留输出。以下命令只查询，不重配设备：

```bash
mkdir -p "$HOME/valkey-validation"
preflight_root=$(mktemp -d "$HOME/valkey-validation/rdma-resume-preflight.XXXXXX")
{
  hostname
  date -Iseconds
  uname -a
  python3 --version
  ulimit -l
  ip -br address
  rdma link show
  ibv_devinfo -v
  ls -l /dev/infiniband
  lscpu
  for field in state link_layer rate active_mtu; do
    path="/sys/class/infiniband/rocep40s0f0/ports/1/$field"
    if test -r "$path"; then printf '%s: ' "$field"; cat "$path"; fi
  done
  if command -v pkg-config >/dev/null 2>&1; then
    pkg-config --modversion libibverbs librdmacm
  fi
} > "$preflight_root/environment.log" 2>&1
cat "$preflight_root/environment.log"
printf 'PREFLIGHT=%s\n' "$preflight_root"
```

202 另检查路由：

```bash
ip route get 192.168.17.204
```

预期仍是 202 的 `192.168.17.202` 经 `enp40s0f0np0` 发往 204 的
`192.168.17.204`；设备 `rocep40s0f0/1` ACTIVE，驱动为此前确认的 `mlx5_core`。
同时查看 `ibv_devinfo -v` 的 GID/固件版本。若 IP、设备、驱动或路径变化，先记录
差异并确认实际可用路径；不要把管理地址 `141.61.17.202/204` 填成 RDMA 地址。

缺少工具、设备不可见或 locked memory 限制变化时，保留实际错误，再按主机管理
流程处理。端口总速率以设备/网卡实际协商信息为准，不只依据单个 active_speed 字段。
采集脚本目前的设备计数查询固定为上述设备名和网卡名；若名称变化，需要更新
相应采集逻辑并固定新的工具 SHA，不能将缺失计数当作零错误。

## 3. 确认被测源码和二进制

在两台主机各自设置变量，后续命令复用它。旧目录仍存在时可以使用：

```bash
# 202 终端：
baseline_source="$HOME/valkey-validation/rdma-cross-client.M6uyNa/source"
```

```bash
# 204 终端：
baseline_source="$HOME/valkey-validation/rdma-20261111-032212-57179/source"
```

分别检查：

```bash
cat "$baseline_source/../source-sha.txt"
test "$(cat "$baseline_source/../source-sha.txt")" = aff2d3350e57e1c222d28743b486a940b0ac992c
sha256sum "$baseline_source/src/valkey-server" \
  "$baseline_source/src/valkey-cli" "$baseline_source/src/valkey-benchmark"
ldd "$baseline_source/src/valkey-server"
ldd "$baseline_source/src/valkey-benchmark"
```

发现缺失文件、库无法加载或源码身份不匹配时，不继续采集。旧树不可用则按主文档
“准备匹配二进制”从固定提交重新归档、构建，再将 `baseline_source` 设为实际路径。
不直接把当前工具仓库 HEAD 当成已验证的被测源码。

## 4. 必要时复验双机功能

若主机重启后设备/库/驱动/源码有变化，或旧功能证据对应的环境无法确认，先复验。
端口 `16380` 用于功能实例；性能实例使用 `16381`，不要将 rdma-test 连接到性能实例，
因为它会写入测试 Key 并执行 BGSAVE。

在 204，设好该机的 `baseline_source`，创建空数据目录并保持前台运行：

```bash
function_root=$(mktemp -d "$HOME/valkey-validation/rdma-resume-function.XXXXXX")
mkdir "$function_root/data"
"$baseline_source/src/valkey-server" \
  --port 0 --rdma-bind 192.168.17.204 --rdma-port 16380 \
  --urma-port 0 --protected-mode no --cluster-enabled no \
  --appendonly no --save "" --io-threads 1 \
  --dir "$function_root/data" --dbfilename cross-rdma.rdb \
  --daemonize no --logfile "" --loglevel verbose \
  > "$function_root/server.log" 2>&1
```

在 204 另一个终端查看该目录的 `server.log`，确认 `Ready to accept connections rdma`。
在 202，设好该机的 `baseline_source` 后执行以下 Bash。两轮之间等待保存完成：

```bash
(
set -euo pipefail
check_root=$(mktemp -d "$HOME/valkey-validation/rdma-resume-check.XXXXXX")
cli=("$baseline_source/src/valkey-cli" --rdma -h 192.168.17.204 -p 16380 --raw)
timeout 10s "${cli[@]}" PING
for threads in 0 4; do
  set +e
  timeout 120s "$baseline_source/tests/rdma/rdma-test" \
    -h 192.168.17.204 -p 16380 --thread "$threads" \
    2>&1 | tee "$check_root/thread-$threads.log"
  status=${PIPESTATUS[0]}
  set -e
  printf '%s\n' "$status" > "$check_root/thread-$threads.exit"
  test "$status" -eq 0
  saved=no
  for attempt in {1..60}; do
    info=$(timeout 5s "${cli[@]}" INFO persistence)
    info=${info//$'\r'/}
    if grep -qx 'rdb_bgsave_in_progress:0' <<< "$info"; then
      printf '%s\n' "$info" > "$check_root/persistence-$threads.log"
      grep -qx 'rdb_last_bgsave_status:ok' <<< "$info"
      saved=yes
      break
    fi
    sleep 1
  done
  test "$saved" = yes
done
printf 'FUNCTION_CHECK=%s\n' "$check_root"
)
```

验收：两轮退出码均为 0，每个线程的 PING、SET、GET 逐值校验成功，204 记录来自
`192.168.17.202` 的 RDMA 连接及保存成功。结束后停止这个专用功能实例。
若需补做重启恢复验证，应另建功能试验：保存确定性的 Key/Value 清单、停止写入、
确认最终快照完成、重启并逐值比对。当前 rdma-test 不保留可跨进程复验的预期值清单，
不能仅用重启后的 DBSIZE 代替恢复验证；该项不与关闭持久化的性能样本混跑。

## 5. 正式双机性能基线

两台都恢复后，优先执行本阶段。同机采集如有需要，使用主文档“在 202 上做同机测试”
的两个终端命令，并单独归档、标注。下面的正式基线两端均显式指定 204 的地址。

204：进入工具仓库根目录，设好该机的 `baseline_source`：

```bash
python3 -u utils/rdma_baseline.py server \
  --source "$baseline_source" --host 192.168.17.204
```

等出现 `READY: 192.168.17.204:16381`，保持该终端运行。202 的另一个会话，进入
同一版本工具仓库并设置该机源码变量：

```bash
python3 -u utils/rdma_baseline.py client \
  --source "$baseline_source" --host 192.168.17.204
```

负载为客户端线程 1、32 连接、Pipeline 1、Value 256 B、10000 个 Key；每样本预热
5 秒、测量 30 秒，SET/GET 各 5 轮。等待约 6～7 分钟，不在同一实例上并行运行
其他 benchmark、功能校验或 BGSAVE。避免同时占用主机的其他重负载任务。

客户端显示 `COMPLETE` 后，在 204 的性能服务终端按一次 Ctrl+C；正常的人工停止会
显示 `Stopped` 并生成服务端归档，不因此推断客户端测量失败。
记录两端各自完整的 `ARCHIVE=` 路径。若失败，保留失败归档；下次重跑启动全新
性能实例，不对不明实例执行 FLUSHDB，也不将部分样本凑成完整一组。

## 6. 回收归档

在 Windows PowerShell，填入刚才两端实际打印的归档路径，不填省略号：

```powershell
$rdmaServerArchive = '/home/h30086230/valkey-validation/实际服务端归档.tar.gz'
$rdmaClientArchive = '/home/h30086230/valkey-validation/实际客户端归档.tar.gz'
$rdmaResultsDir = 'D:\valkey\rdma-baseline\results'
New-Item -ItemType Directory -Force $rdmaResultsDir | Out-Null
scp "h30086230@141.61.17.204:${rdmaServerArchive}" $rdmaResultsDir
scp "h30086230@141.61.17.202:${rdmaClientArchive}" $rdmaResultsDir
Get-ChildItem -LiteralPath $rdmaResultsDir -Filter '*.tar.gz' | Get-FileHash -Algorithm SHA256
```

在源主机可用 `sha256sum /实际/归档.tar.gz` 核对同一文件的传输哈希。
同机试验的两个归档都从 202 回收。保留预检/构建/功能日志；脚本性能归档不会自动
收入其他阶段的目录。结果分析需要两份完整性能归档，单独控制台摘要不含完整 CPU 证据。

## 7. 结果验收和展示

分析前逐项确认：

- 客户端有 `COMPLETE`，`results.csv` 恰有 10 个样本，SET/GET 各 5 个。
- 各样本退出码为 0，正式测量时长约 30 秒，QPS 和分位数来自完整报告。
- 记录被测 SHA、两端二进制/脚本哈希、环境、负载、默认调度与持久化设置。
- 正式双机组的两端 hostname 不同，目标为 `192.168.17.204`，不混入 202 同机组。
- INFO 前后 run_id 一致，无新增 miss、错误回复、淘汰或快照；服务端日志无异常。
- 两端 CPU/RSS 采样覆盖样本窗口，进程身份没有复用；没有缺失采样或明显的其他负载干扰。
- 结合时钟探测对齐两端 CPU，选取窗口内部的稳态样本，剔除预热和结束边界。
  CPU 的 `100%` 表示一个逻辑 CPU；不把近似采样说成精确 30 秒积分。
- 检查前后设备计数。存在丢包/重试/错误或版本差异时保留并解释，不能仅凭非零累计值
  判断本轮异常，也不能在缺失计数时声称无硬件错误。

报告保留每轮数据，给出 QPS 和各轮 p50/p95/p99 的中位数、最小/最大值，分别列出
客户端和服务端 CPU。抖动明显时先检查背景任务、客户端瓶颈、调度及设备状态，再
用新实例复测；不挑选最好一轮替代全部结果。建议表格：

| 路径/拓扑 | Value/连接/Pipeline | 有效轮次 | QPS 中位数/范围 | p50/p95/p99（ms） | 客户端/服务端 CPU | 异常与限制 |
| --- | --- | --- | --- | --- | --- | --- |
| 原生 RoCE / 202→204 | 256 B / 32 / 1 | 待测 | 待测 | 待测 | 待分析 | 待分析 |
| 202 同机（可选） | 同配置 | 待测 | 待测 | 待测 | 待分析 | 同机资源共享 |

## 8. 扩展负载与 UB 比较条件

首组有效后再扩展，先改一个维度。建议先补连接 1/128，再补 Pipeline 16，随后测
64 B、1 KiB、4 KiB、64 KiB。每组仍保留单 I/O 线程、5 秒预热、30 秒测量和至少
5 次重复；更高线程并发、绑核和持久化策略另建分组。

当前脚本只提供 `--source`、`--host` 和角色参数，上述扩展尚未实现为命令行选项。
不要直接给它传未支持的 `-c`、`-P`、`-d`；扩展时需同时修改发生器参数、预填充
长度检查、workload.json、元数据和软件测试，再提交固定版本。先取得首组基线，
本次文档更新没有提前更改该负载。

进入 UB 性能比较前，必须明确实际路径：

| 路径 | 所需条件 | 当前工具的适用范围 |
| --- | --- | --- |
| 原生 RoCE RDMA | 已确认的 mlx5/RoCE 双机环境 | 本脚本当前目标 |
| RoUB 兼容路径 | 实际 UB 设备及可用 Verbs/librdmacm 兼容层 | 先验证兼容性，再核对采集逻辑和版本；明确标注 RoUB |
| 原生 URMA | 实际 UB/URMA 设备、SDK、provider 与原生客户端 | 现有 RDMA benchmark 不提供等价原生 URMA 压测入口 |

原生 URMA 先按 [URMA 说明](../../tests/urma/README.md)完成专用实例及 client-probe
功能验收，记录 PING、二进制 SET/GET/DEL、认证/关闭和错误处理结果。client-probe
是功能客户端，不是正式性能发生器。
原生性能对比需先补齐与 RDMA 等价的 Key/Value、连接、Pipeline、线程、预热/测量
时长、成功计数和延迟直方图；不使用不存在的 `valkey-benchmark --urma` 参数。

对比固定公共代码基线、编译/allocator、持久化、负载、安全策略和 CPU 调度，并
记录硬件速率、NUMA、拓扑和固件差异；不同设备/主机的结果不能只归因于协议。
具备两种路径时交替安排配对轮次，而非相隔很久只各测一组。缺少硬件、等价发生器
或真实数据时，状态保持待测，不给出 UB 提升百分比或可行性结论。

## 完成条件

本轮后续工作的完成条件是：双机首组 10 个有效样本、两端完整归档和环境记录、
CPU/错误/设备计数审阅、每轮与汇总报告齐备。UB 对比另按上述准入条件推进。
202 同机归档待审阅，双机性能和 UB 功能尚无完成证据；本文档上传不改变这些状态。
