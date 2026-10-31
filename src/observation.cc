// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#include "observation.h"
#include <cerrno>
#include "fileutil.h"
#include "find.h"
#include "flags.h"
#include "graph_optimization.h"
#include "native_observation.h"

namespace {
thread_local ObservationLog* active_log = nullptr;
const char* Name(ObservationKind kind) {
  switch (kind) {
    case ObservationKind::File:
      return "file";
    case ObservationKind::Glob:
      return "glob";
    case ObservationKind::NativeFind:
      return "find";
    case ObservationKind::Metadata:
      return "metadata";
    case ObservationKind::Intern:
      return "intern";
  }
  return "invalid";
}
}  // namespace
std::string ObservationRecord::Encode() const {
  std::string bytes;
  AppendGraphString(Name(kind), &bytes);
  for (const auto& field : fields)
    AppendGraphString(field, &bytes);
  return bytes;
}
bool ObservationRecord::Decode(std::string_view bytes,
                               ObservationRecord* record) {
  if (bytes.size() > 1024 * 1024)
    return false;
  std::string name;
  if (!ConsumeGraphString(&bytes, &name))
    return false;
  bool known = false;
  for (const auto kind : {ObservationKind::File, ObservationKind::Glob,
                          ObservationKind::NativeFind,
                          ObservationKind::Metadata, ObservationKind::Intern}) {
    if (name == Name(kind)) {
      record->kind = kind;
      known = true;
      break;
    }
  }
  if (!known)
    return false;
  record->fields.clear();
  while (!bytes.empty()) {
    std::string field;
    if (record->fields.size() >= 100000 || !ConsumeGraphString(&bytes, &field))
      return false;
    record->fields.push_back(std::move(field));
  }
  return true;
}
bool ObservationRecord::Validate(std::string* accepted) const {
  bool valid = false;
  switch (kind) {
    case ObservationKind::File:
    case ObservationKind::Metadata:
      valid = fields.size() == 2 && fields[1] != "unreadable" &&
              GetFileIdentity(fields[0]) == fields[1];
      break;
    case ObservationKind::Glob:
      if (!fields.empty()) {
        const auto& matches = Glob(fields[0]);
        valid = fields.size() == matches.size() + 1 &&
                std::equal(matches.begin(), matches.end(), fields.begin() + 1);
      }
      break;
    case ObservationKind::Intern:
      valid = fields.size() == 1;
      break;
    case ObservationKind::NativeFind: {
      std::string expected, current, packet;
      std::unique_ptr<FindCommand> descriptor;
      if (fields.size() != 4 || !g_flags.use_find_emulator ||
          !DecodeNativeFind(fields[2], fields[3], &expected, &descriptor) ||
          !QueryNativeFind(fields[2], &packet) ||
          !DecodeNativeFind(fields[2], packet, &current, &descriptor))
        return false;
      valid = expected == current;
      if (valid && accepted) {
        auto updated = *this;
        updated.fields[3] = std::move(packet);
        *accepted = updated.Encode();
        return true;
      }
      break;
    }
  }
  if (valid && accepted)
    *accepted = Encode();
  return valid;
}
ObservationLog::ObservationLog()
    : previous_(active_log),
      epoch_(FilesystemObservationEpoch()),
      valid_(!FilesystemEffectsActive()) {
  active_log = this;
}
ObservationLog::~ObservationLog() {
  active_log = previous_;
}
bool ObservationLog::Active() {
  return active_log && active_log->valid_;
}
bool ObservationLog::valid() const {
  return valid_ && epoch_ == FilesystemObservationEpoch() &&
         !FilesystemEffectsActive();
}
void ObservationLog::Record(ObservationRecord record) {
  if (!active_log || !active_log->valid_)
    return;
  size_t bytes = 32;
  for (const auto& field : record.fields)
    bytes += field.size() + 32;
  if (active_log->bytes_ + bytes > 1024 * 1024 ||
      active_log->records_.size() >= 8192) {
    active_log->valid_ = false;
    active_log->records_.clear();
    return;
  }
  active_log->bytes_ += bytes;
  active_log->records_.push_back(std::move(record));
}
void CaptureFileObservation(const std::string& path,
                            bool exists,
                            const struct stat& metadata,
                            int error) {
  if (!ObservationLog::Active())
    return;
  std::string identity = exists ? GetFileIdentityFromStat(metadata)
                         : error == ENOENT || error == ENOTDIR ? "missing"
                                                               : "unreadable";
  ObservationLog::Record(
      {ObservationKind::Metadata, {path, std::move(identity)}});
}
