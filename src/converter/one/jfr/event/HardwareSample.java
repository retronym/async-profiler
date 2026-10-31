/*
 * Copyright The async-profiler authors
 * SPDX-License-Identifier: Apache-2.0
 */

package one.jfr.event;

import one.jfr.JfrReader;

// profiler.HardwareSample: a PMU overflow sample (kperf on macOS);
// value is the number of events it represents (period x coalesced PMIs)
public class HardwareSample extends Event {
    public final String event;
    public final long value;
    public final boolean precise;

    public HardwareSample(JfrReader jfr) {
        super(jfr.getVarlong(), jfr.getVarint(), jfr.getVarint());
        this.event = jfr.getString();
        this.value = jfr.getVarlong();
        this.precise = jfr.getBoolean();
    }

    @Override
    public long value() {
        return value;
    }
}
