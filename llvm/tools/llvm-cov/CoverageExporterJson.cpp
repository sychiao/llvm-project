//===- CoverageExporterJson.cpp - Code coverage export --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements export of code coverage data to JSON.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
//
// The json code coverage export follows the following format
// Root: dict => Root Element containing metadata
// -- Data: array => Homogeneous array of one or more export objects
//   -- Export: dict => Json representation of one CoverageMapping
//     -- Files: array => List of objects describing coverage for files
//       -- File: dict => Coverage for a single file
//         -- Branches: array => List of Branches in the file
//           -- Branch: dict => Describes a branch of the file with counters
//         -- MCDC Records: array => List of MCDC records in the file
//           -- MCDC Values: array => List of T/F covered condition values
//         -- Segments: array => List of Segments contained in the file
//           -- Segment: dict => Describes a segment of the file with a counter
//         -- Expansions: array => List of expansion records
//           -- Expansion: dict => Object that descibes a single expansion
//             -- CountedRegion: dict => The region to be expanded
//             -- TargetRegions: array => List of Regions in the expansion
//               -- CountedRegion: dict => Single Region in the expansion
//             -- Branches: array => List of Branches in the expansion
//               -- Branch: dict => Describes a branch in expansion and counters
//         -- Summary: dict => Object summarizing the coverage for this file
//           -- LineCoverage: dict => Object summarizing line coverage
//           -- FunctionCoverage: dict => Object summarizing function coverage
//           -- RegionCoverage: dict => Object summarizing region coverage
//           -- BranchCoverage: dict => Object summarizing branch coverage
//           -- MCDCCoverage: dict => Object summarizing MC/DC coverage
//     -- Functions: array => List of objects describing coverage for functions
//       -- Function: dict => Coverage info for a single function
//         -- Filenames: array => List of filenames that the function relates to
//   -- Summary: dict => Object summarizing the coverage for the entire binary
//     -- LineCoverage: dict => Object summarizing line coverage
//     -- FunctionCoverage: dict => Object summarizing function coverage
//     -- InstantiationCoverage: dict => Object summarizing inst. coverage
//     -- RegionCoverage: dict => Object summarizing region coverage
//     -- BranchCoverage: dict => Object summarizing branch coverage
//     -- MCDCCoverage: dict => Object summarizing MC/DC coverage
//
//===----------------------------------------------------------------------===//

#include "CoverageExporterJson.h"
#include "CoverageReport.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include <algorithm>
#include <limits>
#include <numeric>
#include <utility>

/// The semantic version combined as a string.
#define LLVM_COVERAGE_EXPORT_JSON_STR "3.0.1"

/// Unique type identifier for JSON coverage export.
#define LLVM_COVERAGE_EXPORT_JSON_TYPE_STR "llvm.coverage.json.export"

using namespace llvm;

