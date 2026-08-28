# DTESSL v0.2.3

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
- [Backend 与 Provider 接入边界](docs/BACKENDS.md)
- [Canonical Value Format v1](docs/CANONICAL_VALUE_V1.md)
- [Exact Numeric Profile v1](docs/EXACT_NUMERIC_PROFILE_V1.md)
- [Relation and Search Profile v1](docs/RELATION_SEARCH_V1.md)
- [Core Logic Surface](docs/CORE_LOGIC_SYNTAX.md)
- [SemanticDescriptor Projection v1](docs/SEMANTIC_DESCRIPTOR_V1.md)
- [Language Service 与 REPL](docs/LANGUAGE_SERVICE.md)
- [变更记录](CHANGELOG.md)

## v0 的闭环

一个程序由三类定义组成：

- `record / variant / enum / newtype` 定义代数与名义领域类型；
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

```dtessl
newtype TaskId = string

port ipc.accept(string)
port pool.reserve(string)
port log.append(string)

variant Mode:
  Idle
  Assigned(TaskId)

state Scheduler @ local initial:
  mode: Mode = Mode.Idle
  credits: int = 2
  workers: set<string> = {"worker-a", "worker-b"}
  invariant:
    credits >= 0 and count(workers) > 0

transition Schedule @ Submit(task: string, worker: string):
  from Scheduler
  to Scheduler:
    mode = Mode.Assigned(TaskId(task))
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
program     = { type-declaration | port | state | transition } ;
type-declaration = name-type | newtype | record | variant | enum ;
name-type   = "name" Name NEWLINE ;
newtype     = "newtype" Name "=" type NEWLINE ;
record      = "record" Name ":" INDENT { Name ":" type NEWLINE } DEDENT ;
variant     = "variant" Name ":" INDENT
                { Name [ "(" type ")" ] NEWLINE }
              DEDENT ;
enum        = "enum" Name ":" INDENT { Name NEWLINE } DEDENT ;
port        = "port" qualified-name "(" [ type { "," type } ] ")" NEWLINE ;
state       = "state" Name [ "@" Name ] [ "initial" ] ":" INDENT
                { field | invariant }
              DEDENT ;
field       = Name ":" type "=" literal [ "merge" ( "equal" | "union" ) ] NEWLINE ;
invariant   = "invariant" ":" INDENT expression DEDENT ;

transition  = "transition" Name "@" event-pattern ":" INDENT
                "from" Name NEWLINE
                "to" Name ":" INDENT { Name "=" expression NEWLINE } DEDENT
                [ "where" ":" INDENT expression DEDENT ]
                [ "do" ":" INDENT action-expression DEDENT ]
              DEDENT ;
event-pattern = Name "(" [ parameter { "," parameter } ] ")" ;
parameter   = Name ":" type ;
type        = "bool" | "int" | "rational" | "string"
            | "[" type "]"
            | "~" type | "~" "(" type { "," type } ")"
            | "list" "<" type ">"
            | "set" "<" type ">"
            | "map" "<" type "," type ">"
            | "bag" "<" type ">"
            | "option" "<" type ">"
            | "result" "<" type "," type ">"
            | "tuple" "<" type { "," type } ">"
            | "relation" "<" type { "," type } ">"
            | Name ;

name-value  = Name "(" Name ")" ;
option-value = "[" [ expression ] "]" ;
relation-value = "~" "{" [ literal { "," literal } ] "}" ;
list-value  = "list" "[" [ literal { "," literal } ] "]" ;

expression  = literal | name | "round" | "before." Name
            | unary | binary
            | "count" "(" expression ")"
            | ( "insert" | "erase" ) "(" expression "," expression ")"
            | "exists" Name ( "in" | "~" ) expression "where" expression
            | ( "E" | "A" ) Name ( "in" | "~" ) expression ":" expression
            | "select" Name ( "in" | "~" ) expression "where" expression
                "by" "lex" "(" expression { "," expression } ")"
            | constructor | record-constructor | match-expression ;
match-expression = "match" name "{"
                     pattern "->" expression
                     { "," pattern "->" expression }
                   "}" ;
pattern     = Name [ "(" Name ")" ] | "[]" | "[" Name "]" | "_" ;

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

同一 round 默认禁止多个 transition 写同一字段。字段可显式声明 `merge equal`（候选值
必须相同）或对 `set<string>` 声明单调的 `merge union`；源顺序永远不是冲突决议规则。

## 值、谓词与搜索

当前有精确的 `bool`、任意精度有符号 `int`、规范化 `rational`、UTF-8 `string`，可递归组合的
`list<T>/set<T>/map<K,V>/bag<T>/option<T>/result<T,E>`，以及 `record`、
`variant`、`enum`、`newtype`。集合、map key 与 bag item 使用规范值顺序；record
字段按字段名规范排序，所有 nominal/variant 值编码自己的类型与构造器身份。
支持：

- `name WorkerId` 定义开放的名义逻辑名称，值写作 `WorkerId(a)`；它不是 string，
  也不是 capability 或 authority；
- `~Worker`、`~(A,B)`、`~{...}` 和 `item ~ relation` 构成紧凑关系语法；
- `[T]`、`[]`、`[value]` 分别表示 typed option、无值和有值，列表显式写作
  `list[...]`；
- 布尔运算 `and/or/not`；
- 相等、精确数值/字符串有序比较；
- integer/rational 的 `+`、`-`、`*`、`/`；整数除法表达式产生 rational，混合运算精确提升；
- `x in set`、`count(set)` 和纯函数式 `insert(set, x)/erase(set, x)`；
- 有限、确定性枚举的 `exists x in set where predicate`。
- record 构造与字段投影、variant/enum 构造、名义 newtype 构造；
- `match value { A(x) -> ..., B -> ... }`，静态拒绝遗漏构造器、重复分支、
  payload 绑定错误和不同结果类型。

`[]` 在有 `[T]` 类型上下文的位置表示空 option，`[value]` 表示有值；旧的
`none<T>()/some(value)` 仍作为 v0 兼容输入。`ok<E>(value)` 和
`err<T>(error)` 给出 result 的另一侧类型。初值已有声明类型上下文，因此可简写为
`none`、`some(value)`、`ok(value)`、`err(value)`。

`~T`/`~(A,B,...)` 是独立的一等有限关系，不是隐藏的 JSON，也不是没有 schema 的 set。
每行是同 arity 的 `tuple<T...>`，按 canonical tuple 顺序排序并去重。当前关系代数包括：

- `project(r, column...)`、`join(left, li, right, ri)`；
- 二元关系的 `compose`、`inverse` 和非自反传递 `closure`；
- `union`、`intersection`、`difference`；
- `E/A` 量词、成员关系和 `count`；
- `select row ~ r where p by lex(score...)`，结果为 typed option。

选择按 score 升序；不同候选若完整 score 相同则拒绝整个 round，绝不以 hash/source
顺序暗中决胜。无候选返回 `none`。稳定 ID 应作为 `lex` 最后一项明确写出。

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

`replay` 使用一个全新引擎重新计算同一事件，并比较完整 `StepResult`。库级
`DTESSL EventTrace` 是一组按 round 排列的原生 typed event batch，只重建逻辑状态与
ActionPlan。它不读取 chenRT journal、snapshot、Provider receipt 或自由文本，也不重新
执行物理副作用；runtime replay 不属于 DTESSL。

## 构建与运行

```sh
cmake -S . -B build -DDTESSL_BUILD_TESTS=ON
cmake --build build --target dtessl_cli dtessl_tests -j
build/dtessl check examples/scheduler.dtessl
build/dtessl version
build/dtessl features examples/scheduler.dtessl
build/dtessl plans examples/relations.dtessl
build/dtessl highlight examples/scheduler.dtessl
build/dtessl repl examples/scheduler.dtessl
build/dtessl run examples/scheduler.dtessl Submit task=task-1 worker=worker-a
build/dtessl replay examples/scheduler.dtessl Submit task=task-1 worker=worker-a
build/dtessl replay examples/core_logic.dtessl Submit minimum=2
build/dtessl replay-batch examples/scheduler.dtessl \
  Submit task=task-1 worker=worker-a -- Note text=same-round
