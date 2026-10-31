/*
 * Copyright The async-profiler authors
 * SPDX-License-Identifier: Apache-2.0
 */

package one.convert;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The samples of a flame graph with their times, and the spans of their threads, for the flame graph's time lane
 * (--spans): each sample points at the frame it lands in, as printed (after --minwidth folds thin subtrees), so the
 * viewer can rebuild the graph from the samples of any time range.
 */
final class Timeline {
    private static final class ThreadSamples {
        final String name;
        long[] times = new long[1024];
        Frame[] leaves = new Frame[1024];
        int size;

        ThreadSamples(String name) {
            this.name = name;
        }

        void add(long time, Frame leaf) {
            if (size == times.length) {
                times = java.util.Arrays.copyOf(times, size * 2);
                leaves = java.util.Arrays.copyOf(leaves, size * 2);
            }
            times[size] = time;
            leaves[size++] = leaf;
        }
    }

    private final Map<Integer, ThreadSamples> threads = new LinkedHashMap<>();
    private IdentityHashMap<Frame, Frame> parents;

    void add(int tid, String threadName, long timeNanos, Frame leaf) {
        threads.computeIfAbsent(tid, k -> new ThreadSamples(threadName)).add(timeNanos, leaf);
    }

    /** Remembers each frame's parent, before thin subtrees are folded away */
    void recordParents(Frame root) {
        parents = new IdentityHashMap<>();
        ArrayDeque<Frame> todo = new ArrayDeque<>();
        todo.push(root);
        while (!todo.isEmpty()) {
            Frame f = todo.pop();
            for (Frame child : f.values()) {
                parents.put(child, f);
                todo.push(child);
            }
        }
    }

    /** The printed frame a sample's leaf lands in: itself, or (folded away) its nearest printed ancestor, or that
     *  ancestor's span leaf with the same tag */
    private int printedIndex(Frame leaf) {
        if (leaf.printIndex >= 0) return leaf.printIndex;
        Frame f = leaf;
        while (f != null && f.printIndex < 0) f = parents.get(f);
        if (f == null) return -1;
        if (leaf.getType() == Frame.TYPE_SPAN) {
            Frame span = f.get(leaf.key);
            if (span != null && span.printIndex >= 0) return span.printIndex;
        }
        return f.printIndex;
    }

    /**
     * {"unit":"us","tags":[...],"threads":[{"name":..,"s":[dt,frame,...],"sp":[start,end,parent,tag,...]}]}: sample
     * times delta-coded, span times relative to the first sample, all in microseconds; `frame` is a printed frame's
     * index in output order, `parent` a span index or -1, `tag` an index into `tags`.
     */
    String toJson(Spans spans) {
        long t0 = Long.MAX_VALUE;
        for (ThreadSamples ts : threads.values()) {
            for (int i = 0; i < ts.size; i++) t0 = Math.min(t0, ts.times[i]);
        }
        if (t0 == Long.MAX_VALUE) return "null";
        List<String> tags = new ArrayList<>();
        Map<String, Integer> tagIds = new LinkedHashMap<>();
        StringBuilder sb = new StringBuilder(1 << 16);
        sb.append("{\"unit\":\"us\",\"threads\":[");
        boolean firstThread = true;
        for (Map.Entry<Integer, ThreadSamples> e : threads.entrySet()) {
            ThreadSamples ts = e.getValue();
            Integer[] order = new Integer[ts.size];
            for (int i = 0; i < ts.size; i++) order[i] = i;
            java.util.Arrays.sort(order, (a, b) -> Long.compare(ts.times[a], ts.times[b]));
            if (!firstThread) sb.append(',');
            firstThread = false;
            sb.append("{\"name\":").append(json(ts.name)).append(",\"s\":[");
            long prev = 0;
            boolean first = true;
            for (Integer i : order) {
                int idx = printedIndex(ts.leaves[i]);
                if (idx < 0) continue;
                long t = (ts.times[i] - t0) / 1000;
                if (!first) sb.append(',');
                first = false;
                sb.append(t - prev).append(',').append(idx);
                prev = t;
            }
            sb.append("],\"sp\":[");
            spans.appendSpans(sb, e.getKey(), t0, tags, tagIds);
            sb.append("]}");
        }
        sb.append("],\"tags\":[");
        for (int i = 0; i < tags.size(); i++) {
            if (i > 0) sb.append(',');
            sb.append(json(tags.get(i)));
        }
        return sb.append("]}").toString();
    }

    static String json(String s) {
        StringBuilder sb = new StringBuilder(s.length() + 2).append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '"' || c == '\\') sb.append('\\').append(c);
            else if (c < 0x20 || c == '<' || c == ' ' || c == ' ') sb.append(String.format("\\u%04x", (int) c));
            else sb.append(c);
        }
        return sb.append('"').toString();
    }
}
