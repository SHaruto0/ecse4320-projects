#!/usr/bin/env bash
# Record the measurement environment to results/env.txt
out=results/env.txt; mkdir -p results
{
  echo "date: $(date -Is)"; echo "== uname"; uname -a
  echo "== os"; (cat /etc/os-release 2>/dev/null | head -4) || sw_vers
  echo "== cpu"; lscpu 2>/dev/null || sysctl -a | grep -E 'machdep.cpu|hw\.(perflevel|l1|l2|l3)'
  echo "== compiler"; ${CC:-gcc} --version | head -1; python3 --version
  echo "== governor"; cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c
  echo "== cur freq (kHz)"; cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq 2>/dev/null | head -4
  echo "== turbo (intel_pstate no_turbo / acpi boost)"; cat /sys/devices/system/cpu/intel_pstate/no_turbo /sys/devices/system/cpu/cpufreq/boost 2>/dev/null
  echo "== SMT"; cat /sys/devices/system/cpu/smt/control 2>/dev/null
  echo "== memory"; free -h 2>/dev/null; sudo -n dmidecode -t memory 2>/dev/null | grep -E 'Speed|Type:' | sort | uniq -c
  echo "== isolation"; cat /sys/devices/system/cpu/isolated 2>/dev/null; cat /proc/cmdline 2>/dev/null
  echo "== perf"; perf --version 2>/dev/null
} > $out 2>&1
echo "wrote $out"
