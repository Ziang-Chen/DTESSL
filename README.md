# DTESSL v0.3.3

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

一个程序的闭环由六类定义组成：

- `record / variant / enum / newtype` 定义代数与名义领域类型；
- `state` 定义一个有类型的状态空间、初值、上下文和不变量；
- `transition` 的 `case (source-set) -> (target-set)` 定义原子状态集重写；
- `procedure` 定义由 RuntimeContext 持有的持久自动机实例及 typed context 准入；
- `trace` 定义原生静态事件序列或按 `@context` 捕获的动态执行投影；
- `Claim` 用 `always`、`eventually` 或 transition 次数约束判定 trace。

执行器以一组可并行事件为一个离散 `round`。同一 round 的 transition 都读取同一个
before snapshot；写集不冲突时原子合并，冲突而没有显式 merge relation 时拒绝整组事件。
因此 round 是模拟批次，不是“每个 transition 自增一次”的全局逻辑时钟。引擎按以下顺序工作：

1. 用 `case` 静态匹配当前正交状态组合；
2. 用当前 typed value 和事件参数动态求值 path-local `where`；
3. 要求恰好一个 path 可用，否则拒绝事件；
4. 根据 `->` 和 path-local `set` 计算候选状态集；
5. 检查所有目标 state 的 `invariant`；
6. 生成所选 path 的 `do` 调用 DAG，然后原子提交全部状态轴和值。

`$name(...)` 不会在解释器内执行。它只是一个带类型实参、上下文和依赖边的外部调用
记录。宿主以后可以向这些名字注入 chenRT IPC 或其他接口；DTESSL 本身没有环境时钟、
文件系统、网络和进程权限。

## 最小语法

快速建模可使用以 `;` 结尾、且从关键字到 `;` 不跨物理行的 compact 形式：

```dtessl
state a, b, c;
trans a -> b when b.val == 0;
trans a -> c when b.val != 0;
procedure p1 a, b.val = 0, c & inject ctodo;
trace @procedure;
```

`trans` 边的连通分量自动成为一个正交状态轴。`procedure` 中每个轴首次出现的
state 是初态；同轴后续名称不重复激活。`b.val = 0` 为该轴声明并初始化 typed field，
`inject ctodo` 提供 TransitionId；只有一个 compact procedure 时，`trace @procedure`
为它建立最小 replay + closed capture（也可以直接写 `trace @p1`）。
compact 节点直接降到下述正式 typed AST，因此 check、搜索、round、trace 和 replay 的
语义完全相同。逗号分隔的 compact `when` 谓词按合取解释。

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
  case (Scheduler @ local) -> (Scheduler @ local):
    where:
      before.mode = Mode.Idle
      and exists candidate in before.workers where candidate = worker
    set @ local:
      mode = Mode.Assigned(TaskId(task))
      credits = before.credits - 1
    do:
      accept: $ipc.accept(task),
      (reserve: $pool.reserve(task) @ worker | audit: $log.append(task) @ local)
```

核心词法和文法骨架如下；缩进构成块，Tab 非法，`//` 开始行注释。

