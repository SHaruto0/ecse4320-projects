#!/usr/bin/env bash
# Record the measurement environment to results/env.txt
out=results/env.txt; mkdir -p results
{
  echo "date: $(date -Is)"; echo "== uname"; uname -a
  echo "== os"; head -4 /etc/os-release
  echo "== cpu"; lscpu
  echo "== compiler"; ${CC:-gcc} --version | head -1; python3 --version
  echo "== governor / freq"; cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c
  echo "== SMT"; cat /sys/devices/system/cpu/smt/control 2>/dev/null
  echo "== memory"; free -h
  echo "== THP"; cat /sys/kernel/mm/transparent_hugepage/enabled /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null
  echo "== hugetlb"; grep -i huge /proc/meminfo
  echo "== NUMA"; lscpu | grep -i numa
  echo "== perf_event_paranoid"; cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null
} > $out 2>&1
echo "wrote $out"
