# Android 16 代码与设备兼容性复核

复核日期：2026-11-14（Asia/Singapore）。基线：`801c598352c7f4ea366f980656fe16a5e72dad14`。
范围：Zygote JNI、ART 初始化、链接器依赖、安装脚本、Root 条件、CPU 架构及 16 KB 二进制布局。
这是源码、构建产物和主机测试复核；没有连接手机或运行已 Root 的 Android 16 模拟器。

## 结论

当前生成的 JNI 表已覆盖 AOSP Android 16 正式版、QPR1 和 QPR2 的三个主要入口。
Android 16 QPR2 已增加 `useFifoUi`，不是 Android 17 才出现；本次纠正生成器注释，
无需更改已有 JNI 包装函数。每个版本的三个签名都加入回归检查。

发现并修复 ART 初始化失败后缓存残留的确定性缺陷：过去拿到 `artMethod` 字段 ID 后
就会记录为已初始化，后续查找失败时第二次调用可能返回成功，但偏移尚未设置。
现在只在完整初始化后发布缓存；检查缺失类、方法、数组、反射对象与 JNI 异常；
拒绝逆序、过小、过大或未对齐的构造器间距，避免无符号下溢和明显无效偏移。
最大间距 256 字节是保守的拒绝边界，不是 Android 对 `ArtMethod` 的稳定 ABI 承诺。

使用真正的生产头文件和模拟 JNI 函数表复现了旧代码的失败路径。18 个独立进程场景
覆盖成功、缺失对象、异常、异常布局，以及失败后恢复与成功缓存复用，启用 UBSan。
这不会证明所有厂商 ART 内存布局有效；仍保留“前两个构造器相邻、末尾两个字段为指针”的假设。

## AOSP 对照基准

直接读取以下固定版本的 `core/java/com/android/internal/os/Zygote.java`，提取方法声明并转换为 JNI 签名。

| 分支 | frameworks/base 提交 | nativeForkAndSpecialize | 另两个入口 |
| --- | --- | --- | --- |
| android16-release | `99b01a65cc4c104933788b3143285ab6bae65827` | 匹配 `fas_u` | 匹配 |
| android16-qpr1-release | `33b96ce8a122757002e5040ac59824bd7a262e00` | 匹配 `fas_u` | 匹配 |
| android16-qpr2-release | `45034f0663f960d9ee5fb0a101a4732b71f6e2f4` | 匹配 `fas_c`，在 `isTopApp` 后增加 `useFifoUi` | 匹配 |

另两个入口为 `nativeSpecializeAppProcess` 和 `nativeForkSystemServer`。
检查的是以上 AOSP 分支，不代表每个安全补丁、QPR3 或厂商分支已经分别核验。

## 设备支持边界

| 条件 | 代码或产物证据 | 结论 |
| --- | --- | --- |
| Android 16 / API 36 | compileSdk=36、minSdk=26；三个 AOSP 版本 JNI 匹配 | 有接口适配基础，启动和注入待真机测试 |
| ARM64 / x86_64，4 KB 或 16 KB 内存页 | 前轮修复启用 NDK r27 flexible page sizes，并检查 LOAD/RELRO 布局 | 既有二进制对齐修复同样适用于 Android 16；每次 CI 验证新 ZIP |
| ARMv7 / x86 32 位设备 | 构建有对应 ELF，安装脚本接受 arm/x86 | 架构可用，不代表 Android 16 厂商 ROM 已认证 |
| 64 位设备中的 32 位应用 | 安装只提取主架构；一个 tracer 管理一个 ABI | 不能宣称覆盖第二套 32 位 Zygote |
| Samsung、HyperOS/MIUI、ColorOS、OriginOS 等 | 部分厂商 JNI 变体存在；没有各厂商设备和源码比对结果 | 无品牌通用兼容认证，需具体 ROM 和日志 |
| Magisk | 安装版本门槛 >=26402 | 仅为项目门槛，不是所有后续版本或分支的兼容保证 |
| KernelSU | 内核版本 >=10940 且 ksud >=11425；安装时验证 SELinux 规则 | 分支、内核和管理器需一起记录 |
| APatch | 版本门槛 >=10762 | 需验证实际内核、权限和启动过程 |
| 多种 Root 共存 | KSU/APatch 安装路径会检查 Magisk | 不支持混合 Root；其他 Zygote 注入器可能争用 ptrace |
| SELinux enforcing | 已有 tmpfs 与 memfd_file 规则 | Android 16 不应直接套用 Android 17 的新设备要求，按内核能力、标签和 AVC 日志核对 |
| Huawei Maple | 安装时若开启则写入 ro.maple.enable=0 | 需要重启，不能据此保证特定华为 ROM 兼容 |

