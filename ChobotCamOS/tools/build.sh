#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BR_VERSION="2026.02"
OUT="${1:-$HERE/build-work}"
mkdir -p "$OUT"
if [ ! -d "$OUT/buildroot" ]; then
  curl -fL "https://buildroot.org/downloads/buildroot-${BR_VERSION}.tar.xz" -o "$OUT/buildroot.tar.xz"
  tar -xJf "$OUT/buildroot.tar.xz" -C "$OUT"
  mv "$OUT/buildroot-${BR_VERSION}" "$OUT/buildroot"
fi
cd "$OUT/buildroot"
make O="$OUT/output" BR2_EXTERNAL="$HERE" raspberrypi3_64_defconfig
CFG="$OUT/output/.config"
# Keep RPi kernel, firmware, disk layout from the upstream Raspberry Pi preset.
# Modify generated defconfig safely without relying on an external config utility.
python3 - "$CFG" "$HERE" <<'PYCFG'
import sys
p, home = sys.argv[1:]
updates = {
    'BR2_PACKAGE_CHOBOTCAM': 'y',
    'BR2_ROOTFS_OVERLAY': '"' + home + '/board/overlay"',
    'BR2_ROOTFS_POST_IMAGE_SCRIPT': '"' + home + '/board/post-image.sh board/raspberrypi3-64/post-image.sh"',
    'BR2_SYSTEM_DHCP': '""',
    'BR2_TARGET_GENERIC_HOSTNAME': '"chobotcam"',
    'BR2_TARGET_GENERIC_ISSUE': '"ChobotCam OS FRC 9585"',
    'BR2_TARGET_ROOTFS_EXT2_SIZE': '"256M"',
}
with open(p) as f:
    lines = f.readlines()
with open(p, 'w') as f:
    for line in lines:
        if any(line.startswith(k+'=') or line.startswith('# '+k+' is not set') for k in updates):
            continue
        f.write(line)
    for k, v in updates.items():
        f.write(k + '=' + v + '\n')
PYCFG
make O="$OUT/output" BR2_EXTERNAL="$HERE" olddefconfig
make O="$OUT/output" BR2_EXTERNAL="$HERE" -j"$(nproc)"
IMG="$OUT/output/images/sdcard.img"
[ -s "$IMG" ] || { echo "No sdcard.img produced";exit 1; }
cp "$IMG" "$HERE/ChobotCamOS_FRC9585.img"
xz -T0 -k -f "$HERE/ChobotCamOS_FRC9585.img"
echo "DONE: $HERE/ChobotCamOS_FRC9585.img.xz"
