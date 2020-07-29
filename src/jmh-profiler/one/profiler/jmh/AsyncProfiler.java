/*
 * Copyright 2020 Andrei Pangin
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package one.profiler.jmh;

import joptsimple.OptionParser;
import joptsimple.OptionSet;
import joptsimple.OptionSpec;
import joptsimple.HelpFormatter;
import org.openjdk.jmh.infra.BenchmarkParams;
import org.openjdk.jmh.infra.IterationParams;
import org.openjdk.jmh.results.BenchmarkResult;
import org.openjdk.jmh.results.IterationResult;
import org.openjdk.jmh.results.Result;
import org.openjdk.jmh.runner.IterationType;
import org.openjdk.jmh.profile.*;

import java.io.*;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.*;


/**
 * A profiler based on <a href="https://github.com/jvm-profiling-tools/async-profiler/commits/master">async-profiler</a>.
 *
 * @author Jason Zaugg
 */
public class AsyncProfiler implements ExternalProfiler, InternalProfiler {
  private static final String DEFAULT_EVENT = "cpu";

  private final String profilerConfig;
  private final one.profiler.AsyncProfiler instance;
  private final Direction direction;
  private final List<OutputType> output;
  private boolean started = false;
  private int measurementIterationCount = 0;
  private final String event;
  private final long interval;
  private final File outDir;
  private final int traces;
  private final int flat;
  private final List<File> generated = new ArrayList<>();

  public AsyncProfiler(String initLine) {
    OptionParser parser = new OptionParser();

    HelpFormatter formatter = JmhInternals.newProfilerOptionFormatter("async");

    parser.formatHelpWith(formatter);

    OptionSpec<OutputType> output = parser.accepts("output", "Output format(s)")
        .withRequiredArg().ofType(OutputType.class).withValuesSeparatedBy(",").describedAs("format+").defaultsTo(OutputType.text);
    OptionSpec<Direction> direction = parser.accepts("direction", "Direction(s) of flame graph")
        .withRequiredArg().ofType(Direction.class).describedAs("direction").defaultsTo(Direction.both);

    OptionSpec<String> libPath = parser.accepts("libPath", "Location of libasyncProfiler.so. " +
        "If not specified, System.loadLibrary will be used and the library must be made available to the forked JVM " +
        "in an entry of -Djava.library.path or LD_LIBRARY_PATH.")
        .withRequiredArg().ofType(String.class).describedAs("path");

    OptionSpec<String> event = parser.accepts("event", "Event to sample: cpu, alloc, wall, lock, cache-misses etc.")
        .withRequiredArg().ofType(String.class).defaultsTo(DEFAULT_EVENT);
    OptionSpec<String> dir = parser.accepts("dir", "Output directory.")
        .withRequiredArg().ofType(String.class).describedAs("dir");
    OptionSpec<Long> interval = parser.accepts("interval", "Profiling interval")
        .withRequiredArg().ofType(Long.class).describedAs("ns").defaultsTo(one.profiler.AsyncProfiler.DEFAULT_INTERVAL);
    OptionSpec<Integer> jstackdepth = parser.accepts("jstackdepth", "Maximum Java stack depth")
        .withRequiredArg().ofType(Integer.class).describedAs("frames");
    OptionSpec<Long> framebuf = parser.accepts("framebuf", "Size of profiler framebuffer")
        .withRequiredArg().ofType(Long.class).describedAs("bytes");
    OptionSpec<Boolean> threads = parser.accepts("threads", "Profile threads separately")
        .withRequiredArg().ofType(Boolean.class).describedAs("int");
    OptionSpec<Boolean> simple = parser.accepts("simple", "Simple class names instead of FQN")
        .withRequiredArg().ofType(Boolean.class).describedAs("bool");
    OptionSpec<Boolean> sig = parser.accepts("sig", "Print method signatures")
        .withRequiredArg().ofType(Boolean.class).describedAs("bool");
    OptionSpec<Boolean> ann = parser.accepts("ann", "Annotate Java method names")
        .withRequiredArg().ofType(Boolean.class).describedAs("bool");
    OptionSpec<String> include = parser.accepts("include", "output only stack traces containing the specified pattern")
        .withRequiredArg()
        .withValuesSeparatedBy(",").ofType(String.class).describedAs("regexp+");
    OptionSpec<String> exclude = parser.accepts("exclude", "exclude stack traces with the specified pattern")
        .withRequiredArg()
        .withValuesSeparatedBy(",").ofType(String.class).describedAs("regexp+");

    // Doesn't make sense to expose here as the the user won't know the thread ID in advance.
    // It could be useful to to use the Java API of async-profiler to pass in the Thread that is executing the benchmark.
    // I think this filtering is only useful for wall-clock profiling.
    /* OptionSpec<Integer> filter = parser.accepts("filter", "Filter thread ID").withRequiredArg().withValuesSeparatedBy(",").ofType(Integer.class);*/

    OptionSpec<String> title = parser.accepts("title", "SVG title")
        .withRequiredArg().ofType(String.class).describedAs("string");
    OptionSpec<Long> width = parser.accepts("width", "SVG width")
        .withRequiredArg().ofType(Long.class).describedAs("pixels");
    OptionSpec<Long> minWidth = parser.accepts("minwidth", "skip frames smaller than px")
        .withRequiredArg().ofType(Long.class).describedAs("pixels");

    OptionSpec<Boolean> allKernel = parser.accepts("allkernel", "only include kernel-mode events")
        .withRequiredArg().ofType(Boolean.class).describedAs("bool");
    OptionSpec<Boolean> allUser = parser.accepts("alluser", "only include user-mode events")
        .withRequiredArg().ofType(Boolean.class).describedAs("bool");
    OptionSpec<CStackMode> cstack = parser.accepts("cstack", "how to traverse C stack")
        .withRequiredArg().ofType(CStackMode.class).describedAs("bool");

    OptionSpec<Boolean> verbose = parser.accepts("verbose", "Output the sequence of commands")
        .withRequiredArg().ofType(Boolean.class).defaultsTo(false).describedAs("bool");

    OptionSpec<Integer> traces = parser.accepts("traces", "Number of top traces to include in the default output")
        .withRequiredArg().ofType(Integer.class).defaultsTo(200).describedAs("int");
    OptionSpec<Integer> flat = parser.accepts("flat", "Number of top flat profiles to include in the default output")
        .withRequiredArg().ofType(Integer.class).defaultsTo(200).describedAs("int");

    OptionSet options = JmhInternals.ProfilerUtils_parseInitLine(initLine, parser);

    StringBuilder profilerOptions = new StringBuilder();

    ProfilerOptionsBuilder builder = new ProfilerOptionsBuilder(options, profilerOptions);
    this.event = event.value(options);
    this.interval = interval.value(options);
    if (!options.has(dir)) {
      String prefix = "jmh-async-profiler-";
      outDir = createTempDir(prefix, null);
    } else {
      outDir = new File(options.valueOf(dir));
    }

    builder.appendIfExists(jstackdepth);
    builder.appendIfTrue(threads);
    builder.appendIfTrue(simple);
    builder.appendIfTrue(sig);
    builder.appendIfTrue(ann);
    builder.appendIfExists(framebuf);
    builder.appendMulti(include);
    builder.appendMulti(exclude);

    builder.appendIfExists(title);
    builder.appendIfExists(width);
    builder.appendIfExists(minWidth);

    builder.appendIfTrue(allKernel);
    builder.appendIfTrue(allUser);
    builder.appendIfExists(cstack);
    this.traces = traces.value(options);
    this.flat = flat.value(options);

    this.profilerConfig = profilerOptions.toString();

    if (options.has(verbose)) {
      System.out.println(profilerConfig);
    }
    if (options.has(libPath)) {
      instance = one.profiler.AsyncProfiler.getInstance(libPath.value(options));
    } else {
      instance = one.profiler.AsyncProfiler.getInstance();
    }
    this.direction = direction.value(options);
    this.output = output.values(options);

  }

