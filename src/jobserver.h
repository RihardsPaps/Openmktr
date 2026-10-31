// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#ifndef JOBSERVER_H_
#define JOBSERVER_H_

// Check that the inherited jobserver can be opened as a read/write FIFO.
bool IsKatiJobserverAvailable();

// Acquire and release one slot from the optional inherited Kati jobserver.
// -1 means no slot; -2 is the inherited slot reserved by a parent recipe.
int AcquireKatiJobToken();
void ReleaseKatiJobToken(int fd);

// Hold one per-launcher load slot until close(fd). No FIFO token is held while
// waiting; fcntl locks release the slot automatically if the helper dies.
// Returns -1 when admission is disabled or interrupted.
int AcquireNinjaLoadSlot();

// A lease returns exactly the reservation it acquired. It never closes the
// shared FIFO descriptor, whose lifetime belongs to the jobserver transport.
class JobLease {
 public:
  explicit JobLease(bool acquire = true);
  ~JobLease();
  JobLease(const JobLease&) = delete;
  JobLease& operator=(const JobLease&) = delete;
  bool reserved() const { return token_ != -1; }

 private:
  int token_;
};

// Admission for every shell, including evaluation-time functions. Acquired
// before shared tokens, released after child cleanup; no evaluator lock is
// needed to return a slot.
class ProcessLease {
 public:
  ProcessLease();
  ~ProcessLease();
  explicit operator bool() const { return acquired_; }
  ProcessLease(const ProcessLease&) = delete;
  ProcessLease& operator=(const ProcessLease&) = delete;

 private:
  bool acquired_ = false;
};

#endif  // JOBSERVER_H_
