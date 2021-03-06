// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_impl.h"

#include <stdint.h>
#include <stdio.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>
#include <zconf.h>
#include <fcntl.h>
#include <include/leveldb/vtable_builder.h>

#include "db/builder.h"
#include "db/db_iter.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/status.h"
#include "leveldb/table.h"
#include "leveldb/table_builder.h"
#include "port/port.h"
#include "table/block.h"
#include "table/merger.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "leveldb/statistics.h"
#include "sstream"
#include "table/value_iter.h"
#include "funcs.h"

static int mergeGCCnt = 0;

namespace leveldb {

FILE* wl = fopen("write_latencies","a+");
FILE* rl = fopen("read_latencies","a+");
const int kNumNonTableCacheFiles = 10;

// Information kept for every waiting writer
struct DBImpl::Writer {
  Status status;
  WriteBatch* batch;
  bool sync;
  bool done;
  port::CondVar cv;

  explicit Writer(port::Mutex* mu) : cv(mu) { }
};

struct DBImpl::CompactionState {
  Compaction* const compaction;

  // Sequence numbers < smallest_snapshot are not significant since we
  // will never have to service a snapshot below smallest_snapshot.
  // Therefore if we have seen a sequence number S <= smallest_snapshot,
  // we can drop all entries for the same key with sequence numbers < S.
  SequenceNumber smallest_snapshot;

  // Files produced by compaction
  struct Output {
    uint64_t number;
    uint64_t file_size;
    InternalKey smallest, largest;
  };
  std::vector<Output> outputs;

  // State kept for output being generated
  WritableFile* outfile;
  TableBuilder* builder;

  uint64_t total_bytes;

  Output* current_output() { return &outputs[outputs.size()-1]; }

  explicit CompactionState(Compaction* c)
      : compaction(c),
        outfile(nullptr),
        builder(nullptr),
        total_bytes(0) {
  }
};

// Fix user-supplied options to be reasonable
template <class T, class V>
static void ClipToRange(T* ptr, V minvalue, V maxvalue) {
  if (static_cast<V>(*ptr) > maxvalue) *ptr = maxvalue;
  if (static_cast<V>(*ptr) < minvalue) *ptr = minvalue;
}
Options SanitizeOptions(const std::string& dbname,
                        const InternalKeyComparator* icmp,
                        const InternalFilterPolicy* ipolicy,
                        const Options& src) {
  Options result = src;
  result.comparator = icmp;
  result.filter_policy = (src.filter_policy != nullptr) ? ipolicy : nullptr;
  ClipToRange(&result.max_open_files,    64 + kNumNonTableCacheFiles, 50000);
  ClipToRange(&result.write_buffer_size, 64<<10,                      1<<30);
  ClipToRange(&result.max_file_size,     1<<20,                       1<<30);
  ClipToRange(&result.block_size,        1<<10,                       4<<20);
  if (result.info_log == nullptr) {
    // Open a log file in the same directory as the db
    src.env->CreateDir(dbname);  // In case it does not exist
    src.env->RenameFile(InfoLogFileName(dbname), OldInfoLogFileName(dbname));
    Status s = src.env->NewLogger(InfoLogFileName(dbname), &result.info_log);
    if (!s.ok()) {
      // No place suitable for logging
      result.info_log = nullptr;
    }
  }
  if (result.block_cache == nullptr) {
    result.block_cache = NewLRUCache(8 << 20);
  }
  return result;
}

static int TableCacheSize(const Options& sanitized_options) {
  // Reserve ten files or so for other uses and give the rest to TableCache.
  return sanitized_options.max_open_files - kNumNonTableCacheFiles;
}

DBImpl::DBImpl(const Options& raw_options, const std::string& dbname)
    : env_(raw_options.env),
      internal_comparator_(raw_options.comparator),
      internal_filter_policy_(raw_options.filter_policy),
      options_(SanitizeOptions(dbname, &internal_comparator_,
                               &internal_filter_policy_, raw_options)),
      owns_info_log_(options_.info_log != raw_options.info_log),
      owns_cache_(options_.block_cache != raw_options.block_cache),
      dbname_(dbname),
      table_cache_(new TableCache(dbname_, options_, TableCacheSize(options_))),
      db_lock_(nullptr),
      shutting_down_(nullptr),
      background_work_finished_signal_(&mutex_),
      mem_(nullptr),
      imm_(nullptr),
      logfile_(nullptr),
      logfile_number_(0),
      log_(nullptr),
      seed_(0),
      tmp_batch_(new WriteBatch),
      background_compaction_scheduled_(false),
      manual_compaction_(nullptr),
      versions_(new VersionSet(dbname_, &options_, table_cache_,
                               &internal_comparator_)),
      lastVlog_(0),
      lastVtable_(0),
      lastMidLog_(0),
      openedFiles_(), lastGCFile_(0), toMerge_(), toGC_(){
  threadPool_ = new ThreadPool(options_.exp_ops.numThreads);
  has_imm_.Release_Store(nullptr);
  if(access((dbname_+"/values").c_str(),F_OK)!=0&&options_.create_if_missing){
    env_->CreateDir(dbname_+"/values");
  }
  writingVlog_.NextFile(vlogPathname(lastVlog_));
  midVlog_.NextFile(valueFilePath("m"+std::to_string(lastMidLog_)));
  std::cerr<<"threads:"<<options_.exp_ops.numThreads<<std::endl;
  //threadPool_->addTask(&DBImpl::scheduleMerge,this);
  threadPool_->addTask(&DBImpl::scheduleGC,this);
  // TODO temp
  //lastVtable_ = 30000;
}

DBImpl::~DBImpl() {
    //TODO remove this
  toGC_.Put("");
  std::cout<<"GCed "<<mergeGCCnt<<" vtables during merge\n";
  delete threadPool_;
  // Wait for background work to finish
  mutex_.Lock();
  shutting_down_.Release_Store(this);  // Any non-null value is ok
  while (background_compaction_scheduled_) {
    background_work_finished_signal_.Wait();
  }
  mutex_.Unlock();

  if (db_lock_ != nullptr) {
    env_->UnlockFile(db_lock_);
  }

  delete versions_;
  if (mem_ != nullptr) mem_->Unref();
  if (imm_ != nullptr) imm_->Unref();
  delete tmp_batch_;
  delete log_;
  delete logfile_;
  delete table_cache_;

  if (owns_info_log_) {
    delete options_.info_log;
  }
  if (owns_cache_) {
    delete options_.block_cache;
  }
}

Status DBImpl::NewDB() {
  VersionEdit new_db;
  new_db.SetComparatorName(user_comparator()->Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);

  const std::string manifest = DescriptorFileName(dbname_, 1);
  WritableFile* file;
  Status s = env_->NewWritableFile(manifest, &file);
  if (!s.ok()) {
    return s;
  }
  {
    log::Writer log(file);
    std::string record;
    new_db.EncodeTo(&record);
    s = log.AddRecord(record);
    if (s.ok()) {
      s = file->Close();
    }
  }
  delete file;
  if (s.ok()) {
    // Make "CURRENT" file that points to the new manifest file.
    s = SetCurrentFile(env_, dbname_, 1);
  } else {
    env_->DeleteFile(manifest);
  }
  return s;
}

void DBImpl::MaybeIgnoreError(Status* s) const {
  if (s->ok() || options_.paranoid_checks) {
    // No change needed
  } else {
    Log(options_.info_log, "Ignoring error %s", s->ToString().c_str());
    *s = Status::OK();
  }
}

void DBImpl::DeleteObsoleteFiles() {
  mutex_.AssertHeld();

  if (!bg_error_.ok()) {
    // After a background error, we don't know whether a new version may
    // or may not have been committed, so we cannot safely garbage collect.
    return;
  }

  // Make a set of all of the live files
  std::set<uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);

  std::vector<std::string> filenames;
  env_->GetChildren(dbname_, &filenames);  // Ignoring errors on purpose
  uint64_t number;
  FileType type;
  for (size_t i = 0; i < filenames.size(); i++) {
    if (ParseFileName(filenames[i], &number, &type)) {
      bool keep = true;
      switch (type) {
        case kLogFile:
          keep = ((number >= versions_->LogNumber()) ||
                  (number == versions_->PrevLogNumber()));
          break;
        case kDescriptorFile:
          // Keep my manifest file, and any newer incarnations'
          // (in case there is a race that allows other incarnations)
          keep = (number >= versions_->ManifestFileNumber());
          break;
        case kTableFile:
          keep = (live.find(number) != live.end());
          break;
        case kTempFile:
          // Any temp files that are currently being written to must
          // be recorded in pending_outputs_, which is inserted into "live"
          keep = (live.find(number) != live.end());
          break;
        case kCurrentFile:
        case kDBLockFile:
        case kInfoLogFile:
          keep = true;
          break;
      }

      if (!keep) {
        if (type == kTableFile) {
          table_cache_->Evict(number);
        }
        Log(options_.info_log, "Delete type=%d #%lld\n",
            static_cast<int>(type),
            static_cast<unsigned long long>(number));
        env_->DeleteFile(dbname_ + "/" + filenames[i]);
      }
    }
  }
}

