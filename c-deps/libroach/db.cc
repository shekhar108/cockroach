// Copyright 2014 The Cockroach Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied.  See the License for the specific language governing
// permissions and limitations under the License.

#include "db.h"
#include <algorithm>
#include <chrono>
#include <google/protobuf/stubs/stringprintf.h>
#include <mutex>
#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/ldb_tool.h>
#include <rocksdb/merge_operator.h>
#include <rocksdb/options.h>
#include <rocksdb/perf_context.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/sst_file_writer.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/write_batch_with_index.h>
#include "encoding.h"
#include "env_switching.h"
#include "eventlistener.h"
#include "fmt.h"
#include "keys.h"
#include "protos/roachpb/data.pb.h"
#include "protos/roachpb/internal.pb.h"
#include "protos/storage/engine/enginepb/mvcc.pb.h"
#include "protos/storage/engine/enginepb/rocksdb.pb.h"

const DBStatus kSuccess = {NULL, 0};

extern "C" {
static void __attribute__((noreturn)) die_missing_symbol(const char* name) {
  fprintf(stderr, "%s symbol missing; expected to be supplied by Go\n", name);
  abort();
}

// These are Go functions exported by storage/engine. We provide these stubs,
// which simply panic if called, to to allow intermediate build products to link
// successfully. Otherwise, when building ccl/storageccl/engineccl, Go will
// complain that these symbols are undefined. Because these stubs are marked
// "weak", they will be replaced by their proper implementation in
// storage/engine when the final cockroach binary is linked.
void __attribute__((weak)) rocksDBLog(char*, int) { die_missing_symbol(__func__); }
char* __attribute__((weak)) prettyPrintKey(DBKey) { die_missing_symbol(__func__); }
}  // extern "C"

#if defined(COMPILER_GCC) || defined(__clang__)
#define WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#else
#define WARN_UNUSED_RESULT
#endif

// DBOpenHook in OSS mode only verifies that no extra options are specified.
__attribute__((weak)) rocksdb::Status DBOpenHook(const std::string& db_dir, const DBOptions opts) {
  if (opts.extra_options.len != 0) {
    return rocksdb::Status::InvalidArgument(
        "DBOptions has extra_options, but OSS code cannot handle them");
  }
  return rocksdb::Status::OK();
}

struct DBCache {
  std::mutex mu;
  std::shared_ptr<rocksdb::Cache> rep;
};

struct DBEngine {
  rocksdb::DB* const rep;

  DBEngine(rocksdb::DB* r) : rep(r) {}
  virtual ~DBEngine() {}

  virtual DBStatus Put(DBKey key, DBSlice value) = 0;
  virtual DBStatus Merge(DBKey key, DBSlice value) = 0;
  virtual DBStatus Delete(DBKey key) = 0;
  virtual DBStatus DeleteRange(DBKey start, DBKey end) = 0;
  virtual DBStatus CommitBatch(bool sync) = 0;
  virtual DBStatus ApplyBatchRepr(DBSlice repr, bool sync) = 0;
  virtual DBSlice BatchRepr() = 0;
  virtual DBStatus Get(DBKey key, DBString* value) = 0;
  virtual DBIterator* NewIter(rocksdb::ReadOptions*) = 0;
  virtual DBStatus GetStats(DBStatsResult* stats) = 0;
  virtual DBString GetCompactionStats() = 0;
  virtual DBStatus EnvWriteFile(DBSlice path, DBSlice contents) = 0;

  DBSSTable* GetSSTables(int* n);
  DBString GetUserProperties();
};

struct DBImpl : public DBEngine {
  std::unique_ptr<rocksdb::Env> switching_env;
  std::unique_ptr<rocksdb::Env> memenv;
  std::unique_ptr<rocksdb::DB> rep_deleter;
  std::shared_ptr<rocksdb::Cache> block_cache;
  std::shared_ptr<DBEventListener> event_listener;

  // Construct a new DBImpl from the specified DB.
  // The DB and passed Envs will be deleted when the DBImpl is deleted.
  // Either env can be NULL.
  DBImpl(rocksdb::DB* r, rocksdb::Env* m, std::shared_ptr<rocksdb::Cache> bc,
         std::shared_ptr<DBEventListener> event_listener, rocksdb::Env* s_env)
      : DBEngine(r),
        switching_env(s_env),
        memenv(m),
        rep_deleter(r),
        block_cache(bc),
        event_listener(event_listener) {}
  virtual ~DBImpl() {
    const rocksdb::Options& opts = rep->GetOptions();
    const std::shared_ptr<rocksdb::Statistics>& s = opts.statistics;
    rocksdb::Info(opts.info_log, "bloom filter utility:    %0.1f%%",
                  (100.0 * s->getTickerCount(rocksdb::BLOOM_FILTER_PREFIX_USEFUL)) /
                      s->getTickerCount(rocksdb::BLOOM_FILTER_PREFIX_CHECKED));
  }

  virtual DBStatus Put(DBKey key, DBSlice value);
  virtual DBStatus Merge(DBKey key, DBSlice value);
  virtual DBStatus Delete(DBKey key);
  virtual DBStatus DeleteRange(DBKey start, DBKey end);
  virtual DBStatus CommitBatch(bool sync);
  virtual DBStatus ApplyBatchRepr(DBSlice repr, bool sync);
  virtual DBSlice BatchRepr();
  virtual DBStatus Get(DBKey key, DBString* value);
  virtual DBIterator* NewIter(rocksdb::ReadOptions*);
  virtual DBStatus GetStats(DBStatsResult* stats);
  virtual DBString GetCompactionStats();
  virtual DBStatus EnvWriteFile(DBSlice path, DBSlice contents);
};

struct DBBatch : public DBEngine {
  int updates;
  bool has_delete_range;
  rocksdb::WriteBatchWithIndex batch;

  DBBatch(DBEngine* db);
  virtual ~DBBatch() {}

  virtual DBStatus Put(DBKey key, DBSlice value);
  virtual DBStatus Merge(DBKey key, DBSlice value);
  virtual DBStatus Delete(DBKey key);
  virtual DBStatus DeleteRange(DBKey start, DBKey end);
  virtual DBStatus CommitBatch(bool sync);
  virtual DBStatus ApplyBatchRepr(DBSlice repr, bool sync);
  virtual DBSlice BatchRepr();
  virtual DBStatus Get(DBKey key, DBString* value);
  virtual DBIterator* NewIter(rocksdb::ReadOptions*);
  virtual DBStatus GetStats(DBStatsResult* stats);
  virtual DBString GetCompactionStats();
  virtual DBStatus EnvWriteFile(DBSlice path, DBSlice contents);
};

struct DBWriteOnlyBatch : public DBEngine {
  int updates;
  rocksdb::WriteBatch batch;

  DBWriteOnlyBatch(DBEngine* db);
  virtual ~DBWriteOnlyBatch() {}

  virtual DBStatus Put(DBKey key, DBSlice value);
  virtual DBStatus Merge(DBKey key, DBSlice value);
  virtual DBStatus Delete(DBKey key);
  virtual DBStatus DeleteRange(DBKey start, DBKey end);
  virtual DBStatus CommitBatch(bool sync);
  virtual DBStatus ApplyBatchRepr(DBSlice repr, bool sync);
  virtual DBSlice BatchRepr();
  virtual DBStatus Get(DBKey key, DBString* value);
  virtual DBIterator* NewIter(rocksdb::ReadOptions*);
  virtual DBStatus GetStats(DBStatsResult* stats);
  virtual DBString GetCompactionStats();
  virtual DBStatus EnvWriteFile(DBSlice path, DBSlice contents);
};

struct DBSnapshot : public DBEngine {
  const rocksdb::Snapshot* snapshot;

  DBSnapshot(DBEngine* db) : DBEngine(db->rep), snapshot(db->rep->GetSnapshot()) {}
  virtual ~DBSnapshot() { rep->ReleaseSnapshot(snapshot); }

  virtual DBStatus Put(DBKey key, DBSlice value);
  virtual DBStatus Merge(DBKey key, DBSlice value);
  virtual DBStatus Delete(DBKey key);
  virtual DBStatus DeleteRange(DBKey start, DBKey end);
  virtual DBStatus CommitBatch(bool sync);
  virtual DBStatus ApplyBatchRepr(DBSlice repr, bool sync);
  virtual DBSlice BatchRepr();
  virtual DBStatus Get(DBKey key, DBString* value);
  virtual DBIterator* NewIter(rocksdb::ReadOptions*);
  virtual DBStatus GetStats(DBStatsResult* stats);
  virtual DBString GetCompactionStats();
  virtual DBStatus EnvWriteFile(DBSlice path, DBSlice contents);
};

struct DBIterator {
  std::unique_ptr<rocksdb::Iterator> rep;
  std::unique_ptr<rocksdb::WriteBatch> kvs;
  std::unique_ptr<rocksdb::WriteBatch> intents;
};

std::string ToString(DBSlice s) { return std::string(s.data, s.len); }

std::string ToString(DBString s) { return std::string(s.data, s.len); }

rocksdb::Slice ToSlice(DBSlice s) { return rocksdb::Slice(s.data, s.len); }

rocksdb::Slice ToSlice(DBString s) { return rocksdb::Slice(s.data, s.len); }

const DBTimestamp kZeroTimestamp = {0, 0};

DBTimestamp ToDBTimestamp(const cockroach::util::hlc::LegacyTimestamp& timestamp) {
  return DBTimestamp{timestamp.wall_time(), timestamp.logical()};
}

DBTimestamp PrevTimestamp(DBTimestamp ts) {
  if (ts.logical > 0) {
    --ts.logical;
  } else if (ts.wall_time == 0) {
    fprintf(stderr, "no previous time for zero timestamp\n");
    abort();
  } else {
    --ts.wall_time;
    ts.logical = std::numeric_limits<int32_t>::max();
  }
  return ts;
}

inline bool operator==(const DBTimestamp& a, const DBTimestamp& b) {
  return a.wall_time == b.wall_time && a.logical == b.logical;
}

inline bool operator!=(const DBTimestamp& a, const DBTimestamp& b) { return !(a == b); }

inline bool operator<(const DBTimestamp& a, const DBTimestamp& b) {
  return a.wall_time < b.wall_time || (a.wall_time == b.wall_time && a.logical < b.logical);
}

inline bool operator>(const DBTimestamp& a, const DBTimestamp& b) { return b < a; }

inline bool operator<=(const DBTimestamp& a, const DBTimestamp& b) { return !(b < a); }

inline bool operator>=(const DBTimestamp& a, const DBTimestamp& b) { return b <= a; }

const int kMVCCVersionTimestampSize = 12;

void EncodeTimestamp(std::string& s, int64_t wall_time, int32_t logical) {
  EncodeUint64(&s, uint64_t(wall_time));
  if (logical != 0) {
    EncodeUint32(&s, uint32_t(logical));
  }
}

std::string EncodeTimestamp(DBTimestamp ts) {
  std::string s;
  s.reserve(kMVCCVersionTimestampSize);
  EncodeTimestamp(s, ts.wall_time, ts.logical);
  return s;
}

// MVCC keys are encoded as <key>[<wall_time>[<logical>]]<#timestamp-bytes>. A
// custom RocksDB comparator (DBComparator) is used to maintain the desired
// ordering as these keys do not sort lexicographically correctly.
std::string EncodeKey(const rocksdb::Slice& key, int64_t wall_time, int32_t logical) {
  std::string s;
  const bool ts = wall_time != 0 || logical != 0;
  s.reserve(key.size() + 1 + (ts ? 1 + kMVCCVersionTimestampSize : 0));
  s.append(key.data(), key.size());
  if (ts) {
    // Add a NUL prefix to the timestamp data. See DBPrefixExtractor.Transform
    // for more details.
    s.push_back(0);
    EncodeTimestamp(s, wall_time, logical);
  }
  s.push_back(char(s.size() - key.size()));
  return s;
}

// MVCC keys are encoded as <key>[<wall_time>[<logical>]]<#timestamp-bytes>. A
// custom RocksDB comparator (DBComparator) is used to maintain the desired
// ordering as these keys do not sort lexicographically correctly.
std::string EncodeKey(DBKey k) { return EncodeKey(ToSlice(k.key), k.wall_time, k.logical); }

WARN_UNUSED_RESULT bool SplitKey(rocksdb::Slice buf, rocksdb::Slice* key,
                                 rocksdb::Slice* timestamp) {
  if (buf.empty()) {
    return false;
  }
  const char ts_size = buf[buf.size() - 1];
  if (ts_size >= buf.size()) {
    return false;
  }
  *key = rocksdb::Slice(buf.data(), buf.size() - ts_size - 1);
  *timestamp = rocksdb::Slice(key->data() + key->size(), ts_size);
  return true;
}

WARN_UNUSED_RESULT bool DecodeTimestamp(rocksdb::Slice* timestamp, int64_t* wall_time,
                                        int32_t* logical) {
  uint64_t w;
  if (!DecodeUint64(timestamp, &w)) {
    return false;
  }
  *wall_time = int64_t(w);
  *logical = 0;
  if (timestamp->size() > 0) {
    // TODO(peter): Use varint decoding here.
    uint32_t l;
    if (!DecodeUint32(timestamp, &l)) {
      return false;
    }
    *logical = int32_t(l);
  }
  return true;
}

WARN_UNUSED_RESULT bool DecodeHLCTimestamp(rocksdb::Slice buf,
                                           cockroach::util::hlc::Timestamp* timestamp) {
  int64_t wall_time;
  int32_t logical;
  if (!DecodeTimestamp(&buf, &wall_time, &logical)) {
    return false;
  }
  timestamp->set_wall_time(wall_time);
  timestamp->set_logical(logical);
  return true;
}

WARN_UNUSED_RESULT inline bool DecodeKey(rocksdb::Slice buf, rocksdb::Slice* key,
                                         int64_t* wall_time, int32_t* logical) {
  key->clear();

  rocksdb::Slice timestamp;
  if (!SplitKey(buf, key, &timestamp)) {
    return false;
  }
  if (timestamp.size() > 0) {
    timestamp.remove_prefix(1);  // The NUL prefix.
    if (!DecodeTimestamp(&timestamp, wall_time, logical)) {
      return false;
    }
  }
  return timestamp.empty();
}

WARN_UNUSED_RESULT bool inline DecodeKey(rocksdb::Slice buf, rocksdb::Slice* key, DBTimestamp* ts) {
  return DecodeKey(buf, key, &ts->wall_time, &ts->logical);
}

rocksdb::Slice KeyPrefix(const rocksdb::Slice& src) {
  rocksdb::Slice key;
  rocksdb::Slice ts;
  if (!SplitKey(src, &key, &ts)) {
    return src;
  }
  // RocksDB requires that keys generated via Transform be comparable with
  // normal encoded MVCC keys. Encoded MVCC keys have a suffix indicating the
  // number of bytes of timestamp data. MVCC keys without a timestamp have a
  // suffix of 0. We're careful in EncodeKey to make sure that the user-key
  // always has a trailing 0. If there is no timestamp this falls out
  // naturally. If there is a timestamp we prepend a 0 to the encoded
  // timestamp data.
  assert(src.size() > key.size() && src[key.size()] == 0);
  return rocksdb::Slice(key.data(), key.size() + 1);
}

DBSlice ToDBSlice(const rocksdb::Slice& s) {
  DBSlice result;
  result.data = const_cast<char*>(s.data());
  result.len = s.size();
  return result;
}

DBSlice ToDBSlice(const DBString& s) {
  DBSlice result;
  result.data = s.data;
  result.len = s.len;
  return result;
}

