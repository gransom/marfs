#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <attr/xattr.h>
#include <sys/types.h>

#include <thread_queue.h>

#include "config/config.h"
#include "datastream/datastream.h"
#include "mdal/mdal.h"
#include "tagging/tagging.h"

#define DEBUG_RM 1 // TODO: Remove
#if defined(DEBUG_ALL)  ||  defined(DEBUG_RM)
#define DEBUG 1
#endif

#define LOG_PREFIX "rsrc_mgr"

#include <logging.h>

#define PROGNAME "marfs-rsrc_mgr"
#define OUTPREFX PROGNAME ": "

// ENOATTR isn't always defined in Linux, so define it
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

// Files created within the threshold will be ignored
#define RECENT_THRESH 0

#define N_SKIP "gc_skip"
#define IN_PROG "IN_PROG"
#define ZERO_FLAG 'z'

// Default TQ values
#define NUM_PROD 2
#define NUM_CONS 2
#define QDEPTH 100

int rank;
int n_ranks;

int n_prod = NUM_PROD;
int n_cons = NUM_CONS;

int del = 0;

typedef struct quota_data_struct {
  size_t size;
  size_t count;
  size_t del_objs;
  size_t del_refs;
} quota_data;

typedef struct tq_global_struct {
  pthread_mutex_t lock;
  marfs_ms* ms;
  marfs_ds* ds;
  MDAL_CTXT ctxt;
  size_t next_node;
} *GlobalState;

typedef struct tq_thread_struct {
  unsigned int tID;
  GlobalState global;
  quota_data q_data;
  HASH_NODE* ref_node;
  MDAL_SCANNER scanner;
} *ThreadState;

typedef struct work_pkg_struct {
  char* node_name; // Name of ref dir
  char* ref_name; // Name of ref file
} *WorkPkg;

/** Garbage collect the reference files and objects of a stream inside the given
 * ranges. NOTE: refs/objects corresponding to the index parameters will not be
 * garbage collected.
 * Ex:  gc(..., 1, 5, 0, 2, ...) will delete refs 2-4 and obj 1
 * @param const marfs_ms* ms : Reference to the current MarFS metadata scheme
 * @param const marfs_ds* ds : Reference to the current MarFS data scheme
 * @param MDAL_CTXT ctxt : Current MDAL_CTXT, associated with the target
 * namespace
 * @param FTAG ftag : A FTAG value within the stream.
 * @param int v_ref : Index of the last valid ref before the range of refs to
 * collect.
 * @param int head_ref : Starting index of the range of refs to collect. Must be
 * >= v_ref.
 * @param int tail_ref : Ending index of the range of refs to collect.
 * @param int first_obj : Starting index of the range of objs to collect.
 * @param int curr_obj : Ending index of the range of refs to collect.
 * @param int eos : Flag indicating if the given ranges are at the end of the
 * stream.
 * @param quota data* q_data : Reference to the quota data struct to populate
 * @return int : Zero on success, or -1 on failure.
 */
