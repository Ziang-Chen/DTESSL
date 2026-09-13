# DTESSL v0.4.8

**中文** | [English](README.en.md)

Licensed under [GNU AGPL version 3 only](LICENSE) (`AGPL-3.0-only`).
Copyright (c) 2026 Ziang-Chen.

This grant also covers historical versions, including snapshots without a
LICENSE file; see [licensing scope and historical grant](LICENSING.md).

允许商用；分发受许可覆盖的作品，以及通过网络提供修改后的版本时，
须遵守许可证规定的相应源码提供义务。第三方作品保留其各自许可。

DTESSL（Discrete-Time Event System Simulation Language，戴特赛尔）是一个独立的、
确定性的离散时间事件系统建模语言。当前参考实现
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
- [State case、有限域与 Delta](docs/STATE_CASE_AND_FINITE_DOMAINS.md)
- [变更记录](CHANGELOG.md)

## 设计思想与形式化语义

DTESSL 将建模分为静态结构、动态状态、关系约束、逻辑转移、因果证据和外部动作计划。
以下公式是对当前项目语义的说明性形式化，不是完整规范或形式化正确性证明。
引用论文提供理论依据；DTESSL 特有的 round、合并、capture 与 ActionPlan 规则是项目设计。

### 1. StateSchema 定义结构，Embedding 表示当前实例

令 $`S`$ 为递归状态结构，$`E_r=(C_r,V_r)`$ 为第 $`r`$ 轮的 Embedding：
$`C_r`$ 记录当前激活的控制分支，$`V_r`$ 是有类型的字段值。

```math
E_r\in\mathrm{Valid}(S),\qquad
R_t(E_r,u,E')\in\{\mathrm{true},\mathrm{false}\}.
```

$`u`$ 是有类型的输入 occurrence，$`R_t`$ 是 transition 的 before/after 关系。
每个激活的 choice 轴选择一个合法分支；嵌套轴随祖先分支激活。
命名 state relation 只匹配当前 $`E_r`$，不拥有隐藏的中间状态或副作用。
例如两个命名模板的合取为 $`P(E_r)\land Q(E_r)`$，二者读取同一状态。

### 2. 确定性来自显式选择与原子提交

令 $`K_t(E_r,u)`$ 为通过控制状态匹配与 `where` 的候选集合。
未声明优化时要求 $`|K_t|=1`$；声明精确数值目标 $`s`$ 时要求唯一最大值：

```math
k^*\in\underset{k\in K_t(E_r,u)}{\mathrm{arg\,max}}\ s(E'_k),
\qquad
\left|\underset{k\in K_t(E_r,u)}{\mathrm{arg\,max}}\ s(E'_k)\right|=1.
```

无候选或最高分并列均拒绝；选中后仍须通过更新合并和不变量检查。
不要与关系值的 `select ... by lex(...)` 混淆：后者按升序选择唯一最小 score tuple。

对同一轮输入 bag $`B_r`$，所有候选读取同一个 before snapshot：

```math
E_{r+1}=\mathrm{Apply}\!\left(E_r,
\mathrm{Merge}\{\Delta_k(E_r,u):u\in B_r\}\right).
```

仅在所有输入通过 admission、Merge 有定义且结果满足不变量时提交。
重复字段写入默认拒绝；`merge equal` 要求值相等，`merge union` 在支持的集合类型上取并集。
失败时保留 $`E_r`$，不返回已接受的动作计划；源码或哈希遍历顺序不用于决胜。

### 3. 因果偏序与 round 不是物理时间

Lamport 将 happened-before 建模为偏序，而非由物理时钟推导的一条必然全序。
[Lamport, 1978](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/12/Time-Clocks-and-the-Ordering-of-Events-in-a-Distributed-System.pdf)。
在 DTESSL 中，令 $`D`$ 为字段依赖与控制状态轴依赖的直接边，$`D^+`$ 为其传递闭包：

```math
a\prec b\iff(a,b)\in D^+,\qquad
a\parallel b\iff\neg(a\prec b)\land\neg(b\prec a)\quad(a\ne b).
```

`RoundId` 标记一次原子批次，同轮 decision 共享它；编号较小本身不能证明两事件存在因果关系。
ActionPlan 内部的调用 DAG 另行描述串行和并行依赖，不应与 occurrence DAG 混为一谈。

### 4. Claim 在执行图与监视器的乘积上检查