Status DBImpl::Recover(VersionEdit* edit, bool *save_manifest) {
  mutex_.AssertHeld();

  // Ignore error from CreateDir since the creation of the DB is
  // committed only when the descriptor is created, and this directory
  // may already exist from a previous failed creation attempt.
  env_->CreateDir(dbname_);
  assert(db_lock_ == nullptr);
  Status s = env_->LockFile(LockFileName(dbname_), &db_lock_);
  if (!s.ok()) {
    return s;
  }

  if (!env_->FileExists(CurrentFileName(dbname_))) {
    if (options_.create_if_missing) {
      s = NewDB();
      if (!s.ok()) {
        return s;
      }
    } else {
      return Status::InvalidArgument(
          dbname_, "does not exist (create_if_missing is false)");
    }
  } else {
    if (options_.error_if_exists) {
      return Status::InvalidArgument(
          dbname_, "exists (error_if_exists is true)");
    }
  }
  s = versions_->Recover(save_manifest);
  if (!s.ok()) {
    return s;
  }
  SequenceNumber max_sequence(0);
  // Recover from all newer log files than the ones named in the
  // descriptor (new log files may have been added by the previous
  // incarnation without registering them in the descriptor).
  //
  // Note that PrevLogNumber() is no longer used, but we pay
  // attention to it in case we are recovering a database
  // produced by an older version of leveldb.
  const uint64_t min_log = versions_->LogNumber();
  const uint64_t prev_log = versions_->PrevLogNumber();
  std::vector<std::string> filenames;
  s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) {
    return s;
  }
  std::set<uint64_t> expected;
  versions_->AddLiveFiles(&expected);
  uint64_t number;
  FileType type;
  std::vector<uint64_t> logs;
  for (size_t i = 0; i < filenames.size(); i++) {
    if (ParseFileName(filenames[i], &number, &type)) {
        expected.erase(number);
      if (type == kLogFile && ((number >= min_log) || (number == prev_log)))
        logs.push_back(number);
    }
  }

    if (!expected.empty()) {
    char buf[50];
    snprintf(buf, sizeof(buf), "%d missing files; e.g.",
             static_cast<int>(expected.size()));
    return Status::Corruption(buf, TableFileName(dbname_, *(expected.begin())));
  }
  // Recover in the order in which the logs were generated
  std::sort(logs.begin(), logs.end());
  for (size_t i = 0; i < logs.size(); i++) {
    s = RecoverLogFile(logs[i], (i == logs.size() - 1), save_manifest, edit,
                       &max_sequence);
    if (!s.ok()) {
      return s;
    }

    // The previous incarnation may not have written any MANIFEST
    // records after allocating this log number.  So we manually
    // update the file number allocation counter in VersionSet.
    versions_->MarkFileNumberUsed(logs[i]);
  }

  if (versions_->LastSequence() < max_sequence) {
    versions_->SetLastSequence(max_sequence);
  }
  return Status::OK();
}

Status DBImpl::RecoverLogFile(uint64_t log_number, bool last_log,
                              bool* save_manifest, VersionEdit* edit,
                              SequenceNumber* max_sequence) {
  struct LogReporter : public log::Reader::Reporter {
    Env* env;
    Logger* info_log;
    const char* fname;
    Status* status;  // null if options_.paranoid_checks==false
    virtual void Corruption(size_t bytes, const Status& s) {
      Log(info_log, "%s%s: dropping %d bytes; %s",
          (this->status == nullptr ? "(ignoring error) " : ""),
          fname, static_cast<int>(bytes), s.ToString().c_str());
      if (this->status != nullptr && this->status->ok()) *this->status = s;
    }
  };

  mutex_.AssertHeld();

  // Open the log file
  std::string fname = LogFileName(dbname_, log_number);
  SequentialFile* file;
  Status status = env_->NewSequentialFile(fname, &file);
  if (!status.ok()) {
    MaybeIgnoreError(&status);
    return status;
  }

  // Create the log reader.
  LogReporter reporter;
  reporter.env = env_;
  reporter.info_log = options_.info_log;
  reporter.fname = fname.c_str();
  reporter.status = (options_.paranoid_checks ? &status : nullptr);
  // We intentionally make log::Reader do checksumming even if
  // paranoid_checks==false so that corruptions cause entire commits
  // to be skipped instead of propagating bad information (like overly
  // large sequence numbers).
  log::Reader reader(file, &reporter, true/*checksum*/,
                     0/*initial_offset*/);
  Log(options_.info_log, "Recovering log #%llu",
      (unsigned long long) log_number);

  // Read all the records and add to a memtable
  std::string scratch;
  Slice record;
  WriteBatch batch;
  int compactions = 0;
  MemTable* mem = nullptr;
  while (reader.ReadRecord(&record, &scratch) &&
         status.ok()) {
    if (record.size() < 12) {
      reporter.Corruption(
          record.size(), Status::Corruption("log record too small"));
      continue;
    }
    WriteBatchInternal::SetContents(&batch, record);

    if (mem == nullptr) {
      mem = new MemTable(internal_comparator_);
      mem->Ref();
    }
    status = WriteBatchInternal::InsertInto(&batch, mem);
    MaybeIgnoreError(&status);
    if (!status.ok()) {
      break;
    }
    const SequenceNumber last_seq =
        WriteBatchInternal::Sequence(&batch) +
        WriteBatchInternal::Count(&batch) - 1;
    if (last_seq > *max_sequence) {
      *max_sequence = last_seq;
    }

    if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) {
      compactions++;
      *save_manifest = true;
      status = WriteLevel0Table(mem, edit, nullptr);
      mem->Unref();
      mem = nullptr;
      if (!status.ok()) {
        // Reflect errors immediately so that conditions like full
        // file-systems cause the DB::Open() to fail.
        break;
      }
    }
  }

  delete file;

  // See if we should keep reusing the last log file.
  if (status.ok() && options_.reuse_logs && last_log && compactions == 0) {
    assert(logfile_ == nullptr);
    assert(log_ == nullptr);
    assert(mem_ == nullptr);
    uint64_t lfile_size;
    if (env_->GetFileSize(fname, &lfile_size).ok() &&
        env_->NewAppendableFile(fname, &logfile_).ok()) {
      Log(options_.info_log, "Reusing old log %s \n", fname.c_str());
      log_ = new log::Writer(logfile_, lfile_size);
      logfile_number_ = log_number;
      if (mem != nullptr) {
        mem_ = mem;
        mem = nullptr;
      } else {
        // mem can be nullptr if lognum exists but was empty.
        mem_ = new MemTable(internal_comparator_);
        mem_->Ref();
      }
    }
  }

  if (mem != nullptr) {
    // mem did not get reused; compact it.
    if (status.ok()) {
      *save_manifest = true;
        status = WriteLevel0Table(mem, edit, nullptr);
    }
    mem->Unref();
  }

  return status;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit,
                                Version* base) {
  mutex_.AssertHeld();
  const uint64_t start_micros = env_->NowMicros();
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  pending_outputs_.insert(meta.number);
  Iterator* iter = mem->NewIterator();
  Log(options_.info_log, "Level-0 table #%llu: started",
      (unsigned long long) meta.number);

  Status s;
  {
    mutex_.Unlock();
    s = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta, lastVtable_, metaTable_);
    // delete old one
    deleteFile(conbineStr({"m",std::to_string(lastMidLog_-1)}));
    mutex_.Lock();
  }

  Log(options_.info_log, "Level-0 table #%llu: %lld bytes %s",
      (unsigned long long) meta.number,
      (unsigned long long) meta.file_size,
      s.ToString().c_str());
  delete iter;
  pending_outputs_.erase(meta.number);


  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  int level = 0;
  if (s.ok() && meta.file_size > 0) {
    const Slice min_user_key = meta.smallest.user_key();
    const Slice max_user_key = meta.largest.user_key();
    if (base != nullptr) {
      level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
    }
    edit->AddFile(level, meta.number, meta.file_size,
                  meta.smallest, meta.largest);
  }

  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros;
  stats.bytes_written = meta.file_size;
  stats_[level].Add(stats);
  return s;
}

void DBImpl::CompactMemTable() {
  mutex_.AssertHeld();
  assert(imm_ != nullptr);

  // Save the contents of the memtable as a new Table
  VersionEdit edit;
  Version* base = versions_->current();
  base->Ref();
  Status s = WriteLevel0Table(imm_, &edit, base);
  //delete old one
  base->Unref();

  if (s.ok() && shutting_down_.Acquire_Load()) {
    s = Status::IOError("Deleting DB during memtable compaction");
  }

  // Replace immutable memtable with the generated Table
  if (s.ok()) {
    edit.SetPrevLogNumber(0);
    edit.SetLogNumber(logfile_number_);  // Earlier logs no longer needed
    s = versions_->LogAndApply(&edit, &mutex_);
  }

  if (s.ok()) {
    // Commit to the new state
    imm_->Unref();
    imm_ = nullptr;
    has_imm_.Release_Store(nullptr);
    DeleteObsoleteFiles();
  } else {
    RecordBackgroundError(s);
  }
}

