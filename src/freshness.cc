// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#include "freshness.h"
#include "fileutil.h"

FileStamp FileStamp::FromStat(const struct stat& metadata) {
#if defined(__APPLE__)
  const auto time = metadata.st_mtimespec;
#else
  const auto time = metadata.st_mtim;
#endif
  return At(time.tv_sec, time.tv_nsec);
}
FileStamp FileStamp::Read(const std::string& path) {
  struct stat metadata;
  return ObserveFile(path, &metadata) ? FromStat(metadata) : Missing();
}
bool InputIsNewer(FileStamp input,
                  FileStamp output,
                  EdgeKind kind,
                  bool low_resolution) {
  if (kind != EdgeKind::Normal || input.kind() == FileStamp::Kind::Ignored)
    return false;
  return (low_resolution ? input.LowResolution() : input) > output;
}
FreshnessDecision DecideFreshness(FileStamp output,
                                  FileStamp newest_input,
                                  bool phony,
                                  bool unconditional) {
  if (phony)
    return {FreshnessReason::Phony};
  if (!output.exists())
    return {FreshnessReason::MissingOutput};
  if (unconditional)
    return {FreshnessReason::Unconditional};
  if (InputIsNewer(newest_input, output))
    return {FreshnessReason::NewerInput};
  return {FreshnessReason::Current};
}
const char* FreshnessDecision::explanation() const {
  switch (reason) {
    case FreshnessReason::Current:
      return "up to date";
    case FreshnessReason::MissingOutput:
      return "outdated: output is missing";
    case FreshnessReason::Phony:
      return "outdated: phony target";
    case FreshnessReason::NewerInput:
      return "outdated: prerequisite is newer or would be rebuilt";
    case FreshnessReason::Unconditional:
      return "outdated: unconditional recipe";
  }
  return "unknown freshness decision";
}