int gc(const marfs_ms* ms, const marfs_ds* ds, MDAL_CTXT ctxt, FTAG ftag, int v_ref, int head_ref, int tail_ref, int first_obj, int curr_obj, int eos, quota_data* q_data) {
  //printf("garbage collecting refs (%d) %d:%d objs %d:%d eos(%d)\n", v_ref, head_ref, tail_ref, first_obj, curr_obj, eos);

  if (head_ref < v_ref) {
    LOG(LOG_ERR, "Rank %d: Invalid ref ranges given\n");
    return -1;
  }

  // If we are supposed to GC ref 0, we will add an indicator to the skip xattr
  int del_zero = 0;
  if (head_ref < 0) {
    del_zero = 1;
  }

  // Calculate value for xattr indicating how many refs will need to be skipped
  // on future runs
  int skip = -1;
  if (!eos) {
    if (head_ref < 0) {
      head_ref = 0;
    }
    if (v_ref < 0) {
      v_ref = 0;
    }
    skip = tail_ref - v_ref - 1;
  }

  if (!del) {
    if (first_obj != curr_obj) {
      q_data->del_objs += curr_obj - (first_obj + 1);
    }
    q_data->del_refs += tail_ref - (head_ref + 1);
    return 0;
  }

  // Determine if the specified refs/objs have already been deleted
  char* rpath;
  ftag.fileno = v_ref;
  if (v_ref < 0) {
    ftag.fileno = 0;
  }

  if ((rpath = datastream_genrpath(&ftag, ms)) == NULL) {
    LOG(LOG_ERR, "Rank %d: Failed to create rpath\n", rank);
    return -1;
  }

  MDAL_FHANDLE handle;
  if ((handle = ms->mdal->openref(ctxt, rpath, O_RDWR, 0)) == NULL) {
    LOG(LOG_ERR, "Rank %d: Failed to open handle for reference path \"%s\"\n", rank, rpath);
    free(rpath);
    return -1;
  }

  char* xattr = NULL;
  int xattr_len = ms->mdal->fgetxattr(handle, 1, N_SKIP, xattr, 0);
  if (xattr_len <= 0 && errno != ENOATTR) {
    LOG(LOG_ERR, "Rank %d: Failed to retrieve xattr skip for reference path \"%s\"\n", rank, rpath);
    ms->mdal->close(handle);
    free(rpath);
    return -1;
  }
  else if (xattr_len >= 0) {
    xattr = calloc(0, xattr_len + 1);
    if (xattr == NULL) {
      LOG(LOG_ERR, "Rank %d: Failed to allocate space for a xattr string value\n", rank);
      ms->mdal->close(handle);
      free(rpath);
      return -1;
    }

    if (ms->mdal->fgetxattr(handle, 1, N_SKIP, xattr, 0) != xattr_len) {
      LOG(LOG_ERR, "Rank %d: xattr skip value for \"%s\" changed while reading\n", rank, rpath);
      ms->mdal->close(handle);
      free(xattr);
      free(rpath);
      return -1;
    }

    // The same deletion has already been completed, no need to repeat
    if (strtol(xattr, NULL, 10) == skip) {
      ms->mdal->close(handle);
      free(rpath);
      return 0;
    }

    free(xattr);
  }

  // Delete the specified objects
  int i;
  char* objname;
  ne_erasure erasure;
  ne_location location;
  for (i = first_obj + 1; i < curr_obj; i++) {
    ftag.objno = i;
    if (datastream_objtarget(&ftag, ds, &objname, &erasure, &location)) {
      LOG(LOG_ERR, "Rank %d: Failed to generate object name\n", rank);
      free(xattr);
      free(rpath);
      return -1;
    }

    LOG(LOG_INFO, "Rank %d: Garbage collecting object %d %s\n", rank, i, objname);
    errno = 0;
    if (ne_delete(ds->nectxt, objname, location)) {
      if (errno != ENOENT) {
        LOG(LOG_ERR, "Rank %d: Failed to delete \"%s\" (%s)\n", rank, objname, strerror(errno));
        free(objname);
        free(xattr);
        free(rpath);
        return -1;
      }
    }
    else {
      q_data->del_objs++;
    }

    free(objname);
  }

  if (tail_ref <= head_ref + 1 && !del_zero) {
    return 0;
  }

  // Set temporary xattr indicating we started deleting the specified refs
  if (eos) {
    xattr_len = 4 + strlen(IN_PROG);
  }
  else if (skip == 0) {
    xattr_len = 3 + strlen(IN_PROG);
  }
  else {
    xattr_len = (int)log10(skip) + 3 + strlen(IN_PROG);
  }
  xattr_len += del_zero;

  if ((xattr = malloc(sizeof(char) * xattr_len)) == NULL) {
    LOG(LOG_ERR, "Rank %d: Failed to allocate xattr string\n", rank);
    free(rpath);
    return -1;
  }

  if (del_zero) {
    if (snprintf(xattr, xattr_len, "%s %d%c", IN_PROG, skip, ZERO_FLAG) != (xattr_len - 1)) {
      LOG(LOG_ERR, "Rank %d: Failed to populate rpath string\n", rank);
      free(xattr);
      free(rpath);
      return -1;
    }
  }
  else {
    if (snprintf(xattr, xattr_len, "%s %d", IN_PROG, skip) != (xattr_len - 1)) {
      LOG(LOG_ERR, "Rank %d: Failed to populate rpath string\n", rank);
      free(xattr);
      free(rpath);
      return -1;
    }
  }

  if (tail_ref > head_ref + 1) {
    if (ms->mdal->fsetxattr(handle, 1, N_SKIP, xattr, xattr_len, 0)) {
      LOG(LOG_ERR, "Rank %d: Failed to set temporary xattr string\n", rank);
      ms->mdal->close(handle);
      free(xattr);
      free(rpath);
      return -1;
    }

    if (ms->mdal->close(handle)) {
      LOG(LOG_ERR, "Rank %d: Failed to close handle\n", rank);
      free(xattr);
      free(rpath);
      return -1;
    }

    // Delete specified refs
    char* dpath;
    for (i = head_ref + 1; i < tail_ref; i++) {
      ftag.fileno = i;
      if ((dpath = datastream_genrpath(&ftag, ms)) == NULL) {
        LOG(LOG_ERR, "Rank %d: Failed to create dpath\n", rank);
        free(xattr);
        free(rpath);
        return -1;
      }

      LOG(LOG_INFO, "Rank %d: Garbage collecting ref %d\n", rank, i);
      errno = 0;
      if (ms->mdal->unlinkref(ctxt, dpath)) {
        if (errno != ENOENT) {
          LOG(LOG_ERR, "Rank %d: failed to unlink \"%s\" (%s)\n", rank, dpath, strerror(errno));
          free(dpath);
          free(xattr);
          free(rpath);
          return -1;
        }
      }
      else {
        q_data->del_refs++;
      }

      free(dpath);
    }

    if (head_ref < 0 && eos) {
      free(xattr);
      free(rpath);
      return 0;
    }

    // Rewrite xattr indicating that we finished the deletion
    if ((handle = ms->mdal->openref(ctxt, rpath, O_WRONLY, 0)) == NULL) {
      LOG(LOG_ERR, "Rank %d: Failed to open handle for reference path \"%s\" (%s)\n", rank, rpath, strerror(errno));
      free(xattr);
      free(rpath);
      return -1;
    }
  }

  xattr_len -= strlen(IN_PROG) + 1;
  xattr += strlen(IN_PROG) + 1;

  if (ms->mdal->fsetxattr(handle, 1, N_SKIP, xattr, xattr_len, 0)) {
    LOG(LOG_ERR, "Rank %d: Failed to set xattr string (%s)\n", rank, strerror(errno));
    ms->mdal->close(handle);
    free(xattr - strlen(IN_PROG) - 1);
    free(rpath);
    return -1;
  }

  if (ms->mdal->close(handle)) {
    LOG(LOG_ERR, "Rank %d: Failed to close handle\n", rank);
    free(xattr - strlen(IN_PROG) - 1);
    free(rpath);
    return -1;
  }

  free(xattr - strlen(IN_PROG) - 1);
  free(rpath);

  return 0;
}

