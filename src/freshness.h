// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#ifndef FRESHNESS_H_
#define FRESHNESS_H_
#include <sys/stat.h>
#include <cstdint>
#include <string>
#include <tuple>

// A graph observation is distinct from a logical update and an ignored edge.
// Never encode those states as special wall-clock times: pre-epoch files and
// sub-microsecond changes are valid inputs.
class FileStamp {
 public:
  enum class Kind { Missing, Ignored, Present, Updated };
  constexpr FileStamp() = default;
  static constexpr FileStamp Missing() { return {}; }
  static constexpr FileStamp Ignored() {
    return FileStamp(Kind::Ignored, 0, 0);
  }
  static constexpr FileStamp Updated() {
    return FileStamp(Kind::Updated, 0, 0);
  }
  static constexpr FileStamp At(int64_t seconds, int32_t nanoseconds) {
    return FileStamp(Kind::Present, seconds, nanoseconds);
  }
  static FileStamp FromStat(const struct stat& metadata);
  static FileStamp Read(const std::string& path);
  bool exists() const { return kind_ == Kind::Present; }
  Kind kind() const { return kind_; }
  FileStamp LowResolution() const { return FileStamp(kind_, seconds_, 0); }
  friend bool operator==(FileStamp a, FileStamp b) {
    return a.Key() == b.Key();
  }
  friend bool operator!=(FileStamp a, FileStamp b) { return !(a == b); }
  friend bool operator<(FileStamp a, FileStamp b) { return a.Key() < b.Key(); }
  friend bool operator>(FileStamp a, FileStamp b) { return b < a; }
  friend bool operator>=(FileStamp a, FileStamp b) { return !(a < b); }

 private:
  constexpr FileStamp(Kind kind, int64_t seconds, int32_t nanoseconds)
      : kind_(kind), seconds_(seconds), nanoseconds_(nanoseconds) {}
  std::tuple<Kind, int64_t, int32_t> Key() const {
    return std::make_tuple(kind_, seconds_, nanoseconds_);
  }
  Kind kind_ = Kind::Missing;
  int64_t seconds_ = 0;
  int32_t nanoseconds_ = 0;
};

enum class EdgeKind { Normal, OrderOnly, Validation };
enum class FreshnessReason {
  Current,
  MissingOutput,
  Phony,
  NewerInput,
  Unconditional
};
struct FreshnessDecision {
  FreshnessReason reason;
  bool dirty() const { return reason != FreshnessReason::Current; }
  const char* explanation() const;
};
FreshnessDecision DecideFreshness(FileStamp output,
                                  FileStamp newest_input,
                                  bool phony = false,
                                  bool unconditional = false);
bool InputIsNewer(FileStamp input,
                  FileStamp output,
                  EdgeKind kind = EdgeKind::Normal,
                  bool low_resolution = false);
#endif
