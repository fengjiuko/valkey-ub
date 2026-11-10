# 202 / 204：从代码拉取到 Valkey 部署验证

日期：2026-11-10。起点按用户最新说明：**204 仅拉取/更新代码，202 仅拉取代码**。
本指南不假定已编译程序、已有 ACL 或已有运行实例；现场结果尚未验收。

## 1. 范围和执行顺序

| 主机 | 本轮角色 | 要完成的工作 |
| --- | --- | --- |
| 204 | 服务端 | 编译服务端与 CLI、本机冒烟、独立 TCP 实例、RDB 保存和重启 |
| 202 | 客户端 | 编译 CLI、跨机认证读写、重启后读回测试数据 |

以下地址沿用此前任务记录，先在现场核对：204 为 `141.61.17.204`，202 为
`141.61.17.202`。架构、系统、页大小和路径以本次输出为准。

顺序：两端只读检查 → 各自独立构建 → 204 本机冒烟 → 204 启动服务 →
204 本机和 202 跨机访问 → 202 写入/保存 → 204 重启 → 202 读回。

- 普通 Linux 账户执行，使用 Bash；下文同一终端的变量需要保留。
- 本轮验证普通 TCP，构建关闭 URMA、RDMA 和 TLS，不要求安装 UMDK/URMA SDK。
- 检查阶段只读；之后会在本用户新建的临时目录编译、创建测试 ACL 和数据、启动测试应用。
- 不安装系统软件，不运行 sudo、make install、systemctl、sysctl、ulimit 设置，
  不修改 NIC、RDMA/RXE、驱动、路由、防火墙或其他人的进程。
- 不加载已有服务配置，不使用共享 ACL；测试账户只允许本轮 Key 前缀和必要命令。
- 不执行 FLUSHALL、CONFIG REWRITE、ACL SAVE 或业务实例的 SAVE/SHUTDOWN。

本文发布在独立分支 `docs/deploy-202-204-tcp`，基于
`work/urma-native-design` 的 `6818fdb938b0561441782d068c00ecdc861e5571`。
新分支只新增本文，不改变原分支。可以在网页阅读本文，两端不必切换现有分支；
下文仅 fetch 固定源码，再导出到新目录构建，不修改原工作区和已有二进制。

## 2. 两端：只读环境检查

分别在两台机器已拉取的仓库根目录、新开的 Bash 终端执行：

```bash
set -euo pipefail
test "$(id -u)" -ne 0 || { printf '请使用非 root 账户\n' >&2; exit 1; }
deployment_repo=$(git rev-parse --show-toplevel)
cd "$deployment_repo"
date -Is
hostname
id
uname -a
getconf PAGESIZE
git branch --show-current
git rev-parse HEAD
git status --short
for deployment_tool in git make cc ar ranlib tar python3 ldd ss ip sha256sum timeout; do
  command -v "$deployment_tool"
done
python3 --version
python3 -c 'import sys; assert sys.version_info >= (3, 7), "需要 Python 3.7+"'
cc --version
make --version
ip -j address show
```

任一必需工具缺失或命令失败，先停止并记录。由环境管理方准备工具链，或使用
已获准的用户目录工具链；本文不提供共享主机的软件安装命令。标准 C 开发头文件
和链接工具还要以实际编译结果确认，command -v 不能证明开发环境完整。
原工作区有修改时保留，不做 reset、clean、stash 或切换分支。

## 3. 两端：取得固定源码并创建独立构建目录

继续在各自同一终端执行。本步骤开始写入本人的 Git 对象库和新建临时目录，
不属于只读检查；不会更新现有本地分支或工作文件。

```bash
git fetch --no-tags https://github.com/fengjiuko/valkey-ub.git work/urma-native-design
deployment_source=6818fdb938b0561441782d068c00ecdc861e5571
git cat-file -e "${deployment_source}^{commit}"
umask 077
deployment_root=$(mktemp -d "${TMPDIR:-/tmp}/valkey-deploy-202204.XXXXXX")
mkdir "$deployment_root/source" "$deployment_root/evidence"
git archive "$deployment_source" | tar -x -C "$deployment_root/source"
printf '%s\n' "$deployment_source" > "$deployment_root/evidence/source-sha.txt"
printf '本机本次构建目录，请记住：%s\n' "$deployment_root"
```

fetch 失败时不要继续。内网不能访问 GitHub，可使用已获准、包含该完整提交的
本地镜像；先用 git cat-file 确认对象存在，再执行上述导出步骤，记录镜像来源。
所有 make 都针对新目录，不需要对原仓库执行 make distclean。

### 3.1 仅 204：编译服务端和 CLI