/** Populate the given ftag struct based on the given reference path. If a
 * previous run of the resource manager failed while in the process of garbage
 * collecting the following reference in the stream, retry this garbage
 * collection.
 * @param const marfs_ms* ms : Reference to the current MarFS metadata scheme
 * @param const marfs_ds* ds : Reference to the current MarFS data scheme
 * @param MDAL_CTXT ctxt : Current MDAL_CTXT, associated with the target
 * namespace
 * @param FTAG* ftag : Reference to the FTAG struct to be populated
 * @param const char* rpath : Reference path of the file to retrieve ftag data
 * from
 * @param quota_data* q_data : Reference to the quota data struct to update
 * @param int* del_zero : Reference to flag to be set in case zero was already
 * deleted
 * @return int : The distance to the next active reference within the stream on
 * success, or -1 if a failure occurred.
 * Ex: If the following ref has not been garbage collected, return 1. If the
 * following two refs have already been garbage collected, return 3.
 */
int get_ftag(const marfs_ms* ms, const marfs_ds* ds, MDAL_CTXT ctxt, FTAG* ftag, const char* rpath, struct stat* st, quota_data* q_data, int* del_zero) {
  MDAL_FHANDLE handle;

  if (ms->mdal->statref(ctxt, rpath, st)) {
    LOG(LOG_ERR, "Rank %d: Failed to stat \"%s\" rpath\n", rank, rpath);
    return -1;
  }

  // Get ftag string from xattr
  if ((handle = ms->mdal->openref(ctxt, rpath, O_RDONLY, 0)) == NULL) {
    LOG(LOG_ERR, "Rank %d: Failed to open handle for reference path \"%s\"\n", rank, rpath);
    return -1;
  }

  char* ftagstr = NULL;
  int ftagsz;
  if ((ftagsz = ms->mdal->fgetxattr(handle, 1, FTAG_NAME, ftagstr, 0)) <= 0) {
    LOG(LOG_ERR, "Rank %d: Failed to retrieve FTAG value for reference path \"%s\"\n", rank, rpath);
    ms->mdal->close(handle);
    return -1;
  }

  ftagstr = calloc(1, ftagsz + 1);
  if (ftagstr == NULL) {
    LOG(LOG_ERR, "Rank %d: Failed to allocate space for a FTAG string value\n", rank);
    ms->mdal->close(handle);
    return -1;
  }

  if (ms->mdal->fgetxattr(handle, 1, FTAG_NAME, ftagstr, ftagsz) != ftagsz) {
    LOG(LOG_ERR, "Rank %d: FTAG value for \"%s\" changed while reading \n", rank, rpath);
    free(ftagstr);
    ms->mdal->close(handle);
    return -1;
  }

  // Populate ftag with values from string
  if (ftag->ctag) {
    free(ftag->ctag);
  }
  if (ftag->streamid) {
    free(ftag->streamid);
  }
  if (ftag_initstr(ftag, ftagstr)) {
    LOG(LOG_ERR, "Rank %d: Failed to parse FTAG string for \"%s\"\n", rank, rpath);
    free(ftagstr);
    return -1;
  }

  free(ftagstr);

  // Look if there is an xattr indicating the following ref(s) was deleted
  char* skipstr = NULL;
  int skipsz;
  int skip = 0;
  if ((skipsz = ms->mdal->fgetxattr(handle, 1, N_SKIP, skipstr, 0)) > 0) {

    skipstr = calloc(0, skipsz + 1);
    if (skipstr == NULL) {
      LOG(LOG_ERR, "Rank %d: Failed to allocate space for a skip string value\n", rank);
      ms->mdal->close(handle);
      return -1;
    }

    if (ms->mdal->fgetxattr(handle, 1, N_SKIP, skipstr, skipsz) != skipsz) {
      LOG(LOG_ERR, "Rank %d: skip value for \"%s\" changed while reading \n", rank, rpath);
      free(skipstr);
      ms->mdal->close(handle);
      return -1;
    }

    char* end;
    errno = 0;
    // If the following ref(s) were deleted, note we need to skip them
    skip = strtol(skipstr, &end, 10);
    if (errno) {
      LOG(LOG_ERR, "Rank %d: Failed to parse skip value for \"%s\" (%s)", rank, rpath, strerror(errno));
      free(skipstr);
      ms->mdal->close(handle);
      return -1;
    }
    else if (end == skipstr) {
      int eos = 0;
      // Check if we started gc'ing the following ref(s), but got interrupted.
      // If so, try again
      skip = strtol(skipstr + strlen(IN_PROG), &end, 10);
      if (errno || (end == skipstr) || (skip == 0)) {
        LOG(LOG_ERR, "Rank %d: Failed to parse skip value for \"%s\" (%s)", rank, rpath, strerror(errno));
        free(skipstr);
        ms->mdal->close(handle);
        return -1;
      }


      if (skip < 0) {
        skip *= -1;
        eos = 1;
      }

      gc(ms, ds, ctxt, *ftag, ftag->fileno, ftag->fileno, ftag->fileno + skip + 1, -1, -1, eos, q_data);

      if (eos) {
        skip = -1;
      }
    }

    *del_zero = (*end == ZERO_FLAG);

    free(skipstr);

    if (skip == 0 && !(*del_zero)) {
      LOG(LOG_ERR, "Rank %d: Invalid skip value\n", rank);
      ms->mdal->close(handle);
      return -1;
    }

  }

  if (ms->mdal->close(handle)) {
    LOG(LOG_ERR, "Rank %d: Failed to close handle for reference path \"%s\"\n", rank, rpath);
  }

  return skip + 1;
}

