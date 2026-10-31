// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#ifndef GENERATION_H_
#define GENERATION_H_
#include <string>
#include <vector>

// Runtime caches remain in the stable output directory. Only the coherent
// graph/environment/launcher/stamp bundle goes through the generation pointer.
std::string ResolveGenerationArtifact(const std::string& path);
std::string GenerationPath(const std::string& directory,
                           const std::string& name);
class GenerationReader {
 public:
  // With no explicit directory, pin the current invocation's published bundle.
  explicit GenerationReader(const std::string& directory = "");
  ~GenerationReader();
  GenerationReader(const GenerationReader&) = delete;
  GenerationReader& operator=(const GenerationReader&) = delete;
  int InheritLease();
  bool valid() const { return valid_; }
  const std::string& directory() const { return directory_; }

 private:
  int descriptor_ = -1;
  bool valid_ = false;
  std::string directory_, previous_;
};
class GenerationTransaction {
 public:
  GenerationTransaction();
  ~GenerationTransaction();
  GenerationTransaction(const GenerationTransaction&) = delete;
  GenerationTransaction& operator=(const GenerationTransaction&) = delete;
  void Commit();
  const std::string& directory() const { return directory_; }

 private:
  int lock_ = -1;
  std::string root_, directory_, previous_;
  bool committed_ = false;
};
#endif