DBString ToDBString(const rocksdb::Slice& s) {
  DBString result;
  result.len = s.size();
  result.data = static_cast<char*>(malloc(result.len));
  memcpy(result.data, s.data(), s.size());
  return result;
}

DBKey ToDBKey(const rocksdb::Slice& s) {
  DBKey key;
  memset(&key, 0, sizeof(key));
  rocksdb::Slice tmp;
  if (DecodeKey(s, &tmp, &key.wall_time, &key.logical)) {
    key.key = ToDBSlice(tmp);
  }
  return key;
}

DBStatus ToDBStatus(const rocksdb::Status& status) {
  if (status.ok()) {
    return kSuccess;
  }
  return ToDBString(status.ToString());
}

DBStatus FmtStatus(const char* fmt_str, ...) {
  va_list ap;
  va_start(ap, fmt_str);
  std::string str;
  fmt::StringAppendV(&str, fmt_str, ap);
  va_end(ap);
  return ToDBString(str);
}

namespace {

DBIterState DBIterGetState(DBIterator* iter) {
  DBIterState state = {};
  state.valid = iter->rep->Valid();
  state.status = ToDBStatus(iter->rep->status());

  if (state.valid) {
    rocksdb::Slice key;
    state.valid = DecodeKey(iter->rep->key(), &key, &state.key.wall_time, &state.key.logical);
    if (state.valid) {
      state.key.key = ToDBSlice(key);
      state.value = ToDBSlice(iter->rep->value());
    }
  }
  return state;
}

const int kChecksumSize = 4;
const int kTagPos = kChecksumSize;
const int kHeaderSize = kTagPos + 1;

rocksdb::Slice ValueDataBytes(const std::string& val) {
  if (val.size() < kHeaderSize) {
    return rocksdb::Slice();
  }
  return rocksdb::Slice(val.data() + kHeaderSize, val.size() - kHeaderSize);
}

cockroach::roachpb::ValueType GetTag(const std::string& val) {
  if (val.size() < kHeaderSize) {
    return cockroach::roachpb::UNKNOWN;
  }
  return cockroach::roachpb::ValueType(val[kTagPos]);
}

void SetTag(std::string* val, cockroach::roachpb::ValueType tag) { (*val)[kTagPos] = tag; }

WARN_UNUSED_RESULT bool ParseProtoFromValue(const std::string& val,
                                            google::protobuf::MessageLite* msg) {
  if (val.size() < kHeaderSize) {
    return false;
  }
  const rocksdb::Slice d = ValueDataBytes(val);
  return msg->ParseFromArray(d.data(), d.size());
}

void SerializeProtoToValue(std::string* val, const google::protobuf::MessageLite& msg) {
  val->resize(kHeaderSize);
  std::fill(val->begin(), val->end(), 0);
  SetTag(val, cockroach::roachpb::BYTES);
  msg.AppendToString(val);
}

bool IsValidSplitKey(const rocksdb::Slice& key, bool allow_meta2_splits) {
  if (key == kMeta2KeyMax) {
    // We do not allow splits at Meta2KeyMax. The reason for this is that the
    // last range is the keyspace will always end at KeyMax, which will be
    // stored at Meta2KeyMax because RangeMetaKey(KeyMax) = Meta2KeyMax. If we
    // allowed splits at this key then the last descriptor would be stored on a
    // non-meta range since the meta ranges would span from [KeyMin,Meta2KeyMax)
    // and the first non-meta range would span [Meta2KeyMax,...).
    return false;
  }
  const auto& no_split_spans =
      allow_meta2_splits ? kSortedNoSplitSpans : kSortedNoSplitSpansWithoutMeta2Splits;
  for (auto span : no_split_spans) {
    // kSortedNoSplitSpans and kSortedNoSplitSpansWithoutMeta2Splits are
    // both reverse sorted (largest to smallest) on the span end key which
    // allows us to early exit if our key to check is above the end of the
    // last no-split span.
    if (key.compare(span.second) >= 0) {
      return true;
    }
    if (key.compare(span.first) > 0) {
      return false;
    }
  }
  return true;
}

class DBComparator : public rocksdb::Comparator {
 public:
  DBComparator() {}

  virtual const char* Name() const override { return "cockroach_comparator"; }

  virtual int Compare(const rocksdb::Slice& a, const rocksdb::Slice& b) const override {
    rocksdb::Slice key_a, key_b;
    rocksdb::Slice ts_a, ts_b;
    if (!SplitKey(a, &key_a, &ts_a) || !SplitKey(b, &key_b, &ts_b)) {
      // This should never happen unless there is some sort of corruption of
      // the keys.
      return a.compare(b);
    }

    const int c = key_a.compare(key_b);
    if (c != 0) {
      return c;
    }
    if (ts_a.empty()) {
      if (ts_b.empty()) {
        return 0;
      }
      return -1;
    } else if (ts_b.empty()) {
      return +1;
    }
    return ts_b.compare(ts_a);
  }

  virtual bool Equal(const rocksdb::Slice& a, const rocksdb::Slice& b) const override {
    return a == b;
  }

  // The RocksDB docs say it is safe to leave these two methods unimplemented.
  virtual void FindShortestSeparator(std::string* start,
                                     const rocksdb::Slice& limit) const override {}

  virtual void FindShortSuccessor(std::string* key) const override {}
};

const DBComparator kComparator;

class DBPrefixExtractor : public rocksdb::SliceTransform {
 public:
  DBPrefixExtractor() {}

  virtual const char* Name() const { return "cockroach_prefix_extractor"; }

  // MVCC keys are encoded as <user-key>/<timestamp>. Extract the <user-key>
  // prefix which will allow for more efficient iteration over the keys
  // matching a particular <user-key>. Specifically, the <user-key> will be
  // added to the per table bloom filters and will be used to skip tables
  // which do not contain the <user-key>.
  virtual rocksdb::Slice Transform(const rocksdb::Slice& src) const { return KeyPrefix(src); }

  virtual bool InDomain(const rocksdb::Slice& src) const { return true; }

  virtual bool InRange(const rocksdb::Slice& dst) const { return Transform(dst) == dst; }
};

class DBBatchInserter : public rocksdb::WriteBatch::Handler {
 public:
  DBBatchInserter(rocksdb::WriteBatchBase* batch) : batch_(batch) {}

  virtual void Put(const rocksdb::Slice& key, const rocksdb::Slice& value) {
    batch_->Put(key, value);
  }
  virtual void Delete(const rocksdb::Slice& key) { batch_->Delete(key); }
  virtual void Merge(const rocksdb::Slice& key, const rocksdb::Slice& value) {
    batch_->Merge(key, value);
  }
  virtual rocksdb::Status DeleteRangeCF(uint32_t column_family_id, const rocksdb::Slice& begin_key,
                                        const rocksdb::Slice& end_key) {
    if (column_family_id == 0) {
      batch_->DeleteRange(begin_key, end_key);
      return rocksdb::Status::OK();
    }
    return rocksdb::Status::InvalidArgument("DeleteRangeCF not implemented");
  }

 private:
  rocksdb::WriteBatchBase* const batch_;
};

// Method used to sort InternalTimeSeriesSamples.
bool TimeSeriesSampleOrdering(const cockroach::roachpb::InternalTimeSeriesSample* a,
                              const cockroach::roachpb::InternalTimeSeriesSample* b) {
  return a->offset() < b->offset();
}

// IsTimeSeriesData returns true if the given protobuffer Value contains a
// TimeSeriesData message.
bool IsTimeSeriesData(const std::string& val) {
  return GetTag(val) == cockroach::roachpb::TIMESERIES;
}

void SerializeTimeSeriesToValue(std::string* val,
                                const cockroach::roachpb::InternalTimeSeriesData& ts) {
  SerializeProtoToValue(val, ts);
  SetTag(val, cockroach::roachpb::TIMESERIES);
}

// MergeTimeSeriesValues attempts to merge two Values which contain
// InternalTimeSeriesData messages. The messages cannot be merged if they have
// different start timestamps or sample durations. Returns true if the merge is
// successful.
WARN_UNUSED_RESULT bool MergeTimeSeriesValues(std::string* left, const std::string& right,
                                              bool full_merge, rocksdb::Logger* logger) {
  // Attempt to parse TimeSeriesData from both Values.
  cockroach::roachpb::InternalTimeSeriesData left_ts;
  cockroach::roachpb::InternalTimeSeriesData right_ts;
  if (!ParseProtoFromValue(*left, &left_ts)) {
    rocksdb::Warn(logger, "left InternalTimeSeriesData could not be parsed from bytes.");
    return false;
  }
  if (!ParseProtoFromValue(right, &right_ts)) {
    rocksdb::Warn(logger, "right InternalTimeSeriesData could not be parsed from bytes.");
    return false;
  }

  // Ensure that both InternalTimeSeriesData have the same timestamp and
  // sample_duration.
  if (left_ts.start_timestamp_nanos() != right_ts.start_timestamp_nanos()) {
    rocksdb::Warn(logger, "TimeSeries merge failed due to mismatched start timestamps");
    return false;
  }
  if (left_ts.sample_duration_nanos() != right_ts.sample_duration_nanos()) {
    rocksdb::Warn(logger, "TimeSeries merge failed due to mismatched sample durations.");
    return false;
  }

  // If only a partial merge, do not sort and combine - instead, just quickly
  // merge the two values together. Values will be processed later after a
  // full merge.
  if (!full_merge) {
    left_ts.MergeFrom(right_ts);
    SerializeTimeSeriesToValue(left, left_ts);
    return true;
  }

  // Initialize new_ts and its primitive data fields. Values from the left and
  // right collections will be merged into the new collection.
  cockroach::roachpb::InternalTimeSeriesData new_ts;
  new_ts.set_start_timestamp_nanos(left_ts.start_timestamp_nanos());
  new_ts.set_sample_duration_nanos(left_ts.sample_duration_nanos());

  // Sort values in right_ts. Assume values in left_ts have been sorted.
  std::stable_sort(right_ts.mutable_samples()->pointer_begin(),
                   right_ts.mutable_samples()->pointer_end(), TimeSeriesSampleOrdering);

  // Merge sample values of left and right into new_ts.
  auto left_front = left_ts.samples().begin();
  auto left_end = left_ts.samples().end();
  auto right_front = right_ts.samples().begin();
  auto right_end = right_ts.samples().end();

  // Loop until samples from both sides have been exhausted.
  while (left_front != left_end || right_front != right_end) {
    // Select the lowest offset from either side.
    long next_offset;
    if (left_front == left_end) {
      next_offset = right_front->offset();
    } else if (right_front == right_end) {
      next_offset = left_front->offset();
    } else if (left_front->offset() <= right_front->offset()) {
      next_offset = left_front->offset();
    } else {
      next_offset = right_front->offset();
    }

    // Create an empty sample in the output collection.
    cockroach::roachpb::InternalTimeSeriesSample* ns = new_ts.add_samples();

    // Only the most recently merged value with a given sample offset is kept;
    // samples merged earlier at the same offset are discarded. We will now
    // parse through the left and right sample sets, finding the most recently
    // merged sample at the current offset.
    cockroach::roachpb::InternalTimeSeriesSample src;
    while (left_front != left_end && left_front->offset() == next_offset) {
      src = *left_front;
      left_front++;
    }
    while (right_front != right_end && right_front->offset() == next_offset) {
      src = *right_front;
      right_front++;
    }

    ns->CopyFrom(src);
  }

  // Serialize the new TimeSeriesData into the left value's byte field.
  SerializeTimeSeriesToValue(left, new_ts);
  return true;
}

// ConsolidateTimeSeriesValue processes a single value which contains
// InternalTimeSeriesData messages. This method will sort the sample collection
// of the value, keeping only the last of samples with duplicate offsets.
// This method is the single-value equivalent of MergeTimeSeriesValues, and is
// used in the case where the first value is merged into the key. Returns true
// if the merge is successful.
WARN_UNUSED_RESULT bool ConsolidateTimeSeriesValue(std::string* val, rocksdb::Logger* logger) {
  // Attempt to parse TimeSeriesData from both Values.
  cockroach::roachpb::InternalTimeSeriesData val_ts;
  if (!ParseProtoFromValue(*val, &val_ts)) {
    rocksdb::Warn(logger, "InternalTimeSeriesData could not be parsed from bytes.");
    return false;
  }

  // Initialize new_ts and its primitive data fields.
  cockroach::roachpb::InternalTimeSeriesData new_ts;
  new_ts.set_start_timestamp_nanos(val_ts.start_timestamp_nanos());
  new_ts.set_sample_duration_nanos(val_ts.sample_duration_nanos());

  // Sort values in the ts value.
  std::stable_sort(val_ts.mutable_samples()->pointer_begin(),
                   val_ts.mutable_samples()->pointer_end(), TimeSeriesSampleOrdering);

  // Consolidate sample values from the ts value with duplicate offsets.
  auto front = val_ts.samples().begin();
  auto end = val_ts.samples().end();

  // Loop until samples have been exhausted.
  while (front != end) {
    // Create an empty sample in the output collection.
    cockroach::roachpb::InternalTimeSeriesSample* ns = new_ts.add_samples();
    ns->set_offset(front->offset());
    while (front != end && front->offset() == ns->offset()) {
      // Only the last sample in the value's repeated samples field with a given
      // offset is kept in the case of multiple samples with identical offsets.
      ns->CopyFrom(*front);
      ++front;
    }
  }

  // Serialize the new TimeSeriesData into the value's byte field.
  SerializeTimeSeriesToValue(val, new_ts);
  return true;
}

WARN_UNUSED_RESULT bool MergeValues(cockroach::storage::engine::enginepb::MVCCMetadata* left,
                                    const cockroach::storage::engine::enginepb::MVCCMetadata& right,
                                    bool full_merge, rocksdb::Logger* logger) {
  if (left->has_raw_bytes()) {
    if (!right.has_raw_bytes()) {
      rocksdb::Warn(logger, "inconsistent value types for merge (left = bytes, right = ?)");
      return false;
    }

    // Replay Advisory: Because merge commands pass through raft, it is possible
    // for merging values to be "replayed". Currently, the only actual use of
    // the merge system is for time series data, which is safe against replay;
    // however, this property is not general for all potential mergeable types.
    // If a future need arises to merge another type of data, replay protection
    // will likely need to be a consideration.

    if (IsTimeSeriesData(left->raw_bytes()) || IsTimeSeriesData(right.raw_bytes())) {
      // The right operand must also be a time series.
      if (!IsTimeSeriesData(left->raw_bytes()) || !IsTimeSeriesData(right.raw_bytes())) {
        rocksdb::Warn(logger, "inconsistent value types for merging time "
                              "series data (type(left) != type(right))");
        return false;
      }
      return MergeTimeSeriesValues(left->mutable_raw_bytes(), right.raw_bytes(), full_merge,
                                   logger);
    } else {
      const rocksdb::Slice rdata = ValueDataBytes(right.raw_bytes());
      left->mutable_raw_bytes()->append(rdata.data(), rdata.size());
    }
    return true;
  } else {
    left->mutable_raw_bytes()->assign(right.raw_bytes());
    if (right.has_merge_timestamp()) {
      left->mutable_merge_timestamp()->CopyFrom(right.merge_timestamp());
    }
    if (full_merge && IsTimeSeriesData(left->raw_bytes())) {
      if (!ConsolidateTimeSeriesValue(left->mutable_raw_bytes(), logger)) {
        return false;
      }
    }
    return true;
  }
}

// MergeResult serializes the result MVCCMetadata value into a byte slice.
DBStatus MergeResult(cockroach::storage::engine::enginepb::MVCCMetadata* meta, DBString* result) {
  // TODO(pmattis): Should recompute checksum here. Need a crc32
  // implementation and need to verify the checksumming is identical
  // to what is being done in Go. Zlib's crc32 should be sufficient.
  result->len = meta->ByteSize();
  result->data = static_cast<char*>(malloc(result->len));
  if (!meta->SerializeToArray(result->data, result->len)) {
    return ToDBString("serialization error");
  }
  return kSuccess;
}

class DBMergeOperator : public rocksdb::MergeOperator {
  virtual const char* Name() const { return "cockroach_merge_operator"; }

