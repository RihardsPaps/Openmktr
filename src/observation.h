// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#ifndef OBSERVATION_H_
#define OBSERVATION_H_
#include <sys/stat.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class ObservationKind { File, Glob, NativeFind, Metadata, Intern };
struct ObservationRecord {
  ObservationKind kind;
  std::vector<std::string> fields;
  std::string Encode() const;
  static bool Decode(std::string_view bytes, ObservationRecord* record);
  bool Validate(std::string* accepted = nullptr) const;
};

// Ordered observations of a pure resolution region. Effects invalidate the
// whole region; overflow falls back to ordinary resolution without caching.
class ObservationLog {
 public:
  ObservationLog();
  ~ObservationLog();
  ObservationLog(const ObservationLog&) = delete;
  ObservationLog& operator=(const ObservationLog&) = delete;
  bool valid() const;
  const std::vector<ObservationRecord>& records() const { return records_; }
  static bool Active();
  static void Record(ObservationRecord record);

 private:
  ObservationLog* previous_;
  uint64_t epoch_;
  bool valid_;
  size_t bytes_ = 0;
  std::vector<ObservationRecord> records_;
};
void CaptureFileObservation(const std::string& path,
                            bool exists,
                            const struct stat& metadata,
                            int error);
#endif
