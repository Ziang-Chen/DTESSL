# DTESSL v0.0.1

DTESSL（Discrete-Time Event System Simulation Language，戴特赛尔）是一个独立的、
确定性的离散时间事件系统建模语言。它不依赖 ChenIR、ChenFlow 或 ChenVM；当前参考实现
是无第三方依赖的 C++20 库与命令行工具。

版本采用 `v里程碑.大特性.小特性`，不是 SemVer。完整设计、版本路线和当前证据分别见：

- [语言总设计](docs/LANGUAGE_DESIGN.md)
- [版本规则](docs/VERSIONING.md)
- [实施路线](docs/ROADMAP.md)
- [Goal contract](docs/GOAL_CONTRACT.md)
- [项目 QA/QC](docs/PROJECT_QC.md)
- [研究依据与项目推论](docs/REFERENCES.md)
- [变更记录](CHANGELOG.md)

## v0 的闭环

一个程序由两类定义组成：

- `state` 定义一个有类型的状态空间、初值、上下文和不变量；
- `transition` 定义一次事件能够引起的整体变化，以及变化后建议宿主执行的调用 DAG。

执行器以一组可并行事件为一个离散 `round`。同一 round 的 transition 都读取同一个
before snapshot；写集不冲突时原子合并，冲突而没有显式 merge relation 时拒绝整组事件。
因此 round 是模拟批次，不是“每个 transition 自增一次”的全局逻辑时钟。引擎按以下顺序工作：

1. 用当前状态和事件参数求值 `where`；
2. 要求恰好一个 transition 可用，否则拒绝事件；
3. 根据 `to` 计算候选新状态；
4. 检查目标 state 的全部 `invariant`；
5. 生成 `do` 的调用 DAG，然后原子提交逻辑状态。

`$name(...)` 不会在解释器内执行。它只是一个带类型实参、上下文和依赖边的外部调用
记录。宿主以后可以向这些名字注入 chenRT IPC 或其他接口；DTESSL 本身没有环境时钟、
文件系统、网络和进程权限。

## 最小语法

```text
state Scheduler @ local initial:
  mode: string = "Idle"
  credits: int = 2
  workers: set<string> = {"worker-a", "worker-b"}
  invariant:
    credits >= 0 and count(workers) > 0

transition Schedule @ Submit(task: string, worker: string):
  from Scheduler
  to Scheduler:
    mode = "Waiting"
    credits = before.credits - 1
  where:
    before.mode = "Idle"
    and exists candidate in before.workers where candidate = worker
  do:
    accept: $ipc.accept(task),
    (reserve: $pool.reserve(task) @ worker | audit: $log.append(task) @ local)
```

核心词法和文法骨架如下；缩进构成块，Tab 非法，`//` 开始行注释。

```ebnf
program     = { state | transition } ;
state       = "state" Name [ "@" Name ] [ "initial" ] ":" INDENT
                { field | invariant }
              DEDENT ;
field       = Name ":" type "=" literal NEWLINE ;
invariant   = "invariant" ":" INDENT expression DEDENT ;

transition  = "transition" Name "@" event-pattern ":" INDENT
                "from" Name NEWLINE
                "to" Name ":" INDENT { Name "=" expression NEWLINE } DEDENT
                [ "where" ":" INDENT expression DEDENT ]
                [ "do" ":" INDENT action-expression DEDENT ]
              DEDENT ;
event-pattern = Name "(" [ parameter { "," parameter } ] ")" ;
parameter   = Name ":" type ;
type        = "bool" | "int" | "string" | "set<string>" ;

expression  = literal | name | "round" | "before." Name
            | unary | binary
            | "count" "(" expression ")"
            | ( "insert" | "erase" ) "(" expression "," expression ")"
            | "exists" Name "in" expression "where" expression ;

action-expression = sequence { "|" sequence } ;
sequence     = action { "," action } ;
action       = Name ":" "$" qualified-name "(" [ arguments ] ")"
               [ "@" qualified-name ]
             | "(" action-expression ")" ;
```

`,` 表示必须按序发生，`|` 表示两边没有顺序边；`,` 的结合优先级高于 `|`。因此
`a, b | c, d` 是两条并行链 `(a, b) | (c, d)`。若要先做 `a` 再并行做 `b/c`，应写
`a, (b | c)`。输出中的 `requires` 是该表达式编译出的 DAG 边，不是另一套表层语法。

`@` 只表达调用或定义所处的上下文，不授予权限。动作未写 `@` 时继承源 state 的上下文；
写 `@ worker` 时，如果 `worker` 是 string 类型事件参数，就绑定到该参数的值，否则它是
一个静态上下文名。

## 值、谓词与搜索

v0 有精确的 `bool`、有符号 64 位 `int`、UTF-8 `string` 和规范排序的 `set<string>`。
支持：

- 布尔运算 `and/or/not`；
- 相等、整数/字符串有序比较；
- 整数 `+/-`，溢出即失败；
- `x in set`、`count(set)` 和纯函数式 `insert(set, x)/erase(set, x)`；
- 有限、确定性枚举的 `exists x in set where predicate`。

集合按字典序枚举，因此相同输入得到相同搜索、状态文本和动作 DAG。`exists` 当前只返回
真假，不把候选绑定泄漏到 `do`；需要选择候选的动态搜索会在后续增加显式、可重放的
`select`，而不会偷偷依赖哈希表顺序。

## 确定性与错误边界

- 一个程序必须且只能有一个 `initial` state；
- 同名事件在所有 transition 上必须拥有相同参数表；
- 字段、事件、赋值、谓词和动作实参在 `check` 时静态检查；
- 同一事件若没有可用 transition，或同时启用多个 transition，执行失败；
- 新状态违反 invariant 时不提交状态，也不产出外部调用计划；
- 调用标签在一个 transition 内必须唯一；
- 输出的 map/set、调用和依赖边都有规范顺序。

`replay` 使用一个全新引擎重新计算同一事件，并比较完整 `StepResult`。正式的多事件 trace
文件、回执输入和外部调用结果属于下一层协议，不在 v0 中伪造。

## 构建与运行

```sh
cmake -S . -B build -DDTESSL_BUILD_TESTS=ON
cmake --build build --target dtessl_cli dtessl_tests -j
build/dtessl check examples/scheduler.dtessl
build/dtessl version
build/dtessl run examples/scheduler.dtessl Submit task=task-1 worker=worker-a
build/dtessl replay examples/scheduler.dtessl Submit task=task-1 worker=worker-a
ctest --test-dir build --output-on-failure
```

## 有意留在 v0 之外

为了先闭合语言核心，v0 不包含 map/bag/relation/matrix、候选选择、概率或
非确定性、连续时间、async/await、物理完成语义、权限系统、solver、字节码和 JIT。
建议的下一个最小增量是：泛型有限集合与 relation、显式 `select ... by`、多事件 trace/
receipt replay，最后才加入稀疏矩阵与可替换搜索后端。它们应继续服从同一条边界：
transition 只计算逻辑变化和调用计划，宿主拥有物理副作用。