  virtual bool FullMerge(const rocksdb::Slice& key, const rocksdb::Slice* existing_value,
                         const std::deque<std::string>& operand_list, std::string* new_value,
                         rocksdb::Logger* logger) const WARN_UNUSED_RESULT {
    // TODO(pmattis): Taken from the old merger code, below are some
    // details about how errors returned by the merge operator are
    // handled. Need to test various error scenarios and decide on
    // desired behavior. Clear the key and it's gone. Corrupt it
    // properly and RocksDB might refuse to work with it at all until
    // you clear it manually, which may also not be what we want. The
    // problem with merges is that RocksDB won't really carry them out
    // while we have a chance to talk back to clients.
    //
    // If we indicate failure (*success = false), then the call to the
    // merger via rocksdb_merge will not return an error, but simply
    // remove or truncate the offending key (at least when the settings
    // specify that missing keys should be created; otherwise a
    // corruption error will be returned, but likely only after the next
    // read of the key). In effect, there is no propagation of error
    // information to the client.
    cockroach::storage::engine::enginepb::MVCCMetadata meta;
    if (existing_value != NULL) {
      if (!meta.ParseFromArray(existing_value->data(), existing_value->size())) {
        // Corrupted existing value.
        rocksdb::Warn(logger, "corrupted existing value");
        return false;
      }
    }

    for (int i = 0; i < operand_list.size(); i++) {
      if (!MergeOne(&meta, operand_list[i], true, logger)) {
        return false;
      }
    }

    if (!meta.SerializeToString(new_value)) {
      rocksdb::Warn(logger, "serialization error");
      return false;
    }
    return true;
  }

  virtual bool PartialMergeMulti(const rocksdb::Slice& key,
                                 const std::deque<rocksdb::Slice>& operand_list,
                                 std::string* new_value,
                                 rocksdb::Logger* logger) const WARN_UNUSED_RESULT {
    cockroach::storage::engine::enginepb::MVCCMetadata meta;

    for (int i = 0; i < operand_list.size(); i++) {
      if (!MergeOne(&meta, operand_list[i], false, logger)) {
        return false;
      }
    }

    if (!meta.SerializeToString(new_value)) {
      rocksdb::Warn(logger, "serialization error");
      return false;
    }
    return true;
  }

 private:
  bool MergeOne(cockroach::storage::engine::enginepb::MVCCMetadata* meta,
                const rocksdb::Slice& operand, bool full_merge,
                rocksdb::Logger* logger) const WARN_UNUSED_RESULT {
    cockroach::storage::engine::enginepb::MVCCMetadata operand_meta;
    if (!operand_meta.ParseFromArray(operand.data(), operand.size())) {
      rocksdb::Warn(logger, "corrupted operand value");
      return false;
    }
    return MergeValues(meta, operand_meta, full_merge, logger);
  }
};

class DBLogger : public rocksdb::Logger {
 public:
  DBLogger(bool enabled) : enabled_(enabled) {}
  virtual void Logv(const char* format, va_list ap) {
    // TODO(pmattis): Benchmark calling Go exported methods from C++
    // to determine if this is too slow.
    if (!enabled_) {
      return;
    }

    // First try with a small fixed size buffer.
    char space[1024];

    // It's possible for methods that use a va_list to invalidate the data in
    // it upon use. The fix is to make a copy of the structure before using it
    // and use that copy instead.
    va_list backup_ap;
    va_copy(backup_ap, ap);
    int result = vsnprintf(space, sizeof(space), format, backup_ap);
    va_end(backup_ap);

    if ((result >= 0) && (result < sizeof(space))) {
      rocksDBLog(space, result);
      return;
    }

    // Repeatedly increase buffer size until it fits.
    int length = sizeof(space);
    while (true) {
      if (result < 0) {
        // Older behavior: just try doubling the buffer size.
        length *= 2;
      } else {
        // We need exactly "result+1" characters.
        length = result + 1;
      }
      char* buf = new char[length];

      // Restore the va_list before we use it again
      va_copy(backup_ap, ap);
      result = vsnprintf(buf, length, format, backup_ap);
      va_end(backup_ap);

      if ((result >= 0) && (result < length)) {
        // It fit
        rocksDBLog(buf, result);
        delete[] buf;
        return;
      }
      delete[] buf;
    }
  }

 private:
  const bool enabled_;
};

// Getter defines an interface for retrieving a value from either an
// iterator or an engine. It is used by ProcessDeltaKey to abstract
// whether the "base" layer is an iterator or an engine.
struct Getter {
  virtual DBStatus Get(DBString* value) = 0;
};

// IteratorGetter is an implementation of the Getter interface which
// retrieves the value currently pointed to by the supplied
// iterator. It is ok for the supplied iterator to be NULL in which
// case no value will be retrieved.
struct IteratorGetter : public Getter {
  rocksdb::Iterator* const base;

  IteratorGetter(rocksdb::Iterator* iter) : base(iter) {}

  virtual DBStatus Get(DBString* value) {
    if (base == NULL) {
      value->data = NULL;
      value->len = 0;
    } else {
      *value = ToDBString(base->value());
    }
    return kSuccess;
  }
};

// DBGetter is an implementation of the Getter interface which
// retrieves the value for the supplied key from a rocksdb::DB.
struct DBGetter : public Getter {
  rocksdb::DB* const rep;
  rocksdb::ReadOptions const options;
  std::string const key;

  DBGetter(rocksdb::DB* const r, rocksdb::ReadOptions opts, std::string&& k)
      : rep(r), options(opts), key(std::move(k)) {}

  virtual DBStatus Get(DBString* value) {
    std::string tmp;
    rocksdb::Status s = rep->Get(options, key, &tmp);
    if (!s.ok()) {
      if (s.IsNotFound()) {
        // This mirrors the logic in rocksdb_get(). It doesn't seem like
        // a good idea, but some code in engine_test.go depends on it.
        value->data = NULL;
        value->len = 0;
        return kSuccess;
      }
      return ToDBStatus(s);
    }
    *value = ToDBString(tmp);
    return kSuccess;
  }
};

// ProcessDeltaKey performs the heavy lifting of processing the deltas
// for "key" contained in a batch and determining what the resulting
// value is. "delta" should have been seeked to "key", but may not be
// pointing to "key" if no updates existing for that key in the batch.
//
// Note that RocksDB WriteBatches append updates
// internally. WBWIIterator maintains an index for these updates on
// <key, seq-num>. Looping over the entries in WBWIIterator will
// return the keys in sorted order and, for each key, the updates as
// they were added to the batch.
//
// Upon return, the delta iterator will point to the next entry past
// key. The delta iterator may not be valid if the end of iteration
// was reached.
DBStatus ProcessDeltaKey(Getter* base, rocksdb::WBWIIterator* delta, rocksdb::Slice key,
                         DBString* value) {
  if (value->data != NULL) {
    free(value->data);
  }
  value->data = NULL;
  value->len = 0;

  int count = 0;
  for (; delta->Valid() && delta->Entry().key == key; ++count, delta->Next()) {
    rocksdb::WriteEntry entry = delta->Entry();
    switch (entry.type) {
    case rocksdb::kPutRecord:
      if (value->data != NULL) {
        free(value->data);
      }
      *value = ToDBString(entry.value);
      break;
    case rocksdb::kMergeRecord: {
      DBString existing;
      if (count == 0) {
        // If this is the first record for the key, then we need to
        // merge with the record in base.
        DBStatus status = base->Get(&existing);
        if (status.data != NULL) {
          if (value->data != NULL) {
            free(value->data);
            value->data = NULL;
            value->len = 0;
          }
          return status;
        }
      } else {
        // Merge with the value we've built up so far.
        existing = *value;
        value->data = NULL;
        value->len = 0;
      }
      if (existing.data != NULL) {
        DBStatus status = DBMergeOne(ToDBSlice(existing), ToDBSlice(entry.value), value);
        free(existing.data);
        if (status.data != NULL) {
          return status;
        }
      } else {
        *value = ToDBString(entry.value);
      }
      break;
    }
    case rocksdb::kDeleteRecord:
      if (value->data != NULL) {
        free(value->data);
      }
      // This mirrors the logic in DBGet(): a deleted entry is
      // indicated by a value with NULL data.
      value->data = NULL;
      value->len = 0;
      break;
    default:
      break;
    }
  }

  if (count > 0) {
    return kSuccess;
  }
  return base->Get(value);
}

// This was cribbed from RocksDB and modified to support merge
// records. A BaseDeltaIterator is an iterator which provides a merged
// view of a base iterator and a delta where the delta iterator is
// from a WriteBatchWithIndex.
class BaseDeltaIterator : public rocksdb::Iterator {
 public:
  BaseDeltaIterator(rocksdb::Iterator* base_iterator, rocksdb::WBWIIterator* delta_iterator,
                    bool prefix_same_as_start)
      : current_at_base_(true),
        equal_keys_(false),
        status_(rocksdb::Status::OK()),
        base_iterator_(base_iterator),
        delta_iterator_(delta_iterator),
        prefix_same_as_start_(prefix_same_as_start) {
    merged_.data = NULL;
  }

  virtual ~BaseDeltaIterator() { ClearMerged(); }

  bool Valid() const override {
    return status_.ok() && (current_at_base_ ? BaseValid() : DeltaValid());
  }

  void SeekToFirst() override {
    base_iterator_->SeekToFirst();
    delta_iterator_->SeekToFirst();
    UpdateCurrent(false /* no prefix check */);
    MaybeSavePrefixStart();
  }

  void SeekToLast() override {
    prefix_start_key_.clear();
    base_iterator_->SeekToLast();
    delta_iterator_->SeekToLast();
    UpdateCurrent(false /* no prefix check */);
    MaybeSavePrefixStart();
  }

  void Seek(const rocksdb::Slice& k) override {
    if (prefix_same_as_start_) {
      prefix_start_key_ = KeyPrefix(k);
    }
    base_iterator_->Seek(k);
    delta_iterator_->Seek(k);
    UpdateCurrent(prefix_same_as_start_);

    // Similar to MaybeSavePrefixStart, but we can avoid computing the
    // prefix again.
    if (prefix_same_as_start_) {
      if (Valid()) {
        prefix_start_buf_ = prefix_start_key_.ToString();
        prefix_start_key_ = prefix_start_buf_;
      } else {
        prefix_start_key_.clear();
      }
    }
  }

  void Next() override {
    if (!Valid()) {
      status_ = rocksdb::Status::NotSupported("Next() on invalid iterator");
    }
    Advance();
  }

  void Prev() override { status_ = rocksdb::Status::NotSupported("Prev() not supported"); }

  rocksdb::Slice key() const override {
    return current_at_base_ ? base_iterator_->key() : delta_key_;
  }

  rocksdb::Slice value() const override {
    if (current_at_base_) {
      return base_iterator_->value();
    }
    return ToSlice(merged_);
  }

  rocksdb::Status status() const override {
    if (!status_.ok()) {
      return status_;
    }
    if (!base_iterator_->status().ok()) {
      return base_iterator_->status();
    }
    return delta_iterator_->status();
  }

 private:
  // -1 -- delta less advanced than base
  // 0 -- delta == base
  // 1 -- delta more advanced than base
  int Compare() const {
    assert(delta_iterator_->Valid() && base_iterator_->Valid());
    return kComparator.Compare(delta_iterator_->Entry().key, base_iterator_->key());
  }

  // Advance the iterator to the next key, advancing either the base
  // or delta iterators or both.
  void Advance() {
    if (equal_keys_) {
      assert(BaseValid() && DeltaValid());
      AdvanceBase();
      AdvanceDelta();
    } else {
      if (current_at_base_) {
        assert(BaseValid());
        AdvanceBase();
      } else {
        assert(DeltaValid());
        AdvanceDelta();
      }
    }
    UpdateCurrent(prefix_same_as_start_);
  }

  // Advance the delta iterator, clearing any cached (merged) value
  // the delta iterator was pointing at.
  void AdvanceDelta() {
    delta_iterator_->Next();
    ClearMerged();
  }

  // Process the current entry the delta iterator is pointing at. This
  // is needed to handle merge operations. Note that all of the
  // entries for a particular key are stored consecutively in the
  // write batch with the "earlier" entries appearing first. Returns
  // true if the current entry is deleted and false otherwise.
  bool ProcessDelta() WARN_UNUSED_RESULT {
    IteratorGetter base(equal_keys_ ? base_iterator_.get() : NULL);
    // The contents of WBWIIterator.Entry() are only valid until the
    // next mutation to the write batch. So keep a copy of the key
    // we're pointing at.
    delta_key_ = delta_iterator_->Entry().key.ToString();
    DBStatus status = ProcessDeltaKey(&base, delta_iterator_.get(), delta_key_, &merged_);
    if (status.data != NULL) {
      status_ = rocksdb::Status::Corruption("unable to merge records");
      free(status.data);
      return false;
    }

    // We advanced past the last entry for key and want to back up the
    // delta iterator, but we can only back up if the iterator is
    // valid.
    if (delta_iterator_->Valid()) {
      delta_iterator_->Prev();
    } else {
      delta_iterator_->SeekToLast();
    }

    return merged_.data == NULL;
  }

  // Advance the base iterator.
  void AdvanceBase() { base_iterator_->Next(); }

  // Save the prefix start key if prefix iteration is enabled. The
  // prefix start key is the prefix of the key that was seeked to. See
  // also Seek() where similar code is inlined.
  void MaybeSavePrefixStart() {
    if (prefix_same_as_start_) {
      if (Valid()) {
        prefix_start_buf_ = KeyPrefix(key()).ToString();
        prefix_start_key_ = prefix_start_buf_;
      } else {
        prefix_start_key_.clear();
      }
    }
  }

  // CheckPrefix checks the specified key against the prefix being
  // iterated over (if restricted), returning true if the key exceeds
  // the iteration boundaries.
  bool CheckPrefix(const rocksdb::Slice key) { return KeyPrefix(key) != prefix_start_key_; }

  bool BaseValid() const { return base_iterator_->Valid(); }

  bool DeltaValid() const { return delta_iterator_->Valid(); }

  // Update the state for the iterator. The check_prefix parameter
  // specifies whether iteration should stop if the next non-deleted
  // key has a prefix that differs from prefix_start_key_.
  //
  // UpdateCurrent is the work horse of the BaseDeltaIterator methods
  // and contains the logic for advancing either the base or delta
  // iterators or both, as well as overlaying the delta iterator state
  // on the base iterator.
  void UpdateCurrent(bool check_prefix) {
    ClearMerged();

    for (;;) {
      equal_keys_ = false;
      if (!BaseValid()) {
        // Base has finished.
        if (!DeltaValid()) {
          // Both base and delta have finished.
          return;
        }
        if (check_prefix && CheckPrefix(delta_iterator_->Entry().key)) {
          // The delta iterator key has a different prefix than the
          // one we're searching for. We set current_at_base_ to true
          // which will cause the iterator overall to be considered
          // not valid (since base currently isn't valid).
          current_at_base_ = true;
          return;
        }
        if (!ProcessDelta()) {
          current_at_base_ = false;
          return;
        }
        // Delta is a deletion tombstone.
        AdvanceDelta();
        continue;
      }

      if (!DeltaValid()) {
        // Delta has finished.
        current_at_base_ = true;
        return;
      }

      // Delta and base are both valid. We need to compare keys to see
      // which to use.

      const int compare = Compare();
      if (compare > 0) {
        // Delta is greater than base (use base).
        current_at_base_ = true;
        return;
      }
      // Delta is less than or equal to base. If check_prefix is true,
      // for base to be valid it has to contain the prefix we were
      // searching for. It follows that delta contains the prefix
      // we're searching for.
      if (compare == 0) {
        // Delta is equal to base.
        equal_keys_ = true;
      }
      if (!ProcessDelta()) {
        current_at_base_ = false;
        return;
      }

      // Delta is less than or equal to base and is a deletion
      // tombstone.
      AdvanceDelta();
      if (equal_keys_) {
        AdvanceBase();
      }
    }
  }

