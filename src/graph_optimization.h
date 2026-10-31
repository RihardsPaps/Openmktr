// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef GRAPH_OPTIMIZATION_H_
#define GRAPH_OPTIMIZATION_H_

#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_set>

// The helper consumes argv starting at OUTPUT, followed by -- and the producer.
int WriteIfChanged(int argc, char** argv);

// Cache payloads are bounded, versioned and checksummed. Missing, obsolete or
// corrupt records are misses. Publication never exposes a partial record.
bool ReadGraphCache(const std::string& path, std::string* payload);
bool WriteGraphCache(const std::string& path, std::string_view payload);
// Publish an immutable object. Hashes select names only; compare full bytes,
// and never replace a different object, even after a collision or corruption.
std::string PublishGraphObject(const std::string& base,
                               std::string_view payload,
                               bool raw = false);
// Defer durability barriers while constructing an unpublished generation.
// Flush before publishing any graph that refers to these objects. Batches are
// bounded; abandoning one leaves only complete, unreferenced immutable files.
class GraphObjectBatch {
 public:
  GraphObjectBatch();
  ~GraphObjectBatch();
  GraphObjectBatch(const GraphObjectBatch&) = delete;
  GraphObjectBatch& operator=(const GraphObjectBatch&) = delete;
  void Add(const std::string& path);
  void Flush();

 private:
  GraphObjectBatch* previous_;
  std::unordered_set<std::string> paths_;
};
void AppendGraphString(std::string_view value, std::string* data);
bool ConsumeGraphString(std::string_view* data, std::string* value);
bool ConsumeGraphStringView(std::string_view* data, std::string_view* value);
std::string GraphDigest(std::string_view value);
// The first double is the mutable validation checkpoint. All structural stamp
// bytes are sealed; refreshes may update only that checkpoint.
bool SealGraphStamp(FILE* stream);
FILE* OpenGraphStamp(const std::string& path, bool writable = false);

#endif