void DBImpl::CompactRange(const Slice* begin, const Slice* end) {
  int max_level_with_files = 1;
  {
    MutexLock l(&mutex_);
    Version* base = versions_->current();
    for (int level = 1; level < config::kNumLevels; level++) {
      if (base->OverlapInLevel(level, begin, end)) {
        max_level_with_files = level;
      }
    }
  }
  TEST_CompactMemTable();  // TODO(sanjay): Skip if memtable does not overlap
  for (int level = 0; level < max_level_with_files; level++) {
    TEST_CompactRange(level, begin, end);
  }
}

void DBImpl::TEST_CompactRange(int level, const Slice* begin,
                               const Slice* end) {
  assert(level >= 0);
  assert(level + 1 < config::kNumLevels);

  InternalKey begin_storage, end_storage;

  ManualCompaction manual;
  manual.level = level;
  manual.done = false;
  if (begin == nullptr) {
    manual.begin = nullptr;
  } else {
    begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    manual.begin = &begin_storage;
  }
  if (end == nullptr) {
    manual.end = nullptr;
  } else {
    end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
    manual.end = &end_storage;
  }

  MutexLock l(&mutex_);
  while (!manual.done && !shutting_down_.Acquire_Load() && bg_error_.ok()) {
    if (manual_compaction_ == nullptr) {  // Idle
      manual_compaction_ = &manual;
      MaybeScheduleCompaction();
    } else {  // Running either my compaction or another compaction.
      background_work_finished_signal_.Wait();
    }
  }
  if (manual_compaction_ == &manual) {
    // Cancel my manual compaction since we aborted early for some reason.
    manual_compaction_ = nullptr;
  }
}

Status DBImpl::TEST_CompactMemTable() {
  // nullptr batch means just wait for earlier writes to be done
  Status s = Write(WriteOptions(), nullptr);
  if (s.ok()) {
    // Wait until the compaction completes
    MutexLock l(&mutex_);
    while (imm_ != nullptr && bg_error_.ok()) {
      background_work_finished_signal_.Wait();
    }
    if (imm_ != nullptr) {
      s = bg_error_;
    }
  }
  return s;
}

void DBImpl::RecordBackgroundError(const Status& s) {
  mutex_.AssertHeld();
  if (bg_error_.ok()) {
    bg_error_ = s;
    background_work_finished_signal_.SignalAll();
  }
}

void DBImpl::MaybeScheduleCompaction() {
  if(options_.exp_ops.noCompaction) return;
  mutex_.AssertHeld();
  if (background_compaction_scheduled_) {
    // Already scheduled
  } else if (shutting_down_.Acquire_Load()) {
    // DB is being deleted; no more background compactions
  } else if (!bg_error_.ok()) {
    // Already got an error; no more changes
  } else if (imm_ == nullptr &&
             manual_compaction_ == nullptr &&
             !versions_->NeedsCompaction()) {
    // No work to be done
  } else {
    background_compaction_scheduled_ = true;
    env_->Schedule(&DBImpl::BGWork, this);
  }
}

void DBImpl::BGWork(void* db) {
  reinterpret_cast<DBImpl*>(db)->BackgroundCall();
}

void DBImpl::BackgroundCall() {
  MutexLock l(&mutex_);
  assert(background_compaction_scheduled_);
  if (shutting_down_.Acquire_Load()) {
    // No more background work when shutting down.
  } else if (!bg_error_.ok()) {
    // No more background work after a background error.
  } else {
    BackgroundCompaction();
  }

  background_compaction_scheduled_ = false;

  // Previous compaction may have produced too many files in a level,
  // so reschedule another compaction if needed.
  MaybeScheduleCompaction();
  background_work_finished_signal_.SignalAll();
}

void DBImpl::BackgroundCompaction() {
  mutex_.AssertHeld();

  if (imm_ != nullptr) {
    CompactMemTable();
    return;
  }

  Compaction* c;
  bool is_manual = (manual_compaction_ != nullptr);
  InternalKey manual_end;
  if (is_manual) {
    ManualCompaction* m = manual_compaction_;
    c = versions_->CompactRange(m->level, m->begin, m->end);
    m->done = (c == nullptr);
    if (c != nullptr) {
      manual_end = c->input(0, c->num_input_files(0) - 1)->largest;
    }
    Log(options_.info_log,
        "Manual compaction at level-%d from %s .. %s; will stop at %s\n",
        m->level,
        (m->begin ? m->begin->DebugString().c_str() : "(begin)"),
        (m->end ? m->end->DebugString().c_str() : "(end)"),
        (m->done ? "(end)" : manual_end.DebugString().c_str()));
  } else {
    c = versions_->PickCompaction();
  }

  Status status;
  if (c == nullptr) {
    // Nothing to do
      // Selective: no trivialmove for merge level
  } else if (!is_manual && c->IsTrivialMove() && c->level()<options_.exp_ops.mergeLevel) {
    // Move file to next level
    assert(c->num_input_files(0) == 1);
    FileMetaData* f = c->input(0, 0);
    c->edit()->DeleteFile(c->level(), f->number);
    c->edit()->AddFile(c->level() + 1, f->number, f->file_size,
                       f->smallest, f->largest);
    status = versions_->LogAndApply(c->edit(), &mutex_);
    if (!status.ok()) {
      RecordBackgroundError(status);
    }
    VersionSet::LevelSummaryStorage tmp;
    Log(options_.info_log, "Moved #%lld to level-%d %lld bytes %s: %s\n",
        static_cast<unsigned long long>(f->number),
        c->level() + 1,
        static_cast<unsigned long long>(f->file_size),
        status.ToString().c_str(),
        versions_->LevelSummary(&tmp));
  } else {
    CompactionState* compact = new CompactionState(c);
    status = DoCompactionWork(compact);
    if (!status.ok()) {
      RecordBackgroundError(status);
    }
    CleanupCompaction(compact);
    c->ReleaseInputs();
    DeleteObsoleteFiles();
  }
  delete c;

  if (status.ok()) {
    // Done
  } else if (shutting_down_.Acquire_Load()) {
    // Ignore compaction errors found during shutting down
  } else {
    Log(options_.info_log,
        "Compaction error: %s", status.ToString().c_str());
  }

  if (is_manual) {
    ManualCompaction* m = manual_compaction_;
    if (!status.ok()) {
      m->done = true;
    }
    if (!m->done) {
      // We only compacted part of the requested range.  Update *m
      // to the range that is left to be compacted.
      m->tmp_storage = manual_end;
      m->begin = &m->tmp_storage;
    }
    manual_compaction_ = nullptr;
  }
}

void DBImpl::CleanupCompaction(CompactionState* compact) {
  mutex_.AssertHeld();
  if (compact->builder != nullptr) {
    // May happen if we get a shutdown call in the middle of compaction
    compact->builder->Abandon();
    delete compact->builder;
  } else {
    assert(compact->outfile == nullptr);
  }
  delete compact->outfile;
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    pending_outputs_.erase(out.number);
  }
  delete compact;
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
  assert(compact != nullptr);
  assert(compact->builder == nullptr);
  uint64_t file_number;
  {
    mutex_.Lock();
    file_number = versions_->NewFileNumber();
    pending_outputs_.insert(file_number);
    CompactionState::Output out;
    out.number = file_number;
    out.smallest.Clear();
    out.largest.Clear();
    compact->outputs.push_back(out);
    mutex_.Unlock();
  }

  // Make the output file
  std::string fname = TableFileName(dbname_, file_number);
  Status s = env_->NewWritableFile(fname, &compact->outfile);
  if (s.ok()) {
    compact->builder = new TableBuilder(options_, compact->outfile);
  }
  return s;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact,
                                          Iterator* input) {
  uint64_t startMicros = NowMicros();
  assert(compact != nullptr);
  assert(compact->outfile != nullptr);
  assert(compact->builder != nullptr);

  const uint64_t output_number = compact->current_output()->number;
  assert(output_number != 0);

  // Check for iterator errors
  Status s = input->status();
  const uint64_t current_entries = compact->builder->NumEntries();
  if (s.ok()) {
    s = compact->builder->Finish();
  } else {
    compact->builder->Abandon();
  }
  const uint64_t current_bytes = compact->builder->FileSize();
  compact->current_output()->file_size = current_bytes;
  compact->total_bytes += current_bytes;
  delete compact->builder;
  compact->builder = nullptr;

  // Finish and check for file errors
  if (s.ok()) {
    s = compact->outfile->Sync();
  }
  if (s.ok()) {
    s = compact->outfile->Close();
  }
  delete compact->outfile;
  compact->outfile = nullptr;

  if (s.ok() && current_entries > 0) {
    // Verify that the table is usable
    Iterator* iter = table_cache_->NewIterator(ReadOptions(),
                                               output_number,
                                               current_bytes);
    s = iter->status();
    delete iter;
    if (s.ok()) {
      Log(options_.info_log,
          "Generated table #%llu@%d: %lld keys, %lld bytes",
          (unsigned long long) output_number,
          compact->compaction->level(),
          (unsigned long long) current_entries,
          (unsigned long long) current_bytes);
    }
  }
  STATS::Time(STATS::GetInstance()->compactionOutputTime,startMicros,NowMicros());
  return s;
}