/** Determine the number of the object a given file ends in
 * @param FTAG* ftag : Reference to the ftag struct of the file
 * @param size_t headerlen : Length of the file header. NOTE: all files within a
 * stream have the same header length, so this value is calculated beforehand
 * @return int : The object number
 */
int end_obj(FTAG* ftag, size_t headerlen) {
  size_t dataperobj = ftag->objsize - (headerlen + ftag->recoverybytes);
  size_t finobjbounds = (ftag->bytes + ftag->offset - headerlen) / dataperobj;
  // special case check
  if ((ftag->state & FTAG_DATASTATE) >= FTAG_FIN && finobjbounds && (ftag->bytes + ftag->offset - headerlen) % dataperobj == 0) {
    // if we exactly align to object bounds AND the file is FINALIZED,
    //   we've overestimated by one object
    finobjbounds--;
  }

  return ftag->objno + finobjbounds;
}

/** Walks the stream that starts at the given reference file, garbage collecting
 * inactive references/objects and collecting quota data
 * @param const marfs_ms* ms : Reference to the current MarFS metadata scheme
 * @param const marfs_ds* ds : Reference to the current MarFS data scheme
 * @param MDAL_CTXT ctxt : Current MDAL_CTXT, associated with the target
 * namespace
 * @param const char* node_name : Name of the reference directory containing the
 * start of the stream
 * @param const char* ref_name : Name of the starting reference file within the
 * stream
 * @param quota data* q_data : Reference to the quota data struct to populate
 * @return int : Zero on success, or -1 on failure.
 */
