/*
 * Copyright The async-profiler authors
 * SPDX-License-Identifier: Apache-2.0
 */

package one.convert;

import one.jfr.event.Event;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The spans of each thread, to tag a sample with the path of spans enclosing it, outermost first, ' > ' separated.
 * Spans are half-open, [start, end). Spans on one thread are expected to nest (a span that starts inside another ends
 * inside it), as spans recorded around nested regions of code do; a span is the parent of a later one only if it
 * contains it entirely.
 */
final class Spans {
    private final Map<Integer, ThreadSpans> byThread = new HashMap<>();
    private final Map<Integer, List<long[]>> pending = new HashMap<>();
    private final List<String> tags = new ArrayList<>();
    private final Map<String, Integer> tagIds = new HashMap<>();

    void add(int tid, long startNanos, long endNanos, String tag) {
        Integer id = tagIds.computeIfAbsent(tag, t -> {
            tags.add(t);
            return tags.size() - 1;
        });
        pending.computeIfAbsent(tid, k -> new ArrayList<>()).add(new long[]{startNanos, endNanos, id});
    }

    void build() {
        pending.forEach((tid, spans) -> byThread.put(tid, new ThreadSpans(spans, tags)));
        pending.clear();
    }

    /** The path of the spans on thread `tid` enclosing `timeNanos`, or null if there are none */
    String pathAt(int tid, long timeNanos) {
        ThreadSpans s = byThread.get(tid);
        return s == null ? null : s.pathAt(timeNanos);
    }

    private static final class ThreadSpans {
        final long[] starts;
        final long[] ends;
        final int[] tags;
        final int[] parents;  // index of the innermost span enclosing each span, or -1
        final String[] paths;  // built on first use
        final List<String> tagNames;

        ThreadSpans(List<long[]> spans, List<String> tagNames) {
            // by start, the longer (outer) span first when two start together
            spans.sort((a, b) -> a[0] != b[0] ? Long.compare(a[0], b[0]) : Long.compare(b[1], a[1]));
            int n = spans.size();
            starts = new long[n];
            ends = new long[n];
            tags = new int[n];
            parents = new int[n];
            paths = new String[n];
            this.tagNames = tagNames;
            int[] open = new int[n];
            int depth = 0;
            for (int i = 0; i < n; i++) {
                long[] s = spans.get(i);
                starts[i] = s[0];
                ends[i] = s[1];
                tags[i] = (int) s[2];
                // close the spans that end before this one starts (half-open: one ending where this starts is closed)
                // or that do not contain it (overlapping without nesting)
                while (depth > 0 && (ends[open[depth - 1]] <= s[0] || ends[open[depth - 1]] < s[1])) depth--;
                parents[i] = depth > 0 ? open[depth - 1] : -1;
                open[depth++] = i;
            }
        }

        String pathAt(long t) {
            // The last span starting at or before t encloses it, or one of its ancestors does: a span that starts
            // earlier and contains t also contains that span's start, so (spans nesting) it encloses that span
            int i = Arrays.binarySearch(starts, t);
            if (i < 0) i = -i - 2;
            else while (i + 1 < starts.length && starts[i + 1] == t) i++;
            while (i >= 0 && ends[i] <= t) i = parents[i];
            return i < 0 ? null : path(i);
        }

        private String path(int i) {
            String p = paths[i];
            if (p == null) {
                String tag = tagNames.get(tags[i]);
                p = paths[i] = parents[i] < 0 ? tag : path(parents[i]) + " > " + tag;
            }
            return p;
        }
    }

    /** An event with the path of the spans enclosing it, grouped by path as well as by its own grouping */
    static final class TaggedEvent extends Event {
        final Event event;
        final String tag;

        TaggedEvent(Event event, String tag) {
            super(event.time, event.tid, event.stackTraceId);
            this.event = event;
            this.tag = tag;
        }

        @Override
        public int hashCode() {
            return event.hashCode() * 31 + tag.hashCode();
        }

        @Override
        public boolean sameGroup(Event o) {
            if (o instanceof TaggedEvent) {
                TaggedEvent t = (TaggedEvent) o;
                return tag.equals(t.tag) && event.sameGroup(t.event);
            }
            return false;
        }

        @Override
        public long classId() {
            return event.classId();
        }

        @Override
        public long samples() {
            return event.samples();
        }

        @Override
        public long value() {
            return event.value();
        }
    }
}
