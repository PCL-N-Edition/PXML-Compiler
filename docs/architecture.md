# 编译器架构

## 数据流与进程边界

```text
source buffer
    -> on-demand lexer / zero-copy token views
    -> arena-backed syntax tree
    -> predefined-directory lookup + component / slot / build-condition expansion
    -> compact PXIR (uint32 IDs, interned strings, contiguous arrays)
    -> fused linear optimizer
    -> typed blueprint lowering
    -> exact PXB layout + one final allocation
    -> one file write
```

三个阶段保持独立：

- `libpxml_expand` / `pxml-expand` 只接受 authoring PXML 与组件、构建符号，输出 expanded PXML。
- `libpxml_opt` / `pxml-opt` 把 expanded PXML lower 到 PXIR，并在线性 IR 上优化。
- `libpxml_compile` / `pxml-compile` 只接受 optimized PXIR，验证属性、markup/binding 并输出 PXB。
- `libpxml_full` / `pxmlc --full` 在一个进程内串联三阶段，不序列化、重读或落盘中间结果。

独立 CLI 用于诊断、测试与 PGO；正常 IDE/build integration 应优先使用 `pxmlc --full` 或 `libpxml_full`。

框架控件不硬编码在展开器中。调用方通过 `predefined_component_directory` / `--predefined-dir` 提供目录；非 Primitive 元素按安全的本地名称映射到 `<directory>/<Name>.pxml`。文件仍走唯一的 Parser、Component property/slot substitution 和递归展开路径，文件名与 `x:Name` 必须一致。由此新增控件只修改 PXML 定义，不修改或重编译 expander。

## Arena 与所有权

`PxmlDocument` 的 source、AST node、attribute/child arrays 和 interned strings 都在 64 KiB 分块 bump arena 中。扩容时旧连续块留在 arena，整个 document 生命周期结束时一次释放 block chain，不逐 node/attribute 调用 `free`。

`PxmlCompactIr` 使用自己的 document arena保存：

```c
PxmlCompactNode nodes[];          /* 32 bytes */
PxmlCompactProperty properties[]; /* 16 bytes */
PxmlStringId strings[];           /* uint32 IDs */
```

树关系是 `first_child`/`next_sibling` 的 `PxmlNodeId`，optimizer 不追逐 `Node *`。编译 blueprint 同样拥有独立 arena；属性名、值、binding expression 与 dependency arrays 随 blueprint 一次释放。

调用者仍拥有 component source buffer。`PxmlBuffer` 由 `pxml_buffer_destroy()` 释放；`PxmlDiagnosticList` 由 `pxml_diagnostics_destroy()` 释放。

## 字符串与哈希

Lexer token 是 `{kind, source pointer, length, span}`，parser 直接消费，不构造 heap token 数组。AST 文本只在需要保留时进入 document arena。AST、PXIR 与 PXB string table 都使用受控的开放寻址表；重复属性名、element 名和常见值比较 hash/ID，而不是复制后反复 `strcmp`。

## Optimizer

Source AST 只承担语法和展开。展开完成后立即 lower 到 compact PXIR。Optimizer 收集 compile-time constants、折叠精确 `{const Name}`、消费指令、规范化 Class token，并在最后一次 compaction 中重写 node/property arrays 与 ID edge。主 pass 是迭代线性扫描，不执行 recursive tree optimizer。

## 扫描与 CPU dispatch

只有适合向量化的 primitive 使用 SIMD：`scan_byte` 目前用于 text/attribute string 的 delimiter 和 newline 搜索。所有目标都有 scalar baseline；x86-64 Clang build 编译独立 AVX2 primitive 并运行时检查 CPU，ARM64 使用 NEON baseline。Parser、AST 和 optimizer 不做整段 intrinsic 化。

## Binary writer

PXIR writer预计算 string/node/property payload，单次分配并逐字段写 little-endian。PXB writer先构造各 section 内容，再计算完整 directory、16-byte alignment 与最终大小，只对最终 binary 执行一次 allocation；directory 原位写入，不创建逐 entry 临时 writer。CLI 最后以单次 `fwrite` 提交 binary。

## PGO contract

PGO 训练严格隔离：

```text
real component/import/slot/build/template source -> pxml-expand.profdata
real expanded PXML -> pxml-opt.profdata
real optimized PXIR -> pxml-compiler.profdata
```

每个阶段最终 executable 在独立 build directory 中使用自己的 profile。`pxmlc` 使用三份 stage profile 的 merge；不会拿一个综合或随机 corpus 同时训练三个程序。

## 确定性

输入遍历、IR compaction、section 顺序、字符串首次出现顺序和 stable-ID domain 都固定。文件格式逐字段编码，不落盘 C struct padding、pointer/address 或 hash-table bucket order。CI 对同一输入重复构建并要求 PXB 逐字节相同。
