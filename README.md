# lfm_stra

C++ 因子、触发和预测。行情按 `global_index` 的到达顺序逐笔推进，触发成立时用当时已经写入的因子做预测，过阈值后再看账本和场景的买入笔数上限。

本目录是交给同事的外壳。因子、触发、模型三个库已经编成 so，外壳仍是源码，可以改运行方式、配置、买入上限和输出，不必重编那三个库。重编 so 仍在 `sp_cpp/cell/base/lfm/`，用 `make -f Makefile.libs`。

## 交付时怎么分

| 内容 | 形式 | 同事要不要改 |
|---|---|---|
| `liblfm_factor.so` | 已编译 | 不改。盘前因子和 snap、trans、ot、otd、sot 的公式在这里 |
| `liblfm_trigger.so` | 已编译 | 不改。各场景的触发条件在这里 |
| `liblfm_model.so` | 已编译 | 不改。读模型参数、`dlopen` 三棵树、算均值并和阈值比较 |
| `lfm_strategy_main.cpp`、`lfm_strategy.cpp`、`lfm_parq_stra.cpp`、`csv_table.cpp` | 源码 | 改启动参数、回放和实盘循环、买卖笔数、写结果 |
| 对应头文件 | 源码，必须留着 | 外壳要包含它们才能链接 so。改类的成员布局会使已编好的 so 对不上 |

三个 so 和 `lfm_stra` 放在同一目录。外壳用 `-Wl,-rpath,'$ORIGIN'` 链接，换机器时不要把 so 挪到别的目录。

`csv_table.cpp` 在外壳里编一份，因子 so 和触发 so 里也各有一份。只改外壳这份并重新 `make`，so 内部的读表逻辑不会变。

本目录已经放好三个 so。在这里执行 `make` 只编外壳 `lfm_stra`，头文件从 `sp_cpp/cell` 取。缺 so 时 make 会停下来。`make clean` 只删除 `lfm_stra`，不删除 so。

## 运行

```text
lfm_stra <交易日> [replay|live] [other_config.json] [live_config.json] [账本] [截止时间]
```

不写模式时是 `live`。账本不写时是 `DE`。`other_config.json`、`live_config.json` 不写时用当前目录下的同名文件。

账本是大写字母，可多选，例如 `D`、`E`、`DE`。重复字母会报错。程序会先排序，所以 `ED` 和 `DE` 是同一次运行，输出目录都是 `DE`。一个进程里跑所选的全部账本，不是每个账本起一次进程。

截止时间是 `HHMMSSmmm`，例如 9:33:00 写成 `93300000`。行情时间大于这个值就停。不写或写 `0` 表示把 `global_index` 读完。`sim_md` 不使用这个截止时间，它仍会把当天行情装进共享内存；截断只发生在 `lfm_stra` 里。

例子：

```bash
lfm_stra 20261015 replay ./other_config.json ./live_config.json DE 93300000
```

`lfm_backtest.sh` 先跑 `sim_md` 装共享内存，再以 `replay` 启动上面的命令。参数顺序是交易日、other_config、live_config、账本、截止时间。

Ctrl+C 或 `SIGTERM` 会把停止标志置上，当前循环退出后写出样本和因子。第二次信号恢复默认处理并再次抛出。`SIGKILL` 来不及写结果。

## 配置

`other_config.json` 三个路径都是字符串，不能空：

```json
{
  "daily_input_path": "/bigdata/shipan/param",
  "lfm_input_path": "/bigdata/shipan/param/lfm_cpp_dir",
  "output_path": "/bigdata/shipan/prod/test"
}
```

- `daily_input_path` 后面再拼交易日，得到当天的盘前文件和 `交易日_trade_config_D.json` 这类配置。
- `lfm_input_path` 不再拼日期。这里放 `model_param_D.json`、`model_param_E.json`，以及参数里点名的 `m_场景_a56_2025_0.so` 等三棵树。
- `output_path` 后面再拼交易日和排序后的账本，例如 `output_path/20261015/DE/`。

`live_config.json` 目前只读策略编号：

```json
{ "strategy_id": 2 }
```

编号用来占锁 `/dev/shm/cell_strategy_<id>.lock`，实盘时也用来过滤本策略的订单和持仓。因子值与这个编号无关。同一台机器上同时跑两份时要用不同编号。

