#!/bin/bash
# Manual integration test: sample Workload under three PMU events and write
# collapsed stacks to out/<event>.collapsed. Needs sudo for kperf_sample.
set -euo pipefail
cd "$(dirname "$0")"
JAVA_HOME=${JAVA_HOME:-/Library/Java/JavaVirtualMachines/temurin-27.jdk/Contents/Home}
mkdir -p out
clang -O2 -Wall -o kperf_sample kperf_sample.c
"$JAVA_HOME/bin/javac" -d out Workload.java

sudo -v
"$JAVA_HOME/bin/java" -XX:+PreserveFramePointer -Xmx1g -cp out Workload 90 > out/workload.log 2>&1 &
JPID=$!
trap 'kill $JPID 2>/dev/null || true' EXIT
until grep -q ready out/workload.log; do sleep 0.5; done
sleep 5 # let C2 compile the hot loops

for spec in CORE_ACTIVE_CYCLE:1000000 INST_ALL:5000000 L1D_CACHE_MISS_LD_NONSPEC:20000; do
  ev=${spec%%:*}; period=${spec##*:}
  echo "=== $ev (period $period)"
  sudo ./kperf_sample -p $JPID -e "$ev" -c "$period" -d 10 -o "out/$ev.collapsed"
done
sudo chown "$(id -u)" out/*.collapsed
echo DONE
