#!/usr/bin/env bash
# Record the Linux-side environment to results/env.txt (SSD model/temperature come from Windows; see README)
TF=${1:-$HOME/fio-test/testfile}; out=results/env.txt; mkdir -p results
{
  echo "date: $(date -Is)"; echo "== uname"; uname -a
  echo "== os"; head -4 /etc/os-release
  echo "== fio"; fio --version
  echo "== cpu"; lscpu | head -20
  echo "== memory"; free -h
  echo "== test file"; ls -l "$TF" 2>/dev/null; df -h "$(dirname "$TF")"; findmnt -T "$(dirname "$TF")"
  echo "== block devices"; lsblk -o NAME,SIZE,ROTA,TYPE,MOUNTPOINT,MODEL
  for q in /sys/block/sd*/queue /sys/block/vd*/queue /sys/block/nvme*/queue; do
    [ -d "$q" ] || continue
    echo "-- $q: scheduler=$(cat $q/scheduler) nr_requests=$(cat $q/nr_requests) rotational=$(cat $q/rotational) max_sectors_kb=$(cat $q/max_sectors_kb) logical_block=$(cat $q/logical_block_size)"
  done
} > $out 2>&1
echo "wrote $out"
