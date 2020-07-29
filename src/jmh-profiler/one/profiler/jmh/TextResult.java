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

import org.openjdk.jmh.results.*;

/**
 * A textual result
 */
class TextResult extends Result<TextResult> {
  private static final long serialVersionUID = 6871141606856800453L;

  private final String output;

  public TextResult(String output, String label) {
    super(ResultRole.SECONDARY, Defaults.PREFIX + label, of(Double.NaN), "---", AggregationPolicy.AVG);
    this.output = output;
  }

  public String getOutput() {
    return output;
  }

  @Override
  protected Aggregator<TextResult> getThreadAggregator() {
    return new TextResultAggregator(label);
  }

  @Override
  protected Aggregator<TextResult> getIterationAggregator() {
    return new TextResultAggregator(label);
  }

  @Override
  public String toString() {
    return "(text only)";
  }

  @Override
  public String extendedInfo() {
    return output;
  }
}