int walk_stream(const marfs_ms* ms, const marfs_ds* ds, MDAL_CTXT ctxt, const char* node_name, const char* ref_name, quota_data* q_data) {
  q_data->size = 0;
  q_data->count = 0;
  q_data->del_objs = 0;
  q_data->del_refs = 0;

  // Generate an ftag for the beginning of the stream
  size_t rpath_len = strlen(node_name) + strlen(ref_name) + 1;
  char* rpath = malloc(sizeof(char) * rpath_len);
  if (rpath == NULL) {
    LOG(LOG_ERR, "Rank %d: Failed to allocate rpath string\n", rank);
    return -1;
  }

  if (snprintf(rpath, rpath_len, "%s%s", node_name, ref_name) != (rpath_len - 1)) {
    LOG(LOG_ERR, "Rank %d: Failed to populate rpath string\n", rank);
    free(rpath);
    return -1;
  }

  FTAG ftag;
  ftag.fileno = 0;
  ftag.ctag = NULL;
  ftag.streamid = NULL;
  struct stat st;
  int del_zero = 0;
  int next = get_ftag(ms, ds, ctxt, &ftag, rpath, &st, q_data, &del_zero);
  if (next < 0) {
    LOG(LOG_ERR, "Rank %d: Failed to retrieve FTAG for \"%s\"\n", rank, rpath);
    free(rpath);
    return -1;
  }

  // Calculate the header size (constant throughout the stream)
  RECOVERY_HEADER header = {
    .majorversion = RECOVERY_CURRENT_MAJORVERSION,
    .minorversion = RECOVERY_CURRENT_MINORVERSION,
    .ctag = ftag.ctag,
    .streamid = ftag.streamid
  };
  size_t headerlen;
  if ((headerlen = recovery_headertostr(&(header), NULL, 0)) < 1) {
    LOG(LOG_ERR, "Rank %d: Failed to identify recovery header length for final file\n", rank);
    headerlen = 0;
  }

  // Iterate through the refs in the stream, gc'ing/ counting quota data
  int inactive = 0;
  int ignore_zero = 0;
  int last_ref = -1;
  int v_ref = -1;
  int last_obj = -1;
  while (ftag.endofstream == 0 && next > 0 && (ftag.state & FTAG_DATASTATE) >= FTAG_FIN) {
    if (difftime(time(NULL), st.st_ctime) > RECENT_THRESH) {
      if (st.st_nlink < 2) {
        // File has been deleted (needs to be gc'ed)
        if ((!inactive || ignore_zero) && ftag.objno > last_obj + 1) {
          last_obj = ftag.objno - 1;
        }
        if (!inactive) {
          if (last_ref >= 0 && ftag.fileno > last_ref + 1) {
            last_ref = ftag.fileno - 1;
          }
          inactive = 1;
        }
        if (ftag.fileno == 0 && del_zero) {
          ignore_zero = 1;
        }
        else {
          ignore_zero = 0;
        }
      }
      else {
        // File is active, so count towards quota and gc previous files if needed
        if (inactive && !ignore_zero) {
          gc(ms, ds, ctxt, ftag, v_ref, last_ref, ftag.fileno, last_obj, ftag.objno, 0, q_data);
        }
        inactive = 0;
        ignore_zero = 0;
        q_data->size += st.st_size;
        q_data->count++;
        last_ref = ftag.fileno;
        v_ref = ftag.fileno;
        last_obj = end_obj(&ftag, headerlen);
      }
    }
    else {
      // File is too young to be gc'ed/counted, but still gc previous files if
      // needed, and treat as 'active' for future gc logic
      if (inactive && !ignore_zero) {
        gc(ms, ds, ctxt, ftag, v_ref, last_ref, ftag.fileno, last_obj, ftag.objno, 0, q_data);
      }
      inactive = 0;
      ignore_zero = 0;
      last_ref = ftag.fileno;
      v_ref = ftag.fileno;
      last_obj = end_obj(&ftag, headerlen);
    }

    ftag.fileno += next;

    // Generate path of next (existing) ref in the stream
    free(rpath);
    if ((rpath = datastream_genrpath(&ftag, ms)) == NULL) {
      LOG(LOG_ERR, "Rank %d: Failed to create rpath\n", rpath);
      free(ftag.ctag);
      free(ftag.streamid);
      return -1;
    }

    if ((next = get_ftag(ms, ds, ctxt, &ftag, rpath, &st, q_data, &del_zero)) < 0) {
      LOG(LOG_ERR, "Rank %d: Failed to retrieve ftag for %s\n", rank, rpath);
      free(rpath);
      free(ftag.ctag);
      free(ftag.streamid);
      return -1;
    }
  }

  // Repeat process in loop, but for last ref in stream
  if (difftime(time(NULL), st.st_ctime) > RECENT_THRESH) {
    if (st.st_nlink < 2) {
      // Since this is the last ref in the stream, gc it along with previous
      // refs if needed
      if (!inactive) {
        if (ftag.objno > last_obj + 1) {
          last_obj = ftag.objno - 1;
        }
        if (last_ref >= 0 && ftag.fileno > last_ref + 1) {
          last_ref = ftag.fileno - 1;
        }
      }

      gc(ms, ds, ctxt, ftag, v_ref, last_ref, ftag.fileno + 1, last_obj, end_obj(&ftag, headerlen) + 1, 1, q_data);
    }
    else {
      // Same as above
      if (inactive && !ignore_zero) {
        gc(ms, ds, ctxt, ftag, v_ref, last_ref, ftag.fileno, last_obj, ftag.objno, 0, q_data);
      }
      q_data->size += st.st_size;
      q_data->count++;
    }
  }
  else {
    if (inactive && !ignore_zero) {
      // Same as above
      gc(ms, ds, ctxt, ftag, v_ref, last_ref, ftag.fileno, last_obj, ftag.objno, 0, q_data);
    }
  }

  free(rpath);
  free(ftag.ctag);
  free(ftag.streamid);

  return 0;
}

/** Initialize the state for a thread in the thread queue
 * @param unsigned int tID : The ID of this thread
 * @param void* global_state : Reference to the thread queue's global state
 * @param void** state : Reference to be populated with this thread's state
 * @return int : Zero on success, or -1 on failure.
 */
int stream_thread_init(unsigned int tID, void* global_state, void** state) {
  *state = malloc(sizeof(struct tq_thread_struct));
  if (!*state) {
    return -1;
  }
  ThreadState tstate = ((ThreadState)*state);

  tstate->tID = tID;
  tstate->global = (GlobalState)global_state;
  tstate->q_data.size = 0;
  tstate->q_data.count = 0;
  tstate->q_data.del_objs = 0;
  tstate->q_data.del_refs = 0;
  tstate->ref_node = NULL;
  tstate->scanner = NULL;

  return 0;
}

/** Consume a work package containing the reference file at the start of the
 * stream, and walk the stream
 * @param void** state : Reference to the consumer thread's state
 * @param void** work_todo : Reference to the work package containing the
 * reference file's path
 * @return int : Zero on success, or -1 on failure.
 */
int stream_cons(void** state, void** work_todo) {
  ThreadState tstate = ((ThreadState)*state);
  WorkPkg work = ((WorkPkg)*work_todo);

  if (!(work->node_name) || !(work->ref_name)) {
    LOG(LOG_ERR, "Rank %d: Thread %u received invalid work package\n", rank, tstate->tID);
    return -1;
  }

  // Walk the stream for the given ref
  // NOTE: It might be better to combine this function with walk_stream(), but I
  // left things as-is to still leave the interface for walking a stream easily
  // exposed, in case we ever decide to walk streams with something other than
  // consumer threads
  quota_data q_data;
  if (walk_stream(tstate->global->ms, tstate->global->ds, tstate->global->ctxt, work->node_name, work->ref_name, &q_data)) {
    LOG(LOG_ERR, "Rank %d: Thread %u failed to walk stream\n", rank, tstate->tID);
    return -1;
  }

  free(work->node_name);
  free(work->ref_name);
  free(work);

  tstate->q_data.size += q_data.size;
  tstate->q_data.count += q_data.count;
  tstate->q_data.del_objs += q_data.del_objs;
  tstate->q_data.del_refs += q_data.del_refs;

  return 0;
}