  @Override
  public void beforeIteration(BenchmarkParams benchmarkParams, IterationParams iterationParams) {
    if (!started) {
      if (iterationParams.getType() == IterationType.MEASUREMENT) {
        try {
          instance.execute(profilerConfig);
        } catch (IOException e) {
          throw new RuntimeException(e);
        }
        instance.start(event, interval);
        started = true;
      }
    }
  }

  @Override
  public Collection<? extends Result> afterIteration(BenchmarkParams benchmarkParams, IterationParams iterationParams,
                                                     IterationResult iterationResult) {
    if (iterationParams.getType() == IterationType.MEASUREMENT) {
      measurementIterationCount += 1;
      if (measurementIterationCount == iterationParams.getCount()) {
        StringBuilder dirName = new StringBuilder();
        dirName.append(sanitizeFileName(benchmarkParams.getBenchmark()));
        dirName.append("?");
        for (String key : benchmarkParams.getParamsKeys()) {
          dirName.append(sanitizeFileName(key)).append("=").append(sanitizeFileName(benchmarkParams.getParam(key))).append("&");
        }
        if (benchmarkParams.getParamsKeys().size() > 0) {
          dirName.deleteCharAt(dirName.length() - 1);
        }
        File specificOutDir = new File(this.outDir, dirName.toString());
        specificOutDir.mkdirs();
        return Collections.singletonList(stopAndDump(specificOutDir));
      }
    }

    return Collections.emptyList();
  }

