# Android compatibility review

Review date: 2026-11-12 UTC. Baseline: `7d0cc085578c5cbd90f7aae59affb11983872dc4`.
This is a source and packaged-binary review, not a device certification. No phone,
rooted emulator, vendor ROM or Android 17 boot test was available for this review.

## Confirmed defect and correction

The Release ZIP from [CI run 43301322905](https://github.com/rhtnll2026/NeoZygisk/actions/runs/43301322905)
contains 4 KiB-aligned `PT_LOAD` segments in both `libzygisk.so` and
`libzygisk_ptrace.so` for **arm64-v8a and x86_64**. The corresponding Rust
`zygiskd` binaries already have 16 KiB alignment. A successful build therefore
did not establish support for 16 KiB devices.

The loader now enables NDK r27's flexible page-size mode and supplies both
`max-page-size=16384` and `common-page-size=16384` at the final link of both
64-bit C++ targets. The latter also protects against a RELRO boundary covering
adjacent writable data. The NDK remains pinned to `27.2.12479018`.

CI checks **both module ZIPs** before uploading artifacts. It checks all 12
packaged native binaries for their ELF class/machine and program headers, and
checks LOAD alignment, offset/address congruence and RELRO layout for the six
64-bit binaries. It does not execute the binaries or certify third-party Zygisk
modules. Each third-party native library needs its own 16 KiB support.

## Android 17 and device matrix

| Area | Evidence | Conclusion |
| --- | --- | --- |
| Android 17 / API 37, classic Zygote | All three native entry-point signatures in AOSP `android17-release` match `jni_hooks.hpp`; generated header reproduces exactly | Signature coverage verified; runtime injection still needs a device |
| Android 17 QPR2 / vendor JNI variants | Generator contains additional variants, including `cgroup_uid`, Samsung and GrapheneOS variants | Presence verified only; not a claim that every QPR/vendor build matches |
| ARM64 / x86_64 on 4 KiB or 16 KiB | Explicit final-link alignment and CI validation of the built ZIPs | Binary-layout support is checked; boot/runtime behavior remains untested |
| ARMv7 / x86 | Packaged 32-bit ELF binaries; installer accepts `arm` and `x86` | Available for 32-bit devices; no 16 KiB claim for these ABIs |
| 32-bit apps on a 64-bit device | `customize.sh` installs only the primary architecture; boot shim starts one tracer; `AppMonitor` owns one ABI manager | Secondary 32-bit Zygote coverage is not provided by the current installation design |
| New native `zygote_next` | AOSP contains a separate Rust implementation; this project recognizes `app_process32/64` and hooks Java Zygote methods | Not covered by this implementation; its presence does not prove classic Java Zygote was replaced on a device |
| Android 17 shared-memory policy | `sepolicy.rule` includes legacy tmpfs rules and `memfd_file` rules for zygote/system_server accessing magisk/su-created objects | Required rule classes are present; actual creator label, kernel policy capability and AVCs must be checked on-device |
| Magisk / KernelSU / APatch | Installer requires Magisk >=26402, KernelSU kernel >=10940 plus ksud >=11425, APatch >=10762 | These are the project's minimum gates, not compatibility certification of every fork/version |
| Huawei Maple | Installer sets `ro.maple.enable=0` when enabled | Requires reboot; hot-install is intentionally not requested when a system property file is written |
| Android 8+ minimum | `minSdk=26` and installer API gate | Build minimum only; not proof that all Android 8-17 devices were tested |

`compileSdk=36` is not itself proof of Android 17 incompatibility: this deliverable
is a native root module ZIP, not a target-SDK-37 APK. Raising the SDK number alone
would not validate private ART/linker structures, SELinux policy or boot behavior.

## Remaining source-review risks

These areas need focused runtime testing before claiming general device support:

- `android_util.hpp` derives private `ArtMethod` offsets from the first two
  reflected `Throwable` constructors. It assumes their order and spacing and
  sets its cached field ID before initialization has fully succeeded. A failed
  initialization followed by a retry can therefore report success with unset
  offsets. This review does not establish safe operation on a changed ART layout.
- `hook_jni_methods` does not check `RegisterNatives`' return value, while the
  entry function reports success after setting up hooks. A successful daemon
  connection/status is consequently insufficient evidence that every JNI hook
  was installed. Check logs for missing signatures and JNI exceptions.
- The ELF symbol parser trusts system-library section/hash-table bounds. Vendor
  ELF layout changes and missing private symbols need explicit negative tests.
- Runtime page arithmetic in the reviewed atexit helper uses `AT_PAGESZ`, and
  the PLT dependency (`lsplt`) uses `getpagesize()`. The unrelated 4096-byte
  socket/read buffers are not mmap alignment assumptions. Binary alignment
  checks still cannot validate all runtime mapping/unmapping behavior.

No new native-Zygote interception, secondary-ABI injection or SELinux-policy
expansion is introduced by the page-alignment correction.

## Reproduce the checks

```sh
python3 -m unittest discover -s tests/compatibility -v
bash tests/host/run.sh
./gradlew zipRelease zipDebug
python3 tests/compatibility/check_elf.py module/build/outputs/release/NeoZygisk-*.zip
```

The compatibility suite includes malformed ELF, 4 KiB alignment rejection,
RELRO cases, ABI checks, missing package entries, Android 17 JNI signatures and
generated-header consistency. The baseline Release ZIP fails on all four C++
64-bit binaries, demonstrating that the check catches the identified defect.

## Device verification still required

Collect a read-only inventory, without installing or changing settings:

```sh
adb shell sh < tests/compatibility/device_info.sh
```

Also record the root solution and its kernel/manager version, the vendor ROM
version and enabled Zygisk modules. The inventory intentionally omits device
serial numbers, accounts and installed-app lists.

On a device with a known recovery path, validate a cold boot and reboot first,
then normal app launch, USAP launch, WebView/child Zygote and system_server
stability. On dual-ABI devices, check the primary and secondary processes
separately. Repeat with SELinux enforcing and with the device's actual page
size; a 4 KiB run does not validate 16 KiB. Confirm module status and relevant
crash/AVC logs rather than relying only on successful installation. Test
late-load/hot-update and the chosen third-party modules separately.

## Primary sources

- [Android 17 behavior changes / API 37](https://developer.android.com/about/versions/17/behavior-changes-all)
- [16 KiB ELF and RELRO requirements](https://developer.android.com/guide/practices/page-sizes)
- [NDK r27 page-size configuration](https://github.com/android/ndk/wiki/Changelog-r27)
- [AOSP Zygote declarations](https://android.googlesource.com/platform/frameworks/base/+/94b4c163b7dfe5ce3607f7bb8456f9573f7de57d/core/java/com/android/internal/os/Zygote.java)
  (retrieved from `android17-release`, three signatures compared directly)
- [Android 17 SELinux shared-memory changes](https://source.android.com/docs/security/features/selinux/compatibility#shared-memory-changes)
- [AOSP native Zygote](https://android.googlesource.com/platform/system/zygote/+/refs/heads/android17-release)
