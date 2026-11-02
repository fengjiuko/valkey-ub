# RDMA / UB 环境检查与验证资料

当前交付是 **非 root 用户可执行的环境预检脚本、检查说明和后续准入方案**。它没有增加 Valkey 的原生 URMA 后端，也不代表 RDMA/UB 兼容性已经验证。

- [只读预检脚本](../../utils/rdma_preflight.py)
- [脚本回归测试](../../tests/rdma/test_preflight.py)
- [功能、故障恢复、可观测性及维护证据准入方案](validation-plan.zh-CN.md)
- [本地尝试与复核摘要](local-results-2026-11-02.zh-CN.md)

## 共享主机上的使用边界

直接使用普通测试账户运行，无需 sudo。默认只查询系统信息和文件元数据：

- 不安装软件、不编译、不加载/卸载模块、不改变内核、sysctl、ulimit 或服务状态。
- 不创建、删除、启停或重配 NIC、RXE、RDMA、GID、IP、路由及其他共享设备。
- 不执行被检目录内的 `valkey-server`、`rdma-test` 或其他应用程序；存在和执行权限检查不等同于程序可运行。
- 不启动服务、绑定端口、建立 CM 连接、创建 QP、注册 MR 或发起数据流量。
- 不导入第三方 `netifaces` 模块，仅检查模块能否被发现；禁用 Python 字节码写入。
- 不自动提权、不修改权限、不调用 chmod/chown。权限不足保留为阻塞或未知证据，交由环境管理方判断。

只有显式指定 `--output` 才创建报告文件；目录必须已经存在，文件不得已存在。文件以 `0600` 模式创建（最终权限仍受文件系统语义约束）。不指定此参数时只向终端输出。脚本只执行白名单中的查询形式，包括 `rdma link show`、`ip -j address show`、`ldconfig -p`、版本/包信息查询及 `git rev-parse HEAD`；不会执行裸 `ldconfig` 或任意用户命令。

默认不运行 `ibv_devinfo`。**可选** `--query-verbs` 会以当前账户运行 `ibv_devinfo`，打开短暂的设备查询上下文并退出；不会创建 QP/MR、建链或产生业务流量。若共享环境要求完全不打开 RDMA 设备上下文，不使用此选项即可。脚本仍需在可信的系统工具和 Python 环境中运行。

## 使用方法

在本仓库根目录执行，Python 3 仅需标准库。

先检查构建依赖（不触发构建）：

```bash
python3 -B utils/rdma_preflight.py --repo . --stage build
```

已有设备与测试二进制时，检查运行前置条件；替换设备及本机 IP：

```bash
python3 -B utils/rdma_preflight.py --repo . --stage run \
  --device mlx5_0 --ib-port 1 --bind-ip 192.0.2.10
```

示例 IP 是文档占位，不应直接照用。`--ib-port` 是设备端口，不是 Valkey listener 端口。单设备可自动选择，多设备必须明确指定。默认 `--stage all` 合并构建与运行检查。

如需留档，在命令末尾加 `--output ./preflight-run.json`，使用自己有写权限的新文件路径；文件可能包含主机设备、地址、版本和本地路径信息，分享前需审阅。原始本机报告不作为仓库交付内容。

Windows 用户通过已有 WSL 执行，例如在本地工作目录结构下：

```powershell
wsl -d Ubuntu -- python3 -B /mnt/d/valkey/valkey-ub/utils/rdma_preflight.py --repo /mnt/d/valkey/valkey --stage all
```

该示例检查原有 `valkey` 源码目录；请按实际检出路径修改脚本和 `--repo` 参数。

## 检查内容与结果

记录：源码 SHA、操作系统/内核、当前 UID/EUID/组、memlock 限制、构建工具和 RDMA 库/头文件、设备/端口状态、link layer、GID/地址映射、固件、内核驱动和 provider 配置。内核驱动不等于用户态 provider，精确 provider 版本和应用实际选中后端仍需另行确认。

设备权限绑定所选设备对应的 uverbs 节点，不以其他设备的权限代替。目录枚举被拒绝、节点缺失、节点存在但当前用户无权限会分别保留证据。无法证明所选端口的地址映射时不会自动通过。

| 退出码 | 含义 |
| --- | --- |
| 0 | 所选阶段没有已识别阻塞；仍需查看 warn，不代表 RDMA 通信通过 |
| 2 | 环境阻塞，包括依赖、设备、权限、地址或尚未构建的程序 |
| 1 | 脚本或报告写入错误，例如已有报告拒绝覆盖 |

输出 `pass/block/warn/info` 分别表示已核对前置项、阻塞、待核验证据、记录值。build 阶段通过不代表已构建；run 阶段通过不代表建链、数据正确性、设备互操作、GPU Direct、故障恢复或性能通过。特殊 sysfs 布局、自定义库/头文件路径等应人工核查，不能将检查无法识别直接解释为硬件不支持。

## 与上游测试脚本的区别

`tests/rdma/rdma_env.py` 负责 RXE setup/cleanup，`runtest-rdma --install-rxe` 也会配置环境。它们会修改设备，不属于本次共享主机的只读检查入口。`runtest-rdma` 还会构建、启动服务、执行测试和清理运行目录，同样不由预检脚本调用。

本交付不提供共享环境的安装/建卡/清理自动化。后续功能测试应在获得测试窗口、专用资源及明确范围后单独执行。

## 回归检查

普通账户运行：

```bash
python3 -B tests/rdma/test_preflight.py
```

测试只在当前用户的临时目录创建私有 fixture/报告，不改变系统设备，覆盖命令白名单、防止执行被检程序、所选设备映射、权限区分和报告独占创建。真实 RDMA 硬件的通过分支仍需独立验证。
