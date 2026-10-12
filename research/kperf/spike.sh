#!/bin/bash
# Manual integration test of the asprof-kperf helper + KperfEvents engine.
set -euo pipefail
cd "$(dirname "$0")/../.."
JAVA_HOME=${JAVA_HOME:-/Library/Java/JavaVirtualMachines/temurin-27.jdk/Contents/Home}
SOCK=/tmp/asprof-kperf.sock
OUT=research/kperf/out
mkdir -p $OUT
"$JAVA_HOME/bin/javac" -d $OUT research/kperf/Workload.java

sudo -v
sudo build/bin/asprof-kperf -s $SOCK > $OUT/helper.log 2>&1 &
trap 'sudo pkill -f "asprof-kperf -s $SOCK" || true' EXIT
until [ -S $SOCK ]; do sleep 0.2; done

for ev in cycles instructions cache-misses; do
  echo "=== $ev"
  "$JAVA_HOME/bin/java" -XX:+PreserveFramePointer -XX:+UnlockDiagnosticVMOptions -XX:+DebugNonSafepoints -Xmx1g \
    -agentpath:build/lib/libasyncProfiler.dylib=start,event=$ev,kperf=$SOCK,collapsed,file=$OUT/spike-$ev.collapsed \
    -cp $OUT Workload 15
done
cat $OUT/helper.log
echo DONE