Android 16 的“4 KB 应用兼容模式”针对应用加载，不能作为在 Zygote 内注入任意原生库的保证。
NeoZygisk 本身和每个第三方 Zygisk 模块都需满足实际页大小要求。Root ZIP 解压后的 ELF
检查与 APK zipalign 是不同检查，不能用 APK 对齐结论代替模块二进制检查。

## 剩余风险

1. **ART 私有布局**：Android 官方明确指出，ART 内部实现可通过 Google Play 系统更新变化，
   不与 Android 版本号一一对应。新增检查防止确定的失败路径，但不验证任意偏移指向的字段语义。
2. **JNI 注册状态**：`hook_jni_methods` 尚未检查 `RegisterNatives` 返回值；
   反射指针和部分 hook 安装失败仍需要专门处理。守护进程在线或状态显示 injected 不是所有 hook 成功的证据。
3. **链接器与 ELF**：项目通过私有符号和启发式偏移访问 `soinfo`；Android 16 AOSP 中可见
   `solinker`/`solist`，但仅有源码符号不能证明厂商二进制保留相同符号、后缀和结构。
   ELF 节表与哈希表边界检查仍不足；本次未改写解析器或私有链接器操作。
4. **设备内核**：ptrace、SELinux、seccomp、MTE/BTI 及系统重启路径未经过真机验证。
   不应通过关闭 SELinux 或设备安全保护来将失败标记为兼容。

## 可复现检查与真机补充

```sh
bash tests/host/run.sh
python3 -m unittest discover -s tests/compatibility -v
./gradlew zipRelease zipDebug
python3 tests/compatibility/check_elf.py module/build/outputs/release/NeoZygisk-*.zip
adb shell sh < tests/compatibility/device_info.sh
```

主机 JNI 检查需 C++20 编译器和 JDK，详见 [README](README.md)。新增用例均接入 Actions。
设备信息脚本只读，未收集序列号、账号或应用列表。还需提供 ROM 完整版本、Root 管理器/内核版本、
Google Play 系统更新日期和启用的 Zygisk 模块；目前没有这些目标设备信息。

真机需分别检查冷启动、重复重启、普通应用、USAP、WebView/子 Zygote、system_server，
并检查崩溃与 AVC 日志。4 KB 测试不代表 16 KB 测试；64 位应用测试不代表 32 位应用测试。
设备应具备可恢复路径，先验证基础模块，再单独加入第三方模块。

## 官方来源

- [Android 16 ART 内部变化及 16 KB 兼容模式](https://developer.android.com/about/versions/16/behavior-changes-all)
- [16 KB 页面与原生库要求](https://developer.android.com/guide/practices/page-sizes)
- [Android 16 Zygote](https://android.googlesource.com/platform/frameworks/base/+/99b01a65cc4c104933788b3143285ab6bae65827/core/java/com/android/internal/os/Zygote.java)
- [Android 16 QPR1 Zygote](https://android.googlesource.com/platform/frameworks/base/+/33b96ce8a122757002e5040ac59824bd7a262e00/core/java/com/android/internal/os/Zygote.java)
- [Android 16 QPR2 Zygote](https://android.googlesource.com/platform/frameworks/base/+/45034f0663f960d9ee5fb0a101a4732b71f6e2f4/core/java/com/android/internal/os/Zygote.java)
- [AOSP Android 16 ArtMethod](https://android.googlesource.com/platform/art/+/android16-release/runtime/art_method.h)
- [AOSP Android 16 linker_main.cpp](https://android.googlesource.com/platform/bionic/+/android16-release/linker/linker_main.cpp)