Status DBImpl::InstallCompactionResults(CompactionState* compact) {
  mutex_.AssertHeld();
  Log(options_.info_log,  "Compacted %d@%d + %d@%d files => %lld bytes",
      compact->compaction->num_input_files(0),
      compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->level() + 1,
      static_cast<long long>(compact->total_bytes));

  // Add compaction outputs
  compact->compaction->AddInputDeletions(compact->compaction->edit());
  const int level = compact->compaction->level();
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& out = compact->outputs[i];
    compact->compaction->edit()->AddFile(
        level + 1,
        out.number, out.file_size, out.smallest, out.largest);
  }
  return versions_->LogAndApply(compact->compaction->edit(), &mutex_);
}

Status DBImpl::DoCompactionWork(CompactionState* compact) {
  const uint64_t start_micros = env_->NowMicros();
  int64_t imm_micros = 0;  // Micros spent doing imm_ compactions

  Log(options_.info_log,  "Compacting %d@%d + %d@%d files",
      compact->compaction->num_input_files(0),
      compact->compaction->level(),
      compact->compaction->num_input_files(1),
      compact->compaction->level() + 1);

  assert(versions_->NumLevelFiles(compact->compaction->level()) > 0);
  assert(compact->builder == nullptr);
  assert(compact->outfile == nullptr);
  if (snapshots_.empty()) {
    compact->smallest_snapshot = versions_->LastSequence();
  } else {
    compact->smallest_snapshot = snapshots_.oldest()->sequence_number();
  }

  // Release mutex while we're actually doing the compaction work
  mutex_.Unlock();

  /* selectiveKV */
  int level = compact->compaction->level();
  char levelPrefix = 'a'+level;
//  std::cerr<<"level prefix:"<<levelPrefix<<std::endl;
  std::string nextprefix(1,levelPrefix+1);
  VtableBuilder* vtableBuilder = new VtableBuilder();
  std::string vtablename;
  std::string vtablepathname;
//  std::cerr<<"compaction level "<<level<<", prefix"<<levelPrefix<<" next prefix "<<nextprefix<<std::endl;
  std::unordered_map<std::string, int> invalidated;

  Iterator* input = versions_->MakeInputIterator(compact->compaction);
  input->SeekToFirst();
  Status status;
  ParsedInternalKey ikey;
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  uint64_t startIter = NowMicros();
  for (; input->Valid() && !shutting_down_.Acquire_Load(); ) {
    // Prioritize immutable compaction work
    if (has_imm_.NoBarrier_Load() != nullptr) {
      const uint64_t imm_start = env_->NowMicros();
      mutex_.Lock();
      if (imm_ != nullptr) {
        CompactMemTable();
        // Wake up MakeRoomForWrite() if necessary.
        background_work_finished_signal_.SignalAll();
      }
      mutex_.Unlock();
      imm_micros += (env_->NowMicros() - imm_start);
    }

    Slice key = input->key();
    if (compact->compaction->ShouldStopBefore(key) &&
        compact->builder != nullptr) {
      status = FinishCompactionOutputFile(compact, input);
      if (!status.ok()) {
        break;
      }
    }

    // Handle key/value, add to state, etc.
    bool drop = false;
    if (!ParseInternalKey(key, &ikey)) {
      // Do not hide error keys
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key ||
          user_comparator()->Compare(ikey.user_key,
                                     Slice(current_user_key)) != 0) {
        // First occurrence of this user key
        current_user_key.assign(ikey.user_key.data(), ikey.user_key.size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }

      if (last_sequence_for_key <= compact->smallest_snapshot) {
        // Hidden by an newer entry for same user key
        drop = true;    // (A)

        // update vfile meta
        uint64_t startMeta = NowMicros();
        std::string value = input->value().ToString();
        if(value.back()=='~') {
          std::string filename;
          std::stringstream ss(input->value().ToString());
          std::getline(ss, filename, '~');
          int invalid = 1;
          // for vlog, update invalid size
          if(value[0]=='l'||value[0]=='g') {
            std::string size;
            std::getline(ss,size,'~');
            std::getline(ss,size,'~');
            invalid = std::stoi(size);
          }
          auto it = invalidated.find(filename);
          if(it!=invalidated.end()) {
            it->second+=invalid;
          } else invalidated[filename] = invalid;
        }
        STATS::Time(STATS::GetInstance()->gcMeta,startMeta,NowMicros());
      } else if (ikey.type == kTypeDeletion &&
                 ikey.sequence <= compact->smallest_snapshot &&
                 compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
        // For this user key:
        // (1) there is no data in higher levels
        // (2) data in lower levels will have larger sequence numbers
        // (3) data in layers that are being compacted here and have
        //     smaller sequence numbers will be dropped in the next
        //     few iterations of this loop (by rule (A) above).
        // Therefore this deletion marker is obsolete and can be dropped.
        drop = true;
      }

      last_sequence_for_key = ikey.sequence;
    }
#if 0
    Log(options_.info_log,
        "  Compact: %s, seq %d, type: %d %d, drop: %d, is_base: %d, "
        "%d smallest_snapshot: %d",
        ikey.user_key.ToString().c_str(),
        (int)ikey.sequence, ikey.type, kTypeValue, drop,
        compact->compaction->IsBaseLevelForKey(ikey.user_key),
        (int)last_sequence_for_key, (int)compact->smallest_snapshot);
#endif

    if (!drop) {
      // merge vtables
      // TODO: GC
      std::string value = input->value().ToString();
      if(value.back()=='~'){
        std::string filename;
        size_t offset, size;
        parseValueInfo(value,filename,offset,size);
        if(value[0]==levelPrefix||(doMerge_&&(level==options_.exp_ops.gcLevel)&&toMerge_.find(filename)!=toMerge_.end())) {
            if (vtableBuilder->Finished()) {
                vtablename = conbineStr({nextprefix, std::to_string(++lastVtable_)});
                vtablepathname = valueFilePath(vtablename);
                vtableBuilder->NextFile(vtablepathname);
            }
//        std::cerr<<"value info:"<<value<<std::endl;
            std::string v = readValue(openValueFile(filename), offset, size);
            offset = vtableBuilder->Add(key, v);
//        std::cerr<<"add done\n";
            value = conbineValueInfo(vtablename, offset, size);
            if (offset > options_.exp_ops.tableSize) {
                int cnt = vtableBuilder->Finish();
                metaTable_[vtablename] = VfileMeta(cnt);
//                std::cerr << "output new vtable: " << vtablename << std::endl;
            }
          auto it = invalidated.find(filename);
          if(it!=invalidated.end()) {
            it->second+=1;
          } else invalidated[filename] = 1;
        }
      }


      // Open output file if necessary
      if (compact->builder == nullptr) {
        status = OpenCompactionOutputFile(compact);
        if (!status.ok()) {
          break;
        }
      }
      if (compact->builder->NumEntries() == 0) {
        compact->current_output()->smallest.DecodeFrom(key);
      }
      compact->current_output()->largest.DecodeFrom(key);
      compact->builder->Add(key, value);

      // Close output file if it is big enough
      if (compact->builder->FileSize() >=
          compact->compaction->MaxOutputFileSize()) {
        status = FinishCompactionOutputFile(compact, input);
        if (!status.ok()) {
          break;
        }
      }
    }

    int64_t startNext = NowMicros();
    input->Next();
    STATS::Time(STATS::GetInstance()->compactionFindNext,startNext,NowMicros());
  }
  if(!vtableBuilder->Finished()){
      int cnt = vtableBuilder->Finish();
      metaTable_[vtablename] = VfileMeta(cnt);
  }
  vtableBuilder->Sync();
  delete vtableBuilder;
  for(auto& p:invalidated){
    updateMeta(p.first,p.second);
  }
  STATS::Time(STATS::GetInstance()->compactionIterTime,startIter,NowMicros());

  if (status.ok() && shutting_down_.Acquire_Load()) {
    status = Status::IOError("Deleting DB during compaction");
  }
  if (status.ok() && compact->builder != nullptr) {
    status = FinishCompactionOutputFile(compact, input);
  }
  if (status.ok()) {
    status = input->status();
  }
  delete input;
  input = nullptr;

  CompactionStats stats;
  stats.micros = env_->NowMicros() - start_micros - imm_micros;
  for (int which = 0; which < 2; which++) {
    for (int i = 0; i < compact->compaction->num_input_files(which); i++) {
      stats.bytes_read += compact->compaction->input(which, i)->file_size;
    }
  }
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    stats.bytes_written += compact->outputs[i].file_size;
  }

  mutex_.Lock();
  stats_[compact->compaction->level() + 1].Add(stats);

  if (status.ok()) {
    status = InstallCompactionResults(compact);
  }
  if (!status.ok()) {
    RecordBackgroundError(status);
  }
  VersionSet::LevelSummaryStorage tmp;
  Log(options_.info_log,
      "compacted to: %s", versions_->LevelSummary(&tmp));
  return status;
}

