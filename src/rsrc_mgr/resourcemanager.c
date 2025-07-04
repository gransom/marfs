/**
 * Copyright 2015. Triad National Security, LLC. All rights reserved.
 *
 * Full details and licensing terms can be found in the License file in the main development branch
 * of the repository.
 *
 * MarFS was reviewed and released by LANL under Los Alamos Computer Code identifier: LA-CC-15-039.
 */

#include <dirent.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>

#if RMAN_USE_MPI
#include <mpi.h>
#endif

// specifically needed for this file
#include "rsrc_mgr/common.h"
#include "rsrc_mgr/manager.h"
#include "rsrc_mgr/resourcethreads.h"
#include "rsrc_mgr/rmanstate.h"
#include "rsrc_mgr/worker.h"

//   -------------   INTERNAL DEFINITIONS    -------------

#define GC_THRESH 604800  // Age of deleted files before they are Garbage Collected
                          // Default to 7 days ago
#define RB_L_THRESH  600  // Age of files before they are rebuilt (based on location)
                          // Default to 10 minutes ago
#define RB_M_THRESH  120  // Age of files before they are rebuilt (based on marker)
                          // Default to 2 minutes ago
#define RP_THRESH 259200  // Age of files before they are repacked
                          // Default to 3 days ago
#define CL_THRESH  86400  // Age of intermediate state files before they are cleaned up (failed repacks, old logs, etc.)
                          // Default to 1 day ago

static void print_usage_info(const int rank) {
   if (rank != 0) {
       return;
   }

   printf("\n"
           "marfs-rman [-c MarFS-Config-File] [-n MarFS-NS-Target] [-r] [-i Iteraion-Name] [-l Log-Root]\n"
           "           [-p Log-Pres-Root] [-d] [-X Execution-Target] [-Q] [-G] [-R] [-P] [-C]\n"
           "           [-T Threshold-Values] [-L [NE-Location]] [-h]\n"
           "\n"
           " Arguments --\n"
           "  -c MarFS-Config-File : Specifies the path of the MarFS config file to use\n"
           "                         (uses the MARFS_CONFIG_PATH env val, if unspecified)\n"
           "  -n MarFS-NS-Target   : Path of the MarFS Namespace to target for processing\n"
           "                         (defaults to the root NS, if unspecified)\n"
           "  -r                   : Specifies to operate recursively on MarFS Subspaces\n"
           "  -i Iteration-Name    : Specifies the name of this marfs-rman instance\n"
           "                         (defaults to a date string, if unspecified)\n"
           "  -l Log-Root          : Specifies the dir to be used for resource log storage\n"
           "                         (defaults to \"%s\", if unspecified)\n"
           "  -p Log-Pres-Root     : Specifies a location to store resource logs to, post-run\n"
           "                         (logfiles will be deleted, if unspecified)\n"
           "  -d                   : Specifies a 'dry-run', logging but skipping execution of all ops\n"
           "  -X Execution-Target  : Specifies the logging path of a previous 'dry-run' iteration to\n"
           "                         be processed by this run. The program will NOT scan reference\n"
           "                         paths to identify operations. Instead, it will exclusively\n"
           "                         perform the operations logged by the targetted iteration.\n"
           "                         This argument is incompatible with any of the below args.\n"
           "  -Q                   : The resource manager will set NS usage values (files / bytes)\n"
           "  -G                   : The resource manager will perform garbage collection\n"
           "  -R                   : The resource manager will perform rebuilds\n"
           "  -P                   : The resource manager will perform repacks\n"
           "                         (!!!CURRENTLY UNIMPLEMENTED!!!)\n"
           "  -C                   : The resource manager will perform cleanup of failed operations\n"
           "  -T Threshold-Values  : Specifies time threshold values for resource manager ops.\n"
           "                         Value Format = <OpFlag><TimeThresh>[<Unit>]\n"
           "                                           [-<OpFlag><TimeThresh>[<Unit>]]*\n"
           "                         Where, <OpFlag>     = 'G', 'R', 'P', or 'C' (see prev args)\n"
           "                                <TimeThresh> = A numeric time value\n"
           "                                               (only files older than this threshold\n"
           "                                                 will be targeted for the specified op)\n"
           "                                <Unit>       = 's' (seconds), 'm' (minutes),\n"
           "                                               'h' (hours), 'd' (days)\n"
           "                                               (assumed to be 's', if omitted)\n"
           "  -L [NE-Location]     : Specifies NE object location to target for rebuilds\n"
           "                         Value Format = <LocType><LocValue>\n"
           "                                           [-<LocType><LocValue>]*\n"
           "                         Where, <LocType>  = 'p' (pod), 'c' (cap), or 's' (scatter)\n"
           "                                <LocValue> = A numeric value for the specified p/c/s location\n"
           "                         NOTE -- Missing 'NE-Location' value implies rebuild of ALL objects!\n"
           "  -h                   : Prints this usage info\n"
           "\n",
           DEFAULT_LOG_ROOT);
}