  // Clear the merged delta iterator value.
  void ClearMerged() const {
    if (merged_.data != NULL) {
      free(merged_.data);
      merged_.data = NULL;
      merged_.len = 0;
    }
  }

  // Is the iterator currently pointed at the base or delta iterator?
  // Also see equal_keys_ which indicates the base and delta iterator
  // keys are the same and both need to be advanced.
  bool current_at_base_;
  bool equal_keys_;
  mutable rocksdb::Status status_;
  // The merged delta value returned when we're pointed at the delta
  // iterator.
  mutable DBString merged_;
  // The base iterator, presumably obtained from a rocksdb::DB.
  std::unique_ptr<rocksdb::Iterator> base_iterator_;
  // The delta iterator obtained from a rocksdb::WriteBatchWithIndex.
  std::unique_ptr<rocksdb::WBWIIterator> delta_iterator_;
  // The key the delta iterator is currently pointed at. We can't use
  // delta_iterator_->Entry().key due to the handling of merge
  // operations.
  std::string delta_key_;
  // Is this a prefix iterator?
  const bool prefix_same_as_start_;
  // The key prefix that we're restricting iteration to. Only used if
  // prefix_same_as_start_ is true.
  std::string prefix_start_buf_;
  rocksdb::Slice prefix_start_key_;
};

}  // namespace

DBSSTable* DBEngine::GetSSTables(int* n) {
  std::vector<rocksdb::LiveFileMetaData> metadata;
  rep->GetLiveFilesMetaData(&metadata);
  *n = metadata.size();
  // We malloc the result so it can be deallocated by the caller using free().
  const int size = metadata.size() * sizeof(DBSSTable);
  DBSSTable* tables = reinterpret_cast<DBSSTable*>(malloc(size));
  memset(tables, 0, size);
  for (int i = 0; i < metadata.size(); i++) {
    tables[i].level = metadata[i].level;
    tables[i].size = metadata[i].size;

    rocksdb::Slice tmp;
    if (DecodeKey(metadata[i].smallestkey, &tmp, &tables[i].start_key.wall_time,
                  &tables[i].start_key.logical)) {
      // This is a bit ugly because we want DBKey.key to be copied and
      // not refer to the memory in metadata[i].smallestkey.
      DBString str = ToDBString(tmp);
      tables[i].start_key.key = DBSlice{str.data, str.len};
    }
    if (DecodeKey(metadata[i].largestkey, &tmp, &tables[i].end_key.wall_time,
                  &tables[i].end_key.logical)) {
      DBString str = ToDBString(tmp);
      tables[i].end_key.key = DBSlice{str.data, str.len};
    }
  }
  return tables;
}

DBString DBEngine::GetUserProperties() {
  rocksdb::TablePropertiesCollection props;
  rocksdb::Status status = rep->GetPropertiesOfAllTables(&props);

  cockroach::storage::engine::enginepb::SSTUserPropertiesCollection all;
  if (!status.ok()) {
    all.set_error(status.ToString());
    return ToDBString(all.SerializeAsString());
  }

  for (auto i = props.begin(); i != props.end(); i++) {
    cockroach::storage::engine::enginepb::SSTUserProperties* sst = all.add_sst();
    sst->set_path(i->first);
    auto userprops = i->second->user_collected_properties;

    auto ts_min = userprops.find("crdb.ts.min");
    if (ts_min != userprops.end() && !ts_min->second.empty()) {
      if (!DecodeHLCTimestamp(rocksdb::Slice(ts_min->second), sst->mutable_ts_min())) {
        fmt::SStringPrintf(
            all.mutable_error(), "unable to decode crdb.ts.min value '%s' in table %s",
            rocksdb::Slice(ts_min->second).ToString(true).c_str(), sst->path().c_str());
        break;
      }
    }

    auto ts_max = userprops.find("crdb.ts.max");
    if (ts_max != userprops.end() && !ts_max->second.empty()) {
      if (!DecodeHLCTimestamp(rocksdb::Slice(ts_max->second), sst->mutable_ts_max())) {
        fmt::SStringPrintf(
            all.mutable_error(), "unable to decode crdb.ts.max value '%s' in table %s",
            rocksdb::Slice(ts_max->second).ToString(true).c_str(), sst->path().c_str());
        break;
      }
    }
  }
  return ToDBString(all.SerializeAsString());
}

DBBatch::DBBatch(DBEngine* db)
    : DBEngine(db->rep), updates(0), has_delete_range(false), batch(&kComparator) {}

DBWriteOnlyBatch::DBWriteOnlyBatch(DBEngine* db) : DBEngine(db->rep), updates(0) {}

DBCache* DBNewCache(uint64_t size) {
  const int num_cache_shard_bits = 4;
  DBCache* cache = new DBCache;
  cache->rep = rocksdb::NewLRUCache(size, num_cache_shard_bits);
  return cache;
}

DBCache* DBRefCache(DBCache* cache) {
  DBCache* res = new DBCache;
  res->rep = cache->rep;
  return res;
}

void DBReleaseCache(DBCache* cache) { delete cache; }

class TimeBoundTblPropCollector : public rocksdb::TablePropertiesCollector {
 public:
  const char* Name() const override { return "TimeBoundTblPropCollector"; }

  rocksdb::Status Finish(rocksdb::UserCollectedProperties* properties) override {
    *properties = rocksdb::UserCollectedProperties{
        {"crdb.ts.min", ts_min_},
        {"crdb.ts.max", ts_max_},
    };
    return rocksdb::Status::OK();
  }

  rocksdb::Status AddUserKey(const rocksdb::Slice& user_key, const rocksdb::Slice& value,
                             rocksdb::EntryType type, rocksdb::SequenceNumber seq,
                             uint64_t file_size) override {
    rocksdb::Slice unused;
    rocksdb::Slice ts;
    if (SplitKey(user_key, &unused, &ts) && !ts.empty()) {
      ts.remove_prefix(1);  // The NUL prefix.
      if (ts_max_.empty() || ts.compare(ts_max_) > 0) {
        ts_max_.assign(ts.data(), ts.size());
      }
      if (ts_min_.empty() || ts.compare(ts_min_) < 0) {
        ts_min_.assign(ts.data(), ts.size());
      }
    }
    return rocksdb::Status::OK();
  }

  virtual rocksdb::UserCollectedProperties GetReadableProperties() const override {
    return rocksdb::UserCollectedProperties{};
  }

 private:
  std::string ts_min_;
  std::string ts_max_;
};

class TimeBoundTblPropCollectorFactory : public rocksdb::TablePropertiesCollectorFactory {
 public:
  explicit TimeBoundTblPropCollectorFactory() {}
  virtual rocksdb::TablePropertiesCollector* CreateTablePropertiesCollector(
      rocksdb::TablePropertiesCollectorFactory::Context context) override {
    return new TimeBoundTblPropCollector();
  }
  const char* Name() const override { return "TimeBoundTblPropCollectorFactory"; }
};

rocksdb::Options DBMakeOptions(DBOptions db_opts) {
  // Use the rocksdb options builder to configure the base options
  // using our memtable budget.
  rocksdb::Options options;
  // Increase parallelism for compactions and flushes based on the
  // number of cpus. Always use at least 2 threads, otherwise
  // compactions and flushes may fight with each other.
  options.IncreaseParallelism(std::max(db_opts.num_cpu, 2));
  // Enable subcompactions which will use multiple threads to speed up
  // a single compaction. The value of num_cpu/2 has not been tuned.
  options.max_subcompactions = std::max(db_opts.num_cpu / 2, 1);
  options.WAL_ttl_seconds = db_opts.wal_ttl_seconds;
  options.comparator = &kComparator;
  options.create_if_missing = !db_opts.must_exist;
  options.info_log.reset(new DBLogger(db_opts.logging_enabled));
  options.merge_operator.reset(new DBMergeOperator);
  options.prefix_extractor.reset(new DBPrefixExtractor);
  options.statistics = rocksdb::CreateDBStatistics();
  options.max_open_files = db_opts.max_open_files;
  options.compaction_pri = rocksdb::kMinOverlappingRatio;
  // Periodically sync both the WAL and SST writes to smooth out disk
  // usage. Not performing such syncs can be faster but can cause
  // performance blips when the OS decides it needs to flush data.
  options.wal_bytes_per_sync = 512 << 10;  // 512 KB
  options.bytes_per_sync = 512 << 10;      // 512 KB

  // The size reads should be performed in for compaction. The
  // internets claim this can speed up compactions, though RocksDB
  // docs say it is only useful on spinning disks. Experimentally it
  // has had no effect.
  // options.compaction_readahead_size = 2 << 20;

  // Do not create bloom filters for the last level (i.e. the largest
  // level which contains data in the LSM store). Setting this option
  // reduces the size of the bloom filters by 10x. This is significant
  // given that bloom filters require 1.25 bytes (10 bits) per key
  // which can translate into gigabytes of memory given typical key
  // and value sizes. The downside is that bloom filters will only be
  // usable on the higher levels, but that seems acceptable. We
  // typically see read amplification of 5-6x on clusters (i.e. there
  // are 5-6 levels of sstables) which means we'll achieve 80-90% of
  // the benefit of having bloom filters on every level for only 10%
  // of the memory cost.
  options.optimize_filters_for_hits = true;

  // We periodically report stats ourselves and by default the info
  // logger swallows log messages.
  options.stats_dump_period_sec = 0;

  // Use the TablePropertiesCollector hook to store the min and max MVCC
  // timestamps present in each sstable in the metadata for that sstable.
  std::shared_ptr<rocksdb::TablePropertiesCollectorFactory> time_bound_prop_collector(
      new TimeBoundTblPropCollectorFactory());
  options.table_properties_collector_factories.push_back(time_bound_prop_collector);

  // The write buffer size is the size of the in memory structure that
  // will be flushed to create L0 files.
  options.write_buffer_size = 64 << 20;  // 64 MB
  // How much memory should be allotted to memtables? Note that this
  // is a peak setting, steady state should be lower. We set this
  // relatively high to account for bursts of writes (e.g. due to a
  // deletion of a large range of keys). In particular, we want this
  // to be somewhat larger than than typical range size so that
  // deletion of a range worth of keys does not cause write stalls.
  options.max_write_buffer_number = 4;
  // Number of files to trigger L0 compaction. We set this low so that
  // we quickly move files out of L0 as each L0 file increases read
  // amplification.
  options.level0_file_num_compaction_trigger = 2;
  // Soft limit on number of L0 files. Writes are slowed down when
  // this number is reached.
  options.level0_slowdown_writes_trigger = 20;
  // Maximum number of L0 files. Writes are stopped at this
  // point. This is set significantly higher than
  // level0_slowdown_writes_trigger to avoid completely blocking
  // writes.
  options.level0_stop_writes_trigger = 32;
  // Flush write buffers to L0 as soon as they are full. A higher
  // value could be beneficial if there are duplicate records in each
  // of the individual write buffers, but perf testing hasn't shown
  // any benefit so far.
  options.min_write_buffer_number_to_merge = 1;
  // Enable dynamic level sizing which reduces both size and write
  // amplification. This causes RocksDB to pick the target size of
  // each level dynamically.
  options.level_compaction_dynamic_level_bytes = true;
  // Follow the RocksDB recommendation to configure the size of L1 to
  // be the same as the estimated size of L0.
  options.max_bytes_for_level_base = 64 << 20;  // 64 MB
  options.max_bytes_for_level_multiplier = 10;
  // Target the base file size (L1) as 4 MB. Each additional level
  // grows the file size by 2. With max_bytes_for_level_base set to 64
  // MB, this translates into the following target level and file
  // sizes for each level:
  //
  //       level-size  file-size  max-files
  //   L1:      64 MB       4 MB         16
  //   L2:     640 MB       8 MB         80
  //   L3:    6.25 GB      16 MB        400
  //   L4:    62.5 GB      32 MB       2000
  //   L5:     625 GB      64 MB      10000
  //   L6:     6.1 TB     128 MB      50000
  //
  // Due to the use of level_compaction_dynamic_level_bytes most data
  // will be in L6. The number of files will be approximately
  // total-data-size / 128 MB.
  //
  // We don't want the target file size to be too large, otherwise
  // individual compactions become more expensive. We don't want the
  // target file size to be too small or else we get an overabundance
  // of sstables.
  options.target_file_size_base = 4 << 20;  // 4 MB
  options.target_file_size_multiplier = 2;

  rocksdb::BlockBasedTableOptions table_options;
  if (db_opts.cache != nullptr) {
    table_options.block_cache = db_opts.cache->rep;

    // Reserve 1 memtable worth of memory from the cache. Under high
    // load situations we'll be using somewhat more than 1 memtable,
    // but usually not significantly more unless there is an I/O
    // throughput problem.
    std::lock_guard<std::mutex> guard(db_opts.cache->mu);
    const int64_t capacity = db_opts.cache->rep->GetCapacity();
    const int64_t new_capacity = std::max<int64_t>(0, capacity - options.write_buffer_size);
    db_opts.cache->rep->SetCapacity(new_capacity);
  }

  // Pass false for use_blocked_base_builder creates a per file
  // (sstable) filter instead of a per-block filter. The per file
  // filter can be consulted before going to the index which saves an
  // index lookup. The cost is an 4-bytes per key in memory during
  // compactions, which seems a small price to pay.
  table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false /* !block_based */));
  table_options.format_version = 2;

  // Increasing block_size decreases memory usage at the cost of
  // increased read amplification.
  table_options.block_size = db_opts.block_size;
  // Disable whole_key_filtering which adds a bloom filter entry for
  // the "whole key", doubling the size of our bloom filters. This is
  // used to speed up Get operations which we don't use.
  table_options.whole_key_filtering = false;
  options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));
  return options;
}

DBStatus DBOpen(DBEngine** db, DBSlice dir, DBOptions db_opts) {
  rocksdb::Options options = DBMakeOptions(db_opts);

  std::string db_dir = ToString(dir);

  // Call hooks to handle db_opts.extra_options.
  auto hook_status = DBOpenHook(db_dir, db_opts);
  if (!hook_status.ok()) {
    return ToDBStatus(hook_status);
  }

  // Register listener for tracking RocksDB stats.
  std::shared_ptr<DBEventListener> event_listener(new DBEventListener);
  options.listeners.emplace_back(event_listener);

  // TODO(mberhault): we shouldn't need two separate env objects,
  // options.env should be sufficient with SwitchingEnv owning any
  // underlying Env.
  std::unique_ptr<rocksdb::Env> memenv;
  if (dir.len == 0) {
    memenv.reset(rocksdb::NewMemEnv(rocksdb::Env::Default()));
    options.env = memenv.get();
  }

  std::unique_ptr<rocksdb::Env> switching_env;
  if (db_opts.use_switching_env) {
    switching_env.reset(NewSwitchingEnv(options.env, options.info_log));
    options.env = switching_env.get();
  }

  rocksdb::DB* db_ptr;
  rocksdb::Status status = rocksdb::DB::Open(options, db_dir, &db_ptr);
  if (!status.ok()) {
    return ToDBStatus(status);
  }
  *db =
      new DBImpl(db_ptr, memenv.release(), db_opts.cache != nullptr ? db_opts.cache->rep : nullptr,
                 event_listener, switching_env.release());
  return kSuccess;
}

