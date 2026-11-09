# 202/204 Valkey 部署基线

日期：2026-11-09。当前目标是确认目标主机能够部署并运行普通 Valkey，再继续
UB/URMA 组件适配。本文件是现场执行步骤，不是通过报告；本次未登录服务器执行。

## 已知条件与验收边界

- 204（worker1，141.61.17.204）：aarch64、openEuler 6.6 内核；现有 Valkey
  `aff2d3350` 已加载配置和动态库，最后一次启动在 URMA OT 能力检查处退出。
- 202（master，141.61.17.202）：aarch64；作为 TCP 客户端。
- 官方 URMA 样例双机成功只证明对应样例路径，不作为 Valkey 部署验收。
- 本次在 204 使用测试端口 16379、独立临时目录；引用现有测试 ACL 并开启 protected mode。
  16379 如果已占用，选其他空闲端口并同步修改全部客户端命令；不停止未知进程。
- 现有源码仅在 `server.urma_port != 0` 时建立 URMA 监听。因此关闭该端口用于
  隔离当前设备能力阻塞；编译进 URMA 的二进制依然可能需要其动态库可被加载。
- 本次保存和重启只操作独立测试实例，不操作业务实例、设备或防火墙。
- 当前用户确认先完成本地验证，202/204 按本文现场执行；本机未配置这两端 SSH。
- 本文地址、系统及上次启动情况来自既有任务记录，仍需 D0 在现场重新核对。

## 可先执行的本机应用验证

在普通 Linux 用户下，使用已经构建的服务端及 CLI：

```bash
python3 tests/urma/test_tcp_deployment.py "$PWD/src/valkey-server" "$PWD/src/valkey-cli"
```

脚本只依赖 Python 标准库。它创建私有临时目录与独立测试 ACL，在回环地址的
临时端口启动本人子进程，显式关闭 URMA；检查无凭据/错误凭据拒绝、认证、
文本/空值/二进制读写、重复连接，以及 SAVE 后重启恢复。它核对连接到的 PID
属于自己启动的子进程，仅停止自己的进程，并删除自己的临时目录。
不会导入现有配置、连接业务实例、改写现有 ACL、操作网卡/设备或调用 sudo。

这是一项**会启动临时应用实例的部署冒烟测试**，与只读的环境 preflight 分开。
它拒绝 root 执行，不测试跨机网络或目标用户的现有 ACL；SDK 构建依然需要
动态库可加载。本地验证结果见 [本轮记录](validation-deployment-2026-11-09.md)。

## D0：先盘点

204 在实际部署仓库根目录运行；202 记录系统，并准备匹配的 valkey-cli：

```bash
hostname
uname -a
getconf PAGESIZE
git rev-parse HEAD
git status --short
src/valkey-server --version
src/valkey-cli --version
ldd src/valkey-server
ldd src/valkey-cli
```

204 另外执行：

```bash
test -r /home/h30086230/valkey-ub/tests/tmp/urma-acl.conf && printf 'Test ACL readable\n'
ss -ltn 'sport = :16379'
```

要求：版本可以执行、无 `not found` 动态库、ACL 可读、16379 无监听。
不打印 ACL 内容和密码。尚无新编译需求时先复用已经能加载的二进制；编译、
运行、原生设备能力分开记录，不通过换 SDK 掩盖普通部署问题。

## D1：204 启动独立测试实例

使用 Bash，在服务端终端运行。保留此终端与变量，D3 重启还要使用同一个目录。
下面使用完整的最小命令行参数，不加载 `valkey-urma-test.conf` 或其他已有配置，
避免其中的 include、复制、集群、模块或额外监听被连带启用。只引用已有测试 ACL，
不写回该 ACL 或现有配置；不对部署目录执行 make install。

```bash
baseline_dir=$(mktemp -d /tmp/valkey-tcp-baseline.XXXXXX)
baseline_acl=/home/h30086230/valkey-ub/tests/tmp/urma-acl.conf
test -r "$baseline_acl" || { printf 'Test ACL is not readable\n' >&2; exit 1; }
printf 'Baseline data and log directory: %s\n' "$baseline_dir"

src/valkey-server \
  --bind 141.61.17.204 \
  --port 16379 \
  --urma-port 0 \
  --rdma-port 0 \
  --protected-mode yes \
  --aclfile "$baseline_acl" \
  --daemonize no \
  --supervised no \
  --cluster-enabled no \
  --logfile "$baseline_dir/server.log" \
  --pidfile "$baseline_dir/server.pid" \
  --dir "$baseline_dir" \
  --dbfilename baseline.rdb \
  --save "" \
  --appendonly no
```

