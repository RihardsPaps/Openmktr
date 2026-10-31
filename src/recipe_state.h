// Copyright 2026 Openmktr contributors
// SPDX-License-Identifier: MPL-2.0
#ifndef RECIPE_STATE_H_
#define RECIPE_STATE_H_
#include <memory>
#include <string>

// Immutable expected recipe records share bounded pack files. Stable per-record
// descriptors keep unrelated commands unchanged when one record changes.
class RecipeStateBatch {
 public:
  RecipeStateBatch();
  ~RecipeStateBatch();
  RecipeStateBatch(const RecipeStateBatch&) = delete;
  RecipeStateBatch& operator=(const RecipeStateBatch&) = delete;
  void Flush();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  RecipeStateBatch* previous_;
  friend std::string PublishNinjaRecipeState(const std::string&,
                                             const std::string&);
};
bool IsPackedRecipeState(const std::string& path);
bool ReadPackedRecipeState(const std::string& path, std::string* state);
#endif