/** Access the reference directory of a namespace, and scan it for reference
 * files corresponding to the start of a stream. Once a reference file is found,
 * create a work package for it and return. If the given reference directory is
 * fully scanned without finding the start of a stream, access a new reference
 * directory and repeat.
 * @param void** state : Reference to the producer thread's state
 * @param void** work_tofill : Reference to be populated with the work package
 * @return int : Zero on success, -1 on failure, and 1 once all reference
 * directories within the namespace have been accessed
 */
int stream_prod(void** state, void** work_tofill) {
  ThreadState tstate = ((ThreadState)*state);
  GlobalState gstate = tstate->global;

  WorkPkg work = malloc(sizeof(struct work_pkg_struct));
  if (!work) {
    LOG(LOG_ERR, "Rank %d: Failed to allocate new work package\n", rank);
    return -1;
  }
  work->node_name = NULL;
  work->ref_name = NULL;

  *work_tofill = (void*)work;

  // Search for the beginning of a stream
  while (1) {
    // Access a new ref dir, if we don't still have scanning to do in our
    // current dir
    if (!(tstate->ref_node)) {
      if (tstate->scanner) {
        gstate->ms->mdal->closescanner(tstate->scanner);
        tstate->scanner = NULL;
      }

      if (pthread_mutex_lock(&(gstate->lock))) {
        LOG(LOG_ERR, "Rank %d: Failed to acquire lock\n", rank);
        return -1;
      }

      if (gstate->next_node >= gstate->ms->refnodecount) {
        pthread_mutex_unlock(&(gstate->lock));
        return 1;
      }

      tstate->ref_node = &(gstate->ms->refnodes[gstate->next_node++]);

      pthread_mutex_unlock(&(gstate->lock));
    }

    if (!(tstate->scanner)) {
      struct stat statbuf;
      if (gstate->ms->mdal->statref(gstate->ctxt, tstate->ref_node->name, &statbuf)) {
        LOG(LOG_ERR, "Rank %d: Failed to stat %s\n", rank, tstate->ref_node->name);
        tstate->ref_node = NULL;
        continue;
      }

      tstate->scanner = gstate->ms->mdal->openscanner(gstate->ctxt, tstate->ref_node->name);
    }

    // Scan through the ref dir until we find the beginning of a stream
    struct dirent* dent;
    while ((dent = gstate->ms->mdal->scan(tstate->scanner))) {
      if (*dent->d_name != '.' && ftag_metatgt_fileno(dent->d_name) == 0) {

        work->node_name = strdup(tstate->ref_node->name);
        work->ref_name = strdup(dent->d_name);

        return 0;
      }
    }

    gstate->ms->mdal->closescanner(tstate->scanner);
    tstate->scanner = NULL;
    tstate->ref_node = NULL;
  }

  return -1;
}

/** No-op function, just to fill out the TQ struct
 */
int stream_pause(void** state, void** prev_work) {
  return 0;
}

/** No-op function, just to fill out the TQ struct
 */
int stream_resume(void** state, void** prev_work) {
  return 0;
}

/** Free a work package if it is still allocated
 * @param void** state : Reference to the thread's state
 * @param void** prev_work : Reference to a possibly-allocated work package
 * @param int flg : Current control flags of the thread
 */
void stream_term(void** state, void** prev_work, int flg) {
  WorkPkg work = ((WorkPkg)*prev_work);
  if (work) {

    if (work->node_name) {
      free(work->node_name);
    }
    if (work->ref_name) {
      free(work->ref_name);
    }
    free(work);
    *prev_work = NULL;
  }
}

/** Launch a thread queue to access all reference directories in a namespace,
 * performing garbage collection and updating quota data.
 * @param const marfs_ns* ns : Reference to the current MarFS namespace
 * @param const char* name : Name of the current MarFS namespace
 * @return int : Zero on success, or -1 on failure.
 */
