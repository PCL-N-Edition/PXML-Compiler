# PXB-C 0.1 记录布局

本文冻结当前 C 编译器 `0.1.x` 的实现布局。它遵循 PXML 1.0 的 header/section-directory 方向，但仍是预发布格式；生产 loader 应同时验证 major/minor 和所需 section。

所有整数均为 little-endian。header 为 36 bytes：

| Offset | Type | Meaning |
|---:|---|---|
| 0 | char[4] | `PXB1` |
| 4 | u16 | format major (`1`) |
| 6 | u16 | format minor (`0`) |
| 8 | u32 | flags；bit 0 表示 release |
| 12 | u64 | content hash low |
| 20 | u64 | content hash high |
| 28 | u32 | section count |
| 32 | u32 | header size (`36`) |

紧随 header 的每个目录项为 32 bytes：`type:u32, flags:u32, offset:u64, size:u64, alignment:u32, reserved:u32`。section payload 起点统一 16-byte 对齐。

## Sections

- `STRS`：`count:u32`，随后 `count` 个相对字符串 blob 的 `offset:u32`，最后是 NUL 结尾 UTF-8 blob。
- `NODE`：`count:u32`，随后每项 44 bytes：parent、first child、child count、node kind、property offset/count、binding offset/count、flags、source line/column，全部为 u32。
- `PROP`：`count:u32`，随后每项 24 bytes：node index、stable property ID、value kind、name string offset、value string offset、flags。
- `BIND`：`count:u32`，随后每项 28 bytes：node index、property ID、markup kind、name/expression string offset、dependency offset/count。
- `DEPS`：`count:u32`，随后为 stable dependency `u64` ID。
- `META`：10 个 u32：compiler semver、language version、profile、release flag、node/property/binding counts。
- `SMAP`：仅 debug build；`count:u32`，随后每项为 node index、line、column 三个 u32。

空索引用 `0xffffffff`。当前内容指纹覆盖 header 之后的完整目录和 sections，但它不是密码学 hash，也不替代 PXPK 的签名/完整性校验。

`node kind = 5` 是保留的旧值，编译器不得产生它。`Button`、`TextBox` 等框架控件必须先从调用方提供的预定义目录加载对应 PXML Component，并在进入 `NODE` 前完全展开为 Primitive；因此 PXB 中不存在控件 class/node kind。