```bash
make -C "$deployment_root/source" -j2 \
  MALLOC=libc BUILD_URMA=no BUILD_RDMA=no BUILD_TLS=no \
  2>&1 | tee "$deployment_root/evidence/build-204.log"
deployment_server="$deployment_root/source/src/valkey-server"
deployment_cli="$deployment_root/source/src/valkey-cli"
"$deployment_server" --version
"$deployment_cli" --version
ldd "$deployment_server" | tee "$deployment_root/evidence/ldd-server.txt"
ldd "$deployment_cli" | tee "$deployment_root/evidence/ldd-cli.txt"
if grep -q 'not found' "$deployment_root"/evidence/ldd-*.txt; then
  printf '动态库缺失，停止部署\n' >&2; exit 1
fi
sha256sum "$deployment_server" "$deployment_cli" | tee "$deployment_root/evidence/binaries.sha256"
```

### 3.2 仅 202：编译 CLI

```bash
make -C "$deployment_root/source/src" -j2 valkey-cli \
  MALLOC=libc BUILD_URMA=no BUILD_RDMA=no BUILD_TLS=no \
  2>&1 | tee "$deployment_root/evidence/build-202.log"
deployment_cli="$deployment_root/source/src/valkey-cli"
"$deployment_cli" --version
ldd "$deployment_cli" | tee "$deployment_root/evidence/ldd-cli.txt"
if grep -q 'not found' "$deployment_root/evidence/ldd-cli.txt"; then
  printf '动态库缺失，停止部署\n' >&2; exit 1
fi
sha256sum "$deployment_cli" | tee "$deployment_root/evidence/binaries.sha256"
```

要求：各自编译退出 0、版本命令能执行、动态库无 not found。202 不需要运行服务端。
使用 libc 是为减少本次构建依赖，不代表完成 jemalloc 或性能验收。
归档构建的 --version 可能显示 sha=00000000；用 source-sha.txt 和二进制哈希追溯来源。

## 4. 仅 204：自动验证本机应用路径

```bash
python3 "$deployment_root/source/tests/urma/test_tcp_deployment.py" \
  "$deployment_server" "$deployment_cli" \
  2>&1 | tee "$deployment_root/evidence/local-smoke-204.log"
```

要求所有项目 PASS、命令退出 0。脚本另建自己的临时目录/ACL/回环端口，检查：
无凭据/错误凭据拒绝，正确凭据认证，文本/空值/65540 字节二进制读写，十次重连，
SAVE 后新进程恢复同一二进制值，以及 URMA 未启用。它只停止自己的子进程，
结束时删除自己的目录。它不启动本指南后续的跨机实例，不验证跨机网络。

## 5. 204：创建跨机测试专用 ACL

继续使用 204 原终端，称为“204 终端 A”。只在本轮私有目录创建新的 ACL，
不引用或改写此前 /home/... 路径下的 ACL。交互输入一个至少 16 字符的测试密码，
并在 202/204 客户端输入同一个密码；不在命令行参数、聊天或证据日志中发送密码。
输入时不要启用 set -x。

```bash
mkdir "$deployment_root/data"
python3 - "$deployment_root/users.acl" <<'PY'
import getpass
import hashlib
import os
import sys

password = getpass.getpass("新的部署测试密码（至少 16 字符）: ")
if len(password) < 16 or password != getpass.getpass("再次输入: "):
    raise SystemExit("密码长度不足或两次输入不一致，未创建 ACL")
digest = hashlib.sha256(password.encode()).hexdigest()
fd = os.open(sys.argv[1], os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, "w") as acl:
    acl.write("user default off\n")
    acl.write("user deploy-baseline on #" + digest +
              " ~valkey:deploy-baseline:* +ping +set +get +del +save +info\n")
print("已创建私有测试 ACL；未打印密码或散列")
PY
```

ACL 用户为 deploy-baseline。ACL 含密码散列，也不要回传该文件。
此测试为普通明文 TCP 认证，只在获准的测试网络使用；不作为加密传输验收。

## 6. 204 终端 A：启动跨机独立实例

确认本机确有服务端地址。只使用 16379，若被占用则停止，不终止已有进程；
需要换端口时，后续所有客户端也必须同步修改。

```bash
deployment_server_ip=141.61.17.204
deployment_port=16379
deployment_listeners=$(ss -H -ltn "sport = :$deployment_port")
test -z "$deployment_listeners" || { printf '测试端口已占用，停止\n' >&2; exit 1; }
start_deployment_server() {
  "$deployment_server" \
    --bind "$deployment_server_ip" --port "$deployment_port" \
    --urma-port 0 --rdma-port 0 --cluster-enabled no \
    --protected-mode yes --aclfile "$deployment_root/users.acl" \
    --daemonize no --supervised no \
    --dir "$deployment_root/data" --dbfilename baseline.rdb \
    --save "" --appendonly no \
    --logfile "$deployment_root/server.log" --pidfile "$deployment_root/server.pid"
}
printf '服务端目录：%s；测试地址：%s:%s\n' "$deployment_root" "$deployment_server_ip" "$deployment_port"
start_deployment_server
printf '本人测试实例退出码：%s\n' "$?"
```

