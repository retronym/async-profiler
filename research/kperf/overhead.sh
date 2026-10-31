#!/bin/bash
# Sampling overhead: does counting kernel mode (the kernel's per-sample work) depress IPC?
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
sudo build/bin/asprof-kperf -s $SOCK > $OUT/helper-overhead.log 2>&1 &
trap 'sudo pkill -f "asprof-kperf -s $SOCK" || true' EXIT
until [ -S $SOCK ]; do sleep 0.2; done

run() {  # name, env, events
  echo "=== $1"
  env $2 "$JAVA_HOME/bin/java" $JOPTS ${3:+-agentpath:$LIB=start,event=$3,kperf=$SOCK,file=$OUT/ovh-$1.jfr,log=$OUT/ovh-$1.log} Workload 15 > $OUT/ovh-$1.out 2>&1
  grep rounds $OUT/ovh-$1.out
  [ -n "$3" ] && cat $OUT/ovh-$1.log
}
run baseline "" ""
run ci-kernel KPERF_KERNEL=1 cycles+instructions
run all-kernel KPERF_KERNEL=1 cycles+instructions+cache-misses+branch-misses
run all-user "" cycles+instructions+cache-misses+branch-misses
run ci-user "" cycles+instructions
# High PMI rates (tens of thousands per second, as in the scalac runs)
run hi-kernel KPERF_KERNEL=1 cycles@100000+instructions@100000+cache-misses@1000+branch-misses@1000
run hi-user "" cycles@100000+instructions@100000+cache-misses@1000+branch-misses@1000

sleep 1
sudo chown -R "$(id -u)" $OUT
for f in ci-kernel all-kernel all-user ci-user hi-kernel hi-user; do
  "$JAVA_HOME/bin/jfr" print --json --events profiler.HardwareSample $OUT/ovh-$f.jfr | python3 -c "
import json,sys,collections; v=collections.Counter()
for e in json.load(sys.stdin)['recording']['events']: v[e['values']['event']]+=e['values']['value']
print('$f: IPC %.2f' % (v['instructions']/v['cycles']), ' misses/kinst %.2f' % (1000*v['cache-misses']/v['instructions']) if v['cache-misses'] else '')"
done
cat $OUT/helper-overhead.log
echo DONE