DBStatus DBDestroy(DBSlice dir) {
  rocksdb::Options options;
  return ToDBStatus(rocksdb::DestroyDB(ToString(dir), options));
}

void DBClose(DBEngine* db) { delete db; }

DBStatus DBFlush(DBEngine* db) {
  rocksdb::FlushOptions options;
  options.wait = true;
  return ToDBStatus(db->rep->Flush(options));
}

DBStatus DBSyncWAL(DBEngine* db) {
#ifdef _WIN32
  // On Windows, DB::SyncWAL() is not implemented due to fact that
  // `WinWritableFile` is not thread safe. To get around that, the only other
  // methods that can be used to ensure that a sync is triggered is to either
  // flush the memtables or perform a write with `WriteOptions.sync=true`. See
  // https://github.com/facebook/rocksdb/wiki/RocksDB-FAQ for more details.
  // Please also see #17442 for more discussion on the topic.

  // In order to force a sync we issue a write-batch containing
  // LogData with 'sync=true'. The LogData forces a write to the WAL
  // but otherwise doesn't add anything to the memtable or sstables.
  rocksdb::WriteBatch batch;
  batch.PutLogData("");
  rocksdb::WriteOptions options;
  options.sync = true;
  return ToDBStatus(db->rep->Write(options, &batch));
#else
  return ToDBStatus(db->rep->SyncWAL());
#endif
}

DBStatus DBCompact(DBEngine* db) { return DBCompactRange(db, DBSlice(), DBSlice()); }

DBStatus DBCompactRange(DBEngine* db, DBSlice start, DBSlice end) {
  rocksdb::CompactRangeOptions options;
  // By default, RocksDB doesn't recompact the bottom level (unless
  // there is a compaction filter, which we don't use). However,
  // recompacting the bottom layer is necessary to pick up changes to
  // settings like bloom filter configurations, and to fully reclaim
  // space after dropping, truncating, or migrating tables.
  options.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;

  // Compacting the entire database in a single-shot can use a
  // significant amount of additional (temporary) disk space. Instead,
  // we loop over the sstables in the lowest level and initiate
  // compactions on smaller ranges of keys. The resulting compacted
  // database is the same size, but the temporary disk space needed
  // for the compaction is dramatically reduced.
  std::vector<rocksdb::LiveFileMetaData> all_metadata;
  std::vector<rocksdb::LiveFileMetaData> metadata;
  db->rep->GetLiveFilesMetaData(&all_metadata);

  const std::string start_key(ToString(start));
  const std::string end_key(ToString(end));

  int max_level = 0;
  for (int i = 0; i < all_metadata.size(); i++) {
    // Skip any SSTables which fall outside the specified range, if a
    // range was specified.
    if ((!start_key.empty() && all_metadata[i].largestkey < start_key) ||
        (!end_key.empty() && all_metadata[i].smallestkey >= end_key)) {
      continue;
    }
    if (max_level < all_metadata[i].level) {
      max_level = all_metadata[i].level;
    }
    // Gather the set of SSTables to compact.
    metadata.push_back(all_metadata[i]);
  }
  all_metadata.clear();

  if (max_level != db->rep->NumberLevels() - 1) {
    // There are no sstables at the lowest level, so just compact the
    // specified key span, wholesale. Due to the
    // level_compaction_dynamic_level_bytes setting, this will only
    // happen on spans containing very little data.
    const rocksdb::Slice start_slice(start_key);
    const rocksdb::Slice end_slice(end_key);
    return ToDBStatus(db->rep->CompactRange(options, !start_key.empty() ? &start_slice : nullptr,
                                            !end_key.empty() ? &end_slice : nullptr));
  }

  // A naive approach to selecting ranges to compact would be to
  // compact the ranges specified by the smallest and largest key in
  // each sstable of the bottom-most level. Unfortunately, the
  // sstables in the bottom-most level have vastly different
  // sizes. For example, starting with the following set of bottom-most
  // sstables:
  //
  //   100M[16] 89M 70M 66M 56M 54M 38M[2] 36M 23M 20M 17M 8M 6M 5M 2M 2K[4]
  //
  // If we compact the entire database in one call we can end up with:
  //
  //   100M[22] 77M 76M 50M
  //
  // If we use the naive approach (compact the range specified by
  // the smallest and largest keys):
  //
  //   100M[18] 92M 68M 62M 61M 50M 45M 39M 31M 29M[2] 24M 23M 18M 9M 8M[2] 7M
  //   2K[4]
  //
  // With the approach below:
  //
  //   100M[19] 80M 68M[2] 62M 61M 53M 45M 36M 31M
  //
  // The approach below is to loop over the bottom-most sstables in
  // sorted order and initiate a compact range every 128MB of data.

  // Gather up the bottom-most sstable metadata.
  std::vector<rocksdb::SstFileMetaData> sst;
  for (int i = 0; i < metadata.size(); i++) {
    if (metadata[i].level != max_level) {
      continue;
    }
    sst.push_back(metadata[i]);
  }
  // Sort the metadata by smallest key.
  std::sort(sst.begin(), sst.end(),
            [](const rocksdb::SstFileMetaData& a, const rocksdb::SstFileMetaData& b) -> bool {
              return a.smallestkey < b.smallestkey;
            });

  // Walk over the bottom-most sstables in order and perform
  // compactions every 128MB.
  rocksdb::Slice last;
  rocksdb::Slice* last_ptr = nullptr;
  uint64_t size = 0;
  const uint64_t target_size = 128 << 20;
  for (int i = 0; i < sst.size(); ++i) {
    size += sst[i].size;
    if (size < target_size) {
      continue;
    }
    rocksdb::Slice cur(sst[i].largestkey);
    rocksdb::Status status = db->rep->CompactRange(options, last_ptr, &cur);
    if (!status.ok()) {
      return ToDBStatus(status);
    }
    last = cur;
    last_ptr = &last;
    size = 0;
  }

  if (size > 0) {
    return ToDBStatus(db->rep->CompactRange(options, last_ptr, nullptr));
  }
  return kSuccess;
}

DBStatus DBApproximateDiskBytes(DBEngine* db, DBKey start, DBKey end, uint64_t* size) {
  const std::string start_key(EncodeKey(start));
  const std::string end_key(EncodeKey(end));
  const rocksdb::Range r(start_key, end_key);
  const uint8_t flags = rocksdb::DB::SizeApproximationFlags::INCLUDE_FILES;

  db->rep->GetApproximateSizes(&r, 1, size, flags);
  return kSuccess;
}

DBStatus DBImpl::Put(DBKey key, DBSlice value) {
  rocksdb::WriteOptions options;
  return ToDBStatus(rep->Put(options, EncodeKey(key), ToSlice(value)));
}

DBStatus DBBatch::Put(DBKey key, DBSlice value) {
  ++updates;
  batch.Put(EncodeKey(key), ToSlice(value));
  return kSuccess;
}

DBStatus DBWriteOnlyBatch::Put(DBKey key, DBSlice value) {
  ++updates;
  batch.Put(EncodeKey(key), ToSlice(value));
  return kSuccess;
}

DBStatus DBSnapshot::Put(DBKey key, DBSlice value) { return FmtStatus("unsupported"); }

DBStatus DBPut(DBEngine* db, DBKey key, DBSlice value) { return db->Put(key, value); }

DBStatus DBImpl::Merge(DBKey key, DBSlice value) {
  rocksdb::WriteOptions options;
  return ToDBStatus(rep->Merge(options, EncodeKey(key), ToSlice(value)));
}

DBStatus DBBatch::Merge(DBKey key, DBSlice value) {
  ++updates;
  batch.Merge(EncodeKey(key), ToSlice(value));
  return kSuccess;
}

DBStatus DBWriteOnlyBatch::Merge(DBKey key, DBSlice value) {
  ++updates;
  batch.Merge(EncodeKey(key), ToSlice(value));
  return kSuccess;
}

DBStatus DBSnapshot::Merge(DBKey key, DBSlice value) { return FmtStatus("unsupported"); }

DBStatus DBMerge(DBEngine* db, DBKey key, DBSlice value) { return db->Merge(key, value); }

DBStatus DBImpl::Get(DBKey key, DBString* value) {
  rocksdb::ReadOptions read_opts;
  DBGetter base(rep, read_opts, EncodeKey(key));
  return base.Get(value);
}

DBStatus DBBatch::Get(DBKey key, DBString* value) {
  rocksdb::ReadOptions read_opts;
  DBGetter base(rep, read_opts, EncodeKey(key));
  if (updates == 0) {
    return base.Get(value);
  }
  if (has_delete_range) {
    // TODO(peter): We don't support iterators when the batch contains
    // delete range entries.
    return FmtStatus("cannot read from a batch containing delete range entries");
  }
  std::unique_ptr<rocksdb::WBWIIterator> iter(batch.NewIterator());
  iter->Seek(base.key);
  return ProcessDeltaKey(&base, iter.get(), base.key, value);
}

DBStatus DBWriteOnlyBatch::Get(DBKey key, DBString* value) { return FmtStatus("unsupported"); }

DBStatus DBSnapshot::Get(DBKey key, DBString* value) {
  rocksdb::ReadOptions read_opts;
  read_opts.snapshot = snapshot;
  DBGetter base(rep, read_opts, EncodeKey(key));
  return base.Get(value);
}

DBStatus DBGet(DBEngine* db, DBKey key, DBString* value) { return db->Get(key, value); }

DBStatus DBImpl::Delete(DBKey key) {
  rocksdb::WriteOptions options;
  return ToDBStatus(rep->Delete(options, EncodeKey(key)));
}

DBStatus DBBatch::Delete(DBKey key) {
  ++updates;
  batch.Delete(EncodeKey(key));
  return kSuccess;
}

DBStatus DBWriteOnlyBatch::Delete(DBKey key) {
  ++updates;
  batch.Delete(EncodeKey(key));
  return kSuccess;
}

DBStatus DBSnapshot::Delete(DBKey key) { return FmtStatus("unsupported"); }

DBStatus DBImpl::DeleteRange(DBKey start, DBKey end) {
  rocksdb::WriteOptions options;
  return ToDBStatus(
      rep->DeleteRange(options, rep->DefaultColumnFamily(), EncodeKey(start), EncodeKey(end)));
}

DBStatus DBBatch::DeleteRange(DBKey start, DBKey end) {
  ++updates;
  has_delete_range = true;
  batch.DeleteRange(EncodeKey(start), EncodeKey(end));
  return kSuccess;
}

DBStatus DBWriteOnlyBatch::DeleteRange(DBKey start, DBKey end) {
  ++updates;
  batch.DeleteRange(EncodeKey(start), EncodeKey(end));
  return kSuccess;
}

DBStatus DBSnapshot::DeleteRange(DBKey start, DBKey end) { return FmtStatus("unsupported"); }

DBStatus DBDelete(DBEngine* db, DBKey key) { return db->Delete(key); }

DBStatus DBDeleteRange(DBEngine* db, DBKey start, DBKey end) { return db->DeleteRange(start, end); }

DBStatus DBDeleteIterRange(DBEngine* db, DBIterator* iter, DBKey start, DBKey end) {
  rocksdb::Iterator* const iter_rep = iter->rep.get();
  iter_rep->Seek(EncodeKey(start));
  const std::string end_key = EncodeKey(end);
  for (; iter_rep->Valid() && kComparator.Compare(iter_rep->key(), end_key) < 0; iter_rep->Next()) {
    DBStatus status = db->Delete(ToDBKey(iter_rep->key()));
    if (status.data != NULL) {
      return status;
    }
  }
  return kSuccess;
}

DBStatus DBImpl::CommitBatch(bool sync) { return FmtStatus("unsupported"); }

DBStatus DBBatch::CommitBatch(bool sync) {
  if (updates == 0) {
    return kSuccess;
  }
  rocksdb::WriteOptions options;
  options.sync = sync;
  return ToDBStatus(rep->Write(options, batch.GetWriteBatch()));
}

DBStatus DBWriteOnlyBatch::CommitBatch(bool sync) {
  if (updates == 0) {
    return kSuccess;
  }
  rocksdb::WriteOptions options;
  options.sync = sync;
  return ToDBStatus(rep->Write(options, &batch));
}

DBStatus DBSnapshot::CommitBatch(bool sync) { return FmtStatus("unsupported"); }

DBStatus DBCommitAndCloseBatch(DBEngine* db, bool sync) {
  DBStatus status = db->CommitBatch(sync);
  if (status.data == NULL) {
    DBClose(db);
  }
  return status;
}

DBStatus DBImpl::ApplyBatchRepr(DBSlice repr, bool sync) {
  rocksdb::WriteBatch batch(ToString(repr));
  rocksdb::WriteOptions options;
  options.sync = sync;
  return ToDBStatus(rep->Write(options, &batch));
}

DBStatus DBBatch::ApplyBatchRepr(DBSlice repr, bool sync) {
  if (sync) {
    return FmtStatus("unsupported");
  }
  // TODO(peter): It would be slightly more efficient to iterate over
  // repr directly instead of first converting it to a string.
  DBBatchInserter inserter(&batch);
  rocksdb::WriteBatch batch(ToString(repr));
  rocksdb::Status status = batch.Iterate(&inserter);
  if (!status.ok()) {
    return ToDBStatus(status);
  }
  updates += batch.Count();
  return kSuccess;
}

DBStatus DBWriteOnlyBatch::ApplyBatchRepr(DBSlice repr, bool sync) {
  if (sync) {
    return FmtStatus("unsupported");
  }
  // TODO(peter): It would be slightly more efficient to iterate over
  // repr directly instead of first converting it to a string.
  DBBatchInserter inserter(&batch);
  rocksdb::WriteBatch batch(ToString(repr));
  rocksdb::Status status = batch.Iterate(&inserter);
  if (!status.ok()) {
    return ToDBStatus(status);
  }
  updates += batch.Count();
  return kSuccess;
}

DBStatus DBSnapshot::ApplyBatchRepr(DBSlice repr, bool sync) { return FmtStatus("unsupported"); }

DBStatus DBApplyBatchRepr(DBEngine* db, DBSlice repr, bool sync) {
  return db->ApplyBatchRepr(repr, sync);
}

DBSlice DBImpl::BatchRepr() { return ToDBSlice("unsupported"); }

DBSlice DBBatch::BatchRepr() { return ToDBSlice(batch.GetWriteBatch()->Data()); }

DBSlice DBWriteOnlyBatch::BatchRepr() { return ToDBSlice(batch.GetWriteBatch()->Data()); }

DBSlice DBSnapshot::BatchRepr() { return ToDBSlice("unsupported"); }

DBSlice DBBatchRepr(DBEngine* db) { return db->BatchRepr(); }

DBEngine* DBNewSnapshot(DBEngine* db) { return new DBSnapshot(db); }

DBEngine* DBNewBatch(DBEngine* db, bool writeOnly) {
  if (writeOnly) {
    return new DBWriteOnlyBatch(db);
  }
  return new DBBatch(db);
}

DBIterator* DBImpl::NewIter(rocksdb::ReadOptions* read_opts) {
  DBIterator* iter = new DBIterator;
  iter->rep.reset(rep->NewIterator(*read_opts));
  return iter;
}