当天交易配置是 `daily_input_path/交易日/交易日_trade_config_<账本>.json`。程序读这三项：

- `univ_list`：这个账本要跑的场景，顺序会记到样本的 `scenario_order`。
- `scenarios`：每个场景的买入笔数上限。
- `target_tot_num`：这个账本的总买入笔数上限。

同一场景出现在两个选中的账本里会直接报错。`single_amt`、`total_buy_amt` 不参与判断。

模型参数是 `lfm_input_path/model_param_<账本>.json`。每个场景有 `name`、恰好 3 个 `models`、一个 `threshold`。预测值是三棵树的均值，均值大于阈值则 `passed=1`。

## 行情怎么走进策略

`lfm_strategy` 继承框架的 `base_strategy`，但不使用基类按股票地图轮询最新快照的 `process_md`。回放和实盘都从 `global_index` 逐条取记录，再按记录里的下标去三块共享内存读这一笔：

- `SIM_UniOrder.md`
- `SIM_UniTrans.md`
- `SIM_MDStockSnap.md`

回放把队列从下标 0 读到空，或读到截止时间。实盘在 `work()` 里同样调用这段读取，只是队列暂时为空时会睡 1 毫秒。因此实盘和回放看到的逐笔顺序一致。

每一笔的处理在 `lfm_parq_stra`：

1. 委托、成交、快照到达后立刻写入因子，不等这一毫秒结束。
2. 问触发器这次有没有命中。`cf7_i`、`cf8_i` 先算，其它场景后算。
3. 命中后算因子行，再调用模型。
4. `passed` 为 1，并且该场景已买笔数小于场景上限，并且该账本已买笔数小于 `target_tot_num`，才把 `bought` 记为 1。两个计数都加 1。
5. 名额按触发到达的先后占用，不按预测值排序。超限的触发仍会留下样本，`bought` 为 0。

D 和 E 各有一份已买笔数，互不影响。因子计算不看这两个上限。

## 三个 so 里有什么

因子库按固定顺序拼一行，长度 930，改顺序会使已编好的模型 so 对不上：

| 段 | 列数 | 起始下标 |
|---|---:|---:|
| 盘前 `s_last_a` | 144 | 0 |
| snap | 94 | 144 |
| trans | 49 | 238 |
| ot | 111 | 287 |
| otd | 479 | 398 |
| sot2 | 53 | 877 |

盘口金额按浮点 `量 × 价 × 0.0001` 计算。成交额仍按 32 位累加，和本地 HDF 的 `TotalAmount * 10` 对齐。实盘快照的 `turnover` 先除 `100000`，回放 HDF 时除数设为 1。

触发库按场景调用三组实现：

- `univ_trigger`：`taaoc3_i`、`tchbt2_i`、`tchbt8_i`、`tchn20_i`、`tchno20_i`、`tchta40_i`。委托和成交都看。
- `brk_trigger`：`brk1_i`、`brk3_i`、`brk7_i`、`brk8_i`、`brk16_i`。只看成交，初始化需要 `lastamtul`。
- `cf_trigger`：`cf7_i`、`cf8_i`。只看成交。

模型库不包含树。树是 `lfm_input_path` 下单独的 `m_<场景>_a56_2025_{0,1,2}.so`，导出函数名和文件名相同，入口是 `double*`。

## 输出

写到 `output_path/交易日/账本/`：

- `交易日_trigger_sample.csv`：股票、场景、触发下标、触发时间、实际时间、委托条数、成交条数、预测值、场景序号、是否买入。
- `交易日_factor.csv`：每一条触发当时的 930 列因子。

样本里的委托条数、成交条数是当时已经写入因子的条数。本地 `basic_daily` 里的 `order_data_idx`、`trade_data_idx` 是原始数组下标。两边对齐时，C++ 条数等于本地下标加 1。

## 同事改外壳时注意

可以改：命令行、两个 json 的读取、账本选择、截止时间、买入笔数条件、样本和因子的写出、停止信号处理、实盘和回放循环。

不要在没有三个库源码的情况下改这些头文件里的类成员顺序和因子下标：`factor_manager.h`、`trigger_manager.h`、`model_manager.h`、`trigger_recorder.h`、`lfm_types.h`。so 是按现在的布局编的。

因子公式、触发条件和三棵树的预测在 so 里。要改这些，需要对应源码并重新 `make -f Makefile.libs`，再重新链接外壳。
