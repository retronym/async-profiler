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
 * The spans of each thread, to find the innermost span enclosing a sample: the one with the latest start
 * among those that contain the sample's time.
 */
final class Spans {
    // How far back to look for an enclosing span, past spans that ended before the sample
    private static final int MAX_LOOKBACK = 64;

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
        pending.forEach((tid, spans) -> byThread.put(tid, new ThreadSpans(spans)));
        pending.clear();
    }

    /** The tag of the innermost span on thread `tid` enclosing `timeNanos`, or null */
    String tagAt(int tid, long timeNanos) {
        ThreadSpans s = byThread.get(tid);
        if (s == null) return null;
        int id = s.innermost(timeNanos);
        return id < 0 ? null : tags.get(id);
    }

    private static final class ThreadSpans {
        final long[] starts;
        final long[] ends;
        final int[] tags;

        ThreadSpans(List<long[]> spans) {
            spans.sort((a, b) -> Long.compare(a[0], b[0]));
            int n = spans.size();
            starts = new long[n];
            ends = new long[n];
            tags = new int[n];
            for (int i = 0; i < n; i++) {
                long[] s = spans.get(i);
                starts[i] = s[0];
                ends[i] = s[1];
                tags[i] = (int) s[2];
            }
        }

        int innermost(long t) {
            int i = Arrays.binarySearch(starts, t);
            if (i < 0) i = -i - 2;  // the last span starting at or before t
            else while (i + 1 < starts.length && starts[i + 1] == t) i++;
            for (int k = 0; i >= 0 && k < MAX_LOOKBACK; i--, k++) {
                if (ends[i] >= t) return tags[i];
            }
            return -1;
        }
    }

    /** An event with the tag of its span, grouped by tag as well as by its own grouping */
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