namespace {

// The JSON library accepts int64_t, but profiling counts are stored as
// uint64_t. Therefore we need to explicitly convert from unsigned to signed,
// since a naive cast is implementation-defined behavior when the unsigned value
// cannot be represented as a signed value. We choose to clamp the values to
// preserve the invariant that counts are always >= 0.
int64_t clamp_uint64_to_int64(uint64_t u) {
  return std::min(u,
                  static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
}

json::Array renderSegment(const coverage::CoverageSegment &Segment) {
  return json::Array({Segment.Line, Segment.Col,
                      clamp_uint64_to_int64(Segment.Count), Segment.HasCount,
                      Segment.IsRegionEntry, Segment.IsGapRegion});
}

json::Array renderRegion(const coverage::CountedRegion &Region) {
  return json::Array(
      {Region.LineStart, Region.ColumnStart, Region.LineEnd, Region.ColumnEnd,
       clamp_uint64_to_int64(Region.ExecutionCount), Region.FileID,
       Region.ExpandedFileID, int64_t(Region.Kind)});
}

json::Array renderBranch(const coverage::CountedRegion &Region) {
  return json::Array(
      {Region.LineStart, Region.ColumnStart, Region.LineEnd, Region.ColumnEnd,
       clamp_uint64_to_int64(Region.ExecutionCount),
       clamp_uint64_to_int64(Region.FalseExecutionCount), Region.FileID,
       Region.ExpandedFileID, int64_t(Region.Kind)});
}

json::Object renderSummary(const FileCoverageSummary &Summary) {
  return json::Object(
      {{"lines",
        json::Object({{"count", int64_t(Summary.LineCoverage.getNumLines())},
                      {"covered", int64_t(Summary.LineCoverage.getCovered())},
                      {"percent", Summary.LineCoverage.getPercentCovered()}})},
       {"functions",
        json::Object(
            {{"count", int64_t(Summary.FunctionCoverage.getNumFunctions())},
             {"covered", int64_t(Summary.FunctionCoverage.getExecuted())},
             {"percent", Summary.FunctionCoverage.getPercentCovered()}})},
       {"instantiations",
        json::Object(
            {{"count",
              int64_t(Summary.InstantiationCoverage.getNumFunctions())},
             {"covered", int64_t(Summary.InstantiationCoverage.getExecuted())},
             {"percent", Summary.InstantiationCoverage.getPercentCovered()}})},
       {"regions",
        json::Object(
            {{"count", int64_t(Summary.RegionCoverage.getNumRegions())},
             {"covered", int64_t(Summary.RegionCoverage.getCovered())},
             {"notcovered", int64_t(Summary.RegionCoverage.getNumRegions() -
                                    Summary.RegionCoverage.getCovered())},
             {"percent", Summary.RegionCoverage.getPercentCovered()}})},
       {"branches",
        json::Object(
            {{"count", int64_t(Summary.BranchCoverage.getNumBranches())},
             {"covered", int64_t(Summary.BranchCoverage.getCovered())},
             {"notcovered", int64_t(Summary.BranchCoverage.getNumBranches() -
                                    Summary.BranchCoverage.getCovered())},
             {"percent", Summary.BranchCoverage.getPercentCovered()}})},
       {"mcdc",
        json::Object(
            {{"count", int64_t(Summary.MCDCCoverage.getNumPairs())},
             {"covered", int64_t(Summary.MCDCCoverage.getCoveredPairs())},
             {"notcovered", int64_t(Summary.MCDCCoverage.getNumPairs() -
                                    Summary.MCDCCoverage.getCoveredPairs())},
             {"percent", Summary.MCDCCoverage.getPercentCovered()}})}});
}

// Stream containers in lexical key order to preserve the output ordering of
// json::Object without retaining the complete JSON tree.
void writeRegions(json::OStream &J, ArrayRef<coverage::CountedRegion> Regions) {
  for (const auto &R : Regions)
    J.value(renderRegion(R));
}

void writeBranches(json::OStream &J,
                   ArrayRef<coverage::CountedRegion> Regions) {
  for (const auto &R : Regions)
    if (!R.TrueFolded || !R.FalseFolded)
      J.value(renderBranch(R));
}

void writeMCDCRecord(json::OStream &J, const coverage::MCDCRecord &Record) {
  const auto &R = Record.getDecisionRegion();
  const auto Decisions = Record.getDecisions();
  J.array([&] {
    J.value(R.LineStart);
    J.value(R.ColumnStart);
    J.value(R.LineEnd);
    J.value(R.ColumnEnd);
    J.value(Decisions.first);
    J.value(Decisions.second);
    J.value(R.FileID);
    J.value(R.ExpandedFileID);
    J.value(int64_t(R.Kind));
    J.array([&] {
      for (unsigned C = 0; C < Record.getNumConditions(); ++C)
        J.value(Record.isConditionIndependencePairCovered(C));
    });
  });
}

void writeMCDCRecords(json::OStream &J,
                      ArrayRef<coverage::MCDCRecord> Records) {
  for (const auto &R : Records)
    writeMCDCRecord(J, R);
}

void writeFilenames(json::OStream &J, ArrayRef<std::string> Filenames) {
  for (const auto &Filename : Filenames)
    J.value(Filename);
}

// Preserve collectNestedBranches' child-before-parent order without collecting
// the entire nested branch list into an additional vector.
void writeNestedBranches(json::OStream &J,
                         const coverage::CoverageMapping &Coverage,
                         ArrayRef<coverage::ExpansionRecord> Expansions) {
  for (const auto &E : Expansions) {
    auto Data = Coverage.getCoverageForExpansion(E);
    writeNestedBranches(J, Coverage, Data.getExpansions());
    for (const auto &R : Data.getBranches())
      if (R.FileID == E.FileID && (!R.TrueFolded || !R.FalseFolded))
        J.value(renderBranch(R));
  }
}

void writeExpansion(json::OStream &J, const coverage::CoverageMapping &Coverage,
                    const coverage::ExpansionRecord &E) {
  J.object([&] {
    J.attributeArray("branches", [&] {
      writeNestedBranches(J, Coverage, ArrayRef<coverage::ExpansionRecord>(E));
    });
    J.attributeArray("filenames",
                     [&] { writeFilenames(J, E.Function.Filenames); });
    J.attribute("source_region", renderRegion(E.Region));
    J.attributeArray("target_regions",
                     [&] { writeRegions(J, E.Function.CountedRegions); });
  });
}

void writeFile(json::OStream &J, const coverage::CoverageMapping &Coverage,
               const std::string &Filename, const FileCoverageSummary &Summary,
               const CoverageViewOptions &Options) {
  J.object([&] {
    if (!Options.ExportSummaryOnly) {
      auto Data = Coverage.getCoverageForFile(Filename);
      J.attributeArray("branches", [&] {
        // Unlike function/expansion branches, file branches are not filtered.
        for (const auto &R : Data.getBranches())
          J.value(renderBranch(R));
      });
      if (!Options.SkipExpansions)
        J.attributeArray("expansions", [&] {
          for (const auto &E : Data.getExpansions())
            writeExpansion(J, Coverage, E);
        });
      J.attribute("filename", Filename);
      J.attributeArray("mcdc_records", [&] {
        for (const auto &R : Data.getMCDCRecords())
          writeMCDCRecord(J, R);
      });
      J.attributeArray("segments", [&] {
        for (const auto &S : Data)
          J.value(renderSegment(S));
      });
    } else {
      J.attribute("filename", Filename);
    }
    J.attribute("summary", renderSummary(Summary));
  });
}

void writeFunction(json::OStream &J, const coverage::FunctionRecord &F) {
  J.object([&] {
    J.attributeArray("branches",
                     [&] { writeBranches(J, F.CountedBranchRegions); });
    J.attribute("count", clamp_uint64_to_int64(F.ExecutionCount));
    J.attributeArray("filenames", [&] { writeFilenames(J, F.Filenames); });
    J.attributeArray("mcdc_records",
                     [&] { writeMCDCRecords(J, F.MCDCRecords); });
    J.attribute("name", F.Name);
    J.attributeArray("regions", [&] { writeRegions(J, F.CountedRegions); });
  });
}

} // end anonymous namespace

void CoverageExporterJson::renderRoot(const CoverageFilters &IgnoreFilters) {
  std::vector<std::string> SourceFiles;
  for (StringRef SF : Coverage.getUniqueSourceFiles())
    if (!IgnoreFilters.matchesFilename(SF))
      SourceFiles.emplace_back(SF);
  renderRoot(SourceFiles);
}

void CoverageExporterJson::renderRoot(ArrayRef<std::string> SourceFiles) {
  FileCoverageSummary Totals("Totals");
  // Compute summaries before emitting files, preserving their thread settings.
  auto FileReports = CoverageReport::prepareFileReports(Coverage, Totals,
                                                        SourceFiles, Options);
  std::vector<unsigned> Order(SourceFiles.size());
  std::iota(Order.begin(), Order.end(), 0);
  llvm::sort(Order, [&](unsigned A, unsigned B) {
    return SourceFiles[A].compare(SourceFiles[B]) < 0;
  });

  json::OStream J(OS);
  J.object([&] {
    J.attributeArray("data", [&] {
      J.object([&] {
        J.attributeArray("files", [&] {
          // Serial file preparation bounds live data to the current file.
          // --num-threads continues to control prepareFileReports above.
          for (unsigned I : Order) {
            writeFile(J, Coverage, SourceFiles[I], FileReports[I], Options);
            J.flush();
          }
        });
        if (!Options.ExportSummaryOnly && !Options.SkipFunctions)
          J.attributeArray("functions", [&] {
            for (const auto &F : Coverage.getCoveredFunctions())
              writeFunction(J, F);
          });
        J.attribute("totals", renderSummary(Totals));
      });
    });
    J.attribute("type", LLVM_COVERAGE_EXPORT_JSON_TYPE_STR);
    J.attribute("version", LLVM_COVERAGE_EXPORT_JSON_STR);
  });
  J.flush();
}
