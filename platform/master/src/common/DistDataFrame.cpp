/********************************************************************
 *A scalable and high-performance platform for R.
 *Copyright (C) [2013] Hewlett-Packard Development Company, L.P.

 *This program is free software; you can redistribute it and/or modify
 *it under the terms of the GNU General Public License as published by
 *the Free Software Foundation; either version 2 of the License, or (at
 *your option) any later version.

 *This program is distributed in the hope that it will be useful, but
 *WITHOUT ANY WARRANTY; without even the implied warranty of
 *MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *General Public License for more details.  You should have received a
 *copy of the GNU General Public License along with this program; if
 *not, write to the Free Software Foundation, Inc., 59 Temple Place,
 *Suite 330, Boston, MA 02111-1307 USA
 ********************************************************************/

#include "common.h"
#include "DistDataFrame.h"
#include "Rdefines.h"

namespace presto{

// to keep track of SEXP object that is malloced by Presto malloc_hook
extern map<void*, size_t> *freemap;

/** DistDataFrame constructor. It creates a shared memory segment object. It does not reserve space yet.
 * @param name the name of shared memory segment in the shared memory region
 */
DistDataFrame::DistDataFrame(const std::string &name, StorageLayer store) 
  : ArrayData(name, DATA_FRAME, store, 0), header(NULL) {
  if(store == WORKER) {
    OpenShm(false);
    header_region = new boost::interprocess::mapped_region(*shm,
        boost::interprocess::read_write);
    header = reinterpret_cast<dframe_header_t*>(header_region->get_address());
  }
}

/* Create shared memory segment and fill input R object into it. This function gets assigned memory on the shared memory
 * @param name the name of shared memory segment of this array
 * @param sexp_from a R object where we will read the value from. The value will be written into the shared memory region. The data frame object should be serialized.
 * @param size the size of serialized data frame to be written
 */
DistDataFrame::DistDataFrame(const std::string &name, StorageLayer store, size_t r_size,
                             const SEXP sexp_from, size_t size) 
  : ArrayData(name, DATA_FRAME, store, r_size), header(NULL) {

  if(store == WORKER) {
  // Get dimensions, create array
  if (Rf_isNull(sexp_from)) {
    throw PrestoWarningException("DistDataFrame: updated value of split is NULL");
  }

  // the size of input array (header + data size)
  size_t m_size = mapped_size(size);
  if (m_size < INMEM_UPDATE_SIZE_LIMIT) {
    OpenShm(false);
  } else {
    OpenShm(true);
    LOG_DEBUG("DistDataFrame: creating data frame in external");
  }

  shm->truncate(mapped_size(sizeof(*header)) + m_size);  // allocate the size in the shared memory region
  header_region = new boost::interprocess::mapped_region(*shm, boost::interprocess::read_write);
  // header part
  header = reinterpret_cast<dframe_header_t*>(header_region->get_address());
  header->type = DATA_FRAME;
  header->size = size;
  header->dims[0] = 0;  // We do not check the data frame # row, # col
  header->dims[1] = 0;
  header->store = store;
  void* data = reinterpret_cast<unsigned char*>(header) + mapped_size(sizeof(*header));
  if (data == NULL) {
    throw PrestoWarningException("DistDataFrame: data writable to shm is NULL");
  }
  memcpy(data, RAW_POINTER(sexp_from), size);
  } else {
    header = new dframe_header_t;
    header->type = DATA_FRAME;
    header->size = r_size;
    header->dims[0] = 0;
    header->dims[1] = 0;
    header->store = store;
  }
}

/** Get dimension of the dense array data (number of rows/columns)
 * @return a pair of number of rows/columns
 */
pair<std::int64_t, std::int64_t> DistDataFrame::GetDims() const {
  return make_pair(header->dims[0], header->dims[1]);
}

/** Load shared memory segments into R-session
 * @param R R-session object
 * @param varname a name of variable in R-session. The shared memory segment value will be written into the varname variable in R-session.
 * @return NULL
 */
void DistDataFrame::LoadInR(RInside &R, const std::string &varname){
  // create an array in R-session
  SEXP arr = PrestoCreateVector(RAWSXP, header->size);
  if (Rf_isNull(arr)) {
    throw PrestoWarningException("DistDataFrame::LoadInR: array is NULL");
  }
  // freemap is filled in malloc_hook
  if (freemap->find(arr) != freemap->end()) {
    // R used malloc and we intercepted.
    // Overwrite the array pointer in R-session.
    createmapping(*shm, mapped_size(sizeof(*header)), header->size, RAW(arr));
  } else {
    // R didn't use malloc
    LOG_DEBUG("DistDataFrame::Loading shm to R session: R didnt malloc for size %zu", header->size);
    // Need to copy data
    std::pair<void*, size_t> array_region = createmapping(*shm,
      mapped_size(sizeof(*header)), header->size, 0);
    memcpy(RAW(arr), array_region.first, header->size);
    munmap(array_region.first, array_region.second);
  }
  Rcpp::Language unserialize_call("unserialize", arr);
  SEXP dframe;
  PROTECT(dframe = Rf_eval(unserialize_call, R_GlobalEnv));
  R[varname] = dframe;  // assign the varname with corresponding value in R-session
  UNPROTECT(2);
}

DistDataFrame::~DistDataFrame(){
  if(store = WORKER) {
    delete header_region;
  } else {
    delete header;
  }
}
}
