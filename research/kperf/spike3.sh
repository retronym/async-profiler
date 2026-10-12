#!/bin/bash
# Manual integration test: event-aware PC-capture allocation, per-event capture stats,
# profiler.HardwareSample JFR events and jfrconv --hw.
set -uo pipefail
cd "$(dirname "$0")/../.."
JAVA_HOME=${JAVA_HOME:-/Library/Java/JavaVirtualMachines/temurin-27.jdk/Contents/Home}
SOCK=/tmp/asprof-kperf.sock
OUT=research/kperf/out
LIB=build/lib/libasyncProfiler.dylib
JOPTS="-XX:+PreserveFramePointer -XX:+UnlockDiagnosticVMOptions -XX:+DebugNonSafepoints -Xmx1g -cp $OUT"
EVENTS=cycles+cache-misses+instructions+branch-misses
mkdir -p $OUT
"$JAVA_HOME/bin/javac" -d $OUT research/kperf/Workload.java

sudo -v
sudo build/bin/asprof-kperf -s $SOCK > $OUT/helper3.log 2>&1 &
trap 'sudo pkill -f "asprof-kperf -s $SOCK" || true' EXIT
until [ -S $SOCK ]; do sleep 0.2; done

echo "=== 1. four events -> JFR"
"$JAVA_HOME/bin/java" $JOPTS -agentpath:$LIB=start,event=$EVENTS,kperf=$SOCK,file=$OUT/multi.jfr,log=$OUT/multi3.log Workload 15

echo "=== 2. same events -> collapsed (root pseudo-frames)"
"$JAVA_HOME/bin/java" $JOPTS -agentpath:$LIB=start,event=$EVENTS,kperf=$SOCK,collapsed,file=$OUT/multi3.collapsed Workload 10

sleep 1
sudo chown -R "$(id -u)" $OUT
cat $OUT/helper3.log $OUT/multi3.log
echo DONE