启动后前台保持运行，终端 A 不再输入其他命令。另开 204 终端 B 查看同一目录的
server.log，要求出现 TCP Ready to accept connections、无启动失败。
这里只是进程启动，仍须完成下面的认证、数据与重启验证。
不要把端口改成已有业务端口，也不要加载原有 valkey-urma-test.conf。

## 7. 204 终端 B、202：认证与读写

先在 204 终端 B，随后在 202 各执行一次。输入的是**当前这台机器第 3 步打印的
构建目录**，202 不要填 204 的目录。202 保留此终端、变量和凭据，供第 8 步使用。

```bash
set -euo pipefail
read -r -p '本机本次构建目录（绝对路径）: ' deployment_root
deployment_cli="$deployment_root/source/src/valkey-cli"
test -x "$deployment_cli"
deployment_server_ip=141.61.17.204
deployment_port=16379
read -r -s -p '部署测试密码: ' VALKEYCLI_AUTH
printf '\n'
export VALKEYCLI_AUTH
deployment_cli_cmd() {
  timeout 10s "$deployment_cli" -h "$deployment_server_ip" -p "$deployment_port" \
    --user deploy-baseline --raw -e "$@"
}
deployment_key="valkey:deploy-baseline:$(hostname):$(date +%s):$$"
test "$(deployment_cli_cmd PING)" = PONG
deployment_cli_cmd INFO server | tee "$deployment_root/evidence/server-before.txt"
tr -d '\r' < "$deployment_root/evidence/server-before.txt" | grep -qx "tcp_port:$deployment_port"
deployment_cli_cmd INFO urma | tee "$deployment_root/evidence/urma.txt"
for deployment_field in urma_compiled urma_listener_enabled urma_transports urma_buffer_used urma_initialization_failures; do
  tr -d '\r' < "$deployment_root/evidence/urma.txt" | grep -qx "$deployment_field:0"
done
test "$(deployment_cli_cmd SET "$deployment_key" tcp-check EX 600)" = OK
test "$(deployment_cli_cmd GET "$deployment_key")" = tcp-check
test "$(deployment_cli_cmd DEL "$deployment_key")" = 1
for deployment_attempt in {1..10}; do
  test "$(deployment_cli_cmd PING)" = PONG
done
printf 'PASS：%s 到 204 的认证、读写删除、十次重连\n' "$(hostname)" \
  | tee "$deployment_root/evidence/client-result.txt"
```

核对 INFO server 的 process_id 与 204 本次测试启动日志的 PID 一致，再执行 SAVE。
本次普通构建要求 urma_compiled=0；若为 1，应核对是否误用了旧二进制。
这些命令每次建立新连接。CLI 的 -e 使服务端错误返回非零，timeout 限制单次等待，
set -euo pipefail 防止前面失败后继续打印 PASS。204 终端 B 完成后可执行
`unset VALKEYCLI_AUTH`；202 留到第 8 步结束再清除。

## 8. 202 写入并保存 → 204 重启 → 202 读回

### 8.1 仅 202，在第 7 步原终端

确认目标是 204 本次 16379 独立实例后执行。SAVE 只保存本次私有数据目录：

```bash
deployment_restore_key="valkey:deploy-baseline:restore:$(date +%s):$$"
deployment_before_run=$(deployment_cli_cmd INFO server | tr -d '\r' | sed -n 's/^run_id://p')
test -n "$deployment_before_run"
test "$(deployment_cli_cmd SET "$deployment_restore_key" restore-check EX 3600)" = OK
test "$(deployment_cli_cmd SAVE)" = OK
deployment_cli_cmd INFO persistence | tee "$deployment_root/evidence/persistence-before.txt"
printf '保存前 run_id=%s\n' "$deployment_before_run" \
  | tee "$deployment_root/evidence/save-result.txt"
```

SET 和 SAVE 必须均为 OK。SAVE 返回值是本次同步保存成功的直接证据；
INFO persistence 用于辅助留档，不能仅凭旧的 bgsave 状态判定本次 SAVE 成功。

### 8.2 仅 204 终端 A

在本人启动服务的前台终端按 Ctrl+C。要求正常退出、退出码 0；确认日志中正常
关闭的是本人实例。不要使用 pkill/killall 或按端口杀进程。
在该终端继续执行，复用原来的函数和变量：