DBIterator* DBBatch::NewIter(rocksdb::ReadOptions* read_opts) {
  DBIterator* iter = new DBIterator;
  if (has_delete_range) {
    // TODO(peter): We don't support iterators when the batch contains
    // delete range entries.
    return NULL;
  }
  rocksdb::Iterator* base = rep->NewIterator(*read_opts);
  rocksdb::WBWIIterator* delta = batch.NewIterator();
  iter->rep.reset(new BaseDeltaIterator(base, delta, read_opts->prefix_same_as_start));
  return iter;
}

DBIterator* DBWriteOnlyBatch::NewIter(rocksdb::ReadOptions* read_opts) { return NULL; }

DBIterator* DBSnapshot::NewIter(rocksdb::ReadOptions* read_opts) {
  read_opts->snapshot = snapshot;
  DBIterator* iter = new DBIterator;
  iter->rep.reset(rep->NewIterator(*read_opts));
  return iter;
}

// GetStats retrieves a subset of RocksDB stats that are relevant to
// CockroachDB.
DBStatus DBImpl::GetStats(DBStatsResult* stats) {
  const rocksdb::Options& opts = rep->GetOptions();
  const std::shared_ptr<rocksdb::Statistics>& s = opts.statistics;

  uint64_t memtable_total_size;
  rep->GetIntProperty("rocksdb.cur-size-all-mem-tables", &memtable_total_size);

  uint64_t table_readers_mem_estimate;
  rep->GetIntProperty("rocksdb.estimate-table-readers-mem", &table_readers_mem_estimate);

  uint64_t pending_compaction_bytes_estimate;
  rep->GetIntProperty("rocksdb.estimate-pending-compaction-bytes",
                      &pending_compaction_bytes_estimate);

  stats->block_cache_hits = (int64_t)s->getTickerCount(rocksdb::BLOCK_CACHE_HIT);
  stats->block_cache_misses = (int64_t)s->getTickerCount(rocksdb::BLOCK_CACHE_MISS);
  stats->block_cache_usage = (int64_t)block_cache->GetUsage();
  stats->block_cache_pinned_usage = (int64_t)block_cache->GetPinnedUsage();
  stats->bloom_filter_prefix_checked =
      (int64_t)s->getTickerCount(rocksdb::BLOOM_FILTER_PREFIX_CHECKED);
  stats->bloom_filter_prefix_useful =
      (int64_t)s->getTickerCount(rocksdb::BLOOM_FILTER_PREFIX_USEFUL);
  stats->memtable_total_size = memtable_total_size;
  stats->flushes = (int64_t)event_listener->GetFlushes();
  stats->compactions = (int64_t)event_listener->GetCompactions();
  stats->table_readers_mem_estimate = table_readers_mem_estimate;
  stats->pending_compaction_bytes_estimate = pending_compaction_bytes_estimate;
  return kSuccess;
}

DBStatus DBBatch::GetStats(DBStatsResult* stats) { return FmtStatus("unsupported"); }

DBStatus DBWriteOnlyBatch::GetStats(DBStatsResult* stats) { return FmtStatus("unsupported"); }

DBStatus DBSnapshot::GetStats(DBStatsResult* stats) { return FmtStatus("unsupported"); }

DBString DBImpl::GetCompactionStats() {
  std::string tmp;
  rep->GetProperty("rocksdb.cfstats-no-file-histogram", &tmp);
  return ToDBString(tmp);
}

DBString DBBatch::GetCompactionStats() { return ToDBString("unsupported"); }

DBString DBWriteOnlyBatch::GetCompactionStats() { return ToDBString("unsupported"); }

DBString DBSnapshot::GetCompactionStats() { return ToDBString("unsupported"); }

// EnvWriteFile writes the given data as a new "file" in the given engine.
DBStatus DBImpl::EnvWriteFile(DBSlice path, DBSlice contents) {
  rocksdb::Status s;

  const rocksdb::EnvOptions soptions;
  rocksdb::unique_ptr<rocksdb::WritableFile> destfile;
  s = this->rep->GetEnv()->NewWritableFile(ToString(path), &destfile, soptions);
  if (!s.ok()) {
    return ToDBStatus(s);
  }

  s = destfile->Append(ToSlice(contents));
  if (!s.ok()) {
    return ToDBStatus(s);
  }

  return kSuccess;
}

DBStatus DBBatch::EnvWriteFile(DBSlice path, DBSlice contents) { return FmtStatus("unsupported"); }

DBStatus DBWriteOnlyBatch::EnvWriteFile(DBSlice path, DBSlice contents) {
  return FmtStatus("unsupported");
}

DBStatus DBSnapshot::EnvWriteFile(DBSlice path, DBSlice contents) {
  return FmtStatus("unsupported");
}

DBStatus DBEnvWriteFile(DBEngine* db, DBSlice path, DBSlice contents) {
  return db->EnvWriteFile(path, contents);
}

DBIterator* DBNewIter(DBEngine* db, bool prefix) {
  rocksdb::ReadOptions opts;
  opts.prefix_same_as_start = prefix;
  opts.total_order_seek = !prefix;
  return db->NewIter(&opts);
}

DBIterator* DBNewTimeBoundIter(DBEngine* db, DBTimestamp min_ts, DBTimestamp max_ts) {
  const std::string min = EncodeTimestamp(min_ts);
  const std::string max = EncodeTimestamp(max_ts);
  rocksdb::ReadOptions opts;
  opts.total_order_seek = true;
  opts.table_filter = [min, max](const rocksdb::TableProperties& props) {
    auto userprops = props.user_collected_properties;
    auto tbl_min = userprops.find("crdb.ts.min");
    if (tbl_min == userprops.end() || tbl_min->second.empty()) {
      return true;
    }
    auto tbl_max = userprops.find("crdb.ts.max");
    if (tbl_max == userprops.end() || tbl_max->second.empty()) {
      return true;
    }
    // If the timestamp range of the table overlaps with the timestamp range we
    // want to iterate, the table might contain timestamps we care about.
    return max.compare(tbl_min->second) >= 0 && min.compare(tbl_max->second) <= 0;
  };
  return db->NewIter(&opts);
}

void DBIterDestroy(DBIterator* iter) { delete iter; }

DBIterState DBIterSeek(DBIterator* iter, DBKey key) {
  iter->rep->Seek(EncodeKey(key));
  return DBIterGetState(iter);
}

DBIterState DBIterSeekToFirst(DBIterator* iter) {
  iter->rep->SeekToFirst();
  return DBIterGetState(iter);
}

DBIterState DBIterSeekToLast(DBIterator* iter) {
  iter->rep->SeekToLast();
  return DBIterGetState(iter);
}

DBIterState DBIterNext(DBIterator* iter, bool skip_current_key_versions) {
  // If we're skipping the current key versions, remember the key the
  // iterator was pointing out.
  std::string old_key;
  if (skip_current_key_versions && iter->rep->Valid()) {
    rocksdb::Slice key;
    rocksdb::Slice ts;
    if (!SplitKey(iter->rep->key(), &key, &ts)) {
      DBIterState state = {0};
      state.valid = false;
      state.status = FmtStatus("failed to split mvcc key");
      return state;
    }
    old_key = key.ToString();
  }

  iter->rep->Next();

  if (skip_current_key_versions && iter->rep->Valid()) {
    rocksdb::Slice key;
    rocksdb::Slice ts;
    if (!SplitKey(iter->rep->key(), &key, &ts)) {
      DBIterState state = {0};
      state.valid = false;
      state.status = FmtStatus("failed to split mvcc key");
      return state;
    }
    if (old_key == key) {
      // We're pointed at a different version of the same key. Fall
      // back to seeking to the next key.
      old_key.append("\0", 1);
      DBKey db_key;
      db_key.key = ToDBSlice(old_key);
      db_key.wall_time = 0;
      db_key.logical = 0;
      iter->rep->Seek(EncodeKey(db_key));
    }
  }

  return DBIterGetState(iter);
}

DBIterState DBIterPrev(DBIterator* iter, bool skip_current_key_versions) {
  // If we're skipping the current key versions, remember the key the
  // iterator was pointed out.
  std::string old_key;
  if (skip_current_key_versions && iter->rep->Valid()) {
    rocksdb::Slice key;
    rocksdb::Slice ts;
    if (SplitKey(iter->rep->key(), &key, &ts)) {
      old_key = key.ToString();
    }
  }

  iter->rep->Prev();

  if (skip_current_key_versions && iter->rep->Valid()) {
    rocksdb::Slice key;
    rocksdb::Slice ts;
    if (SplitKey(iter->rep->key(), &key, &ts)) {
      if (old_key == key) {
        // We're pointed at a different version of the same key. Fall
        // back to seeking to the prev key. In this case, we seek to
        // the "metadata" key and that back up the iterator.
        DBKey db_key;
        db_key.key = ToDBSlice(old_key);
        db_key.wall_time = 0;
        db_key.logical = 0;
        iter->rep->Seek(EncodeKey(db_key));
        if (iter->rep->Valid()) {
          iter->rep->Prev();
        }
      }
    }
  }

  return DBIterGetState(iter);
}

DBStatus DBMergeOne(DBSlice existing, DBSlice update, DBString* new_value) {
  new_value->len = 0;

  cockroach::storage::engine::enginepb::MVCCMetadata meta;
  if (!meta.ParseFromArray(existing.data, existing.len)) {
    return ToDBString("corrupted existing value");
  }

  cockroach::storage::engine::enginepb::MVCCMetadata update_meta;
  if (!update_meta.ParseFromArray(update.data, update.len)) {
    return ToDBString("corrupted update value");
  }

  if (!MergeValues(&meta, update_meta, true, NULL)) {
    return ToDBString("incompatible merge values");
  }
  return MergeResult(&meta, new_value);
}

const int64_t kNanosecondPerSecond = 1e9;

inline int64_t age_factor(int64_t fromNS, int64_t toNS) {
  // Careful about implicit conversions here.
  // toNS/1e9 - fromNS/1e9 is not the same since
  // "1e9" is a double.
  return toNS / kNanosecondPerSecond - fromNS / kNanosecondPerSecond;
}

// TODO(tschottdorf): it's unfortunate that this method duplicates the logic
// in (*MVCCStats).AgeTo. Passing now_nanos in is semantically tricky if there
// is a chance that we run into values ahead of now_nanos. Instead, now_nanos
// should be taken as a hint but determined by the max timestamp encountered.
//
// This implementation must match engine.ComputeStatsGo.
MVCCStatsResult MVCCComputeStatsInternal(::rocksdb::Iterator* const iter_rep, DBKey start,
                                         DBKey end, int64_t now_nanos) {
  MVCCStatsResult stats;
  memset(&stats, 0, sizeof(stats));

  iter_rep->Seek(EncodeKey(start));
  const std::string end_key = EncodeKey(end);

  cockroach::storage::engine::enginepb::MVCCMetadata meta;
  std::string prev_key;
  bool first = false;
  // NB: making this uninitialized triggers compiler warnings
  // with `-Werror=maybe-uninitialized`. This warning seems like
  // a false positive (changing the above line to `first=true`
  // which results in equivalent code does not remove it either).
  // An assertion has been placed where the compiler would warn.
  int64_t accrue_gc_age_nanos = 0;

  for (; iter_rep->Valid() && kComparator.Compare(iter_rep->key(), end_key) < 0; iter_rep->Next()) {
    const rocksdb::Slice key = iter_rep->key();
    const rocksdb::Slice value = iter_rep->value();

    rocksdb::Slice decoded_key;
    int64_t wall_time = 0;
    int32_t logical = 0;
    if (!DecodeKey(key, &decoded_key, &wall_time, &logical)) {
      stats.status = FmtStatus("unable to decode key");
      return stats;
    }

    const bool isSys = (rocksdb::Slice(decoded_key).compare(kLocalMax) < 0);
    const bool isValue = (wall_time != 0 || logical != 0);
    const bool implicitMeta = isValue && decoded_key != prev_key;
    prev_key.assign(decoded_key.data(), decoded_key.size());

    if (implicitMeta) {
      // No MVCCMetadata entry for this series of keys.
      meta.Clear();
      meta.set_key_bytes(kMVCCVersionTimestampSize);
      meta.set_val_bytes(value.size());
      meta.set_deleted(value.size() == 0);
      meta.mutable_timestamp()->set_wall_time(wall_time);
    }

    if (!isValue || implicitMeta) {
      const int64_t meta_key_size = decoded_key.size() + 1;
      const int64_t meta_val_size = implicitMeta ? 0 : value.size();
      const int64_t total_bytes = meta_key_size + meta_val_size;
      first = true;

      if (!implicitMeta && !meta.ParseFromArray(value.data(), value.size())) {
        stats.status = FmtStatus("unable to decode MVCCMetadata");
        return stats;
      }

      if (isSys) {
        stats.sys_bytes += total_bytes;
        stats.sys_count++;
      } else {
        if (!meta.deleted()) {
          stats.live_bytes += total_bytes;
          stats.live_count++;
        } else {
          stats.gc_bytes_age += total_bytes * age_factor(meta.timestamp().wall_time(), now_nanos);
        }
        stats.key_bytes += meta_key_size;
        stats.val_bytes += meta_val_size;
        stats.key_count++;
        if (meta.has_raw_bytes()) {
          stats.val_count++;
        }
      }
      if (!implicitMeta) {
        continue;
      }
    }

    const int64_t total_bytes = value.size() + kMVCCVersionTimestampSize;
    if (isSys) {
      stats.sys_bytes += total_bytes;
    } else {
      if (first) {
        first = false;
        if (!meta.deleted()) {
          stats.live_bytes += total_bytes;
        } else {
          stats.gc_bytes_age += total_bytes * age_factor(meta.timestamp().wall_time(), now_nanos);
        }
        if (meta.has_txn()) {
          stats.intent_bytes += total_bytes;
          stats.intent_count++;
          stats.intent_age += age_factor(meta.timestamp().wall_time(), now_nanos);
        }
        if (meta.key_bytes() != kMVCCVersionTimestampSize) {
          stats.status = FmtStatus("expected mvcc metadata key bytes to equal %d; got %d",
                                   kMVCCVersionTimestampSize, int(meta.key_bytes()));
          return stats;
        }
        if (meta.val_bytes() != value.size()) {
          stats.status = FmtStatus("expected mvcc metadata val bytes to equal %d; got %d",
                                   int(value.size()), int(meta.val_bytes()));
          return stats;
        }
        accrue_gc_age_nanos = meta.timestamp().wall_time();
      } else {
        bool is_tombstone = value.size() == 0;
        if (is_tombstone) {
          stats.gc_bytes_age += total_bytes * age_factor(wall_time, now_nanos);
        } else {
          assert(accrue_gc_age_nanos > 0);
          stats.gc_bytes_age += total_bytes * age_factor(accrue_gc_age_nanos, now_nanos);
        }
        accrue_gc_age_nanos = wall_time;
      }
      stats.key_bytes += kMVCCVersionTimestampSize;
      stats.val_bytes += value.size();
      stats.val_count++;
    }
  }

  stats.last_update_nanos = now_nanos;
  return stats;
}

MVCCStatsResult MVCCComputeStats(DBIterator* iter, DBKey start, DBKey end, int64_t now_nanos) {
  return MVCCComputeStatsInternal(iter->rep.get(), start, end, now_nanos);
}

bool MVCCIsValidSplitKey(DBSlice key, bool allow_meta2_splits) {
  return IsValidSplitKey(ToSlice(key), allow_meta2_splits);
}

