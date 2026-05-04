# MiniSync

MiniSync 是一个 Linux/POSIX C 实现的多进程实时文件同步与智能备份系统。客户端使用 `monitor_process -> transfer_process -> logger_process` 的多进程边界，传输进程内部使用 scheduler 线程和 worker 线程池处理任务。

## 构建

```bash
make clean
make
```

生成：

```bash
bin/minisync_client
bin/minisync_server
```

## 本地备份

```bash
./bin/minisync_client --mode local --src ./test_dir --dst ./backup_dir
```

备份目录结构：

```text
backup_dir/
├── current/
├── .versions/
└── .trash/
```

## 远程同步

服务端：

```bash
./bin/minisync_server --port 9000 --root ./server_backup
```

客户端：

```bash
./bin/minisync_client --mode remote --src ./test_dir --server 127.0.0.1 --port 9000
```

## 可选参数

客户端：

```bash
--threads 4
--block-size 4096
--hash-threshold 10485760
--debounce-ms 1500
--log ./logs/client.log
```

服务端：

```bash
--log ./logs/server.log
```

## 技术特点

- 多进程：客户端主进程 fork 出 monitor、transfer、logger 三个子进程。
- 多线程：transfer 进程包含 scheduler 线程和 worker 线程池；服务端为每个客户端连接创建线程。
- IPC：monitor 通过匿名 pipe 发送固定长度 `SyncTask`，所有客户端进程通过日志 pipe 发送 `LogMessage`。
- TCP 半包/粘包处理：协议使用定长 `SyncHeader`，header、路径和文件块都通过 `readn/write_full` 循环读写。
- inotify 递归竞态兜底：新目录创建或移动进入监听树时，立即添加 watch 并短扫描该目录，补齐 add_watch 前可能丢失的文件事件。
- 防抖合并：scheduler 对 `CREATE/MODIFY/FULLSCAN` 事件做 pending 合并，默认等待 1500ms 文件稳定后再入队。
- 原子写入：同步目标先写 `.syncing` 临时文件，hash 校验成功后再 rename。
- 跨文件系统 rename：`safe_rename_or_copy_unlink` 在 `EXDEV` 时退化为完整复制再删除源文件。
- 版本备份：覆盖前将旧文件复制到 `.versions/时间戳/相对路径`。
- 回收站：删除不会直接移除备份文件，而是移动到 `.trash/时间戳/相对路径`。
- Hash：内置 FNV-1a 64 位 hash，无需 OpenSSL。
- 大文件：超过阈值后按块传输，并先查询服务端 `.syncing` 大小实现断点续传。

## 验收示例

本地新增：

```bash
echo "hello" > ./test_dir/a.txt
```

期望：

```text
backup_dir/current/a.txt
logs/client.log
```

远程新增：

```bash
echo "remote hello" > ./test_dir/r.txt
```

期望：

```text
server_backup/current/r.txt
logs/server.log
```

大文件：

```bash
dd if=/dev/urandom of=./test_dir/big.bin bs=1M count=20
```

日志中应出现 `strategy=large-file`、`resume offset=` 和 hash 信息。

## 工程目录树

```text
MiniSync/
├── Makefile                         # 项目构建脚本，生成 bin/minisync_client 和 bin/minisync_server
├── README.md                        # 项目说明、构建方式、运行方式、技术特点和目录说明
├── config/
│   └── minisync.conf                # MiniSync 默认配置示例，记录推荐参数和默认路径
├── include/
│   ├── common.h                     # 全局常量、公共宏、ClientConfig/ServerConfig 和通用函数声明
│   ├── file_utils.h                 # 文件系统工具接口：建目录、原子复制、版本备份、回收站、路径安全检查
│   ├── hash.h                       # FNV-1a 64 位 hash 接口，支持 buffer、整文件和文件区间 hash
│   ├── logger.h                     # 日志级别、LogMessage 结构体和 logger_process/log_message 声明
│   ├── monitor.h                    # 目录监控模块接口，包含 watch 映射和递归扫描函数声明
│   ├── net_utils.h                  # 网络与可靠读写接口：readn/writen、协议头收发、socket 创建
│   ├── protocol.h                   # MiniSync TCP 协议定义：魔数、消息类型和 SyncHeader
│   ├── server.h                     # 服务端运行、客户端连接处理和远程文件操作接口
│   ├── task.h                       # 文件事件类型和 SyncTask 任务结构体定义
│   ├── task_queue.h                 # 线程安全任务队列结构体与 push/pop/stop 接口
│   └── transfer.h                   # 传输进程、pending 防抖表、scheduler/worker 线程接口
├── src/
│   ├── client_main.c                # 客户端入口：解析参数、创建 pipe、fork monitor/transfer/logger 子进程
│   ├── file_utils.c                 # 文件工具实现：递归 mkdir、原子复制、EXDEV rename 兜底、版本和回收站
│   ├── hash.c                       # FNV-1a 64 位 hash 实现
│   ├── logger.c                     # 独立日志进程与日志格式化写入实现
│   ├── monitor.c                    # inotify 递归监控、初始扫描、新目录竞态短扫描和轮询兜底
│   ├── net_utils.c                  # TCP/pipe 可靠读写、协议头收发、客户端/服务端 socket 工具
│   ├── server.c                     # 服务端核心：accept 多客户端、协议接收、版本备份、回收站、断点续传
│   ├── server_main.c                # 服务端入口：解析 --port/--root/--log 并启动 run_server
│   ├── task_queue.c                 # pthread mutex/cond 实现的线程安全任务队列
│   └── transfer.c                   # 客户端传输引擎：防抖合并、线程池、本地同步和远程同步
├── bin/
│   ├── minisync_client              # make 生成的客户端可执行文件
│   └── minisync_server              # make 生成的服务端可执行文件
├── build/
│   ├── client/                      # 客户端目标文件目录，make 编译时自动生成
│   └── server/                      # 服务端目标文件目录，make 编译时自动生成
├── logs/
│   ├── client.log                   # 本地模式客户端运行日志
│   ├── client_remote.log            # 远程模式客户端验证日志
│   ├── client_large.log             # 大文件/分块传输验证日志
│   ├── server.log                   # 服务端运行日志
│   └── server_large.log             # 大文件/分块传输服务端验证日志
├── test_dir/
│   └── ...                          # 客户端监控源目录，用户在这里创建、修改、删除文件
├── backup_dir/
│   ├── current/                     # 本地模式当前最新备份文件
│   ├── .versions/                   # 本地模式覆盖前旧版本备份
│   └── .trash/                      # 本地模式删除文件回收站
└── server_backup/
    ├── current/                     # 远程服务端当前最新备份文件
    ├── .versions/                   # 远程服务端覆盖前旧版本备份
    └── .trash/                      # 远程服务端删除文件回收站
```