//   -------------   CORE BEHAVIOR LOOPS   -------------

typedef struct {
    time_t skip;    // for skipping seemingly inactive logs of previous runs
    time_t gc;
    time_t rbl;
    time_t rbm;
    time_t rp;
    time_t cl;
} ArgThresholds_t;

typedef struct {
    rmanstate* rman;
    char* config_path;
    char* ns_path;
    int recurse;

    struct timeval currenttime;
    ArgThresholds_t thresh;
} Args_t;

// getopt and fill in config
static int parse_args(int argc, char** argv,
                      Args_t* args) {
   // get the initialization time of the program, to identify thresholds
   if (gettimeofday(&args->currenttime, NULL)) {
      fprintf(stderr, "failed to get current time for first walk\n");
      return -1;
   }

   rmanstate* rman    = args->rman;
   args->config_path  = getenv("MARFS_CONFIG_PATH");
   args->ns_path      = ".";
   args->thresh       = (ArgThresholds_t) {
       .skip = args->currenttime.tv_sec - INACTIVE_RUN_SKIP_THRESH,
       .gc   = args->currenttime.tv_sec - GC_THRESH,
       .rbl  = args->currenttime.tv_sec - RB_L_THRESH,
       .rbm  = args->currenttime.tv_sec - RB_M_THRESH,
       .rp   = args->currenttime.tv_sec - RP_THRESH,
       .cl   = args->currenttime.tv_sec - CL_THRESH,
   };
   ArgThresholds_t* thresh = &args->thresh;

   // set some resourcemanager specific values
   rman->tqopts.thread_init_func     = rthread_init;
   rman->tqopts.thread_consumer_func = rthread_all_consumer;
   rman->tqopts.thread_producer_func = rthread_all_producer;
   rman->tqopts.thread_pause_func    = NULL;
   rman->tqopts.thread_resume_func   = NULL;
   rman->tqopts.thread_term_func     = rthread_term;

   // parse all position-independent arguments
   int print_usage = 0;
   int c;
   while ((c = getopt(argc, (char* const*)argv, "c:n:ri:l:p:dX:QGRPCT:L:h")) != -1) {
      switch (c) {
      case 'c':
         args->config_path = optarg;
         break;
      case 'n':
         args->ns_path = optarg;
         break;
      case 'r':
         args->recurse = 1;
         break;
      case 'i':
         if (strlen(optarg) >= ITERATION_STRING_LEN) {
            fprintf(stderr, "ERROR: Iteration string exceeds allocated length %u: \"%s\"\n",
                    ITERATION_STRING_LEN, optarg);
            return -1;
         }
         snprintf(rman->iteration, ITERATION_STRING_LEN, "%s", optarg);
         break;
      case 'l':
         rman->logroot = optarg;
         break;
      case 'p':
         rman->preservelogtgt = optarg;
         break;
      case 'd':
         rman->gstate.dryrun = 1;
         break;
      case 'X':
         rman->execprevroot = optarg;
         break;
      case 'Q':
         rman->quotas = 1;
         break;
      case 'G':
         rman->gstate.thresh.gcthreshold = 1;       // time_t used as bool for now
         break;
      case 'R':
         rman->gstate.thresh.rebuildthreshold = 1;  // time_t used as bool for now
         break;
      case 'P':
         rman->gstate.thresh.repackthreshold = 1;   // time_t used as bool for now
         break;
      case 'C':
         rman->gstate.thresh.cleanupthreshold = 1;  // time_t used as bool for now
         break;
      case 'T':
      {
         char* threshparse = optarg;

         // check for a Threshold type flag
         char tflag = *threshparse;
         if (tflag == '-') {
            tflag = *++threshparse;
         }

         if ((tflag != 'G') && (tflag != 'R') &&
             (tflag != 'P') && (tflag != 'C')) {
            printf("ERROR: Failed to parse '-T' argument value: \"%s\"\n",
                   optarg);
            print_usage = 1;
            break;
         }

         // move to the value part of the string
         threshparse++;

         // parse the expected numeric value trailing the type flag
         char* endptr = NULL;
         unsigned long long parseval = strtoull(threshparse, &endptr, 10);
         if ((parseval == ULLONG_MAX) ||
             (endptr == NULL) ||
             ((*endptr != 's') && (*endptr != 'm') &&
              (*endptr != 'h') && (*endptr != 'd') &&
              (*endptr != '-') && (*endptr != '\0'))) {
            printf("ERROR: Failed to parse '%c' threshold from '-T' argument value: \"%s\"\n",
                   tflag, optarg);
            print_usage = 1;
            break;
         }

         if (*endptr == 'm') { parseval *= 60; }
         else if (*endptr == 'h') { parseval *= 60 * 60; }
         else if (*endptr == 'd') { parseval *= 60 * 60 * 24; }

         // actually assign the parsed value to the appropriate threshold
         switch (tflag) {
            case 'G':
               thresh->gc = args->currenttime.tv_sec - parseval;
               break;
             case 'R':
                thresh->rbl = args->currenttime.tv_sec - parseval;
                thresh->rbm = args->currenttime.tv_sec - parseval;
                break;
             case 'P':
                thresh->rp = args->currenttime.tv_sec - parseval;
                break;
             case 'C':
                thresh->cl = args->currenttime.tv_sec - parseval;
                break;
         }

         break;
      }
      case 'L':
      {
         rman->gstate.lbrebuild = 1;

         char* locparse = optarg;
         char lflag = *locparse;
         if (lflag == '-') {
            lflag = *++locparse;
         }

         // check for a location value type flag
         if ((*locparse != 'p') &&
             (*locparse != 'c') &&
             (*locparse != 's')) {
            printf("ERROR: Failed to parse '-L' argument value: \"%s\"\n", optarg);
            print_usage = 1;
            break;
         }

         // move to the value part of the string
         locparse++;

         // parse the expected numeric value trailing the type flag
         char* endptr = NULL;
         unsigned long long parseval = strtoull(locparse, &endptr, 10);
         if ((parseval == ULLONG_MAX) || (endptr == NULL) ||
             ((*endptr != '-') && (*endptr != '\0'))) {
            printf("ERROR: Failed to parse '%c' location from '-L' argument value: \"%s\"\n",
                   lflag, optarg);
            print_usage = 1;
            break;
         }

         // actually assign the parsed value to the appropriate location value
         switch (lflag) {
            case 'p':
               rman->gstate.rebuildloc.pod = parseval;
               break;
             case 'c':
               rman->gstate.rebuildloc.cap = parseval;
               break;
             case 's':
               rman->gstate.rebuildloc.scatter = parseval;
               break;
         }

         break;
      }
      case '?':
         printf("ERROR: Unrecognized cmdline argument: \'%c\'\n", optopt);
         // fall through
      case 'h':
         print_usage = 1;
         break;
      default:
         printf("ERROR: Failed to parse command line options\n");
         return -1;
      }
   }
   if (print_usage) {
      print_usage_info(rman->ranknum);
      return -1;
   }

   // validate arguments
   if (rman->execprevroot) {
      // check if we were incorrectly passed any args
      if (rman->gstate.thresh.gcthreshold      ||  rman->gstate.thresh.rebuildthreshold  ||
          rman->gstate.thresh.repackthreshold  ||  rman->gstate.thresh.cleanupthreshold  ||
          rman->iteration[0] != '\0') {
         fprintf(stderr, "ERROR: The '-G', '-R', '-P', and '-i' args are incompatible with '-X'\n");
         return -1;
      }
      // parse over the specified path, looking for RECORD_ITERATION_PARENT
      char* prevdup = strdup(rman->execprevroot);
      char* pathelem = strtok(prevdup, "/");
      while (pathelem) {
         if (strcmp(RECORD_ITERATION_PARENT, pathelem) == 0) {
             break;
         }
         pathelem = strtok(NULL, "/");
      }
      if (pathelem == NULL) {
         fprintf(stderr, "ERROR: The specified previous run path is missing the expected '%s' path component: \"%s\"\n",
                 RECORD_ITERATION_PARENT, rman->execprevroot);
         free(prevdup);
         return -1;
      }

      size_t keepbytes = strlen(pathelem) + (pathelem - prevdup); // identify the strlen of the path up to this elem
      // get our iteration name from the subsequent path element
      pathelem = strtok(NULL, "/");
      if (snprintf(rman->iteration, ITERATION_STRING_LEN, "%s", pathelem) >= ITERATION_STRING_LEN) {
         fprintf(stderr, "ERROR: Parsed invalid iteration string from previous run path: \"%s\"\n",
                 rman->execprevroot);
         free(prevdup);
         return -1;
      }
      free(prevdup);

      // identify the log root we will be pulling from
      char* baseroot = rman->execprevroot;
      rman->execprevroot = malloc(sizeof(char) * (keepbytes + 1));
      snprintf(rman->execprevroot, keepbytes + 1, "%s", baseroot); // use snprintf to truncate to appropriate length
   }

   // fill in more of rman

   const char* iteration_parent = rman->gstate.dryrun?(const char*) RECORD_ITERATION_PARENT:(const char*) MODIFY_ITERATION_PARENT;

   // construct logroot subdirectory path and replace variable
   const char* logroot = rman->logroot?rman->logroot:(char *) DEFAULT_LOG_ROOT;
   const size_t newroot_len = sizeof(char) * (strlen(logroot) + 1 + strlen(iteration_parent));
   char* newroot = malloc(newroot_len + 1);
   snprintf(newroot, newroot_len + 1, "%s/%s", rman->logroot, iteration_parent);
   rman->logroot = newroot;

   // create logging root dir
   if (mkdir(rman->logroot, 0700) && (errno != EEXIST)) {
       fprintf(stderr, "ERROR: Failed to create logging root dir: \"%s\"\n", rman->logroot);
       free(rman->logroot);
       rman->logroot = NULL;
       return -1;
   }

   // construct log preservation root subdirectory path and replace variable
   if (rman->preservelogtgt) {
      const size_t presroot_len = strlen(rman->preservelogtgt) + 1 + strlen(iteration_parent);
      char* presroot = calloc(sizeof(char), presroot_len + 1);
      snprintf(presroot, presroot_len + 1, "%s/%s",
               rman->preservelogtgt, iteration_parent);
      rman->preservelogtgt = presroot;

      if (mkdir(rman->preservelogtgt, 0700) && (errno != EEXIST)) {
          fprintf(stderr, "ERROR: Failed to create log preservation root dir: \"%s\"\n", rman->preservelogtgt);
          free(rman->logroot);
          rman->logroot = NULL;
          free(rman->preservelogtgt);
          rman->preservelogtgt = NULL;
          return -1;
      }
   }

   // populate an iteration string, if missing
   if (rman->iteration[0] == '\0') {
      struct tm* timeinfo = localtime(&args->currenttime.tv_sec);
      strftime(rman->iteration, ITERATION_STRING_LEN, "%Y-%m-%d-%H:%M:%S", timeinfo);
   }

   // substitute in appropriate threshold values, if specified
   if (rman->gstate.thresh.gcthreshold) {
       rman->gstate.thresh.gcthreshold = args->thresh.gc;
   }
   if (rman->gstate.thresh.rebuildthreshold) {
      if (rman->gstate.lbrebuild) {
          rman->gstate.thresh.rebuildthreshold = args->thresh.rbl;
      }
      else {
          rman->gstate.thresh.rebuildthreshold = args->thresh.rbm;
      }
   }
   if (rman->gstate.thresh.repackthreshold) {
       rman->gstate.thresh.repackthreshold = args->thresh.rp;
   }
   if (rman->gstate.thresh.cleanupthreshold) {
       rman->gstate.thresh.cleanupthreshold = args->thresh.cl;
   }

   return 0;
}