namespace {

struct IterState {
  port::Mutex* const mu;
  Version* const version GUARDED_BY(mu);
  MemTable* const mem GUARDED_BY(mu);
  MemTable* const imm GUARDED_BY(mu);

  IterState(port::Mutex* mutex, MemTable* mem, MemTable* imm, Version* version)
      : mu(mutex), version(version), mem(mem), imm(imm) { }
};

static void CleanupIteratorState(void* arg1, void* arg2) {
  IterState* state = reinterpret_cast<IterState*>(arg1);
  state->mu->Lock();
  state->mem->Unref();
  if (state->imm != nullptr) state->imm->Unref();
  state->version->Unref();
  state->mu->Unlock();
  delete state;
}

}  // anonymous namespace

Iterator* DBImpl::NewInternalIterator(const ReadOptions& options,
                                      SequenceNumber* latest_snapshot,
                                      uint32_t* seed) {
  mutex_.Lock();
  *latest_snapshot = versions_->LastSequence();

  // Collect together all needed child iterators
  std::vector<Iterator*> list;
  list.push_back(mem_->NewIterator());
  mem_->Ref();
  if (imm_ != nullptr) {
    list.push_back(imm_->NewIterator());
    imm_->Ref();
  }
  versions_->current()->AddIterators(options, &list);
  Iterator* internal_iter =
      NewMergingIterator(&internal_comparator_, &list[0], list.size());
  versions_->current()->Ref();

  IterState* cleanup = new IterState(&mutex_, mem_, imm_, versions_->current());
  internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, nullptr);

  *seed = ++seed_;
  mutex_.Unlock();
  return internal_iter;
}

Iterator* DBImpl::TEST_NewInternalIterator() {
  SequenceNumber ignored;
  uint32_t ignored_seed;
  return NewInternalIterator(ReadOptions(), &ignored, &ignored_seed);
}

int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes() {
  MutexLock l(&mutex_);
  return versions_->MaxNextLevelOverlappingBytes();
}

Status DBImpl::Get(const ReadOptions& options,
                   const Slice& key,
                   std::string* value) {
  uint64_t startMicros = Env::Default()->NowMicros();
  Status s;
  MutexLock l(&mutex_);
  SequenceNumber snapshot;
  if (options.snapshot != nullptr) {
    snapshot =
        static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number();
  } else {
    snapshot = versions_->LastSequence();
  }

  MemTable* mem = mem_;
  MemTable* imm = imm_;
  Version* current = versions_->current();
  mem->Ref();
  if (imm != nullptr) imm->Ref();
  current->Ref();

  bool have_stat_update = false;
  Version::GetStats stats;

  // Unlock while reading from files and memtables
  {
    mutex_.Unlock();
    // First look in the memtable, then in the immutable memtable (if any).
    LookupKey lkey(key, snapshot);
    if (mem->Get(lkey, value, &s)) {
      // Done
    } else if (imm != nullptr && imm->Get(lkey, value, &s)) {
      // Done
    } else {
      s = current->Get(options, lkey, value, &stats);
      have_stat_update = true;
    }
    mutex_.Lock();
  }

  if (have_stat_update && current->UpdateStats(stats)) {
    MaybeScheduleCompaction();
  }
  mem->Unref();
  if (imm != nullptr) imm->Unref();
  current->Unref();
  //std::cerr<<*value<<std::endl;
  if (s.ok()) {
    if (!options.value_pos) {
      readValueWithAddress(*value);
    }
  }
  STATS::TimeAndCount(STATS::GetInstance()->readStat,startMicros,Env::Default()->NowMicros());
  //fprintf(rl,"%lu,",NowMicros()-startMicros);
  //std::cerr<<*value<<std::endl;
  return s;
}

Iterator* DBImpl::NewIterator(const ReadOptions& options) {
  SequenceNumber latest_snapshot;
  uint32_t seed;
  Iterator* iter = NewInternalIterator(options, &latest_snapshot, &seed);
  return NewDBIterator(
      this, user_comparator(), iter,
      (options.snapshot != nullptr
       ? static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number()
       : latest_snapshot),
      seed);
}

void DBImpl::RecordReadSample(Slice key) {
  MutexLock l(&mutex_);
  if (versions_->current()->RecordReadSample(key)) {
    MaybeScheduleCompaction();
  }
}

const Snapshot* DBImpl::GetSnapshot() {
  MutexLock l(&mutex_);
  return snapshots_.New(versions_->LastSequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  MutexLock l(&mutex_);
  snapshots_.Delete(static_cast<const SnapshotImpl*>(snapshot));
}

// Convenience methods
Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& val) {
//  std::cerr<<"valuesize "<<val.size()<<std::endl;
  uint64_t startMicros = NowMicros();
  Status s;
    if(val.size()>=options_.exp_ops.mediumThreshold){
    std::string filename = "l"+std::to_string(lastVlog_);
    uint64_t startMicros = NowMicros();
    size_t offset = writingVlog_.AddRecord(key.ToString(), val.ToString());
    std::string valueInfo = conbineValueInfo(filename,offset,val.size());
    //midVlog_.AddRecord(key.ToString(),valueInfo);
    if(offset>=options_.exp_ops.logSize){
      //writingVlog_.Sync();
      writingVlog_.NextFile(vlogPathname(++lastVlog_));
      metaTable_[conbineStr({"l",std::to_string(lastVlog_)})] = VfileMeta(options_.exp_ops.logSize);
    }
    STATS::TimeAndCount(STATS::GetInstance()->writeVlogStat,startMicros,NowMicros());
    s = DB::Put(o, key, valueInfo);
    } else {
    std::string filename = "m"+std::to_string(lastMidLog_);
    uint64_t startMicros = NowMicros();
    size_t offset = midVlog_.AddRecord(key.ToString(), val.ToString());
    STATS::Time(STATS::GetInstance()->writeMidLog,startMicros,NowMicros());
    if(val.size()>=options_.exp_ops.smallThreshold){
      std::string valueInfo = conbineValueInfo(filename,offset,val.size());
      s = DB::Put(o, key, valueInfo);
    } else {
      s = DB::Put(o, key, val);
    }
  }
  STATS::TimeAndCount(STATS::GetInstance()->writeStat, startMicros, NowMicros());
  //fprintf(wl,"%lu,",NowMicros()-startMicros);
  return s;
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
  uint64_t startMicros = NowMicros();
  return DB::Delete(options, key);
  STATS::TimeAndCount(STATS::GetInstance()->writeStat,startMicros,NowMicros());
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* my_batch) {
  Writer w(&mutex_);
  w.batch = my_batch;
  w.sync = options.sync;
  w.done = false;

  MutexLock l(&mutex_);
  writers_.push_back(&w);
  while (!w.done && &w != writers_.front()) {
    w.cv.Wait();
  }
  if (w.done) {
    return w.status;
  }

  // May temporarily unlock and wait.
  Status status = MakeRoomForWrite(my_batch == nullptr);
  uint64_t last_sequence = versions_->LastSequence();
  Writer* last_writer = &w;
  if (status.ok() && my_batch != nullptr) {  // nullptr batch is for compactions
    WriteBatch* updates = BuildBatchGroup(&last_writer);
    WriteBatchInternal::SetSequence(updates, last_sequence + 1);
    last_sequence += WriteBatchInternal::Count(updates);

    // Add to log and apply to memtable.  We can release the lock
    // during this phase since &w is currently responsible for logging
    // and protects against concurrent loggers and concurrent writes
    // into mem_.
    {
      mutex_.Unlock();
      uint64_t startLog = NowMicros();
      status = log_->AddRecord(WriteBatchInternal::Contents(updates));
      bool sync_error = false;
      if (status.ok() && options.sync) {
        status = logfile_->Sync();
        if (!status.ok()) {
          sync_error = true;
        }
      }
      STATS::Time(STATS::GetInstance()->writeLog,startLog,NowMicros());
      uint64_t startMem = NowMicros();
      if (status.ok()) {
        status = WriteBatchInternal::InsertInto(updates, mem_);
      }
      STATS::Time(STATS::GetInstance()->writeMemtable,startMem,NowMicros());
      mutex_.Lock();
      if (sync_error) {
        // The state of the log file is indeterminate: the log record we
        // just added may or may not show up when the DB is re-opened.
        // So we force the DB into a mode where all future writes fail.
        RecordBackgroundError(status);
      }
    }
    if (updates == tmp_batch_) tmp_batch_->Clear();

    versions_->SetLastSequence(last_sequence);
  }

  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      ready->cv.Signal();
    }
    if (ready == last_writer) break;
  }

  // Notify new head of write queue
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }

  return status;
}

