#!/usr/bin/env bash
# 依次编译所有运行时驱动（避免 sed 串联改错文件名）
set -u
cd "$(dirname "$0")" || exit 1
for f in cap_drv cap_drv2 cap_drv3 cap_drv4; do
  bash build_drv.sh "$f" >/tmp/${f}.log 2>&1
  if grep -qa "error" /tmp/${f}.log; then
    echo "=== $f FAILED ==="; iconv -f GBK -t UTF-8 /tmp/${f}.log | grep -a -i error | head -5
  else
    echo "$f ok"
  fi
done