//   -------------    STARTUP BEHAVIOR     -------------

int main(int argc, char** argv) {
#if RMAN_USE_MPI
   // Initialize MPI
   if (MPI_Init(&argc, &argv)) {
      fprintf(stderr, "ERROR: Failed to initialize MPI\n");
      return -1;
   }
#endif

   // check how many ranks we have
   int rankcount = 1;
   int rank = 0;
#if RMAN_USE_MPI
   if (MPI_Comm_size(MPI_COMM_WORLD, &rankcount)) {
      fprintf(stderr, "ERROR: Failed to identify rank count\n");
      RMAN_ABORT();
   }
   if (MPI_Comm_rank(MPI_COMM_WORLD, &rank)) {
      fprintf(stderr, "ERROR: Failed to identify process rank\n");
      RMAN_ABORT();
   }
#endif
   rmanstate rman;
   rmanstate_init(&rman, rank, rankcount);

   Args_t args = {
      .rman        = &rman,
      .config_path = NULL,
      .ns_path     = NULL,
      .recurse     = 0,
      .currenttime = {0},
      .thresh      = {0},
   };

   if (parse_args(argc, argv, &args) != 0) {
      RMAN_ABORT();
   }

   // for multi-rank invocations, we must synchronize our iteration string across all ranks
   //     (due to clock drift + varied startup times across multiple hosts)
   if (rman.totalranks > 1  &&
#if RMAN_USE_MPI
      MPI_Bcast(rman.iteration, ITERATION_STRING_LEN, MPI_CHAR, 0, MPI_COMM_WORLD)) {
#else
      1) {
#endif
       fprintf(stderr, "ERROR: Failed to synchronize iteration string across all ranks\n");
       rmanstate_fini(&rman, 0);
       RMAN_ABORT();
   }

   // Initialize the MarFS Config
   pthread_mutex_t erasurelock;
   pthread_mutex_init(&erasurelock, NULL);

   if (rmanstate_complete(&rman, args.config_path, args.ns_path,
                          args.thresh.skip, args.recurse, &erasurelock) != 0) {
       pthread_mutex_destroy(&erasurelock);
       rmanstate_fini(&rman, 0);
       RMAN_ABORT();
   }

#if RMAN_USE_MPI
   // synchronize here, to avoid having some ranks run ahead with modifications while other workers
   //    are hung on earlier initialization (which may still fail)
   if (MPI_Barrier(MPI_COMM_WORLD)) {
       fprintf(stderr, "ERROR: Failed to synchronize mpi ranks prior to execution\n");
       pthread_mutex_destroy(&erasurelock);
       rmanstate_fini(&rman, 1);
       RMAN_ABORT();
   }
#endif

   // actually perform core behavior loops
   int bres = 0;
   if (rman.ranknum == 0) {
      bres = managerbehavior(&rman);
   }
   else {
      bres = workerbehavior(&rman);
   }

   if (!bres) {
      bres = (rman.fatalerror) ? -((int)rman.fatalerror) : (int)rman.nonfatalerror;
   }

   pthread_mutex_destroy(&erasurelock);
   rmanstate_fini(&rman, !!bres);

#if RMAN_USE_MPI
    MPI_Finalize();
#endif

   return bres;
}