// REQUIRES: Writer list must be non-empty
// REQUIRES: First writer must have a non-null batch
WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  mutex_.AssertHeld();
  assert(!writers_.empty());
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  assert(result != nullptr);

  size_t size = WriteBatchInternal::ByteSize(first->batch);

  // Allow the group to grow up to a maximum size, but if the
  // original write is small, limit the growth so we do not slow
  // down the small write too much.
  size_t max_size = 1 << 20;
  if (size <= (128<<10)) {
    max_size = size + (128<<10);
  }

  *last_writer = first;
  std::deque<Writer*>::iterator iter = writers_.begin();
  ++iter;  // Advance past "first"
  for (; iter != writers_.end(); ++iter) {
    Writer* w = *iter;
    if (w->sync && !first->sync) {
      // Do not include a sync write into a batch handled by a non-sync write.
      break;
    }

    if (w->batch != nullptr) {
      size += WriteBatchInternal::ByteSize(w->batch);
      if (size > max_size) {
        // Do not make batch too big
        break;
      }

      // Append to *result
      if (result == first->batch) {
        // Switch to temporary batch instead of disturbing caller's batch
        result = tmp_batch_;
        assert(WriteBatchInternal::Count(result) == 0);
        WriteBatchInternal::Append(result, first->batch);
      }
      WriteBatchInternal::Append(result, w->batch);
    }
    *last_writer = w;
  }
  return result;
}

// REQUIRES: mutex_ is held
// REQUIRES: this thread is currently at the front of the writer queue
Status DBImpl::MakeRoomForWrite(bool force) {
  mutex_.AssertHeld();
  assert(!writers_.empty());
  bool allow_delay = !force;
  Status s;
  while (true) {
    if (!bg_error_.ok()) {
      // Yield previous error
      s = bg_error_;
      break;
    } else if (
        allow_delay &&
        versions_->NumLevelFiles(0) >= config::kL0_SlowdownWritesTrigger) {
      // We are getting close to hitting a hard limit on the number of
      // L0 files.  Rather than delaying a single write by several
      // seconds when we hit the hard limit, start delaying each
      // individual write by 1ms to reduce latency variance.  Also,
      // this delay hands over some CPU to the compaction thread in
      // case it is sharing the same core as the writer.
      mutex_.Unlock();
      env_->SleepForMicroseconds(1000);
      allow_delay = false;  // Do not delay a single write more than once
      mutex_.Lock();
    } else if (!force &&
               (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size)) {
//        std::cerr<<mem_->ApproximateMemoryUsage()<<" "<<options_.write_buffer_size<<std::endl;
      // There is room in current memtable
      break;
    } else if (imm_ != nullptr) {
      // We have filled up the current memtable, but the previous
      // one is still being compacted, so we wait.
      Log(options_.info_log, "Current memtable full; waiting...\n");
      uint64_t start_micro = NowMicros();
      background_work_finished_signal_.Wait();
      STATS::Time(STATS::GetInstance()->waitFlush,start_micro,NowMicros());
    } else if (versions_->NumLevelFiles(0) >= config::kL0_StopWritesTrigger) {
      // There are too many level-0 files.
      Log(options_.info_log, "Too many L0 files; waiting...\n");
      background_work_finished_signal_.Wait();
    } else {
      // Attempt to switch to a new memtable and trigger compaction of old
      assert(versions_->PrevLogNumber() == 0);
      uint64_t new_log_number = versions_->NewFileNumber();
      WritableFile* lfile = nullptr;
      s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &lfile);
      if (!s.ok()) {
        // Avoid chewing through file number space in a tight loop.
        versions_->ReuseFileNumber(new_log_number);
        break;
      }
      delete log_;
      delete logfile_;
      // next mid log
      midVlog_.NextFile(valueFilePath(conbineStr({"m",std::to_string(++lastMidLog_)})));
      logfile_ = lfile;
      logfile_number_ = new_log_number;
      log_ = new log::Writer(lfile);
      imm_ = mem_;
      has_imm_.Release_Store(imm_);
      mem_ = new MemTable(internal_comparator_);
      mem_->Ref();
      force = false;   // Do not force another compaction if have room
      MaybeScheduleCompaction();
    }
  }
  return s;
}

bool DBImpl::GetProperty(const Slice& property, std::string* value) {
  value->clear();

  MutexLock l(&mutex_);
  Slice in = property;
  Slice prefix("leveldb.");
  if (!in.starts_with(prefix)) return false;
  in.remove_prefix(prefix.size());

  if (in.starts_with("num-files-at-level")) {
    in.remove_prefix(strlen("num-files-at-level"));
    uint64_t level;
    bool ok = ConsumeDecimalNumber(&in, &level) && in.empty();
    if (!ok || level >= config::kNumLevels) {
      return false;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "%d",
               versions_->NumLevelFiles(static_cast<int>(level)));
      *value = buf;
      return true;
    }
  } else if (in == "stats") {
    printf("------------ experiment stats -----------------\n");
    STATS::GetInstance()-> printAll();
    char buf[200];
    snprintf(buf, sizeof(buf),
             "                               Compactions\n"
             "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
             "--------------------------------------------------\n"
             );
    value->append(buf);
    for (int level = 0; level < config::kNumLevels; level++) {
      int files = versions_->NumLevelFiles(level);
      if (stats_[level].micros > 0 || files > 0) {
        snprintf(
            buf, sizeof(buf),
            "%3d %8d %8.0f %9.0f %8.0f %9.0f\n",
            level,
            files,
            versions_->NumLevelBytes(level) / 1048576.0,
            stats_[level].micros / 1e6,
            stats_[level].bytes_read / 1048576.0,
            stats_[level].bytes_written / 1048576.0);
        value->append(buf);
      }
    }
    return true;
  } else if (in == "sstables") {
    *value = versions_->current()->DebugString();
    return true;
  } else if (in == "approximate-memory-usage") {
    size_t total_usage = options_.block_cache->TotalCharge();
    if (mem_) {
      total_usage += mem_->ApproximateMemoryUsage();
    }
    if (imm_) {
      total_usage += imm_->ApproximateMemoryUsage();
    }
    char buf[50];
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(total_usage));
    value->append(buf);
    return true;
  }

  return false;
}

void DBImpl::GetApproximateSizes(
    const Range* range, int n,
    uint64_t* sizes) {
  // TODO(opt): better implementation
  Version* v;
  {
    MutexLock l(&mutex_);
    versions_->current()->Ref();
    v = versions_->current();
  }

  for (int i = 0; i < n; i++) {
    // Convert user_key into a corresponding internal key.
    InternalKey k1(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey k2(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
    uint64_t start = versions_->ApproximateOffsetOf(v, k1);
    uint64_t limit = versions_->ApproximateOffsetOf(v, k2);
    sizes[i] = (limit >= start ? limit - start : 0);
  }

  {
    MutexLock l(&mutex_);
    v->Unref();
  }
}

// Default implementations of convenience methods that subclasses of DB
// can call if they wish
Status DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value) {
  WriteBatch batch;
  batch.Put(key, value);
  Status s = Write(opt, &batch);
  return s;
}

Status DB::Delete(const WriteOptions& opt, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(opt, &batch);
}

DB::~DB() { }

Status DB::Open(const Options& options, const std::string& dbname,
                DB** dbptr) {
  *dbptr = nullptr;

  DBImpl* impl = new DBImpl(options, dbname);
  impl->mutex_.Lock();
  VersionEdit edit;
  // Recover handles create_if_missing, error_if_exists
  bool save_manifest = false;
  Status s = impl->Recover(&edit, &save_manifest);

  if (s.ok() && impl->mem_ == nullptr) {
    // Create new log and a corresponding memtable.
    uint64_t new_log_number = impl->versions_->NewFileNumber();
    WritableFile* lfile;
    s = options.env->NewWritableFile(LogFileName(dbname, new_log_number),
                                     &lfile);
    if (s.ok()) {
      edit.SetLogNumber(new_log_number);
      impl->logfile_ = lfile;
      impl->logfile_number_ = new_log_number;
      impl->log_ = new log::Writer(lfile);
      impl->mem_ = new MemTable(impl->internal_comparator_);
      impl->mem_->Ref();
    }
  }
  if (s.ok() && save_manifest) {
    edit.SetPrevLogNumber(0);  // No older logs needed after recovery.
    edit.SetLogNumber(impl->logfile_number_);
    s = impl->versions_->LogAndApply(&edit, &impl->mutex_);
  }
  if (s.ok()&&!options.exp_ops.noCompaction) {
    impl->DeleteObsoleteFiles();
    impl->MaybeScheduleCompaction();
  }
  impl->mutex_.Unlock();
  if (s.ok()) {
    assert(impl->mem_ != nullptr);
    *dbptr = impl;
  } else {
    delete impl;
  }
  //impl->RecoverMeta();
  return s;
}

Snapshot::~Snapshot() {
}

Status DestroyDB(const std::string& dbname, const Options& options) {
  Env* env = options.env;
  std::vector<std::string> filenames;
  Status result = env->GetChildren(dbname, &filenames);
  if (!result.ok()) {
    // Ignore error in case directory does not exist
    return Status::OK();
  }

  FileLock* lock;
  const std::string lockname = LockFileName(dbname);
  result = env->LockFile(lockname, &lock);
  if (result.ok()) {
    uint64_t number;
    FileType type;
    for (size_t i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &type) &&
          type != kDBLockFile) {  // Lock file will be deleted at end
        Status del = env->DeleteFile(dbname + "/" + filenames[i]);
        if (result.ok() && !del.ok()) {
          result = del;
        }
      }
    }
    env->UnlockFile(lock);  // Ignore error since state is already gone
    env->DeleteFile(lockname);
    env->DeleteDir(dbname);  // Ignore error in case dir contains other files
  }
  return result;
}

