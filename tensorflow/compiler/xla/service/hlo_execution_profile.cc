/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/hlo_execution_profile.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "tensorflow/compiler/xla/metric_table_report.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/types.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/lib/strings/stringprintf.h"

namespace xla {

void HloExecutionProfile::AddProfileResult(const HloInstruction* hlo,
                                           uint64 cycles_taken) {
  hlo_to_cycles_taken_[hlo] = cycles_taken;
  profiled_computations_.insert(hlo->parent());
}

uint64 HloExecutionProfile::GetProfileResult(const HloInstruction& hlo) const {
  auto iter = hlo_to_cycles_taken_.find(&hlo);
  if (iter == hlo_to_cycles_taken_.end()) {
    return 0;
  }
  return iter->second;
}

string HloExecutionProfile::ToString(
    const HloComputation& computation,
    const DeviceDescription& device_description,
    const HloCostAnalysis::ShapeSizeFunction& shape_size) const {
  HloCostAnalysis cost_analysis(shape_size);
  tensorflow::Status analysis_status =
      computation.root_instruction()->Accept(&cost_analysis);
  if (!analysis_status.ok()) {
    return "";
  }

  using Item = std::pair<const HloInstruction*, uint64>;
  std::vector<Item> items;
  for (Item item : hlo_to_cycles_taken_) {
    // Only include the HLOs which are part of the desired computation.
    if (item.first->parent() == &computation) {
      items.push_back(item);
    }
  }
  auto custom_less = [](const Item& lhs, const Item& rhs) {
    return lhs.second > rhs.second;
  };
  std::sort(items.begin(), items.end(), custom_less);
  string result;
  const int64 total_cycles = total_cycles_executed(computation);
  double clock_rate_ghz = device_description.clock_rate_ghz();
  CHECK_GE(clock_rate_ghz, 1e-9);

  const auto cycles_to_microseconds = [&](double cycles) {
    return cycles / clock_rate_ghz / 1000.0;
  };

  auto append_item = [&](int64 cycles, int64 flops, int64 bytes_accessed,
                         const string& name) {
    double nsecs = cycles / clock_rate_ghz;
    string bytes_per_sec;
    string bytes_per_cycle;
    if (cycles <= 0 || bytes_accessed < 0) {
      bytes_per_sec = "<unknown>";
      bytes_per_cycle = "<unknown>";
    } else {
      bytes_per_sec = tensorflow::strings::HumanReadableNumBytes(
          bytes_accessed / (nsecs / 1e9));
      bytes_per_cycle =
          tensorflow::strings::HumanReadableNumBytes(bytes_accessed / cycles);
    }

    double cycles_percent = 0;
    if (total_cycles > 0) {
      cycles_percent = cycles / static_cast<double>(total_cycles) * 100;
    }

    tensorflow::strings::StrAppend(
        &result,
        tensorflow::strings::Printf(
            "%15lld cycles (%6.2f%%) :: %12.1f usec @ f_nom :: %18s :: %12s/s "
            ":: "
            "%12s/cycle :: "
            "%s",
            cycles, cycles_percent, cycles_to_microseconds(cycles),
            flops <= 0 ? "<none>" : HumanReadableNumFlops(flops, nsecs).c_str(),
            bytes_per_sec.c_str(), bytes_per_cycle.c_str(), name.c_str()));
  };
  tensorflow::strings::StrAppend(
      &result, tensorflow::strings::Printf(
                   "HLO execution profile for %s: (%s @ f_nom)\n\t",
                   computation.name().c_str(),
                   tensorflow::strings::HumanReadableElapsedTime(
                       total_cycles / clock_rate_ghz / 1e9)
                       .c_str()));

  append_item(total_cycles, -1, -1, "[total]");
  for (const auto& item : items) {
    const HloInstruction* hlo = item.first;
    tensorflow::strings::StrAppend(&result, "\n\t");
    const int64 flops = (hlo == nullptr) ? -1 : cost_analysis.flop_count(*hlo);
    const int64 bytes_accessed =
        (hlo == nullptr) ? -1 : cost_analysis.bytes_accessed(*hlo);
    const string display = (hlo == nullptr) ? "<none>" : hlo->ToString();
    append_item(item.second, flops, bytes_accessed, display);
  }

  if (total_cycles <= 0) {
    result += "****** 0 total cycles ******\n";
  } else {
    MetricTableReport table;
    table.SetMetricName("microseconds");
    table.SetEntryName("ops");
    table.SetShowCategoryTable();
    for (const auto& item : items) {
      MetricTableReport::Entry entry;
      entry.text = item.first->ToString();
      entry.short_text = item.first->ToString(/*compact_operands=*/true);
      entry.category_text = item.first->ToCategory();
      entry.metric = cycles_to_microseconds(item.second);
      table.AddEntry(std::move(entry));
    }
    result += table.MakeReport(cycles_to_microseconds(total_cycles));
  }

  return result;
}

}  // namespace xla
