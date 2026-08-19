# PXML Compiler

PXML Compiler 是 PXML 1.0 的官方原生 C17 工具链。项目由三个可独立使用的阶段库和程序组成，并提供一个不落盘中间结果的完整驱动器：

```text
pxml-expand   PXML + component directories/build symbols -> expanded PXML
pxml-opt      expanded PXML                  -> compact PXIR
pxml-compile  optimized PXIR                 -> PXB1
pxmlc --full  source -> expand -> optimize -> compile（内存内串联）
```

仓库：[PCL-N-Edition/PXML-Compiler](https://github.com/PCL-N-Edition/PXML-Compiler)

## 性能架构

- Lexer 按需产生一个 token，token 只保存源切片，不分配 token object。
- AST、Compact IR 和编译 blueprint 使用分块 bump arena；一次编译结束时统一释放。
- 字符串通过开放寻址 intern table 变成 `uint32_t` ID；PXIR node/property 是连续定长数组。
- Optimizer 对 compact linear IR 做迭代扫描，不在 pointer tree 上执行多轮递归 pass。
- 热字节扫描保留 scalar baseline，并在支持的平台运行时选择 AVX2；ARM64 使用 NEON。
- PXB writer 先计算 section layout，再一次分配最终 binary，最后一次写文件。
- 三个阶段拥有各自的真实 PGO workload 和 profile，不共享“综合 corpus”。

更完整的所有权与数据布局见 [docs/architecture.md](docs/architecture.md)。PXIR/PXB 的首版记录格式见 [docs/PXB-C-0.1.md](docs/PXB-C-0.1.md)。

## 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

正式 Release 使用：

```text
-O3 -flto=full -DNDEBUG -fomit-frame-pointer
```

Windows/Linux 链接增加 `-fuse-ld=lld`。官方矩阵为 Windows、Linux、macOS 的 x86-64/ARM64 六个平台；Windows/Linux 使用 LLVM Clang，macOS 使用 Apple Clang。

## 使用

```bash
pxml-expand samples/Hello.pxml -o Hello.expanded.pxml \
  --predefined-dir components/predefined \
  --component samples/components/ActionCard.pxml -D WINDOWS

pxml-opt Hello.expanded.pxml -o Hello.pxir
pxml-compile Hello.pxir -o Hello.pxb --strict

# 常规 IDE/构建路径：中间表示不落盘
pxmlc --full samples/Hello.pxml -o Hello.pxb \
  --predefined-dir components/predefined \
  --component samples/components/ActionCard.pxml -D WINDOWS --strict --release

pxmlc inspect Hello.pxb
pxmlc dump Hello.pxb
```

公开静态库为 `libpxml_core`、`libpxml_expand`、`libpxml_opt`、`libpxml_compile` 与 `libpxml_full`。

`--predefined-dir` 是框架控件目录。展开器遇到非 Primitive 的 `<Button>`、`<TextBox>` 等元素时，按名称读取 `Button.pxml`、`TextBox.pxml`，然后执行普通 Component 展开。控件定义不编译进展开器；Release 将默认定义安装到 `share/pxml/components`。显式 `--component` 继续用于项目私有组件，且重名定义会被拒绝。

## PGO

`scripts/build_pgo.py` 产生并分别使用：

```text
pxml-expand.profdata    component/import/slot/build-condition/template authoring workload
pxml-opt.profdata       expanded PXML -> compact optimized PXIR workload
pxml-compiler.profdata  optimized PXIR -> PXB workload
```

`pxmlc` 使用三份 profile 的 merge，使共享 hot functions 获得实际阶段数据，但训练时三种 corpus 和 `.profraw` 目录严格隔离。Release 包内保留三份 `.profdata` 与 `pgo-build.json` 便于审计。

## 安全边界

解析器拒绝 DTD、外部实体和任意 processing instruction，只解码 XML 五个预定义实体；组件展开有深度上限，binding 调用只允许白名单纯函数。PXIR/PXB reader 会验证 magic、版本、长度、索引、树拓扑、section 对齐/重叠和内容指纹。

当前内容指纹用于确定性与损坏检测，不是密码学签名。许可证：Apache-2.0。