/* selective */
Status DBImpl::readValueWithAddress(std::string &valueInfo) {
    Status s;
    if(valueInfo.back()!='~') // it's a real value
        return s;
    //std::cerr<<"*read value "<<valueInfo<<std::endl;
    // get filename, offset and value size
    std::string filename;
    size_t offset;
    size_t valueSize;
    parseValueInfo(valueInfo, filename, offset, valueSize);
    FILE* f = openValueFile(filename);
    valueInfo = readValue(f, offset, valueSize);
    return s;
}

std::string DBImpl::readValue(FILE* f, size_t offset, size_t size) {
    uint64_t startMicros = NowMicros();
    char value[size+1];
    size_t got = pread(fileno(f),value,size,offset);
    if(got!=size){
      STATS::Add(STATS::GetInstance()->getErrorCnt,1);
//      std::cerr<<"get value error "<< strerror(errno) <<" offset "<<offset<<" size "<<size<<" got "<<got<<std::endl;
    }
    std::string ret = std::string(value);
    STATS::Time(STATS::GetInstance()->readValueFile,startMicros,NowMicros());
    return ret;
}

void DBImpl::readaheadForScan(leveldb::BlockQueue<leveldb::ValueLoc> &locs) {
    while(1){
        ValueLoc vl = locs.Get();
        if(!vl.f){
            return;
        }
        if(ftell(vl.f)>vl.offset)
        readahead(fileno(vl.f),vl.offset,vl.size);
    }
}

void DBImpl::readValueForScan(std::vector<std::string> &values, leveldb::BlockQueue<ValueLoc> &locs) {
    while(1){
        ValueLoc vl = locs.Get();
        if(!vl.f) {
            return;
        }
	//std::cerr<<"read\n";
        values.emplace_back(readValue(vl.f,vl.offset,vl.size));
	//std::cerr<<"read done\n";
    }
}

FILE* DBImpl::openValueFile(const std::string &filename) {
  uint64_t startMicro = NowMicros();
  fileMutex_.Lock();
  FILE* f = openedFiles_[filename];
  if(!f){
    f = fopen(valueFilePath(filename).c_str(), "a+");
    openedFiles_[filename] = f;
  }
  fileMutex_.Unlock();
//  std::cerr<<"open "<<filename<<std::endl;
  STATS::Time(STATS::GetInstance()->openFileTime, startMicro, NowMicros());
  return f;
}

void DBImpl::closeValueFile(const std::string &filename) {
  fileMutex_.Lock();
  FILE* f = openedFiles_[filename];
  if(f) {
    fclose(f);
    openedFiles_.erase(filename);
  }
  fileMutex_.Unlock();
}

/*
    Status DBImpl::Scan(const leveldb::ReadOptions &options, const std::string &start, const std::string &end,
                        std::vector<std::string> &keys, std::vector<std::string> &values, size_t num) {
        //std::cerr<<"scan\n";
        auto iter = NewIterator(options);
        iter->Seek(start);
        std::vector<std::future<std::string>> retValues;
        std::vector<std::string> valueInfos;
        size_t cnt = 0;
        uint64_t iterStart = NowMicros();
        std::unordered_map<std::string, size_t> fileReadSize;
        while(iter->Valid()&&cnt<num){
            //std::cerr<<"iter\n";
            keys.push_back(iter->key().ToString());
            std::string valueInfo = iter->value().ToString();
            std::string filename;
            size_t offset, size;
            if(valueInfo.back()=='~') {
                parseValueInfo(valueInfo, filename, offset, size);
                FILE* f = openValueFile(filename);
                uint64_t startAdvice = NowMicros();
                posix_fadvise(fileno(f),offset,size,POSIX_FADV_WILLNEED);
                STATS::Time(STATS::GetInstance()->fadviceTime, startAdvice, NowMicros());
                if (filename[0] == 't') {
                    if (fileReadSize[filename]) fileReadSize[filename] += size;
                    else {
                        fileReadSize[filename] = size;
                    }
                }
                values.emplace_back(readValue(f,offset,size));
            } else {
                // don't read real value for now
                // TODO: support it
            }
            cnt++;
            iter->Next();
        }
        STATS::Time(STATS::GetInstance()->scanVlogIter, iterStart, NowMicros());
        delete(iter);
        return Status();
    }
    */

Status DBImpl::Scan(const leveldb::ReadOptions &options, const std::string &start, const std::string &end,
                    std::vector<std::string> &keys, std::vector<std::string> &values, size_t num) {
    uint64_t startScan = NowMicros();
    auto iter = NewIterator(options);
    iter->Seek(start);
    std::future<void> readDone;
    std::vector<std::string> valueInfos;
    size_t cnt = 0;

    uint64_t iterStart = NowMicros();
    //auto fileReadSize = std::make_shared<std::unordered_map<std::string, size_t>>();
    BlockQueue<ValueLoc> bq;
    //BlockQueue<ValueLoc> bq2;
    readDone = threadPool_->addTask(&DBImpl::readValueForScan,this,std::ref(values),std::ref(bq));
    //threadPool_->addTask(&DBImpl::readaheadForScan,this,std::ref(bq2));
    while(iter->Valid()&&cnt<num){
        keys.push_back(iter->key().ToString());
        std::string valueInfo = iter->value().ToString();
        std::string filename;
        size_t offset, size;

        if(valueInfo.back()=='~') {    // value location
          parseValueInfo(valueInfo, filename, offset, size);
          FILE* f = openValueFile(filename);
          if(!f) std::cerr<<"error\n";
          uint64_t startAdvice = NowMicros();
//          threadPool_->addTask(readahead,fileno(f),offset,size);
          readahead(fileno(f),offset,size);
//          threadPool_->addTask(readahead,fileno(f),offset,size);
          ValueLoc vl(f,offset,size);
          bq.Put(vl);
          //bq2.Put(vl);
          STATS::Time(STATS::GetInstance()->fadviceTime, startAdvice, NowMicros());
          /*
          if (filename[0] == 't') {
            if ((*fileReadSize)[filename]) (*fileReadSize)[filename] += size;
            else {
              (*fileReadSize)[filename] = size;
            }
          }
           */
        } else {
          // don't read real value for now
          // TODO: support it
        }
        cnt++;
        iter->Next();
    }
    bq.Put(ValueLoc(nullptr,0,0));
    //bq2.Put(ValueLoc(nullptr,0,0));
    STATS::Time(STATS::GetInstance()->scanVlogIter, iterStart, NowMicros());
    delete(iter);
    uint64_t wait = NowMicros();
    std::cerr<<"wait\n";
    readDone.wait();
    std::cerr<<"wait donw\n";
    STATS::Time(STATS::GetInstance()->waitScanThreadsFinish, wait, NowMicros());
    STATS::TimeAndCount(STATS::GetInstance()->scanVlogStat, startScan, NowMicros());
    //threadPool_->addTask(&DBImpl::mayScheduleMerge,this,fileReadSize);
    return Status();
}

/*
void DBImpl::mayScheduleMerge(std::shared_ptr<std::unordered_map<std::string, size_t>> fileReadSize){
    if(fileReadSize->size()==0) return;
    size_t sum = 0;
    size_t canMerge = 0;
    std::vector<std::string> toSchedule;
    for(const auto& item:*fileReadSize){
        sum+=item.second;
        if(item.second<3000) {
          toSchedule.push_back(item.first);
          canMerge+=item.second;
        }
    }
    if(sum/fileReadSize->size()<4096&&canMerge>10000){
        for(const auto file:toSchedule){
            toMerge_.Put(file);
        }
    }
}


void DBImpl::scheduleMerge(){
    auto toSchedule = std::make_shared<std::unordered_set<std::string>>();
    auto merging = std::make_shared<std::unordered_set<std::string>>();
    while(1){
        std::string filename = toMerge_.Get();
        if(merging->find(filename)==merging->end()) toSchedule->insert(filename);
        if(toSchedule->size()>50&&merging->empty()) {
            std::cerr<<"schedule merge\n";
            merging = toSchedule;
            toSchedule = std::make_shared<std::unordered_set<std::string>>();
            threadPool_->addTask(&DBImpl::mergeVtables,this,std::ref(merging));
        }
    }
}
 */