int ref_paths(const marfs_ns* ns, const char* name) {
  marfs_ms* ms = &ns->prepo->metascheme;
  marfs_ds* ds = &ns->prepo->datascheme;

  // Initialize namespace context
  char* ns_path;
  if (config_nsinfo(ns->idstr, NULL, &ns_path)) {
    LOG(LOG_ERR, "Rank %d: Failed to retrieve path of NS: \"%s\"\n", rank, ns->idstr);
    return -1;
  }

  MDAL_CTXT ctxt;
  if ((ctxt = ms->mdal->newctxt(ns_path, ms->mdal->ctxt)) == NULL) {
    LOG(LOG_ERR, "Rank %d: Failed to create new MDAL context for NS: \"%s\"\n", rank, ns_path);
    free(ns_path);
    return -1;
  }

  // Initialize global state and launch threadqueue
  struct  tq_global_struct gstate;
  if (pthread_mutex_init(&(gstate.lock), NULL)) {
    LOG(LOG_ERR, "Rank %d: Failed to initialize TQ lock\n", rank);
    ms->mdal->destroyctxt(ctxt);
    free(ns_path);
    return -1;
  }

  gstate.ms = ms;
  gstate.ds = ds;
  gstate.ctxt = ctxt;
  gstate.next_node = 0;

  int ns_prod = n_prod;
  if (ms->refnodecount < n_prod) {
    ns_prod = ms->refnodecount;
  }

  TQ_Init_Opts tqopts;
  tqopts.log_prefix = ns_path;
  tqopts.init_flags = TQ_HALT;
  tqopts.global_state = (void*)&gstate;
  tqopts.num_threads = n_cons + ns_prod;
  tqopts.num_prod_threads = ns_prod;
  tqopts.max_qdepth = QDEPTH;
  tqopts.thread_init_func = stream_thread_init;
  tqopts.thread_consumer_func = stream_cons;
  tqopts.thread_producer_func = stream_prod;
  tqopts.thread_pause_func = stream_pause;
  tqopts.thread_resume_func = stream_resume;
  tqopts.thread_term_func = stream_term;

  ThreadQueue tq = tq_init(&tqopts);
  if (!tq) {
    LOG(LOG_ERR, "Rank %d: Failed to initialize tq\n", rank);
    pthread_mutex_destroy(&(gstate.lock));
    ms->mdal->destroyctxt(ctxt);
    free(ns_path);
    return -1;
  }

  // Wait until all threads are done executing
  TQ_Control_Flags flags = 0;
  while (!(flags & TQ_ABORT) && !(flags & TQ_FINISHED)) {
    if (tq_wait_for_flags(tq, 0, &flags)) {
      LOG(LOG_ERR, "Rank %d: NS %s failed to get TQ flags\n", rank, ns_path);
      pthread_mutex_destroy(&(gstate.lock));
      ms->mdal->destroyctxt(ctxt);
      free(ns_path);
      return -1;
    }

    // Thread queue should never halt, so just clear flag
    if (flags & TQ_HALT) {
      tq_unset_flags(tq, TQ_HALT);
    }
  }
  if (flags & TQ_ABORT) {
    LOG(LOG_ERR, "Rank %d: NS %s TQ aborted\n", rank, ns_path);
    pthread_mutex_destroy(&(gstate.lock));
    ms->mdal->destroyctxt(ctxt);
    free(ns_path);
    return -1;
  }

  // Aggregate quota data from all consumer threads
  quota_data totals;
  totals.size = 0;
  totals.count = 0;
  totals.del_objs = 0;
  totals.del_refs = 0;
  int tres = 0;
  ThreadState tstate = NULL;
  while ((tres = tq_next_thread_status(tq, (void**)&tstate)) > 0) {
    if (!tstate) {
      LOG(LOG_ERR, "Rank %d: NS %s received NULL status for thread\n", rank, ns_path);
      pthread_mutex_destroy(&(gstate.lock));
      ms->mdal->destroyctxt(ctxt);
      free(ns_path);
      return -1;
    }

    totals.size += tstate->q_data.size;
    totals.count += tstate->q_data.count;
    totals.del_objs += tstate->q_data.del_objs;
    totals.del_refs += tstate->q_data.del_refs;

    if (tstate->scanner) {
      ms->mdal->closescanner(tstate->scanner);
    }

    free(tstate);
  }
  if (tres < 0) {
    LOG(LOG_ERR, "Rank %d: Failed to retrieve next thread status\n", rank);
    pthread_mutex_destroy(&(gstate.lock));
    ms->mdal->destroyctxt(ctxt);
    free(ns_path);
    return -1;
  }

  if (tq_close(tq) > 0) {
    WorkPkg work = NULL;
    while (tq_dequeue(tq, TQ_ABORT, (void**)&work) > 0) {
      free(work);
    }
    tq_close(tq);
  }

  pthread_mutex_destroy(&(gstate.lock));

  // Update quota values for the namespace
  if (ms->mdal->setdatausage(ctxt, totals.size)) {
    LOG(LOG_ERR, "Rank %d: Failed to set data usage for namespace %s\n", rank, ns_path);
  }
  if (ms->mdal->setinodeusage(ctxt, totals.count)) {
    LOG(LOG_ERR, "Rank %d: Failed to set inode usage for namespace %s\n", rank, ns_path);
  }

  char* efgc = "Eligible for GC";
  if (del) {
    efgc = "Deleted";
  }

  char msg[1024];
  snprintf(msg, 1024, "Rank: %d NS: \"%s\" Count: %lu Size: %lfTiB %s: (Objs: %lu Refs: %lu)", rank, name, totals.count, totals.size / 1024.0 / 1024.0 / 1024.0 / 1024.0, efgc, totals.del_objs, totals.del_refs);
  MPI_Send(msg, 1024, MPI_CHAR, 0, 0, MPI_COMM_WORLD);

  ms->mdal->destroyctxt(ctxt);
  free(ns_path);

  return 0;
}