  private static String sanitizeFileName(String s) {
    try {
      return URLEncoder.encode(s, StandardCharsets.UTF_8.name());
    } catch (UnsupportedEncodingException e) {
      throw new RuntimeException(e);
    }
  }

  private TextResult stopAndDump(File specificOutDir) {
    instance.stop();

    StringWriter output = new StringWriter();
    PrintWriter pw = new PrintWriter(output);
    for (OutputType outputType : this.output) {
      switch (outputType) {
        case text:
          String textOutput = dump(specificOutDir, "summary-%s.txt", "summary,flat=" + flat + ",traces=" + traces);
          pw.println(textOutput);
          break;
        case collapsed:
          dump(specificOutDir, "collapsed-%s.csv", "collapsed");
          break;
        case flamegraph:
          if (direction == Direction.both || direction == Direction.forward) {
            dump(specificOutDir, "flame-%s-forward.svg", "svg");
          }
          if (direction == Direction.both || direction == Direction.reverse) {
            dump(specificOutDir, "flame-%s-reverse.svg", "svg,reverse");
          }
          break;
        case tree:
          dump(specificOutDir, "tree-%s.html", "tree");
          break;
        case jfr:
          dump(specificOutDir, "%s.jfr", "jfr");
          break;
      }
    }
    for (File file : generated) {
      pw.println(file.getAbsolutePath());
    }
    pw.flush();
    pw.close();

    return new TextResult(output.toString(), "async");
  }

  private String dump(File specificOutDir, String fileNameFormatString, String content) {
    File output = new File(specificOutDir, String.format(fileNameFormatString, event));
    generated.add(output);
    try {
      String result = instance.execute(content + "," + profilerConfig);
      write(output, result);
      return result;
    } catch (IOException e) {
      throw new RuntimeException(e);
    }
  }

  private void write(File output, String s) {
    try {
      BufferedWriter writer = new BufferedWriter(new OutputStreamWriter(new FileOutputStream(output)));
      try {
        writer.write(s);
      } finally {
        writer.close();
      }
    } catch (IOException e) {
      throw new RuntimeException(e);
    }
  }

  private static File createTempDir(String prefix, File dir) {
    try {
      File tempFile = File.createTempFile(prefix, "", dir);
      tempFile.delete();
      tempFile.mkdir();
      return tempFile;
    } catch (IOException e) {
      throw new RuntimeException(e);
    }
  }


  public enum CStackMode {
    fp,
    lbr,
    no
  }

  public enum OutputType {
    //NONE,
    text,
    collapsed,
    flamegraph,
    tree,
    jfr
  }

  public enum Direction {
    forward,
    reverse,
    both,
  }

  private static class ProfilerOptionsBuilder {
    private final OptionSet optionSet;
    private final StringBuilder profilerOptions;

    ProfilerOptionsBuilder(OptionSet optionSet, StringBuilder profilerOptions) {
      this.optionSet = optionSet;
      this.profilerOptions = profilerOptions;
    }

    <T> void appendIfExists(OptionSpec<T> option) {
      if (optionSet.has(option)) {
        append(option);
      }
    }

    <T> void append(OptionSpec<T> option) {
      assert (option.options().size() == 1);
      String optionName = option.options().iterator().next();
      if (profilerOptions.length() > 0) {
        profilerOptions.append(',');
      }
      profilerOptions.append(optionName).append('=').append(optionSet.valueOf(option).toString());
    }

    void appendIfTrue(OptionSpec<Boolean> option) {
      if (optionSet.has(option) && optionSet.valueOf(option)) {
        append(option);
      }
    }

    private <T> void appendMulti(OptionSpec<T> option) {
      if (optionSet.has(option)) {
        assert (option.options().size() == 1);
        String optionName = option.options().iterator().next();
        for (T value : optionSet.valuesOf(option)) {
          profilerOptions.append(',').append(optionName).append('=').append(value.toString());
        }
      }
    }
  }

  @Override
  public Collection<String> addJVMInvokeOptions(BenchmarkParams params) {
    return Collections.emptyList();
  }

  @Override
  public Collection<String> addJVMOptions(BenchmarkParams params) {
    List<String> args = new ArrayList<>();
    args.add("-XX:+UnlockDiagnosticVMOptions");
    args.add("-XX:+DebugNonSafepoints");
    return args;
  }

  @Override
  public void beforeTrial(BenchmarkParams benchmarkParams) {
  }

  @Override
  public Collection<? extends Result> afterTrial(BenchmarkResult br, long pid, File stdOut, File stdErr) {
    return Collections.emptyList();
  }

  @Override
  public boolean allowPrintOut() {
    return true;
  }

  @Override
  public boolean allowPrintErr() {
    return true;
  }

  @Override
  public String getDescription() {
    return "async-profiler profiler provider.";
  }
}
