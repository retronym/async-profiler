#!/bin/bash
# Manual integration test: precise PC capture on PMC 5-7, multiple events, helper exclusivity.
set -uo pipefail
cd "$(dirname "$0")/../.."
JAVA_HOME=${JAVA_HOME:-/Library/Java/JavaVirtualMachines/temurin-27.jdk/Contents/Home}
SOCK=/tmp/asprof-kperf.sock
OUT=research/kperf/out
LIB=build/lib/libasyncProfiler.dylib
JOPTS="-XX:+PreserveFramePointer -XX:+UnlockDiagnosticVMOptions -XX:+DebugNonSafepoints -Xmx1g -cp $OUT"
mkdir -p $OUT
"$JAVA_HOME/bin/javac" -d $OUT research/kperf/Workload.java

sudo -v
sudo build/bin/asprof-kperf -s $SOCK > $OUT/helper2.log 2>&1 &
trap 'sudo pkill -f "asprof-kperf -s $SOCK" || true' EXIT
until [ -S $SOCK ]; do sleep 0.2; done

echo "=== 1. three events at once"
"$JAVA_HOME/bin/java" $JOPTS -agentpath:$LIB=start,event=cycles+cache-misses+instructions,kperf=$SOCK,collapsed,file=$OUT/multi.collapsed,log=$OUT/multi.log Workload 15

echo "=== 2. cache-misses, precise PC vs skid (line numbers)"
"$JAVA_HOME/bin/java" $JOPTS -agentpath:$LIB=start,event=cache-misses,kperf=$SOCK,collapsed,lines,file=$OUT/miss-precise.collapsed,log=$OUT/miss-precise.log Workload 15
KPERF_NO_PC_CAPTURE=1 "$JAVA_HOME/bin/java" $JOPTS -agentpath:$LIB=start,event=cache-misses,kperf=$SOCK,collapsed,lines,file=$OUT/miss-skid.collapsed,log=$OUT/miss-skid.log Workload 15

echo "=== 3. exclusivity: second session while the server is sampling"
"$JAVA_HOME/bin/java" $JOPTS -agentpath:$LIB=start,event=cycles,kperf=$SOCK,collapsed,file=$OUT/excl-a.collapsed Workload 20 > /dev/null &
A=$!
"$JAVA_HOME/bin/java" $JOPTS Workload 20 > $OUT/excl-b.log &
B=$!
sleep 8
sudo build/bin/asprof -e cycles -d 2 -o collapsed -f $OUT/excl-b.collapsed $B; echo "asprof exit status: $?"
wait $A $B

sleep 1
cat $OUT/helper2.log $OUT/multi.log $OUT/miss-precise.log $OUT/miss-skid.log
sudo chown -R "$(id -u)" $OUT
echo DONE