/** Iterate through all the namespaces in a given tree, performing garbage
 * collection and updating quota data on namespaces with indexes that
 * correspond to the current process rank.
 * @param const marfs_ns* ns : Reference to the namespace at the root of the
 * tree
 * @param int idx : Absolute index of the namespace at the root of the tree
 * @param const char* name : Name of the namespace at the root of the tree
 * @return int : Zero on success, or -1 on failure.
 */
int find_rank_ns(const marfs_ns* ns, int idx, const char* name, const char* ns_tgt) {
  // Access the namespace if it corresponds to rank
  if (ns_tgt) {
    if (rank != 0) {
      return 0;
    }
    if (!strcmp(name, ns_tgt)) {
      if (ref_paths(ns, name)) {
        return -1;
      }
      return 0;
    }
  }
  else if (idx % n_ranks == rank) {
    if (ref_paths(ns, name)) {
      return -1;
    }

  }
  idx++;

  // Iterate through all subspaces
  int i;
  for (i = 0; i < ns->subnodecount; i++) {
    idx = find_rank_ns(ns->subnodes[i].content, idx, ns->subnodes[i].name, ns_tgt);
    if (idx < 0) {
      return -1;
    }
  }

  if (ns_tgt && idx) {
    return -1;
  }

  return idx;
}

int main(int argc, char** argv) {
  errno = 0;
  char* config_path = NULL;
  char* ns_tgt = NULL;
  char* delim = NULL;
  char config_v = 0;

  char pr_usage = 0;
  int c;
  // parse all position-indepenedent arguments
  while ((c = getopt(argc, argv, "c:dn:t:v")) != -1) {
    switch (c) {
    case 'c':
      config_path = optarg;

      break;
    case 'd':
      del = 1;

      break;
    case 'n':
      ns_tgt = optarg;

      break;
    case 't':
      delim = strchr(optarg, ':');
      if (!delim || (n_prod = strtol(optarg, NULL, 10)) <= 0 || (n_cons = strtol(delim + 1, NULL, 10)) <= 0) {
        fprintf(stderr, "Invalid thread argument %s\n", optarg);
        return -1;
      }

      break;
    case 'v':
      config_v = 1;

      break;
    case '?':
      pr_usage = 1;

      break;
    default:
      fprintf(stderr, "Failed to parse command line options\n");
      return -1;
    }
  }

  // check if we need to print usage info
  if (pr_usage) {
    printf(OUTPREFX "Usage info --\n");
    printf(OUTPREFX "%s -c configpath [-v] [-d] [-n namespace] [-t p:c]\n", PROGNAME);
    printf(OUTPREFX "   -c : Path of the MarFS config file\n");
    printf(OUTPREFX "   -v : Verify the MarFS config\n");
    printf(OUTPREFX "   -d : Delete resources eligible for garbage collection\n");
    printf(OUTPREFX "   -n : Name of MarFS namespace\n");
    printf(OUTPREFX "   -t : Launch p producer and c consumer threads for each namespace\n");
    printf(OUTPREFX "   -h : Print this usage info\n");
    return -1;
  }

  // verify that a config was defined
  if (config_path == NULL) {
    fprintf(stderr, OUTPREFX "no config path defined ( '-c' arg )\n");
    return -1;
  }

  // Initialize MarFS context
  marfs_config* cfg = config_init(config_path);
  if (cfg == NULL) {
    fprintf(stderr, OUTPREFX "Rank %d: Failed to initialize config: \"%s\" ( %s )\n", rank, config_path, strerror(errno));
    return -1;
  }

  if (config_v) {
    if (config_verify(cfg, 1)) {
      fprintf(stderr, OUTPREFX "Rank %d: Failed to verify config: %s\n", rank, strerror(errno));
      config_term(cfg);
      return -1;
    }
  }

  // MPI Initialization
  if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
    LOG(LOG_ERR, "Error in MPI_Init\n");
    config_term(cfg);
    return -1;
  }

  if (MPI_Comm_size(MPI_COMM_WORLD, &n_ranks) != MPI_SUCCESS) {
    LOG(LOG_ERR, "Error in MPI_Comm_size\n");
    config_term(cfg);
    return -1;
  }

  if (MPI_Comm_rank(MPI_COMM_WORLD, &rank) != MPI_SUCCESS) {
    LOG(LOG_ERR, "Error in MPI_Comm_rank\n");
    config_term(cfg);
    return -1;
  }

  // Iterate through all namespaces, gc'ing and updating quota data
  int total_ns = find_rank_ns(cfg->rootns, 0, "root", ns_tgt);
  if (total_ns < 0) {
    if (ns_tgt) {
      fprintf(stderr, "Failed to find namespace \"%s\"\n", ns_tgt);
    }
    config_term(cfg);
    return -1;
  }

  if (ns_tgt) {
    total_ns = 1;
  }

  // Rank 0 collects a message for each namespace from other processes
  int i;
  if (rank == 0) {
    char* msg = malloc(sizeof(char) * 1024);
    for (i = 0; i < total_ns; i++) {
      MPI_Recv(msg, 1024, MPI_CHAR, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      printf(OUTPREFX "%s\n", msg);
    }
    free(msg);
  }

  MPI_Finalize();

  config_term(cfg);

  return 0;
}