服务端保持前台运行。另开终端，使用刚才打印的绝对目录查看 `server.log`：
要求出现 TCP 就绪日志，无启动错误，进程在后续客户端测试期间持续运行。
该目录初始为空，避免加载其他实例的数据。不要再次启动 URMA 样例或 6380 探针。
若当前二进制不识别其中参数，应停止并核对源码提交及构建来源，不忽略错误继续。

## D2：本机与跨机 TCP 命令

先在 204 的另一终端、再在 202 各执行以下 Bash 段。需要匹配的 valkey-cli，
用户名/密码使用现有获准测试 ACL 用户，不新增或放宽用户权限。
需要该用户允许 PING、SET、GET、DEL、INFO 以及 `valkey:deploy-baseline:*` 测试 Key。
ACL 拒绝单独记录；不通过关闭 protected mode 或禁用认证绕过。

```bash
set -euo pipefail
read -r -p 'Test ACL username: ' baseline_user
read -r -s -p 'Test ACL password: ' VALKEYCLI_AUTH
printf '\n'
export VALKEYCLI_AUTH
baseline_cli=(src/valkey-cli -h 141.61.17.204 -p 16379 --user "$baseline_user" --raw -e)
baseline_key="valkey:deploy-baseline:$(hostname):$(date +%s):$$"

test "$("${baseline_cli[@]}" PING)" = PONG
test "$("${baseline_cli[@]}" SET "$baseline_key" tcp-baseline EX 600)" = OK
test "$("${baseline_cli[@]}" GET "$baseline_key")" = tcp-baseline
test "$("${baseline_cli[@]}" DEL "$baseline_key")" = 1
for baseline_attempt in {1..10}; do
  test "$("${baseline_cli[@]}" PING)" = PONG
done
"${baseline_cli[@]}" INFO server
"${baseline_cli[@]}" INFO urma
printf 'TCP baseline PING/SET/GET/DEL and ten reconnects: PASS\n'
unset VALKEYCLI_AUTH
```

每次 CLI 调用建立新 TCP 连接，因此循环覆盖重复连接。PASS 只有前面每条命令
成功才会执行；它仅代表本段检查。INFO 应确认测试端口 16379、运行版本，以及
`urma_listener_enabled:0`、`urma_transports:0`、`urma_buffer_used:0`。
`urma_compiled:1` 仅代表编译能力，并不表示启用了 URMA。

## D3：独立实例的保存和重启恢复

只在确认连接到 16379 的独立实例后执行。使用获准的测试管理用户，需有
SET/GET/DEL、SAVE、INFO 权限；仍通过环境变量或 `--askpass` 输入密码。

1. 从 202 写入一个唯一、带足够 TTL 的 Key：

   ```bash
   baseline_restore_key="valkey:deploy-baseline:restore:$(date +%s):$$"
   # 先按 D2 重新输入/导出凭据并定义 baseline_cli；记住本变量以便重启后读回。
   "${baseline_cli[@]}" SET "$baseline_restore_key" restore-check EX 3600
   "${baseline_cli[@]}" SAVE
   "${baseline_cli[@]}" INFO persistence
   ```

   SET 和 SAVE 均须返回 OK，检查 `rdb_last_bgsave_status:ok`；SAVE 自身返回值
   是本次同步保存成功的直接证据。204 确认临时目录内生成非空 `baseline.rdb`。

2. 在 204 原服务端终端按 Ctrl+C，仅停止本人启动的测试实例。确认日志正常退出。
   记录旧 PID/`run_id`，确认正常退出后，使用 D1 的同一条启动命令、同一个
   `baseline_dir` 重启；不要重新执行 mktemp。重启后核对 PID/`run_id` 已变化。

3. 202 使用相同用户名重新认证，并检查：

   ```bash
   test "$("${baseline_cli[@]}" GET "$baseline_restore_key")" = restore-check
   test "$("${baseline_cli[@]}" DEL "$baseline_restore_key")" = 1
   printf 'Standalone RDB save/restart/restore: PASS\n'
   unset VALKEYCLI_AUTH
   ```

   新启动日志须能对应到加载的测试 RDB，数据读回正确。最后在 204 原终端正常
   停止测试进程；保留目录和日志供复核。目录仅属本次测试，不覆盖业务文件。

## 回传与判定

回传 D0 输出、D1 启动日志、两端 D2 结果及 INFO、D3 保存/重启结果及日志。
不回传密码、ACL 文件内容或 Token。记录测试端口、完整 SHA 和执行时间。

只有上述现场步骤通过，才勾选 todo.md 的 D0–D3 并恢复组件适配工作。
若失败，按动态库/配置/ACL/网络/文件权限/重启恢复的实际错误处理，先建立普通
Valkey 可部署、可运行、可访问和可恢复的证据。该基线不声明 systemd、AOF、
主从、集群、生产可用性、性能或原生 URMA 验收通过。
