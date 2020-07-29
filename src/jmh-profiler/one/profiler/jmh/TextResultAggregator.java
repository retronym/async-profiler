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

import java.util.Collection;
import org.openjdk.jmh.results.*;

/** An aggregator for @{@link one.profiler.jmh.TextResult} that concatenates */
public class TextResultAggregator implements Aggregator<TextResult> {
  private String label;

  TextResultAggregator(String label) {
    this.label = label;
  }

  @Override
  public TextResult aggregate(Collection<TextResult> results) {
    StringBuilder output = new StringBuilder();
    for (TextResult r : results) {
      output.append(r.getOutput());
    }
    return new TextResult(output.toString(), label);
  }
}
