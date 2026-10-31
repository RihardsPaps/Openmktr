// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#ifndef FRESHNESS_H_
#define FRESHNESS_H_
#include <sys/stat.h>
#include <cstdint>
#include <string>

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
  static FileStamp Read(const char* path);
  static FileStamp Read(const std::string& path);
  bool exists() const { return kind_ == Kind::Present; }
  Kind kind() const { return kind_; }
  FileStamp LowResolution() const { return FileStamp(kind_, seconds_, 0); }
  friend bool operator==(FileStamp a, FileStamp b) {
    return a.kind_ == b.kind_ && a.seconds_ == b.seconds_ &&
           a.nanoseconds_ == b.nanoseconds_;
  }
  friend bool operator!=(FileStamp a, FileStamp b) { return !(a == b); }
  friend bool operator<(FileStamp a, FileStamp b) {
    if (a.kind_ != b.kind_)
      return a.kind_ < b.kind_;
    if (a.seconds_ != b.seconds_)
      return a.seconds_ < b.seconds_;
    return a.nanoseconds_ < b.nanoseconds_;
  }
  friend bool operator>(FileStamp a, FileStamp b) { return b < a; }
  friend bool operator>=(FileStamp a, FileStamp b) { return !(a < b); }

 private:
  constexpr FileStamp(Kind kind, int64_t seconds, int32_t nanoseconds)
      : seconds_(seconds), nanoseconds_(nanoseconds), kind_(kind) {}
  // Keep this value within two machine words. Apart from graph storage, this
  // lets common ABIs pass freshness observations in registers.
  int64_t seconds_ = 0;
  int32_t nanoseconds_ = 0;
  Kind kind_ = Kind::Missing;
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