```ebnf
program     = { type-declaration | port | function | state | compact-state
              | transition | compact-transition | compact-procedure | compact-trace
              | procedure | trace | claim } ;
compact-state = "state" Name { "," Name } ";" NEWLINE ;
compact-transition = "trans" Name "->" Name
                     [ "when" expression { "," expression } ] ";" NEWLINE ;
compact-procedure = "procedure" Name compact-item { "," compact-item }
                    [ "&" "inject" Name ] ";" NEWLINE ;
compact-item = Name | Name "." Name "=" scalar-literal ;
compact-trace = "trace" "@" Name ";" NEWLINE ;
type-declaration = name-type | newtype | record | variant | enum ;
name-type   = "name" Name NEWLINE ;
newtype     = "newtype" Name "=" type NEWLINE ;
record      = "record" Name ":" INDENT { Name ":" type NEWLINE } DEDENT ;
variant     = "variant" Name ":" INDENT
                { Name [ "(" type ")" ] NEWLINE }
              DEDENT ;
enum        = "enum" Name ":" INDENT { Name NEWLINE } DEDENT ;
port        = "port" qualified-name "(" [ type { "," type } ] ")" NEWLINE ;
function    = "function" Name "(" [ parameter { "," parameter } ] ")"
              "->" type ":" INDENT expression DEDENT ;
state       = "state" Name [ "@" Name ] [ "initial" ] ":" INDENT
                { field | invariant }
              DEDENT ;
field       = Name ":" type "=" literal [ "merge" ( "equal" | "union" ) ] NEWLINE ;
invariant   = "invariant" ":" INDENT expression DEDENT ;

transition  = "transition" Name transition-head ":" INDENT
                case { case }
              DEDENT ;
transition-head = "(" [ parameter { "," parameter } ] ")"
                    [ "@" scope optimizer ]
                | "@" scope optimizer
                    "(" [ parameter { "," parameter } ] ")"
                | "@" legacy-event
                    "(" [ parameter { "," parameter } ] ")" ;
optimizer   = "[" "optimized_score" "=" expression "]" ;
case        = "case" [ Name ] state-pattern-set "->" exact-state-set ":" INDENT
                [ "where" ":" INDENT expression DEDENT ]
                [ "set" ":" INDENT
                    { Name "=" expression "@" Name NEWLINE }
                  DEDENT ]
                [ "do" ":" INDENT action-expression DEDENT ]
              DEDENT ;
state-pattern-set = "(" state-pattern { "," state-pattern } ")" ;
state-pattern = ( Name | "{" Name { "," Name } "}" | "_" ) [ "@" Name ] ;
exact-state-set = "(" state-binding { "," state-binding } ")" ;
state-binding = Name [ "@" Name ] ;
procedure   = "procedure" Name "@" Name ":" INDENT
                "initial" exact-state-set NEWLINE
              DEDENT ;
trace       = "trace" Name [ "@" Name ] ":" INDENT
                [ "replay" ":" INDENT replay-round { replay-round } DEDENT ]
                [ "capture" ( "closed" | "projected" ) ":"
                    INDENT { capture-filter } DEDENT ]
              DEDENT ;
replay-round = replay-occurrence { "|" replay-occurrence } NEWLINE ;
replay-occurrence = "inject" Name
                    "(" [ literal { "," literal } ] ")" "@" Name
                    [ "->" qualified-name ] ;
capture-filter = "state" "(" [ state-binding { "," state-binding } ] ")" NEWLINE
               | "transition" "(" [ qualified-name { "," qualified-name } ] ")" NEWLINE
               | "procedure" "(" [ Name { "," Name } ] ")" NEWLINE ;
claim       = "Claim" Name "@" Name ":" INDENT
                ( ( "always" | "eventually" ) ":" INDENT expression DEDENT
                | "count" Name "<=" integer NEWLINE )
              DEDENT ;
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

`case` 的源括号是合取状态集：其中每个 `state @ context` 必须同时成立。重复的
`case` 表示候选 path 的析取；`{Idle, Waiting} @ scheduler` 是有限模式，`_ @ scheduler`
是该轴的通配模式。模式在验证阶段展开为有限候选；目标侧只接受精确状态，生产执行中
绝不从多个目标中暗选。`where` 是 path-local 的动态 typed guard，`set` 与 `do` 也只属于
被唯一选中或显式优化选中的 path。可选的 case 名形成 `Transition.caseName` 规范身份，供 trace 捕获和
`Claim count` 精确引用；不写名字的单 path 仍使用 transition 名。v0 旧式
`from/to/where/do` 与 `set @ context:` 仍作为兼容输入。主要更新形式是一个
`set:` 块，每行用 `field = expression @ context` 标注目标状态轴。

`transition Dispatch(task: Task)` 同时定义可注入的消息/transition 类型；不再需要另一套
Event 名。旧式 `transition Dispatch @ Tick(task: Task)` 暂时保留给普通 Event API 兼容，
但 procedure 的 `inject` 始终按 `Dispatch` 这个 TransitionId 寻址。

需要允许多个候选 path 时，必须显式给出优化目标：

```dtessl
function utility(value: int) -> int:
  value

transition Choose @ scheduler [optimized_score = utility(scheduler.score - before.scheduler.score)]():
  case low (Idle @ scheduler) -> (Idle @ scheduler):
    set:
      score = 2 @ scheduler
  case high (Idle @ scheduler) -> (Idle @ scheduler):
    set:
      score = 9 @ scheduler