build/dtessl descriptor-check examples/scheduler.semantic
build/dtessl descriptor-generate examples/scheduler.semantic
build/dtessl descriptor-source-map examples/scheduler.semantic
build/dtessl descriptor-manifest examples/scheduler.semantic
build/dtessl descriptor-replay examples/scheduler.semantic \
  Submit minimum=1 task=task-1
ctest --test-dir build --output-on-failure
```

`SemanticDescriptor v1` 是由已有系统主动提供给 DTESSL 的显式、有损模型投影，
不是 DTESSL 对已有系统、JavaScript 或日志的逆向建模。它固定 provenance、source map、
descriptor/source digest、coverage 与 gap 分类。生成的 operational mirror 永远不会被工具
冒充 independently-authored assurance model。生成的 `do` 只能调用静态声明并检查过参数类型
的 port，DTESSL 仍只返回 ActionPlan。

## 有意留在 v0 之外

为了逐层闭合语言核心，v0.2.3 仍不包含 matrix、概率或
非确定性、连续时间、async/await、物理完成语义、权限系统、solver、字节码和 JIT。
下一个增量进入完整 state theory 与多组件 transition，随后扩展原生 typed EventTrace，
最后才加入稀疏矩阵与可替换 solver backend。
它们应继续服从同一条边界：
transition 只计算逻辑变化和调用计划，宿主拥有物理副作用。
