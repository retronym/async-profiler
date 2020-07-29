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

import joptsimple.HelpFormatter;
import joptsimple.OptionParser;
import joptsimple.OptionSet;

import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

class JmhInternals {
  static final Method ProfilerUtils_parseInitLine;
  static final Constructor<?> profilerOptionFormatter_init;

  static {
    // TODO Submit a patch to JMH to publicise ProfilerUtils.parseInitLine and ProfilerOptionFormatter
    ProfilerUtils_parseInitLine = lookupProfilerUtils_ParseInitLine();
    ProfilerUtils_parseInitLine.setAccessible(true);
    profilerOptionFormatter_init = lookupProfilerOptionFormatter_init();
    profilerOptionFormatter_init.setAccessible(true);
  }

  static OptionSet ProfilerUtils_parseInitLine(String initLine, OptionParser parser) {
    try {
      return (OptionSet) ProfilerUtils_parseInitLine.invoke(null, initLine, parser);
    } catch (IllegalAccessException | InvocationTargetException e) {
      throw new RuntimeException(e);
    }
  }
  static HelpFormatter newProfilerOptionFormatter(String name) {
    try {
      return (HelpFormatter) profilerOptionFormatter_init.newInstance(name);
    } catch (InstantiationException | IllegalAccessException | InvocationTargetException e) {
      throw new RuntimeException(e);
    }
  }

  private static Method lookupProfilerUtils_ParseInitLine() {
    try {
      Class<?> profilerUtilsClass = Class.forName("org.openjdk.jmh.profile.ProfilerUtils");
      return profilerUtilsClass.getMethod("parseInitLine", String.class, OptionParser.class);
    } catch (NoSuchMethodException | ClassNotFoundException e) {
      throw new ExceptionInInitializerError(e);
    }
  }

  private static Constructor<?> lookupProfilerOptionFormatter_init() {
    try {
      Class<?> cls = Class.forName("org.openjdk.jmh.profile.ProfilerOptionFormatter");
      return cls.getConstructor(String.class);
    } catch (ClassNotFoundException | NoSuchMethodException e) {
      throw new RuntimeException(e);
    }
  }
}