```bash
test -s "$deployment_root/data/baseline.rdb"
tail -n 25 "$deployment_root/server.log"
start_deployment_server
printf '本人重启实例退出码：%s\n' "$?"
```

不要重新执行 mktemp、mkdir、创建 ACL 或整个第 6 步；保持同一个数据目录。
待日志出现加载 RDB 并 TCP 就绪后，再在 202 执行下一段。

### 8.3 仅 202，第 7 步原终端

```bash
test "$(deployment_cli_cmd PING)" = PONG
deployment_after_run=$(deployment_cli_cmd INFO server | tr -d '\r' | sed -n 's/^run_id://p')
test -n "$deployment_after_run"
test "$deployment_before_run" != "$deployment_after_run"
test "$(deployment_cli_cmd GET "$deployment_restore_key")" = restore-check
test "$(deployment_cli_cmd DEL "$deployment_restore_key")" = 1
deployment_cli_cmd INFO server | tee "$deployment_root/evidence/server-after.txt"
printf 'PASS：新进程从 RDB 恢复测试值；run_id=%s\n' "$deployment_after_run" \
  | tee "$deployment_root/evidence/restore-result.txt"
unset VALKEYCLI_AUTH
```

必须在一小时 TTL 内完成；超时则本次恢复检查无效，重新建立一次独立测试记录。
最后在 204 终端 A 按 Ctrl+C，确认第二个本人实例也正常退出。保留两个主机各自
本轮目录和日志用于复核；不自动删除目录，不把 ACL、RDB 或整个目录打包回传。

## 9. 验收清单和失败处理

| 项目 | 需要的现场证据 | 当前状态 |
| --- | --- | --- |
| 环境与构建 | 两端系统/用户/源码 SHA；204 服务端+CLI、202 CLI 编译退出 0 | 待执行 |
| 二进制可用 | 版本、动态库、二进制 SHA-256 | 待执行 |
| 204 本机冒烟 | local-smoke-204.log 全部 PASS、退出 0 | 待执行 |
| 204 独立启动 | 本人 PID、指定 IP/端口就绪、URMA/RDMA 关闭 | 待执行 |
| 本机和跨机 | 两端认证、读写删除及十次重连 PASS，INFO 属于本人实例 | 待执行 |
| 保存和恢复 | SAVE OK、非空 RDB、正常退出、新 run_id、202 读回一致 | 待执行 |
| 最终停止 | 第二次本人实例正常退出，未影响其他实例 | 待执行 |

回传上述 evidence 下的文本、204 启动/重启日志和环境记录，注明主机与执行时间。
不回传密码、密码散列、ACL、Token 或私钥。所有检查通过才确认本轮普通部署可用。

- 编译失败：保留首个编译错误及构建日志；先解决工具链/头文件/链接问题。
- 版本命令失败或库缺失：核对是否使用本轮绝对路径；不要通过换 SDK 掩盖普通构建问题。
- 地址无法绑定或端口占用：用只读 ip/ss 核对地址和监听，停止本次启动，不改设备或已有进程。
- 202 超时/拒绝连接：先看 204 日志和 ss，再核查两端实际地址及获准网络策略；不自行改防火墙。
- NOAUTH/WRONGPASS/NOPERM：核对专用用户、输入密码和 Key 前缀，不关闭认证或扩大共享 ACL。
- 恢复失败：核对 SAVE 返回值、同一数据目录、TTL、日志和 run_id，不能仅以 PING 成功判通过。

本文不是性能、生产托管、systemd、AOF、主从/集群、原生 URMA 或远端 Value 验收。
普通部署通过后，再单独进行 SDK/provider/驱动/硬件组合归档和原生 URMA 双机验证。
已有本地结果仅供参考：[2026-11-09 本地记录](../../tasks/validation-deployment-2026-11-09.md)。

## 10. 本文命令的本机核验范围

2026-11-10 已在非 root WSL/x86_64 环境核验 11 段 Bash 示例语法、ACL Python
语法和本地文档链接；按第 3 步固定源码分别新建目录，202 的 CLI-only 构建、
204 的完整构建均退出 0，版本、动态库和哈希命令正常。
随后执行了第 4 步冒烟，并用本文 ACL 生成代码、启动参数、客户端检查及
SAVE/重启/恢复命令完成隔离实例验证，均通过。

流程核验仅将目标地址改为本机 127.0.0.1 和临时空闲端口，测试密码由核验程序
在内存中提供；没有登录 202/204，也没有验证现场地址、权限或网络。
因此第 9 节所有现场项目继续保持待执行，不能以这项本机核验代替现场记录。
