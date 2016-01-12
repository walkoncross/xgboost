/*!
 * Copyright 2015 by Contributors
 * \file sparse_page_source.cc
 */
#include <dmlc/base.h>
#include <dmlc/timer.h>
#include <xgboost/logging.h>
#include <memory>
#include "./sparse_page_source.h"

namespace xgboost {
namespace data {

SparsePageSource::SparsePageSource(const std::string& cache_prefix)
    : base_rowid_(0), page_(nullptr) {
  // read in the info files.
  {
    std::string name_info = cache_prefix;
    std::unique_ptr<dmlc::Stream> finfo(dmlc::Stream::Create(name_info.c_str(), "r"));
    int tmagic;
    CHECK_EQ(finfo->Read(&tmagic, sizeof(tmagic)), sizeof(tmagic));
    this->info.LoadBinary(finfo.get());
  }
  // read in the cache files.
  std::string name_row = cache_prefix + ".row.page";
  fi_.reset(dmlc::SeekStream::CreateForRead(name_row.c_str()));
  prefetcher_.Init([this] (SparsePage** dptr) {
      if (*dptr == nullptr) {
        *dptr = new SparsePage();
      }
      return (*dptr)->Load(fi_.get());
    }, [this] () { fi_->Seek(0); });
}

SparsePageSource::~SparsePageSource() {
  delete page_;
}

bool SparsePageSource::Next() {
  if (page_ != nullptr) {
    prefetcher_.Recycle(&page_);
  }
  if (prefetcher_.Next(&page_)) {
    batch_ = page_->GetRowBatch(base_rowid_);
    base_rowid_ += batch_.size;
    return true;
  } else {
    return false;
  }
}

void SparsePageSource::BeforeFirst() {
  base_rowid_ = 0;
  prefetcher_.BeforeFirst();
}

const RowBatch& SparsePageSource::Value() const {
  return batch_;
}

bool SparsePageSource::CacheExist(const std::string& cache_prefix) {
  std::string name_info = cache_prefix;
  std::string name_row = cache_prefix + ".row.page";
  std::unique_ptr<dmlc::Stream> finfo(dmlc::Stream::Create(name_info.c_str(), "r", true));
  std::unique_ptr<dmlc::Stream> frow(dmlc::Stream::Create(name_row.c_str(), "r", true));
  return finfo.get() != nullptr && frow.get() != nullptr;
}

void SparsePageSource::Create(dmlc::Parser<uint32_t>* src,
                              const std::string& cache_prefix) {
  // read in the info files.
  std::string name_info = cache_prefix;
  std::string name_row = cache_prefix + ".row.page";
  std::unique_ptr<dmlc::Stream> fo(dmlc::Stream::Create(name_row.c_str(), "w"));
  MetaInfo info;
  SparsePage page;
  size_t bytes_write = 0;
  double tstart = dmlc::GetTime();

  while (src->Next()) {
    const dmlc::RowBlock<uint32_t>& batch = src->Value();
    if (batch.label != nullptr) {
      info.labels.insert(info.labels.end(), batch.label, batch.label + batch.size);
    }
    if (batch.weight != nullptr) {
      info.weights.insert(info.weights.end(), batch.weight, batch.weight + batch.size);
    }
    info.num_row += batch.size;
    info.num_nonzero +=  batch.offset[batch.size] - batch.offset[0];
    for (size_t i = batch.offset[0]; i < batch.offset[batch.size]; ++i) {
      uint32_t index = batch.index[i];
      info.num_col = std::max(info.num_col,
                              static_cast<size_t>(index + 1));
    }
    page.Push(batch);
    if (page.MemCostBytes() >= kPageSize) {
      bytes_write += page.MemCostBytes();
      page.Save(fo.get());
      page.Clear();
      double tdiff = dmlc::GetTime() - tstart;
      LOG(CONSOLE) << "Writing to " << name_row << " in "
                   << ((bytes_write >> 20UL) / tdiff) << " MB/s, "
                   << (bytes_write >> 20UL) << " written";
    }
  }

  if (page.data.size() != 0) {
    page.Save(fo.get());
  }

  fo.reset(dmlc::Stream::Create(name_info.c_str(), "w"));
  int tmagic = kMagic;
  fo->Write(&tmagic, sizeof(tmagic));
  info.SaveBinary(fo.get());

  LOG(CONSOLE) << "SparsePageSource: Finished writing to " << cache_prefix;
}

void SparsePageSource::Create(DMatrix* src,
                              const std::string& cache_prefix) {
  // read in the info files.
  std::string name_info = cache_prefix;
  std::string name_row = cache_prefix + ".row.page";
  std::unique_ptr<dmlc::Stream> fo(dmlc::Stream::Create(name_row.c_str(), "w"));

  SparsePage page;
  size_t bytes_write = 0;
  double tstart = dmlc::GetTime();
  dmlc::DataIter<RowBatch>* iter = src->RowIterator();

  while (iter->Next()) {
    page.Push(iter->Value());
    if (page.MemCostBytes() >= kPageSize) {
      bytes_write += page.MemCostBytes();
      page.Save(fo.get());
      page.Clear();
      double tdiff = dmlc::GetTime() - tstart;
      LOG(CONSOLE) << "Writing to " << name_row << " in "
                   << ((bytes_write >> 20UL) / tdiff) << " MB/s, "
                   << (bytes_write >> 20UL) << " written";
    }
  }

  if (page.data.size() != 0) {
    page.Save(fo.get());
  }

  fo.reset(dmlc::Stream::Create(name_info.c_str(), "w"));
  int tmagic = kMagic;
  fo->Write(&tmagic, sizeof(tmagic));
  src->info().SaveBinary(fo.get());

  LOG(CONSOLE) << "SparsePageSource: Finished writing to " << cache_prefix;
}

}  // namespace data
}  // namespace xgboost
