/*
 * Copyright The async-profiler authors
 * SPDX-License-Identifier: Apache-2.0
 */

package one.convert;

import one.jfr.JfrReader;
import one.jfr.StackTrace;
import one.jfr.event.AllocationSample;
import one.jfr.event.Event;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.io.IOException;
import java.io.OutputStream;
import java.io.PrintStream;

import static one.convert.Frame.*;

/**
 * Converts .jfr output to HTML Flame Graph.
 */
public class JfrToFlame extends JfrConverter {
    private final FlameGraph fg;

    public JfrToFlame(JfrReader jfr, Arguments args) {
        super(jfr, args);
        this.fg = new FlameGraph(args);
    }

    // With --spans: the samples of the current chunk with their times, resolved to frames after aggregation
    private Timeline timeline;
    private final List<Spans.TaggedEvent> chunkEvents = new ArrayList<>();
    private long[] chunkTimes = new long[1024];

    @Override
    protected void onSample(Spans.TaggedEvent event, long timeNanos) {
        if (timeline == null) {
            timeline = new Timeline();
            fg.setTimeline(timeline, spans());
        }
        if (chunkEvents.size() == chunkTimes.length) chunkTimes = Arrays.copyOf(chunkTimes, chunkTimes.length * 2);
        chunkTimes[chunkEvents.size()] = timeNanos;
        chunkEvents.add(event);
    }

    // Samples are aggregated before they reach the flame graph: the aggregator's grouping, to find the frame of each
    private final class GroupKey {
        final Event event;

        GroupKey(Event event) {
            this.event = event;
        }

        @Override
        public int hashCode() {
            return event.hashCode() + (args.threads ? event.tid * 31 : 0);
        }

        @Override
        public boolean equals(Object o) {
            if (!(o instanceof GroupKey)) return false;
            Event e = ((GroupKey) o).event;
            return e.stackTraceId == event.stackTraceId && (!args.threads || e.tid == event.tid) && e.sameGroup(event);
        }
    }

    @Override
    protected boolean supportsSpans() {
        // the span path is a leaf of each stack; reversed, it would be the root (the viewer has its own bottom-up view)
        return !args.reverse && !(args.nativemem && args.leak);
    }

    @Override
    protected void convertChunk() {
        Map<GroupKey, Frame> leaves = timeline != null ? new HashMap<>() : null;
        collector.forEach(new AggregatedEventVisitor() {
            final CallStack stack = new CallStack();

            @Override
            public void visit(Event event, long value) {
                Event key = event;
                String spanTag = null;
                if (event instanceof Spans.TaggedEvent) {
                    spanTag = ((Spans.TaggedEvent) event).tag;
                    event = ((Spans.TaggedEvent) event).event;
                }
                StackTrace stackTrace = jfr.stackTraces.get(event.stackTraceId);
                if (stackTrace != null) {
                    Arguments args = JfrToFlame.this.args;
                    long[] methods = stackTrace.methods;
                    byte[] types = stackTrace.types;
                    int[] locations = stackTrace.locations;

                    if (args.threads) {
                        stack.push(getThreadName(event.tid), TYPE_NATIVE);
                    }
                    if (args.classify) {
                        Classifier.Category category = getCategory(stackTrace);
                        stack.push(category.title, category.type);
                    }
                    for (int i = methods.length; --i >= 0; ) {
                        String methodName = getMethodName(methods[i], types[i]);
                        int location;
                        if (args.lines && (location = locations[i] >>> 16) != 0) {
                            methodName += ":" + location;
                        } else if (args.bci && (location = locations[i] & 0xffff) != 0) {
                            methodName += "@" + location;
                        }
                        stack.push(methodName, types[i]);
                    }
                    long classId = event.classId();
                    if (classId != 0) {
                        stack.push(getClassName(classId), (event instanceof AllocationSample)
                                && ((AllocationSample) event).tlabSize == 0 ? TYPE_KERNEL : TYPE_INLINED);
                    }
                    if (spanTag != null) {
                        stack.push(spanTag, TYPE_SPAN);
                    }

                    Frame leaf = fg.addSample(stack, value);
                    if (leaves != null && leaf != null) leaves.put(new GroupKey(key), leaf);
                    stack.clear();
                }
            }
        });
        if (leaves != null) {
            for (int i = 0; i < chunkEvents.size(); i++) {
                Spans.TaggedEvent event = chunkEvents.get(i);
                Frame leaf = leaves.get(new GroupKey(event));
                if (leaf != null) timeline.add(event.tid, getThreadName(event.tid), chunkTimes[i], leaf);
            }
        }
        chunkEvents.clear();
    }

    public void dump(OutputStream out) throws IOException {
        fg.dump(out);
    }

    public static FlameGraph parse(String input, Arguments args) throws IOException {
        try (JfrReader jfr = new JfrReader(input)) {
            JfrToFlame converter = new JfrToFlame(jfr, args);
            converter.convert();
            return converter.fg;
        }
    }

    public static void convert(String input, String output, Arguments args) throws IOException {
        FlameGraph fg = parse(input, args);
        try (PrintStream out = new PrintStream(output, "UTF-8")) {
            fg.dump(out);
        }
    }
}