```

score 在每个候选的拟提交后状态上计算，必须返回精确 `int` 或 `rational`，最高分获选。
最高分并列会拒绝整个 round；没有 `optimized_score` 时仍保持“恰好一个候选”的规则。
`@ scheduler` 是优化读取和比较的显式状态 scope，不是权限。输出会保存选中的 score，
因此 replay 可同时验证路径和优化证据。

`trace` 可先有 `replay:`，再有 `capture:`，两者可同时存在但次序不可颠倒。正式 replay
输入是 `inject Transition(...) @ Procedure` typed transition occurrence；一行就是一个
1-based causal round，`|` 分隔同 round injection。同 round 的跨 procedure decision 共享
同一 RoundId，运行器的遍历顺序不能产生时间顺序。`inject` 只把 occurrence 加入语言
RuntimeContext 的待处理集合，不执行外部搜索，也不指定 case。后端先按 TransitionId 定位
transition family，再按活动状态签名定位候选 case，最后计算 `where`、验证不变量并生成 ActionPlan。

可选的 `-> Transition.caseName` 是搜索 replay 断言。它只验证派生结果，绝不强制状态
跳转。例如 `inject Dispatch(task) @ Run -> Dispatch.ready` 注入 `Dispatch` occurrence，
并检查后端搜索出了 `Dispatch.ready`。动态
`capture closed/projected` 的 canonical AST 是 `CaptureFilter{states, transitions, procedures}`。
捕获 procedure 会为每个全局 RoundId 保留不可变帧，包括该 procedure 本 round 空闲时的帧；
公共 API 可按 `(trace, procedure, RoundId)` 精确访问。capture 只观察 DTESSL RuntimeContext
的原生 typed step，不读取外部日志。
`Claim` 对闭合 trace 返回 `satisfied/violated`；开放 trace 在没有反例或见证时返回
`pending`。有 causal gap 的 projected trace 不得给出肯定的 `satisfied`。

`procedure P @ context` 只命名一个 RuntimeContext 中的 logical instance 入口：`@ context`
是 initial context，`initial (...)` 是正交状态轴的 initial state 组合。创建实例即启动语言
自己的搜索循环；初始没有待处理 transition 时立即静止。外部或另一个 procedure 后续用
`inject Transition(...) @ P` 增加 occurrence，循环再次运行。procedure 本身没有 transition、
replay、capture 或步骤正文。active states、typed values、revision、injection history 和
causal frontier 均由语言 RuntimeContext 持续保存；
`trace` 只是独立的测试/判定投影，不是 procedure 的步骤容器。`function` 是非递归、纯、总的
typed expression 封装；只能读取
参数，不能观察 `before`/`round`，不能修改状态或生成 ActionPlan。

`capture closed` 对 filter 命中的 procedure 做保守的完整闭包：保留其声明初态、全部 typed
injection、所有 RoundId（包括空闲帧）和派生 decision，输出 `ProcedureArtifact` 并标记
`replayable=yes`。这比裁剪单条因果边更宽，但不会漏依赖。`capture projected` 只供观察，始终
`replayable=no`，也不会生成可冒充完整重放输入的 artifact。

REPL 直接暴露同一套 RuntimeContext，而不是另造执行器：

```text
dtessl repl examples/procedure_replay.dtessl
:inject SessionA Increment delta=1 -- SessionB Increment delta=2
:inject SessionA Increment delta=3
:runtime
:capture InterleavedRuntime
:replay-procedures
```

`:inject` 会按需启动 procedure；`--` 两侧的注入共享一个 RoundId 并原子提交。
`:capture` 生成包含初始配置和完整 typed injection history 的闭合 artifact；
`:replay-procedures` 重新准入这些 context 并重新搜索 decision DAG。`:trace NAME` 和
`:claims NAME` 执行源码声明的 trace；legacy Engine 的动态观察明确使用
`:trace-live`/`:claims-live`，不与正式 procedure replay 混用。

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

- 每个 `@context` 状态轴必须且只能有一个 `initial` state；
- 同名事件在所有 transition 上必须拥有相同参数表；
- 字段、事件、赋值、谓词和动作实参在 `check` 时静态检查；
- 同一事件若没有可用 path，或同时启用多个 `case` path，执行失败；
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

为了逐层闭合语言核心，v0.3.3 仍不包含 matrix、概率或
非确定性、连续时间、async/await、物理完成语义、权限系统、solver、字节码和 JIT。
下一个增量补 derived/shared state 与更丰富的 typed destructuring，随后才加入稀疏矩阵
与可替换 solver backend。
它们应继续服从同一条边界：
transition 只计算逻辑变化和调用计划，宿主拥有物理副作用。