自动机验证将性质检查联系到执行与性质自动机的组合；
[Vardi–Wolper, 1986](https://www.cs.rice.edu/~vardi/papers/lics86.pdf)
及 [SPIN 理论说明](https://spinroot.com/spin/theory.html) 提供这一方法的依据。
DTESSL 的解释性模型为：

```math
\mathcal P=\mathrm{EmbeddingExpand}\times\mathrm{ClaimMonitor},\qquad
(E,q)\longrightarrow(E',\delta(q,L(E,u,E'))).
```

$`q`$ 是监视器状态，$`L`$ 是由当前逻辑步骤得到的观测；监视器状态不混入用户的 StateSchema。
Solver 按需探索可达乘积节点，寻找有限前缀、死锁或 lasso 反例。
无法编译的片段或耗尽的搜索预算不能当作证明成功，应保留 `inconclusive`。

闭合有限 trace $`\pi=E_0\ldots E_n`$ 上的基本时序读法是：

```math
\pi,i\models\mathbf F p\iff\exists j\in[i,n]:\pi,j\models p,\qquad
\pi,i\models\mathbf G p\iff\forall j\in[i,n]:\pi,j\models p.
```

这里 $`0\le i\le n`$，初态也参与判断。有限 trace 的理论依据见
[De Giacomo–Vardi, 2013](https://www.ijcai.org/Proceedings/13/Papers/132.pdf)。
开放 trace 不凭有限前缀推断未来；尚无决定性证据时为 `pending`，
有因果缺口的投影不能被当作完整的肯定证据。项目并未声称实现论文中的完整 LDLf。

### 5. Capture 选择 occurrence，Replay 重算证据

令 $`A`$ 为 state/transition/procedure 过滤器选出的 occurrence seed，
$`\mathrm{Pred}`$ 为显式因果前驱，则基础因果闭包可写为最小不动点：

```math
\mathrm{CausalClosure}(A)
=\mu X.\left(A\cup\mathrm{Pred}(X)\right).
```

带 `eventually` 的捕获还保留从 anchor 到首个 witness 的时间区间。
procedure 是可选分组，不自动把同一实例的无关 occurrence 全部纳入。
可见闭包与重放输入不同：`TraceArtifact` 保存从初始状态到最后选中轮的 typed input 前缀，
再由同一核心重算 path、Embedding、因果边及 ActionPlan。`capture projected` 不可重放。

### 6. 逻辑变化与物理副作用分离

```math
\mathrm{Step}(E_r,B_r)=(E_{r+1},A_r,H_r),\qquad
A_r=\mathrm{Lower}(E_r,E_{r+1},\mathrm{bindings}).
```

该式描述接受的步骤：$`A_r`$ 是出站 ActionPlan，$`H_r`$ 是逻辑历史证据。
核心生成计划但不调用外部 Provider。幂等、重试和 delivery 声明是宿主需要兑现的 contract；
非确定性结果作为后续 typed input 进入，不能直接参与当前 after-state。
Replay 重注记录的输入并重算计划，不重新执行物理副作用。

完整依据与规划见 [研究依据](docs/REFERENCES.md)、[语言设计](docs/LANGUAGE_DESIGN.md)
及 [路线图](docs/ROADMAP.md)。这些公式不表示外部适配器、字节码或完整 solver 插件已实现。

## 语言组成

一个程序主要由六类定义组成：

- `record / variant / enum / newtype` 定义代数与名义领域类型；
- `state` 以命名/匿名 `case`、`,`（合取）、`|`（互斥选择）和括号递归定义有类型状态空间；
- `transition` 的 `case (source-set) -> (target-set)` 定义原子状态集重写；
- `procedure` 定义持久自动机实例的初态/上下文，并可内联定义匿名局部 transition；
- `trace` 定义原生静态事件序列或按 `@context` 捕获的动态执行投影；
- `Claim` 可绑定 `trace/state/procedure/relation`，并以 `always/eventually/until/within/since`
  判定有限 trace 或 EmbeddingExpand 路径。

语义上，`StateSchema` 是递归静态结构，`Embedding` 是它在一个时刻的具体嵌入，
`EmbeddingExpand` 是以 Embedding 为节点的可达图。执行 lowering 使用 `RawKeyMap`
把控制/值语义路径映射到原始 embedding 向量偏移；digest 只作证据，不作节点身份。
`state case` 本身不生成边，只有 transition 能显式重写一个或多个 case 轴。每条运行时/
solver witness 边都携带可重构并校验 digest 的 `EmbeddingDelta`。

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
记录。宿主以后可以向这些名字注入 host runtime IPC 或其他接口；DTESSL 本身没有环境时钟、
文件系统、网络和进程权限。

## 最小语法

一般递归 state：

```dtessl
state Scheduler @ session initial:
  case Phase:
    Idle
    | Running(attempts: int32[0:3] = 0,
              case Stage: Reserving | Committing,
              invariant(attempts >= 0))
  case Health:
    Healthy | Degraded
  credits: int32[0:10] = 2
  workers: relation WorkerId = {WorkerId(a), WorkerId(b)}
  invariant:
    credits >= 0
    and WorkerId(a) ~ workers
```

同一 AST 的紧凑写法：

```dtessl
state Switch @ local = Mode(Off | On(Level(Low | High))), Health(Good | Failed), changes: int = 0;
```

`,` 是结构合取，`|` 是结构选择，`A(B)` 是递归包含。transition 可按同一结构匹配：

```dtessl
case (Switch(Mode.Off, Health.Good) @ local)
  -> (Switch(Mode.On.Level.Low) @ local):
```

关系满足只有一个核心形式：`subject ~ relation`。复合筛选显式保留
subject 与匿名 relation pattern：

```dtessl
<stateA, stateB> ~ (stateA ~ SameEpoch, stateB ~ Migratable) ~ Equal
```

`,` 在 predicate/guard 块中表示合取，在 `<...>`/`{...}` 内只是容器分隔；
`|` 只表示并行组合，不再兼任逻辑析取，嵌套布尔逻辑使用 `and/or`。

它递归降为三个 typed RelationMatch 的合取；
普通 `~` 不引入隐式搜索，只有 `E/A/select` 和显式有限参数域会搜索。
左侧同样是递归的 typed value pattern，可包含 tuple、record、variant 和
embedding 字段投影。放在 transition 中时，完整匹配规范化为
`StructuralPattern and RelationMatch`：`case` 匹配激活的复合控制状态，`~`
匹配其中的值关系；控制状态名不会被偷换成 string 或普通 relation row。
`= / != / < / <= / > / >= / in / ~` 在 typed AST 中全都属于
`RelationMatch`；runtime guard、invariant、trace 与 ClaimMonitor 共用同一验证和
求值入口，solver 不另藏一套比较逻辑。

快速建模可使用以 `;` 结尾、且从关键字到 `;` 不跨物理行的 compact 形式：

```dtessl
state a, b, c;
trans ctodo: a -> b when b.val == 0;
trans ctodo: a -> c when b.val != 0;
procedure p1 a, b.val = 0, c & inject ctodo;
trace @procedure;
```

同名 `trans ctodo:` 声明一个 transition family 的不同路径；`inject ctodo` 只能
引用已经声明的 family，不能隐式创造 transition。匿名 `trans a -> b` 会得到稳定的
生成名称，但不会冒充其他注入名称。`trans` 边的连通分量自动成为一个正交状态轴。
`procedure` 中每个轴首次出现的
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
program     = { type-declaration | port | do-declaration | function | relation-declaration
              | state-relation | state | compact-state
              | transition | compact-transition | compact-procedure | compact-trace
              | procedure | trace | claim } ;
compact-state = "state" Name { "," Name } ";" NEWLINE ;
compact-transition = "trans" [ Name ":" ] Name "->" Name
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
do-declaration = "do" Name "(" [ parameter { "," parameter } ] ")"
                 [ "@" qualified-name ] [ do-contract ] ":"
                 INDENT action-expression DEDENT ;
do-contract = "[" do-property { "," do-property } "]" ;
do-property = "context" "=" ( "fixed" | "inherited" )
            | "idempotent_by" "=" "(" expression ")"
            | "result" "=" ( "opaque" | "nondeterministic"
                             | "consistent_by" "(" expression ")" )
            | "delivery" "=" ( "at_most_once" | "at_least_once" )
            | "ordering" "=" ( "unordered"
                               | "ordered_by" "(" expression ")" )
            | "retry" "=" ( "forbidden" | "safe" | "reconcile" )
            | "replay" "=" ( "suppress" | "reinject" ) ;
function    = "function" Name "(" [ parameter { "," parameter } ] ")"
              "->" type ":" INDENT expression DEDENT ;
state       = "state" Name [ "@" Name ] [ "initial" ] [ extensions ]
              ( ":" INDENT { state-component | invariant } DEDENT
              | "=" state-component { "," state-component } ";" NEWLINE ) ;
state-component = choice | field ;
choice      = Name "(" control { "|" control } ")" ;
control     = Name [ "(" state-component { "," state-component }
                    { "," "invariant" "(" expression ")" } ")" ] ;
field       = Name ":" type "=" literal [ "merge" ( "equal" | "union" ) ] NEWLINE ;
invariant   = "invariant" ":" INDENT expression DEDENT ;

transition  = "transition" Name [ transition-head ] ":" INDENT
                ( transition-relation { "|" transition-relation }
                | case { case } )
              DEDENT ;
transition-head = "(" [ parameter { "," parameter } ] ")"
                    [ "@" scope ] [ extensions ]
                | "@" scope [ extensions ]
                    "(" [ parameter { "," parameter } ] ")"
                | "@" legacy-event
                    "(" [ parameter { "," parameter } ] ")" ;
extensions  = "[" extension { "," extension } "]" ;
extension   = "capture" "=" capture-target { "|" capture-target }
            | "optimized_score" "=" expression ;
capture-target = Name [ "/" Name ] ;
case        = "case" [ Name ] state-pattern-set "->" exact-state-set ":" INDENT
                [ "where" ":" INDENT expression DEDENT ]
                [ "set" ":" INDENT
                    { Name "=" expression "@" Name NEWLINE }
                  DEDENT ]
                [ "do" ":" INDENT action-expression DEDENT ]
                [ "ensure" ":" INDENT temporal-expression DEDENT ]
              DEDENT ;
transition-relation = "<" relation-state-set "," relation-state-set ">"
                      [ "+" ( named-do-call | "(" action-expression ")" ) ]
                      [ branch-extensions ]
                      [ ":" INDENT
                          [ "where" ":" INDENT expression DEDENT ]
                          [ "set" [ "@" Name ] ":" INDENT
                              { Name "=" expression NEWLINE } DEDENT ]
                          [ "do" ":" INDENT action-expression DEDENT ]
                          [ "ensure" ":" INDENT temporal-expression DEDENT ]
                        DEDENT ] ;
relation-state-set = state-binding
                   | "{" state-binding { "," state-binding } "}" ;
branch-extensions = "[" branch-extension { "," branch-extension } "]" ;
branch-extension = "label" "=" Name
                 | "where" "=" "(" expression ")"
                 | "do" "=" "(" action-expression ")" ;
state-pattern-set = "(" state-pattern { "," state-pattern } ")" ;
state-pattern = ( Name | "{" Name { "," Name } "}" | "_" ) [ "@" Name ] ;
exact-state-set = "(" state-binding { "," state-binding } ")" ;
state-binding = Name [ "@" Name ] ;
procedure   = "procedure" Name "@" Name [ extensions ] ":" INDENT
                "initial" exact-state-set NEWLINE
                { anonymous-transition }
              DEDENT ;
anonymous-transition = state-pattern-set "->" exact-state-set ":" INDENT
                         [ "where" ":" INDENT expression DEDENT ]
                         [ "set" "@" Name ":" INDENT
                             { Name "=" expression NEWLINE } DEDENT ]
                         [ "do" ":" INDENT action-expression DEDENT ]
                         [ "ensure" ":" INDENT temporal-expression DEDENT ]
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
               | "procedure" "(" [ Name { "," Name } ] ")" NEWLINE
               | "eventually" "state" "(" state-binding ")" NEWLINE
               | "eventually" "transition" "(" qualified-name ")" NEWLINE ;
claim       = "Claim" Name "@" [ "trace" | "state" | "procedure" | "relation" ] Name
                [ "@" "(" Name { "," Name } ")" ] ":" INDENT
                ( temporal-expression NEWLINE
                | ( "always" | "eventually" ) ":" INDENT expression DEDENT
                | "count" Name "<=" integer NEWLINE )
              DEDENT ;
temporal-expression = expression
                    | "<" temporal-expression "," temporal-expression ">"
                      "~" "happens_before"
                    | "always" "(" temporal-expression ")"
                    | "eventually" "(" temporal-expression ")"
                    | "until" "(" temporal-expression "," temporal-expression ")"
                    | "within" "(" integer "," temporal-expression ")"
                    | "since" "(" temporal-expression "," temporal-expression ")"
                    | "never" "(" temporal-expression ")"
                    | "before" "(" temporal-expression "," temporal-expression ")"
                    | "weak_until" "(" temporal-expression "," temporal-expression ")" ;
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
            | "tuple" "<" type { "," type } ">" // v0 compatibility
            | "relation" type | "relation" "<" type { "," type } ">"
            | "relation" "(" type { "," type } ")" // v0 compatibility
            | Name ;

name-value  = Name "(" Name ")" ;
option-value = "[" [ expression ] "]" ;
relation-value = "~" "{" [ literal { "," literal } ] "}" ;
list-value  = "list" "[" [ literal { "," literal } ] "]" ;

expression  = literal | name | "round" | "before." Name
            | "<" expression { "," expression } ">"
            | set-comprehension
            | "relation" "{" expression ":" comprehension-clause
                { "," comprehension-clause } "}"
            | unary | binary
            | "count" "(" expression ")"
            | ( "insert" | "erase" ) "(" expression "," expression ")"
            | "exists" Name ( "in" | "~" ) expression "where" expression
            | ( "E" | "A" ) Name ( "in" | "~" ) expression ":" expression
            | "select" Name ( "in" | "~" ) expression "where" expression
                "by" "lex" "(" expression { "," expression } ")"
            | subject "~" relation
            | subject "~" "(" expression { "," expression } ")"
                "~" relation
            | constructor | record-constructor | match-expression ;
comprehension-clause = Name "in" expression | expression ;
set-comprehension = "{" comprehension-branch
                      { "," comprehension-branch } "}" ;
comprehension-branch = Name ":" comprehension-clause
                         { "," comprehension-clause }
                     | expression ":" comprehension-clause
                         { "," comprehension-clause } ;
relation-declaration = "relation" Name "(" parameter { "," parameter } ")"
                         [ "@" Name ]
                         ":" INDENT comprehension-clause
                         { "," comprehension-clause } DEDENT
                     | "relation" Name "<" parameter { "," parameter } ">"
                         [ "@" Name ] ":" comprehension-clause
                         { "," comprehension-clause } ";"
                     | "relation" "<" parameter { "," parameter } ">"
                         "~" Name [ "@" Name ] ":" comprehension-clause
                         { "," comprehension-clause } ";"
                     | "relation" Name [ "@" Name ] ":" relation-type
                         "=" expression ;
state-relation = "relation" Name [ "@" Name ] ":" INDENT
                   state-relation-route
                   { "|" state-relation-route } DEDENT ;
state-relation-route = relation-state-set
                         [ "[" [ "label" "=" Name "," ]
                           "where" "=" "(" expression ")" "]" ]
                         [ ":" INDENT
                             "where" ":" INDENT expression DEDENT
                           DEDENT ] ;
transition-template-branch = relation-expression "->" relation-state-set
                               [ branch-extensions ]
                               [ ":" INDENT
                                   [ "where" ":" INDENT expression DEDENT ]
                                   [ "set" [ "@" Name ] ":" INDENT
                                       { Name "=" expression NEWLINE } DEDENT ]
                                   [ "do" ":" INDENT action-expression DEDENT ]
                                   [ "ensure" ":" INDENT temporal-expression DEDENT ]
                                 DEDENT ] ;
relation-expression = relation-conjunction
                        { "|" relation-conjunction } ;
relation-conjunction = relation-factor { "," relation-factor } ;
relation-factor = Name | "(" relation-expression ")" ;
match-expression = "match" name "{"
                     pattern "->" expression
                     { "," pattern "->" expression }
                   "}" ;
pattern     = Name [ "(" Name ")" ] | "[]" | "[" Name "]" | "_" ;

action-expression = sequence { "|" sequence } ;
sequence     = action { "," action } ;
action       = Name ":" "$" qualified-name "(" [ arguments ] ")"
               [ "@" qualified-name ]
             | [ Name ":" ] named-do-call
             | "(" action-expression ")" ;
named-do-call = Name "(" [ arguments ] ")" ;
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

更直接的 canonical 表面把 Transition 写成 before/after Embedding 关系：

```dtessl
transition Dispatch:
  <{Idle @ worker, Open @ session},
   {Busy @ worker, Closed @ session}> [label=accept]:
    where:
      before.worker.credits > 0
    do:
      accepted: $audit.emit("accepted") @ worker
  | <{Busy @ worker, Closed @ session},
     {Idle @ worker, Open @ session}> [label=release]
```

外层 `<before-set,after-set>` 是有序 Product，内层 `{...}` 是无序、合取的
Embedding 配置，`|` 是 relation branch 的并集。`[label=...]` 给分支稳定路径名；
compact `[where=(...), do=(...)]` 和块式 `where/set/do/ensure` 降到同一个
Transition branch AST。关系只决定 successor；`do` 是选中唯一 witness 后的
`<before,after,bindings> -> ActionPlan` lowering，不参与关系真假。

关系规则的 name-first 与 tuple-first 紧凑写法永久并行：

```dtessl
relation Eligible<w: Worker, t: Task> @ scheduler: w in workers, t in waiting;
relation <w: Worker, t: Task> ~ EligibleTuple @ scheduler: w in workers, t in waiting;
```

两者进入同一个 typed AST；`@scheduler` 只把 `workers` 等裸字段名解析到
`scheduler.workers`，不代表 authority 或实例所有权。状态关系也可以定义为只读的
当前 Embedding 匹配模板，再由 transition 构造 after：

```dtessl
relation Admit @ scheduler:
  Idle [where=(credits > 0)]

transition Dispatch:
  Admit -> Busy @ scheduler:
    set @ scheduler:
      credits = before.credits - 1
    do:
      accepted: $audit.emit("accepted") @ scheduler
```

relation 头部的 `@scheduler` 只作用于模板中的 state 和纯谓词字段解析。
命名 relation 只允许 state pattern 与 `where`；`after/set/do/ensure` 全部属于
transition。旧的 named `<before,after>` relation 会给出迁移诊断，不再执行。

在 transition 的命名关系表达式中，`,` 是对同一个当前 Embedding 的合取，且
优先级高于 `|`：

```dtessl
transition Dispatch:
  Admit, Authorized | Trusted -> Busy @ scheduler
```

含义是 `(Admit and Authorized) or Trusted`。所有模板都读取同一个 before Embedding；
不存在隐藏的中间 Embedding、隐式状态更新或 relation-owned ActionPlan。箭头右侧以及
`set/do/ensure` 只由 transition 拥有。

可复用的副作用计划用独立的 typed `do` 定义；它不是 relation，也不改变
Embedding：

```dtessl
do StartTask(task: TaskId, operation: OperationId) @ worker [context=fixed, idempotent_by=(<task, operation>), result=consistent_by(<task, operation>), delivery=at_least_once, ordering=ordered_by(task), retry=safe, replay=suppress]:
  invoke: $worker.start(task, operation)

transition Dispatch @ Submit(task: TaskId, operation: OperationId):
  <Idle @ scheduler, Busy @ scheduler> + StartTask(task, operation) [label=accept]
```

`<before,after>` 仍是唯一的逻辑状态变化；`+` 只在该 transition case
被选中后附加 named-do ActionPlan。named `do` 内 `,` 是串行依赖、`|` 是并行；
同一模板重复使用时可写 `first: StartTask(...)` 给实例稳定前缀。contract 使用
封闭枚举而不是字符串 attributes，并随每个 ActionCall 输出。`context=fixed`
固定的是逻辑路由上下文，不是 authority。`consistent_by`、幂等、delivery 和 retry
是 host adapter 必须兑现并提供证据的 contract，DTESSL 不会凭声明伪造物理事实。

`result=nondeterministic` 只能配 `replay=reinject`：其结果不能参与当前 transition
的 after/set；它必须以后续 typed input 进入模型。replay 的 reinject 指重注已记录
结果，绝不重新调用 Provider。`unpredictable`、`uniform`、`cryptographic` 之类质量
主张仍属于 Provider evidence，不是语言中写一个词就成立的事实。

`ensure:` 与 `where:` 不同：`where` 只看当前 Embedding 并决定边是否可用；
`ensure` 在边发生后的 successor 上激活时序 obligation，由 finite Trace monitor 或
`EmbeddingExpand × ClaimMonitor` 检查。执行器绝不会为了判断 `eventually` 而预知未来。
时序同样使用关系表面：`<a, b> ~ happens_before` 是 trace-domain 的
`TraceRelationMatch`，可嵌套在 `always/eventually/until/within/since` 中；它不会进入
只看当前 Embedding 的瞬时 evaluator。有限 trace 可直接判定完整嵌套，当前 Solver
无法编译的 future-under-future 片段明确返回 `inconclusive`。
Procedure 中直接写的 `(source-set) -> (target-set):` 是匿名局部 transition，Solver 只在
该 Procedure 的状态空间中展开它；它不是按源码顺序执行的 workflow step。

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
`capture closed/projected` 的 canonical AST 由 state/transition/procedure seed relation 与可选的
temporal rule 组成。三种 seed 是并集；它们只标记实际 occurrence，procedure 只是 occurrence
上的可选分组标签，游离 state/transition 完全合法。闭合捕获按 `OccurrenceId` 展开显式因果前驱；
`eventually state(...)` 或 `eventually transition(...)` 再为每个 seed 打开时间区间，保留到首个
见证 occurrence 为止。开放流显示 `pending`，闭合但无见证显示 `unresolved`。capture 只观察
DTESSL 原生 typed step，不读取外部日志。
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

`capture closed` 输出通用 `TraceArtifact`：可见 trace 仍只含 occurrence/时间闭包，但 artifact
保存从初始状态到最后一个被选 RoundId 的 typed input 前缀及完整预期逻辑结果，因此能够确定性
重建并逐轮核对 path、Embedding、因果边与 ActionPlan，且
适用于有或没有 procedure 的模型。`ProcedureArtifact` 只是带 procedure 标签 occurrence 的兼容
投影，不再是闭包所有者或重放权威。`capture projected` 只供观察，始终 `replayable=no`，也不会
生成可冒充完整重放输入的 artifact。

每个 accepted decision 在所属 RoundId 内直接保存不可变 `OccurrenceInput`：区分开放
Event dispatch 与精确 TransitionId injection，保留请求 symbol、规范化 event、全部 typed
fields、目标 procedure 和 initial context。同时保存 before/after active states 与 typed
values，及其派生 ActionPlan。procedure frame 也保存该 procedure 本轮的 before/after
Embedding 与 input 列表；空闲轮的两侧相等且 input 为空。`captured_round_at(trace, RoundId)` 和
`captured_procedure_at(trace, procedure, RoundId)` 分别按全局 round 与 procedure frame 查询。
`$port(...) @ context` 是出站 ActionPlan，不是入站变量读取；它的求值后 arguments 和 context
已随 decision 保存。任何宿主 context/value 必须作为 typed occurrence field value-copy 后进入。
DTESSL 不捕获活指针或环境内存；未来的大对象只能使用不可伪造的 immutable ArtifactRef/digest
profile。逻辑时间及 `[time=...]` 映射仍与 RoundId 分离，未在本阶段引入。

`state`、`transition`、`procedure` 可用统一后缀扩展把分散的 capture seed 汇入显式 `trace`：

```dtessl
state Active @ session [capture=Audit/default | Debug/sessionA]:
transition Dispatch() @ scheduler [capture=Audit/default]:
procedure Session @ system [capture=Audit/default]:
```

这里没有第二套 capture 声明。`Audit/default` 汇入 `trace Audit`；非默认 session 形成
`Trace/Session` 实例。若只有分散标注而没有显式 trace，生成的实例默认是
`capture projected`，不能冒充可重放证据；只有显式 `capture closed` 模板才赋予闭包行为。
state seed 同时匹配进入和离开该 state 的 decision，transition family seed 匹配它的命名
case。不同 seed 是并集；命中后的行为是
`mark occurrence -> causal predecessors -> optional temporal interval -> emit trace`。procedure
只参与 seed 匹配和结果分组，不会把同 procedure 的无关 occurrence 自动卷入；此过程不改变
relation matcher 的真假语义，也不执行状态转移。

REPL 直接复用 production parser、`Engine`、`RuntimeContext` 与通用
`replay_trace_artifact`，没有展示专用执行旁路。Occurrence/时间闭包的主演示是：

```text
dtessl repl examples/occurrence_capture.dtessl
:trace StartUntilDone
:replay-artifact StartUntilDone

:reset
:step Start
:step Progress
:trace-live StartUntilDone
:step Finish
:step After
:trace-live StartUntilDone close
:replay-artifact StartUntilDone live
```

`:step` 直接调用精确 TransitionId admission，所以游离 transition/state 无需伪造
procedure。`:trace` 执行源码声明，`:trace-live` 观察当前 Engine；两者都显示
OccurrenceId、因果前驱、temporal anchor/witness 和 typed input prefix。
`:replay-artifact` 将该 prefix 交回正式核心逐轮重算路径、before/after Embedding、因果边与
ActionPlan。例子中的第四个 `After` 与前三次属于同一 procedure，但不会被
`Start -> eventually Done` 时间区间卷入。

持久 procedure 的 `:start`、同 RoundId 多路 `:inject`、`:runtime`、`:capture` 与兼容
`:replay-procedures` 仍然保留；procedure artifact 只是通用 occurrence artifact 的兼容投影，
不拥有 capture closure。

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
- `relation Worker`/`relation <A,B>` 定义关系值；顶层 `relation Name(...)`
  定义惰性规则 relation，`subject ~ relation` 执行统一 RelationMatch；
- `relation Name: relation (...) = algebra(...)` 定义依赖当前 Embedding 的
  命名派生 RelationPlan；`Claim ... @ relation Name` 使用同一 Property/Solver
  基础设施搜索关系属性反例；
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

`relation T`/`relation <A,B,...>` 是独立的一等有限关系，不是隐藏的 JSON，也不是没有 schema 的 set。
每行是同 arity 的 `tuple<T...>`，按 canonical tuple 顺序排序并去重。当前关系代数包括：

- `project(r, column...)`、`join(left, li, right, ri)`；
- 二元关系的 `compose`、`inverse` 和非自反传递 `closure`；
- `union`、`intersection`、`difference`、`domain`、`range`、`product`、
  `identity`、`image`、`preimage`、`reflexive_closure`；
- `subset/disjoint/functional/injective/reflexive/irreflexive/symmetric/`
  `antisymmetric/transitive/acyclic/equivalence/partial_order/left_total/`
  `surjective/bijective/total_order` 等有限关系性质；
- `E/A` 量词、成员关系和 `count`；
- `select row ~ r where p by lex(score...)`，结果为 typed option。

选择按 score 升序；不同候选若完整 score 相同则拒绝整个 round，绝不以 hash/source
顺序暗中决胜。无候选返回 `none`。稳定 ID 应作为 `lex` 最后一项明确写出。

集合按规范顺序枚举，因此相同输入得到相同搜索、状态文本和动作 DAG。`exists` 只返回
真假，不把候选绑定泄漏到 `do`；需要选择候选时使用已实现的显式、可重放
`select ... by lex(...)`，不依赖哈希表顺序。

## 确定性与错误边界

- 每个 `@context` 状态轴必须且只能有一个 `initial` state；
- 同名事件在所有 transition 上必须拥有相同参数表；
- 字段、事件、赋值、谓词和动作实参在 `check` 时静态检查；
- 同一事件若没有可用 path，或没有显式优化却启用多个 `case` path，执行失败；
  显式优化也必须存在唯一最高分，否则拒绝；
- 新状态违反 invariant 时不提交状态，也不产出外部调用计划；
- 调用标签在一个 transition 内必须唯一；
- 输出的 map/set、调用和依赖边都有规范顺序。

`replay` 使用一个全新引擎重新计算同一事件，并比较完整 `StepResult`。库级
`DTESSL EventTrace` 是一组按 round 排列的原生 typed event batch，只重建逻辑状态与
ActionPlan。它不读取 host runtime journal、snapshot、Provider receipt 或自由文本，也不重新
执行物理副作用；runtime replay 不属于 DTESSL。

## 构建与运行

```sh
cmake -S . -B build -DDTESSL_BUILD_TESTS=ON
cmake --build build --target dtessl_cli dtessl_tests -j
build/dtessl check examples/scheduler.dtessl
build/dtessl version
build/dtessl features examples/scheduler.dtessl
build/dtessl plans examples/relations.dtessl
build/dtessl bench examples/encoding/stage1_dense_state/ring_32.dtessl Advance 10000
build/dtessl verify-claim examples/claims/safety_counterexample.dtessl NoFailure
build/dtessl claims examples/temporal_trace.dtessl History
build/dtessl verify-claim examples/temporal_procedure.dtessl StartedSinceDone
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

当前实现为 C++20 参考解释器与内置 Solver，使用离散 round 和精确数值。
模型检查受支持的时序片段及搜索预算限制；实际 I/O 和副作用由宿主处理。
开发计划见 [路线图](docs/ROADMAP.md)。