DBStatus MVCCFindSplitKey(DBIterator* iter, DBKey start, DBKey end, DBKey min_split,
                          int64_t target_size, bool allow_meta2_splits, DBString* split_key) {
  auto iter_rep = iter->rep.get();
  const std::string start_key = EncodeKey(start);
  iter_rep->Seek(start_key);
  const std::string end_key = EncodeKey(end);
  const rocksdb::Slice min_split_key = ToSlice(min_split.key);

  int64_t size_so_far = 0;
  std::string best_split_key = start_key;
  int64_t best_split_diff = std::numeric_limits<int64_t>::max();
  std::string prev_key;
  int n = 0;

  for (; iter_rep->Valid() && kComparator.Compare(iter_rep->key(), end_key) < 0; iter_rep->Next()) {
    const rocksdb::Slice key = iter_rep->key();
    rocksdb::Slice decoded_key;
    int64_t wall_time = 0;
    int32_t logical = 0;
    if (!DecodeKey(key, &decoded_key, &wall_time, &logical)) {
      return FmtStatus("unable to decode key");
    }

    ++n;
    const bool valid = n > 1 && IsValidSplitKey(decoded_key, allow_meta2_splits) &&
                       decoded_key.compare(min_split_key) >= 0;
    int64_t diff = target_size - size_so_far;
    if (diff < 0) {
      diff = -diff;
    }
    if (valid && diff < best_split_diff) {
      best_split_key = decoded_key.ToString();
      best_split_diff = diff;
    }
    // If diff is increasing, that means we've passed the ideal split point and
    // should return the first key that we can. Note that best_split_key may
    // still be empty if we haven't reached min_split_key yet.
    if (diff > best_split_diff && !best_split_key.empty()) {
      break;
    }

    const bool is_value = (wall_time != 0 || logical != 0);
    if (is_value && decoded_key == prev_key) {
      size_so_far += kMVCCVersionTimestampSize + iter_rep->value().size();
    } else {
      size_so_far += decoded_key.size() + 1 + iter_rep->value().size();
      if (is_value) {
        size_so_far += kMVCCVersionTimestampSize;
      }
    }
    prev_key.assign(decoded_key.data(), decoded_key.size());
  }
  if (best_split_key == start_key) {
    return kSuccess;
  }
  *split_key = ToDBString(best_split_key);
  return kSuccess;
}

// kMaxItersBeforeSeek is the number of calls to iter->{Next,Prev}()
// to perform when looking for the next/prev key or a particular
// version before calling iter->Seek(). Note that mvccScanner makes
// this number adaptive. It starts with a value of kMaxItersPerSeek/2
// and increases the value every time a call to iter->{Next,Prev}()
// successfully finds the desired next key. It decrements the value
// whenever a call to iter->Seek() occurs. The adaptive
// iters-before-seek value is constrained to the range
// [1,kMaxItersBeforeSeek].
static const int kMaxItersBeforeSeek = 10;

// mvccScanner implements the MVCCGet, MVCCScan and MVCCReverseScan
// operations. Parameterizing the code on whether a forward or reverse
// scan is performed allows the different code paths to be compiled
// efficiently while still reusing the common code without difficulty.
//
// WARNING: Do not use iter_rep_->key() or iter_rep_->value()
// directly, use cur_raw_key_, cur_key_, and cur_value instead. In
// order to efficiently support reverse scans, we maintain a single
// entry buffer that allows "peeking" at the previous key. But the
// operation of "peeking" cause iter_rep_->{key,value}() to point to
// different data than what the scanner class considers the "current"
// key/value.
template <bool reverse> class mvccScanner {
 public:
  mvccScanner(DBIterator* iter, DBSlice start, DBSlice end, DBTimestamp timestamp, int64_t max_keys,
              DBTxn txn, bool consistent)
      : iter_(iter),
        iter_rep_(iter->rep.get()),
        start_key_(ToSlice(start)),
        end_key_(ToSlice(end)),
        max_keys_(max_keys),
        timestamp_(timestamp),
        txn_id_(ToSlice(txn.id)),
        txn_epoch_(txn.epoch),
        txn_max_timestamp_(txn.max_timestamp),
        consistent_(consistent),
        check_uncertainty_(timestamp < txn.max_timestamp),
        kvs_(new rocksdb::WriteBatch),
        intents_(new rocksdb::WriteBatch),
        peeked_(false),
        iters_before_seek_(kMaxItersBeforeSeek / 2) {
    memset(&results_, 0, sizeof(results_));
    results_.status = kSuccess;

    iter_->kvs.reset();
    iter_->intents.reset();
  }

  // The MVCC data is sorted by key and descending timestamp. If a key
  // has a write intent (i.e. an uncommitted transaction has written
  // to the key) a key with a zero timestamp, with an MVCCMetadata
  // value, will appear. We arrange for the keys to be sorted such
  // that the intent sorts first. For example:
  //
  //   A @ T3
  //   A @ T2
  //   A @ T1
  //   B <intent @ T2>
  //   B @ T2
  //
  // Here we have 2 keys, A and B. Key A has 3 versions, T3, T2 and
  // T1. Key B has 1 version, T1, and an intent. Scanning involves
  // looking for values at a particular timestamp. For example, let's
  // consider scanning this entire range at T2. We'll first seek to A,
  // discover the value @ T3. This value is newer than our read
  // timestamp so we'll iterate to find a value newer than our read
  // timestamp (the value @ T2). We then iterate to the next key and
  // discover the intent at B. What happens with the intent depends on
  // the mode we're reading in and the timestamp of the intent. In
  // this case, the intent is at our read timestamp. If we're
  // performing an inconsistent read we'll return the intent and read
  // at the instant of time just before the intent (for only that
  // key). If we're reading consistently, we'll either return the
  // intent along with an error or read the intent value if we're
  // reading transactionally and we own the intent.

  const DBScanResults& get() {
    if (!iterSeek(EncodeKey(start_key_, 0, 0))) {
      return results_;
    }
    if (cur_key_ == start_key_) {
      getAndAdvance();
    }
    return fillResults();
  }

  const DBScanResults& scan() {
    // TODO(peter): Remove this timing/debugging code.
    // auto pctx = rocksdb::get_perf_context();
    // pctx->Reset();
    // auto start_time = std::chrono::steady_clock::now();
    // auto elapsed = std::chrono::steady_clock::now() - start_time;
    // auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    // printf("seek %d: %s\n", int(micros), pctx->ToString(true).c_str());

    if (reverse) {
      if (!iterSeekReverse(EncodeKey(start_key_, 0, 0))) {
        return results_;
      }
      for (; cur_key_.compare(end_key_) >= 0;) {
        if (!getAndAdvance()) {
          break;
        }
      }
    } else {
      if (!iterSeek(EncodeKey(start_key_, 0, 0))) {
        return results_;
      }
      for (; cur_key_.compare(end_key_) < 0;) {
        if (!getAndAdvance()) {
          break;
        }
      }
    }

    return fillResults();
  }

 private:
  const DBScanResults& fillResults() {
    if (results_.status.len == 0) {
      if (kvs_->Count() > 0) {
        results_.data = ToDBSlice(kvs_->Data());
      }
      if (intents_->Count() > 0) {
        results_.intents = ToDBSlice(intents_->Data());
      }
      iter_->kvs.reset(kvs_.release());
      iter_->intents.reset(intents_.release());
    }
    return results_;
  }

  bool uncertaintyError(DBTimestamp ts) {
    results_.uncertainty_timestamp = ts;
    kvs_->Clear();
    intents_->Clear();
    return false;
  }

  bool setStatus(const DBStatus& status) {
    results_.status = status;
    return false;
  }

  bool getAndAdvance() {
    const bool is_value = cur_timestamp_ != kZeroTimestamp;

    if (is_value) {
      if (timestamp_ >= cur_timestamp_) {
        // 1. Fast path: there is no intent and our read timestamp is
        // newer than the most recent version's timestamp.
        return addAndAdvance(cur_value_);
      }

      if (check_uncertainty_) {
        // 2. Our txn's read timestamp is less than the max timestamp
        // seen by the txn. We need to check for clock uncertainty
        // errors.
        if (txn_max_timestamp_ >= cur_timestamp_) {
          return uncertaintyError(cur_timestamp_);
        }
        // Delegate to seekVersion to return a clock uncertainty error
        // if there are any more versions above txn_max_timestamp_.
        return seekVersion(txn_max_timestamp_, true);
      }

      // 3. Our txn's read timestamp is greater than or equal to the
      // max timestamp seen by the txn so clock uncertainty checks are
      // unnecessary. We need to seek to the desired version of the
      // value (i.e. one with a timestamp earlier than our read
      // timestamp).
      return seekVersion(timestamp_, false);
    }

    if (!meta_.ParseFromArray(cur_value_.data(), cur_value_.size())) {
      return setStatus(FmtStatus("unable to decode MVCCMetadata"));
    }

    if (meta_.has_raw_bytes()) {
      // 4. Emit immediately if the value is inline.
      return addAndAdvance(meta_.raw_bytes());
    }

    if (!meta_.has_txn()) {
      return setStatus(FmtStatus("intent without transaction"));
    }

    const bool own_intent = (meta_.txn().id() == txn_id_);
    const DBTimestamp meta_timestamp = ToDBTimestamp(meta_.timestamp());
    if (timestamp_ < meta_timestamp && !own_intent) {
      // 5. The key contains an intent, but we're reading before the
      // intent. Seek to the desired version. Note that if we own the
      // intent (i.e. we're reading transactionally) we want to read
      // the intent regardless of our read timestamp and fall into
      // case 8 below.
      return seekVersion(timestamp_, false);
    }

    if (!consistent_) {
      // 6. The key contains an intent and we're doing an inconsistent
      // read at a timestamp newer than the intent. We ignore the
      // intent by insisting that the timestamp we're reading at is a
      // historical timestamp < the intent timestamp. However, we
      // return the intent separately; the caller may want to resolve
      // it.
      intents_->Put(cur_raw_key_, cur_value_);
      return seekVersion(PrevTimestamp(ToDBTimestamp(meta_.timestamp())), false);
    }

    if (!own_intent) {
      // 7. The key contains an intent which was not written by our
      // transaction and our read timestamp is newer than that of the
      // intent. Note that this will trigger an error on the Go
      // side. We continue scanning so that we can return all of the
      // intents in the scan range.
      intents_->Put(cur_raw_key_, cur_value_);
      return advanceKey();
    }

    if (txn_epoch_ == meta_.txn().epoch()) {
      // 8. We're reading our own txn's intent. Note that we read at
      // the intent timestamp, not at our read timestamp as the intent
      // timestamp may have been pushed forward by another
      // transaction. Txn's always need to read their own writes.
      return seekVersion(meta_timestamp, false);
    }

    if (txn_epoch_ < meta_.txn().epoch()) {
      // 9. We're reading our own txn's intent but the current txn has
      // an earlier epoch than the intent. Return an error so that the
      // earlier incarnation of our transaction aborts (presumably
      // this is some operation that was retried).
      return setStatus(FmtStatus("failed to read with epoch %u due to a write intent with epoch %u",
                                 txn_epoch_, meta_.txn().epoch()));
    }

    // 10. We're reading our own txn's intent but the current txn has a
    // later epoch than the intent. This can happen if the txn was
    // restarted and an earlier iteration wrote the value we're now
    // reading. In this case, we ignore the intent and read the
    // previous value as if the transaction were starting fresh.
    return seekVersion(PrevTimestamp(ToDBTimestamp(meta_.timestamp())), false);
  }

  // nextKey advances the iterator to point to the next MVCC key
  // greater than cur_key_. Returns false if the iterator is exhausted
  // or an error occurs.
  bool nextKey() {
    // Check to see if the next key is the end key. This avoids
    // advancing the iterator unnecessarily. For example, SQL can take
    // advantage of this when doing single row reads with an
    // appropriately set end key.
    if (cur_key_.size() + 1 == end_key_.size() && end_key_.starts_with(cur_key_) &&
        end_key_[cur_key_.size()] == '\0') {
      return false;
    }

    key_buf_.assign(cur_key_.data(), cur_key_.size());

    for (int i = 0; i < iters_before_seek_; ++i) {
      if (!iterNext()) {
        return false;
      }
      if (cur_key_ != key_buf_) {
        iters_before_seek_ = std::max<int>(kMaxItersBeforeSeek, iters_before_seek_ + 1);
        return true;
      }
    }

    // We're pointed at a different version of the same key. Fall back
    // to seeking to the next key. We append 2 NULs to account for the
    // "next-key" and a trailing zero timestamp. See EncodeKey and
    // SplitKey for more details on the encoded key format.
    iters_before_seek_ = std::max<int>(1, iters_before_seek_ - 1);
    key_buf_.append("\0\0", 2);
    return iterSeek(key_buf_);
  }

  // backwardLatestVersion backs up the iterator to the latest version
  // for the specified key. The parameter i is used to maintain the
  // iteration count between the loop here and the caller (usually
  // prevKey). Returns false if an error occurred.
  bool backwardLatestVersion(const rocksdb::Slice& key, int i) {
    key_buf_.assign(key.data(), key.size());

    for (; i < iters_before_seek_; ++i) {
      rocksdb::Slice peeked_key;
      if (!iterPeekPrev(&peeked_key)) {
        return false;
      }
      if (peeked_key != key_buf_) {
        // The key changed which means the current key is the latest
        // version.
        iters_before_seek_ = std::max<int>(kMaxItersBeforeSeek, iters_before_seek_ + 1);
        return true;
      }
      if (!iterPrev()) {
        return false;
      }
    }

    iters_before_seek_ = std::max<int>(1, iters_before_seek_ - 1);
    key_buf_.append("\0", 1);
    return iterSeek(key_buf_);
  }

  // prevKey backs up the iterator to point to the prev MVCC key less
  // than the specified key. Returns false if the iterator is
  // exhausted or an error occurs.
  bool prevKey(const rocksdb::Slice& key) {
    if (peeked_ && iter_rep_->key().compare(end_key_) < 0) {
      // No need to look at the previous key if it is less than our
      // end key.
      return false;
    }

    key_buf_.assign(key.data(), key.size());

    for (int i = 0; i < iters_before_seek_; ++i) {
      rocksdb::Slice peeked_key;
      if (!iterPeekPrev(&peeked_key)) {
        return false;
      }
      if (peeked_key != key_buf_) {
        return backwardLatestVersion(peeked_key, i + 1);
      }
      if (!iterPrev()) {
        return false;
      }
    }

    iters_before_seek_ = std::max<int>(1, iters_before_seek_ - 1);
    key_buf_.append("\0", 1);
    return iterSeekReverse(key_buf_);
  }

  // advanceKey advances the iterator to point to the next MVCC
  // key. Returns false if the iterator is exhausted or an error
  // occurs.
  bool advanceKey() {
    if (reverse) {
      return prevKey(cur_key_);
    } else {
      return nextKey();
    }
  }

  bool advanceKeyAtEnd() {
    if (reverse) {
      // Iterating to the next key might have caused the iterator to
      // reach the end of the key space. If that happens, back up to
      // the very last key.
      clearPeeked();
      iter_rep_->SeekToLast();
      if (!updateCurrent()) {
        return false;
      }
      return advanceKey();
    } else {
      // We've reached the end of the iterator and there is nothing
      // left to do.
      return false;
    }
  }

  bool advanceKeyAtNewKey(const rocksdb::Slice& key) {
    if (reverse) {
      // We've advanced to the next key but need to move back to the
      // previous key.
      return prevKey(key);
    } else {
      // We're already at the new key so there is nothing to do.
      return true;
    }
  }

  bool addAndAdvance(const rocksdb::Slice& value) {
    if (value.size() > 0) {
      kvs_->Put(cur_raw_key_, value);
      if (kvs_->Count() > max_keys_) {
        return false;
      }
    }
    return advanceKey();
  }

  // seekVersion advances the iterator to point to an MVCC version for
  // the specified key that is earlier than <ts_wall_time,
  // ts_logical>. Returns false if the iterator is exhausted or an
  // error occurs. On success, advances the iterator to the next key.
  //
  // If the iterator is exhausted in the process or an error occurs,
  // return false, and true otherwise. If check_uncertainty is true,
  // then observing any version of the desired key with a timestamp
  // larger than our read timestamp results in an uncertainty error.
  //
  // TODO(peter): Passing check_uncertainty as a boolean is a bit
  // ungainly because it makes the subsequent comparison with
  // timestamp_ a bit subtle. Consider passing a
  // uncertainAboveTimestamp parameter. Or better, templatize this
  // method and pass a "check" functor.
  bool seekVersion(DBTimestamp desired_timestamp, bool check_uncertainty) {
    key_buf_.assign(cur_key_.data(), cur_key_.size());

    for (int i = 0; i < iters_before_seek_; ++i) {
      if (!iterNext()) {
        return advanceKeyAtEnd();
      }
      if (cur_key_ != key_buf_) {
        iters_before_seek_ = std::min<int>(kMaxItersBeforeSeek, iters_before_seek_ + 1);
        return advanceKeyAtNewKey(key_buf_);
      }
      if (desired_timestamp >= cur_timestamp_) {
        iters_before_seek_ = std::min<int>(kMaxItersBeforeSeek, iters_before_seek_ + 1);
        if (check_uncertainty && timestamp_ < cur_timestamp_) {
          return uncertaintyError(cur_timestamp_);
        }
        return addAndAdvance(cur_value_);
      }
    }

    iters_before_seek_ = std::max<int>(1, iters_before_seek_ - 1);
    if (!iterSeek(EncodeKey(key_buf_, desired_timestamp.wall_time, desired_timestamp.logical))) {
      return advanceKeyAtEnd();
    }
    if (cur_key_ != key_buf_) {
      return advanceKeyAtNewKey(key_buf_);
    }
    if (desired_timestamp >= cur_timestamp_) {
      if (check_uncertainty && timestamp_ < cur_timestamp_) {
        return uncertaintyError(cur_timestamp_);
      }
      return addAndAdvance(cur_value_);
    }
    return advanceKey();
  }

  bool updateCurrent() {
    if (!iter_rep_->Valid()) {
      return false;
    }
    cur_raw_key_ = iter_rep_->key();
    cur_value_ = iter_rep_->value();
    cur_timestamp_ = kZeroTimestamp;
    if (!DecodeKey(cur_raw_key_, &cur_key_, &cur_timestamp_)) {
      return setStatus(FmtStatus("failed to split mvcc key"));
    }
    return true;
  }

  // iterSeek positions the iterator at the first key that is greater
  // than or equal to key.
  bool iterSeek(const rocksdb::Slice& key) {
    clearPeeked();
    iter_rep_->Seek(key);
    return updateCurrent();
  }

  // iterSeekReverse positions the iterator at the last key that is
  // less than key.
  bool iterSeekReverse(const rocksdb::Slice& key) {
    clearPeeked();

    // SeekForPrev positions the iterator at the key that is less than
    // key. NB: the doc comment on SeekForPrev suggests it positions
    // less than or equal, but this is a lie.
    iter_rep_->SeekForPrev(key);
    if (!updateCurrent()) {
      return false;
    }
    if (cur_timestamp_ == kZeroTimestamp) {
      // We landed on an intent or inline value.
      return true;
    }

    // We landed on a versioned value, we need to back up to find the
    // latest version.
    return backwardLatestVersion(cur_key_, 0);
  }

  bool iterNext() {
    if (reverse && peeked_) {
      // If we had peeked at the previous entry, we need to advance
      // the iterator twice to get to the real next entry.
      peeked_ = false;
      iter_rep_->Next();
      if (!iter_rep_->Valid()) {
        return false;
      }
    }
    iter_rep_->Next();
    return updateCurrent();
  }

  bool iterPrev() {
    if (peeked_) {
      peeked_ = false;
      return updateCurrent();
    }
    iter_rep_->Prev();
    return updateCurrent();
  }

  // iterPeekPrev "peeks" at the previous key before the current
  // iterator position.
  bool iterPeekPrev(rocksdb::Slice *peeked_key) {
    if (!peeked_) {
      peeked_ = true;
      // We need to save a copy of the current iterator key and value
      // and adjust cur_raw_key_, cur_key and cur_value to point to
      // this saved data. We use a single buffer for this purpose:
      // saved_buf_.
      saved_buf_.resize(0);
      saved_buf_.reserve(cur_raw_key_.size() + cur_value_.size());
      saved_buf_.append(cur_raw_key_.data(), cur_raw_key_.size());
      saved_buf_.append(cur_value_.data(), cur_value_.size());
      cur_raw_key_ = rocksdb::Slice(saved_buf_.data(), cur_raw_key_.size());
      cur_value_ = rocksdb::Slice(saved_buf_.data() + cur_raw_key_.size(), cur_value_.size());
      rocksdb::Slice dummy_timestamp;
      if (!SplitKey(cur_raw_key_, &cur_key_, &dummy_timestamp)) {
        return setStatus(FmtStatus("failed to split mvcc key"));
      }

      // With the current iterator state saved we can move the
      // iterator to the previous entry.
      iter_rep_->Prev();
      if (!iter_rep_->Valid()) {
        // Peeking at the previous key should never leave the iterator
        // invalid. Instead, we seek back to the first key and set the
        // peeked_key to the empty key. Note that this prevents using
        // reverse scan to scan to the empty key.
        peeked_ = false;
        *peeked_key = rocksdb::Slice();
        iter_rep_->SeekToFirst();
        return updateCurrent();
      }
    }

    rocksdb::Slice dummy_timestamp;
    if (!SplitKey(iter_rep_->key(), peeked_key, &dummy_timestamp)) {
      return setStatus(FmtStatus("failed to split mvcc key"));
    }
    return true;
  }

  // clearPeeked clears the peeked flag. This should be called before
  // any iterator movement operations on iter_rep_.
  void clearPeeked() {
    if (reverse) {
      peeked_ = false;
    }
  }

 public:
  DBIterator* const iter_;
  rocksdb::Iterator* const iter_rep_;
  const rocksdb::Slice start_key_;
  const rocksdb::Slice end_key_;
  const int64_t max_keys_;
  const DBTimestamp timestamp_;
  const rocksdb::Slice txn_id_;
  const uint32_t txn_epoch_;
  const DBTimestamp txn_max_timestamp_;
  const bool consistent_;
  const bool check_uncertainty_;
  DBScanResults results_;
  std::unique_ptr<rocksdb::WriteBatch> kvs_;
  std::unique_ptr<rocksdb::WriteBatch> intents_;
  std::string key_buf_;
  std::string saved_buf_;
  bool peeked_;
  cockroach::storage::engine::enginepb::MVCCMetadata meta_;
  // cur_raw_key_ holds either iter_rep_->key() or the saved value of
  // iter_rep_->key() if we've peeked at the previous key (and peeked_
  // is true).
  rocksdb::Slice cur_raw_key_;
  // cur_key_ is the decoded MVCC key, separated from the timestamp
  // suffix.
  rocksdb::Slice cur_key_;
  // cur_value_ holds either iter_rep_->value() or the saved value of
  // iter_rep_->value() if we've peeked at the previous key (and
  // peeked_ is true).
  rocksdb::Slice cur_value_;
  // cur_timestamp_ is the timestamp for a decoded MVCC key.
  DBTimestamp cur_timestamp_;
  int iters_before_seek_;
};