void DBImpl::scheduleGC() {
    auto toGC = std::make_shared<std::unordered_set<std::string>>();
    auto inGC = std::make_shared<std::unordered_set<std::string>>();
    std::future<void> ret;
    while(1){
        std::string filename = toGC_.Get();
        if(filename==""){
          ret.wait();
          break;
        }
        if(inGC->find(filename)==inGC->end()) toGC->insert(filename);
        if(toGC->size()>options_.exp_ops.numFileGC&&inGC->empty()){
            inGC = toGC;
            toGC = std::make_shared<std::unordered_set<std::string>>();
            ret = threadPool_->addTask(&DBImpl::GarbageCollect,this,inGC);
        }
    }
}

Status DBImpl::RecoverMeta() {
    std::string lastvtable;
    Get(ReadOptions(),"lastvtable",&lastvtable);
    if(lastvtable!="") {
      std::cerr<<"last vtable:"<<lastvtable<<std::endl;
      lastVtable_ = std::stoul(lastvtable);
    }
    return Status();
};


void DBImpl::GarbageCollect(std::shared_ptr<std::unordered_set<std::string>> inGC) {
    std::cerr<<"start gc "<<inGC->size()<<std::endl;
    // TODO: optimize, use new filename
    for(std::string filename:(*inGC)){
      uint64_t startMicros = NowMicros();
      uint64_t gcWriteBack = 0;
      uint64_t gcSize = 0;
//      std::cerr<<"gc "<<filename<<std::endl;
      gcSize+=options_.exp_ops.logSize;

      std::string gcfile = conbineStr({"g",std::to_string(lastGCFile_.load())});
//      std::cerr<<"filename "<<filename<<" gc filename "<<gcfile<<std::endl;
      FILE* f = openValueFile(gcfile);
      fseek(f,0,SEEK_SET);

      std::string filepath = valueFilePath(filename);
        if(access(filepath.c_str(),F_OK)!=0){
//            std::cerr<<"has been gced\n";
            //metaTable_.erase(filename);
//            std::cerr<<"continue\n";
            continue;
        }
      Iterator* iter = new ValueIterator(filepath,this);
      iter->SeekToFirst();
      while(iter->Valid()){
        size_t ksize = iter->key().size();
        size_t vsize = iter->value().size();
        //std::cerr<<"gc write back "<<vsize<<std::endl;
        int64_t startWriteValue = NowMicros();
        fwrite(conbineKVPair(iter->key().ToString(),iter->value().ToString()).c_str(),ksize+vsize+2,1,f);
        STATS::Time(STATS::GetInstance()->gcWriteValue,startWriteValue,NowMicros());
        size_t offset = ftell(f)-vsize-1;
        uint64_t startWriteLSM = NowMicros();
        Put(leveldb::WriteOptions(),iter->key(),conbineValueInfo(gcfile,offset,vsize));
        STATS::Time(STATS::GetInstance()->gcWriteLSM,startWriteLSM,NowMicros());
        if(offset>=options_.exp_ops.logSize) { // next gc file
//          fsync(fileno(f));
          closeValueFile(gcfile);
          lastGCFile_+=1;
          gcfile = conbineStr({"g",std::to_string(lastGCFile_.load())});
          f = openValueFile(gcfile);
        }
        iter->Next();
        gcWriteBack+=vsize+ksize;
      }
      delete iter;
      closeValueFile(filename);
      deleteFile(filename);
      //metaTable_.erase(filename);
//      std::cerr<<"gc done, gc "<<gcSize<<" write back "<<gcWriteBack<<std::endl;
      STATS::Add(STATS::GetInstance()->gcWritebackBytes,gcWriteBack);
      STATS::Add(STATS::GetInstance()->gcSize,gcSize);
      STATS::Time(STATS::GetInstance()->gcTime,startMicros,NowMicros());
    }
    inGC->clear();
}

std::string DBImpl::valueFilePath(const std::string &filename) {
    return conbineStr({dbname_,"/values/",filename});
}

std::string DBImpl::vlogPathname(size_t filenum) {
  return valueFilePath(conbineStr({"l",std::to_string(filenum)}));
}

/*
size_t DBImpl::writeVlog(const std::string &key, const std::string &value) {
    uint64_t startMicros = NowMicros();
    //fwrite((conbineKVPair(key,value)).c_str(),key.size()+value.size()+2,1,writingVlog_);
    write(fileno(writingVlog_),(conbineKVPair(key,value)).data(),key.size()+value.size()+2);
    size_t offset = ftell(writingVlog_);
    if(offset>=options_.exp_ops.logSize){
      fsync(fileno(writingVlog_));
      STATS::Add(STATS::GetInstance()->vlogWriteDisk,ftell(writingVlog_));
      lastVlog_+=1;
      fclose(writingVlog_);
      writingVlog_ = fopen(vlogPathname(lastVlog_).c_str(),"a+");
      metaTable_[conbineStr({"l",std::to_string(lastVlog_)})] = VfileMeta(options_.exp_ops.logSize);
    }
    STATS::TimeAndCount(STATS::GetInstance()->writeVlogStat,startMicros,NowMicros());
    return offset-value.size()-1;
}
 */

Status DBImpl::deleteFile(const std::string &filename){
//      std::cerr<<"delete "<<filename<<std::endl;
      return remove(valueFilePath(filename).c_str()) == 0 ? Status() : Status().IOError("delete vlog error\n");
    }

Status DBImpl::mergeVtables(std::shared_ptr<std::unordered_set<std::string>> inMerge) {
    std::cerr<<"start merge "<<inMerge->size()<<" files\n";
    std::vector<Iterator*> iters;
    for(const auto& file:*inMerge){
        iters.push_back(new ValueIterator(valueFilePath(file),this));
    }
    std::cerr<<std::endl;
    Iterator* iter = NewMergingIterator(options_.comparator,&iters[0],iters.size());

    std::string vtablename = conbineStr({"t",std::to_string(nextVtable())});
    VtableBuilder* builder = new VtableBuilder(valueFilePath(vtablename));
    iter->SeekToFirst();
    while(iter->Valid()){
      Slice key = iter->key();
      Slice value = iter->value();
      size_t offset = builder->Add(key,value);
      std::string valueInfo = conbineValueInfo(vtablename,offset,value.size());
      Put(leveldb::WriteOptions(),key,valueInfo);
      if(offset>options_.exp_ops.tableSize){
        builder->Finish();
        vtablename = conbineStr({"t",std::to_string(nextVtable())});
        builder->NextFile(valueFilePath(vtablename));
      }
      iter->Next();
    }
  if(!builder->Finished()) builder->Finish();
  delete builder;
  std::cerr<<"merge done\n";
  for(const auto& file:*inMerge) deleteFile(file);
  delete iter;
  //todo remove these
  std::string stats;
  GetProperty("leveldb.stats",&stats);
  std::cout<<stats<<std::endl;
  inMerge->clear();

  return Status();
}

void DBImpl::updateMeta(const std::string &filename, int invalid) {
  auto it = metaTable_.find(filename);
  if(it!=metaTable_.end()) {
    auto& item = it->second;
    invalid += item.invalidKV;
//    if(toMerge_.find(filename)!=toMerge_.end()) {
//      std::cerr << filename << " invalid " << invalid << "/" << item.valid << std::endl;
//    }
    if (invalid == item.valid) {
      deleteFile(filename);
      metaTable_.erase(it);
      if(doMerge_){
        mergeGCCnt+=toMerge_.erase(filename);
//        std::cerr<<"deleted "<<mergeGCCnt<<"vtables during merge\n";
        if(toMerge_.size()==0) doMerge_ = false;
      }
    } else {
      item.invalidKV = invalid;
//      std::cerr<<filename<<" "<<(double) invalid / item.valid << std::endl;
      if (filename[0]>'a'+options_.exp_ops.gcLevel && (double) invalid / item.valid > options_.exp_ops.gcRatio) {
        //TODO: reliability
        if(filename[0]<'g'){
          toMerge_.insert(filename);
//          std::cerr<<"to merge "<<filename<<", num:"<<toMerge_.size()<<std::endl;
          if(toMerge_.size()>=50) {
//            std::cerr<<"merge "<<toMerge_.size()<<"vtables\n";
            doMerge_ = true;
//            std::swap(toMerge_,inMerge_);
          }
          return;
        }
        metaTable_.erase(filename);
        toGC_.Put(filename);
      }
    }
  }
}

}  // namespace leveldb
