#!/usr/bin/env bash
# Reduce noise before measuring (Linux, needs sudo). Undo by rebooting.
set -x
sudo cpupower frequency-set -g performance 2>/dev/null || \
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance | sudo tee $g >/dev/null; done
# Disable turbo so cycles/element = ns * fixed GHz is meaningful (pick ONE that exists)
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost 2>/dev/null
# Optional: disable SMT so the pinned core has no sibling noise
# echo off | sudo tee /sys/devices/system/cpu/smt/control
sudo sysctl -w kernel.perf_event_paranoid=1 2>/dev/null