typedef mvccScanner<false> mvccForwardScanner;
typedef mvccScanner<true> mvccReverseScanner;

DBScanResults MVCCGet(DBIterator* iter, DBSlice key, DBTimestamp timestamp, DBTxn txn,
                      bool consistent) {
  // Get is implemented as a scan where we retrieve a single key. Note
  // that the semantics of max_keys is that we retrieve one more key
  // than is specified in order to maintain the existing semantics of
  // resume span. See storage/engine/mvcc.go:MVCCScan.
  //
  // We specify an empty key for the end key which will ensure we
  // don't retrieve a key different than the start key. This is a bit
  // of a hack.
  const DBSlice end = {0, 0};
  mvccForwardScanner scanner(iter, key, end, timestamp, 0 /* max_keys */, txn, consistent);
  return scanner.get();
}

DBScanResults MVCCScan(DBIterator* iter, DBSlice start, DBSlice end, DBTimestamp timestamp,
                       int64_t max_keys, DBTxn txn, bool consistent, bool reverse) {
  if (reverse) {
    mvccReverseScanner scanner(iter, end, start, timestamp, max_keys, txn, consistent);
    return scanner.scan();
  } else {
    mvccForwardScanner scanner(iter, start, end, timestamp, max_keys, txn, consistent);
    return scanner.scan();
  }
}

// DBGetStats queries the given DBEngine for various operational stats and
// write them to the provided DBStatsResult instance.
DBStatus DBGetStats(DBEngine* db, DBStatsResult* stats) { return db->GetStats(stats); }

DBString DBGetCompactionStats(DBEngine* db) { return db->GetCompactionStats(); }

DBSSTable* DBGetSSTables(DBEngine* db, int* n) { return db->GetSSTables(n); }

DBString DBGetUserProperties(DBEngine* db) { return db->GetUserProperties(); }

DBStatus DBIngestExternalFile(DBEngine* db, DBSlice path, bool move_file) {
  const std::vector<std::string> paths = {ToString(path)};
  rocksdb::IngestExternalFileOptions ingest_options;
  // If move_files is true and the env supports it, RocksDB will hard link.
  // Otherwise, it will copy.
  ingest_options.move_files = move_file;
  // If snapshot_consistency is true and there is an outstanding RocksDB
  // snapshot, a global sequence number is forced (see the allow_global_seqno
  // option).
  ingest_options.snapshot_consistency = true;
  // If a file is ingested over existing data (including the range tombstones
  // used by range snapshots) or if a RocksDB snapshot is outstanding when this
  // ingest runs, then after moving/copying the file, RocksDB will edit it
  // (overwrite some of the bytes) to have a global sequence number. If this is
  // false, it will error in these cases instead.
  ingest_options.allow_global_seqno = true;
  // If there are mutations in the memtable for the keyrange covered by the file
  // being ingested, this option is checked. If true, the memtable is flushed
  // and the ingest run. If false, an error is returned.
  ingest_options.allow_blocking_flush = true;
  rocksdb::Status status = db->rep->IngestExternalFile(paths, ingest_options);
  if (!status.ok()) {
    return ToDBStatus(status);
  }

  return kSuccess;
}

struct DBSstFileWriter {
  std::unique_ptr<rocksdb::Options> options;
  std::unique_ptr<rocksdb::Env> memenv;
  rocksdb::SstFileWriter rep;

  DBSstFileWriter(rocksdb::Options* o, rocksdb::Env* m)
      : options(o), memenv(m), rep(rocksdb::EnvOptions(), *o, o->comparator) {}
  virtual ~DBSstFileWriter() {}
};

DBSstFileWriter* DBSstFileWriterNew() {
  // TODO(dan): Right now, backup is the only user of this code, so that's what
  // the options are tuned for. If something else starts using it, we'll likely
  // have to add some configurability.

  rocksdb::BlockBasedTableOptions table_options;
  // Larger block size (4kb default) means smaller file at the expense of more
  // scanning during lookups.
  table_options.block_size = 64 * 1024;
  // The original LevelDB compatible format. We explicitly set the checksum too
  // to guard against the silent version upconversion. See
  // https://github.com/facebook/rocksdb/blob/972f96b3fbae1a4675043bdf4279c9072ad69645/include/rocksdb/table.h#L198
  table_options.format_version = 0;
  table_options.checksum = rocksdb::kCRC32c;

  rocksdb::Options* options = new rocksdb::Options();
  options->comparator = &kComparator;
  options->table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

  std::unique_ptr<rocksdb::Env> memenv;
  memenv.reset(rocksdb::NewMemEnv(rocksdb::Env::Default()));
  options->env = memenv.get();

  return new DBSstFileWriter(options, memenv.release());
}

DBStatus DBSstFileWriterOpen(DBSstFileWriter* fw) {
  rocksdb::Status status = fw->rep.Open("sst");
  if (!status.ok()) {
    return ToDBStatus(status);
  }
  return kSuccess;
}

DBStatus DBSstFileWriterAdd(DBSstFileWriter* fw, DBKey key, DBSlice val) {
  rocksdb::Status status = fw->rep.Put(EncodeKey(key), ToSlice(val));
  if (!status.ok()) {
    return ToDBStatus(status);
  }
  return kSuccess;
}

DBStatus DBSstFileWriterFinish(DBSstFileWriter* fw, DBString* data) {
  rocksdb::Status status = fw->rep.Finish();
  if (!status.ok()) {
    return ToDBStatus(status);
  }

  uint64_t file_size;
  status = fw->memenv->GetFileSize("sst", &file_size);
  if (!status.ok()) {
    return ToDBStatus(status);
  }

  const rocksdb::EnvOptions soptions;
  rocksdb::unique_ptr<rocksdb::SequentialFile> sst;
  status = fw->memenv->NewSequentialFile("sst", &sst, soptions);
  if (!status.ok()) {
    return ToDBStatus(status);
  }

  // scratch is eventually returned as the array part of data and freed by the
  // caller.
  char* scratch = static_cast<char*>(malloc(file_size));

  rocksdb::Slice sst_contents;
  status = sst->Read(file_size, &sst_contents, scratch);
  if (!status.ok()) {
    return ToDBStatus(status);
  }
  if (sst_contents.size() != file_size) {
    return FmtStatus("expected to read %d bytes but got %d", file_size, sst_contents.size());
  }

  // The contract of the SequentialFile.Read call above is that it _might_ use
  // scratch as the backing data for sst_contents, but it also _might not_. If
  // it didn't, copy sst_contents into scratch, so we can unconditionally return
  // a DBString backed by scratch (which can then always be freed by the
  // caller). Note that this means the data is always copied exactly once,
  // either by Read or here.
  if (sst_contents.data() != scratch) {
    memcpy(scratch, sst_contents.data(), sst_contents.size());
  }
  data->data = scratch;
  data->len = sst_contents.size();

  return kSuccess;
}

void DBSstFileWriterClose(DBSstFileWriter* fw) { delete fw; }

namespace {

class CockroachKeyFormatter : public rocksdb::SliceFormatter {
  std::string Format(const rocksdb::Slice& s) const {
    char* p = prettyPrintKey(ToDBKey(s));
    std::string ret(p);
    free(static_cast<void*>(p));
    return ret;
  }
};

}  // unnamed namespace

void DBRunLDB(int argc, char** argv) {
  rocksdb::Options options = DBMakeOptions(DBOptions());
  rocksdb::LDBOptions ldb_options;
  ldb_options.key_formatter.reset(new CockroachKeyFormatter);
  rocksdb::LDBTool tool;
  tool.Run(argc, argv, options, ldb_options);
}

const rocksdb::Comparator* CockroachComparator() { return &kComparator; }

rocksdb::WriteBatch::Handler* GetDBBatchInserter(::rocksdb::WriteBatchBase* batch) {
  return new DBBatchInserter(batch);
}

DBStatus DBLockFile(DBSlice filename, DBFileLock* lock) {
  return ToDBStatus(
      rocksdb::Env::Default()->LockFile(ToString(filename), (rocksdb::FileLock**)lock));
}

DBStatus DBUnlockFile(DBFileLock lock) {
  return ToDBStatus(rocksdb::Env::Default()->UnlockFile((rocksdb::FileLock*)lock));
}
