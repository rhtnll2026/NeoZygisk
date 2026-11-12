#!/system/bin/sh
# Read-only compatibility inventory. No serial numbers, account data, app list,
# properties, mounts, SELinux policy or boot state are changed by this script.

echo 'NeoZygisk device compatibility inventory'
for prop in ro.product.manufacturer ro.product.model ro.build.version.release \
    ro.build.version.sdk ro.build.version.security_patch ro.product.cpu.abi \
    ro.product.cpu.abilist ro.product.cpu.abilist32 ro.product.cpu.abilist64 \
    ro.zygote ro.vendor.api_level ro.board.api_level \
    persist.zygote.zygote_next.start_on_boot; do
    printf '%s=%s\n' "$prop" "$(getprop "$prop")"
done
printf 'kernel=%s\n' "$(uname -r)"
printf 'machine=%s\n' "$(uname -m)"
pages=$(getconf PAGE_SIZE 2>/dev/null)
printf 'page_size=%s\n' "${pages:-unknown}"
printf 'selinux=%s\n' "$(getenforce 2>/dev/null)"
cap=/sys/fs/selinux/policy_capabilities/memfd_class
if [ -r "$cap" ]; then
    printf 'memfd_class=%s\n' "$(cat "$cap")"
else
    echo 'memfd_class=unavailable (absent or permission denied)'
fi
for process in zygote zygote64 zygote_next; do
    pids=$(pidof "$process" 2>/dev/null)
    printf '%s_pids=%s\n' "$process" "${pids:-not visible}"
done
