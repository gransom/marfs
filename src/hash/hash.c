/*
Copyright (c) 2015, Los Alamos National Security, LLC
All rights reserved.

Copyright 2015.  Los Alamos National Security, LLC. This software was
produced under U.S. Government contract DE-AC52-06NA25396 for Los
Alamos National Laboratory (LANL), which is operated by Los Alamos
National Security, LLC for the U.S. Department of Energy. The
U.S. Government has rights to use, reproduce, and distribute this
software.  NEITHER THE GOVERNMENT NOR LOS ALAMOS NATIONAL SECURITY,
LLC MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR ASSUMES ANY LIABILITY
FOR THE USE OF THIS SOFTWARE.  If software is modified to produce
derivative works, such modified software should be clearly marked, so
as not to confuse it with the version available from LANL.

Additionally, redistribution and use in source and binary forms, with
or without modification, are permitted provided that the following
conditions are met: 1. Redistributions of source code must retain the
above copyright notice, this list of conditions and the following
disclaimer.

2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
3. Neither the name of Los Alamos National Security, LLC, Los Alamos
National Laboratory, LANL, the U.S. Government, nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY LOS ALAMOS NATIONAL SECURITY, LLC AND
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL LOS
ALAMOS NATIONAL SECURITY, LLC OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

-----
NOTE:
-----
MarFS is released under the BSD license.

MarFS was reviewed and released by LANL under Los Alamos Computer Code
identifier: LA-CC-15-039.

MarFS uses libaws4c for Amazon S3 object communication. The original
version is at https://aws.amazon.com/code/Amazon-S3/2601 and under the
LGPL license.  LANL added functionality to the original work. The
original work plus LANL contributions is found at
https://github.com/jti-lanl/aws4c.

GNU licenses can be found at http://www.gnu.org/licenses/.
*/
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

//   -------------   INTERNAL DEFINITIONS    -------------

// this seed value was chosen arbitrarily
#define KEY_SEED 17

#define TARGET_NODE_COUNT 50000

typedef struct virtual_node_struct {
   uint64_t   id[2];             // ID value of this virtual node
   size_t     nodenum;           // location of the real node, which this virtual node corresponds to
} VIRTUAL_NODE;


typedef struct hash_table_struct {
   size_t         nodecount;     // count of real nodes in the table
   HASH_NODE*     nodes;         // array of node pointers
   size_t         vnodecount;    // count of virtual nodes in the table
   VIRTUAL_NODE*  vnodes;        // array of virtual node pointers
   size_t         curnode;       // position of the next node ( for iterating )
   size_t         iterated;      // number of nodes returned so far ( for iterating )
}* HASH_TABLE;


//   -------------   INTERNAL FUNCTIONS    -------------

// see implemtation/release info, below
void MurmurHash3_x64_128( const void* key, const int len, const uint32_t seed, void* out );


static void identifier( const char *key, uint64_t *id ) {
   MurmurHash3_x64_128(key, strlen(key), KEY_SEED, id);
}


static inline compareID( uint64_t *id_a, uint64_t *id_b ) {
   if(id_a[0] > id_b[0]) {
      return 1;
   }
   else if(id_a[0] < id_b[0]) {
      return -1;
   }
   else {
      if(id_a[1] > id_b[1]) {
         return 1;
      }
      else if(id_a[1] < id_b[1]) {
         return -1;
      }
      else {
         return 0;
      }
   }
}


// impose an ordering on nodes based first on id, then nodenum
static int compare_nodes(const void *a, const void *b) {
   VIRTUAL_NODE *node_a = (VIRTUAL_NODE*)a;
   VIRTUAL_NODE *node_b = (VIRTUAL_NODE*)b;
   int comparison_result = compareID(node_a->id, node_b->id);
   if(comparison_result != 0)
      return comparison_result;
   else {
      if(node_a->nodenum < node_b->nodenum)
         return -1;
      else if(node_b->nodenum == node_a->nodenum)
         return 0;
      else
         return 1;
   }
}


//   -------------   EXTERNAL FUNCTIONS    -------------


HASH_TABLE hash_init( HASH_NODE* nodes, size_t count, char lookup ) {
   // allocate the hash_table structure
   HASH_TABLE table = malloc( sizeof( struct hash_table_struct ) );
   if ( table == NULL ) {
      LOG( LOG_ERR, "Failed to allocate space for HASH_TABLE\n" );
      return -1;
   }
   table->nodecount = count;
   table->nodes = nodes;

   // iterate over node list, gathering weight/name info
   int totalweight = 0;
   int zerocount = 0;
   int maxname = 0;
   size_t curnode = 0;
   for ( ; curnode < count; curnode++ ) {
      if ( nodes[curnode].weight < 0 ) {
         // negative weight values are unacceptable
         LOG( LOG_ERR, "Node %zd has negative weight value: %d\n", curnode, nodes[curnode].weight );
         free( table );
         errno = EINVAL;
         return -1;
      }
      if ( nodes[curnode].weight == 0 )
         zerocount++;  // zeros are special, so we need to count these separately
      else
         totalweight += nodes[curnode].weight;  // otherwise, add to our running total
      if ( maxname < strlen( nodes[curnode].name ) )
         maxname = strlen( nodes[curnode].name ); // track the longest name
   }

   // calculate how many vnodes we will need
   int weightratio = 1;
   if ( totalweight ) {
      weightratio = (int) ( TARGET_NODE_COUNT / totalweight );
      if ( TARGET_NODE_COUNT % totalweight ) weightratio++; // overestimate our ratio, to ensure we hit the target
   }
   else if ( !(lookup) ) {
      LOG( LOG_ERR, "recieved all zero weight values for a non-lookup hash table\n" );
      free( table );
      errno = EINVAL;
      return -1;
   }
   table->vnodecount = ( weightratio * totalweight );
   // for a lookup table, we need to add a single vnode per zero value weight
   if ( lookup ) {
      table->vnodecount += zerocount;
   }

   // allocate space for all virtual nodes
   table->vnodes = malloc( sizeof( struct virtual_node_struct ) * table->vnodecount );
   if ( table->vnodes == NULL ) {
      LOG( LOG_ERR, "Failed to allocate space for virtual nodes\n" );
      free( table );
      return -1;
   }

   // allocate space for all vnode name strings
   int maxdigits = 1;
   int tmpratio = weightratio;
   while ( ( (int)( tmpratio = tmpratio / 10 ) ) ) { maxdigits++; }
   char* vnodename = malloc( sizeof( char ) * ( maxname + 2 + maxdigits ) );
   if ( vnodename == NULL ) {
      LOG( LOG_ERR, "Failed to allocate space for virtual node names\n" );
      free( table->vnodes );
      free( table );
      return -1;
   }

   // create all virtual nodes
   size_t curvnode = 0;
   for ( curnode = 0; curnode < count; curnode++ ) {
      if ( nodes[curnode].weight ) {
         // if we have a weight value, create weight*weightratio virtual nodes
         size_t tmpvnode = 0;
         for ( ; tmpvnode < ( nodes[curnode].weight * weightratio ); tmpvnode++ ) {
            table->vnodes[curvnode + tmpvnode].nodenum = curnode;
            int pval = snprintf( vnodename, maxname + maxdigits + 2, "%s-%d", nodes[curnode].name, tmpvnode );
            if ( pval < 0  ||  pval >= ( maxname + maxdigits + 2 ) ) {
               LOG( LOG_ERR, "Failed to generate vnode name string\n" );
               free( vnodename );
               free( table->vnodes );
               free( table );
               return -1;
            }
            identifier( vnodename, table->vnodes[curvnode + tmpvnode].id );
         }
         curvnode += tmpvnode;
      }
      else if ( lookup ) {
         // for a lookup table, zero weight indicates a single virtual node, with a matching name
         curvnode++;
         table->vnodes[curvnode].nodenum = curnode;
         identifier( nodes[curnode].name, table->vnodes[curvnode].id );
      }
   }

   // sort all virtual nodes
   qsort(table->vnodes, table->vnodecount, sizeof( struct virtual_node_struct ), compare_nodes);

   // initialize iterator values (probably unnecessary)
   table->curnode = 0;
   table->iterated = 0;

   return table;
}


int hash_term( HASH_TABLE table, HASH_NODE** nodes, size_t* count ) {
   // check for a NULL table
   if ( table == NULL ) {
      LOG( LOG_ERR, "Received a NULL HASH_TABLE reference\n" );
      errno = EINVAL;
      return -1;
   }

   // set our node return values
   *nodes = table->nodes;
   *count = table->nodecount;
   // cleanup memory structures
   free( table->vnodes );
   free( table );
   return 0;
}


int hash_lookup( HASH_TABLE table, const char* target, HASH_NODE** node ) {
   // check for a NULL table
   if ( table == NULL ) {
      LOG( LOG_ERR, "Received a NULL HASH_TABLE reference\n" );
      errno = EINVAL;
      return -1;
   }

   // generate an ID value from the target string
   uint64_t tid[2];
   identifier( target, tid );
   int retval = 1; // assume an approximate match

   // perform a binary search of the vnode ring for a matching ID value
   // NOTE -- the builtin bsearch() function is not ideal for this, as it is looking only 
   //         for an exact match.  While our function prefers an exact match, we will settle 
   //         for a 'successor' vnode, if an exact match is not present.
   //         Additionally, we consider our vnode array a 'ring'.  ID values beyond the end of 
   //         the array will loop back to the beginning.  This means that EVERY hash_lookup() 
   //         will produce a node entry, just not necessarily one which matches exactly.
   size_t curnode = table->vnodecount / 2;
   size_t min = 0;                 // minimum is an INCLUSIVE bound
   size_t max = table->vnodecount; // maximum is an EXCLUSIVE bound
   while ( min != max ) {
      int comparison = compareID( table->vnodes[curnode].id, tid );
      if ( comparison > 0 ) {
         // the current node ID value is too high
         max = curnode;
      }
      else if ( comparison < 0 ) {
         // the current node ID value is too low
         min = curnode + 1;
      }
      else {
         // the current node ID matches
         // while this is VERY likely to be an exact match on name as well, we must verify
         if ( strncmp( node->name, target, strlen(target) + 1 ) ) {
            // we have an ID collision, rather than an actual match
            // this is an unfortunate edge case, as we must do some extra checks against nearby nodes
            break;  // NOTE -- it is impossible for curnode==max here, so that will be our catch for this case
         }
         // this node is a true exact match
         retval = 0;
         max = curnode; // to avoid the ID collision case
      }

      curnode = min + ( ( max - min ) / 2 );
   }

   // check for the ID collision case
   if ( curnode != max ) {
      // check surrounding nodes for an exact match
      size_t tmpnode = curnode + 1;
      int step = 1; // check higher postions first
      while ( 1 ) {
         // check upper bound and ID match
         if ( tmpnode == max  ||  compareID( table->vnodes[tmpnode].id, tid ) ) {
            // no point checking further in this direction
            if ( step > 0 ) {
               // reverse direction
               if ( curnode == 0 ) {
                  // no lesser nodes exist, so we are done
                  // this check is REQUIRED to avoid unsigned value overflow
                  break;
               }
               step = -1;
               tmpnode = curnode - 1;
               continue;
            }
            break; // we've checked both directions, so give up
         }
         // we've found an ID match, now check for a name match
         HASH_NODE* newnode = table->nodes + table->vnodes[tmpnode].nodenum;
         if ( strncmp( newnode.name, target, strlen(target) + 1 ) == 0 ) {
            // a real exact match; return this instead
            curnode = tmpnode;
            retval = 0;
            break;
         }
         // yet *another* ID collision, so we must continue
         if ( tmpnode == min ) { break; } // lower bound check
         // proceed along our current direction
         tmpnode = tmpnode + step;
      }
      // we have checked all surrounding nodes, but no exact match exists
   }

   // If we get here, either curnode references a matching ID value or min/max/curnode will have converged 
   //  on an ID value *just* above the target
   // we now have to check for the 'loop' condition
   if ( curnode == table->vnodecount ) {
      // if min/max/curnode == vnodecount, the target ID is beyond our largest existing value
      // in such a case, loop back to the beginning of the ring
      curnode = 0;
   }
   // map the resulting virtual node to the actual node
   *node = table->nodes + table->vnodes[curnode].nodenum;
   // set new iterator values
   table->curnode = table->vnodes[curnode].nodenum + 1;
   if ( table->curnode >= table->nodecount ) { table->curnode = 0; }
   table->iterated = 1;
   return retval;
}


int hash_iterate( HASH_TABLE table, HASH_NODE** node ) {
   // check for a NULL table
   if ( table == NULL ) {
      LOG( LOG_ERR, "Received a NULL HASH_TABLE reference\n" );
      errno = EINVAL;
      return -1;
   }

   // check if iteration is complete
   if ( table->iterated >= table->nodecount ) {
      LOG( LOG_INFO, "Iteration complete\n" );
      *node = NULL;
      return 0;
   }

   // return the current node reference
   *node = table->nodes + table->curnode;
   // update iterator values
   table->iterated++;
   table->curnode++;
   if ( table->curnode >= table->nodecount ) { table->curnode = 0; }
   return 1; 
}


// POLYHASH implementation
// NOTE -- not currently in use, just here for potential future reference
//
// Computes a good, uniform, hash of the string.
//
// Treats each character in the length n string as a coefficient of a
// degree n polynomial.
//
// f(x) = string[n -1] + string[n - 2] * x + ... + string[0] * x^(n-1)
//
// The hash is computed by evaluating the polynomial for x=33 using
// Horner's rule.
//
// Reference: http://cseweb.ucsd.edu/~kube/cls/100/Lectures/lec16/lec16-14.html
uint64_t polyhash(const char* string) {
   // According to http://www.cse.yorku.ca/~oz/hash.html
   // 33 is a magical number that inexplicably works the best.
   const int salt = 33;
   char c;
   uint64_t h = *string++;
   while((c = *string++))
      h = salt * h + c;
   return h;
}

// compute the hash function h(x) = (a*x) >> 32
uint64_t h_a(const uint64_t key, uint64_t a) {
   return ((a * key) >> 32);
}

/*  POLYHASH usage example
   // We will use the hash in multiple places, save it to avoid
   // recomputing.
   //
   // Hash the actual object ID so the hash will remain the same,
   // regadless of changes to the "file-ification" format.
   unsigned long objid_hash = polyhash(objid);

   char *mc_path_format = repo->host;

   unsigned int num_blocks    = MC_CONFIG(ctx)->n + MC_CONFIG(ctx)->e;
   unsigned int num_pods      = MC_CONFIG(ctx)->num_pods;
   unsigned int num_cap       = MC_CONFIG(ctx)->num_cap;
   unsigned int scatter_width = MC_CONFIG(ctx)->scatter_width;

   unsigned int seed = objid_hash;
   uint64_t     a[3];
   int          i;
   for(i = 0; i < 3; i++)
      a[i] = rand_r(&seed) * 2 + 1; // generate 32 random bits

   MC_CONTEXT(ctx)->pod         = objid_hash % num_pods;
   MC_CONTEXT(ctx)->cap         = h_a(objid_hash, a[0]) % num_cap;
   unsigned long scatter        = h_a(objid_hash, a[1]) % scatter_width;
   MC_CONTEXT(ctx)->start_block = h_a(objid_hash, a[2]) % num_blocks;

*/




// The following implementation of MurmurHash was retrieved from --
//    https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp
// 
// The release info for the code has been reproduced below.
//
//-----------------------------------------------------------------------------
// MurmurHash3 was written by Austin Appleby, and is placed in the public
// domain. The author hereby disclaims copyright to this source code.
//


#define BIG_CONSTANT(x) (x##LLU)
#define FORCE_INLINE inline __attribute__((always_inline))
#define ROTL64(x,y) rotl64(x,y)


static FORCE_INLINE uint64_t rotl64 ( uint64_t x, int8_t r )
{
  return (x << r) | (x >> (64 - r));
}

//-----------------------------------------------------------------------------
// Block read - if your platform needs to do endian-swapping or can only
// handle aligned reads, do the conversion here
#define getblock64(p,i) (p[i])

//-----------------------------------------------------------------------------
// Finalization mix - force all bits of a hash block to avalanche
static FORCE_INLINE uint64_t fmix64 ( uint64_t k )
{
  k ^= k >> 33;
  k *= BIG_CONSTANT(0xff51afd7ed558ccd);
  k ^= k >> 33;
  k *= BIG_CONSTANT(0xc4ceb9fe1a85ec53);
  k ^= k >> 33;

  return k;
}


void MurmurHash3_x64_128 ( const void * key, const int len,
                           const uint32_t seed, void * out )
{
  const uint8_t * data = (const uint8_t*)key;
  const int nblocks = len / 16;

  uint64_t h1 = seed;
  uint64_t h2 = seed;

  const uint64_t c1 = BIG_CONSTANT(0x87c37b91114253d5);
  const uint64_t c2 = BIG_CONSTANT(0x4cf5ad432745937f);

  //----------
  // body

  const uint64_t * blocks = (const uint64_t *)(data);

  for(int i = 0; i < nblocks; i++)
  {
    uint64_t k1 = getblock64(blocks,i*2+0);
    uint64_t k2 = getblock64(blocks,i*2+1);

    k1 *= c1; k1  = ROTL64(k1,31); k1 *= c2; h1 ^= k1;

    h1 = ROTL64(h1,27); h1 += h2; h1 = h1*5+0x52dce729;

    k2 *= c2; k2  = ROTL64(k2,33); k2 *= c1; h2 ^= k2;

    h2 = ROTL64(h2,31); h2 += h1; h2 = h2*5+0x38495ab5;
  }

  //----------
  // tail

  const uint8_t * tail = (const uint8_t*)(data + nblocks*16);

  uint64_t k1 = 0;
  uint64_t k2 = 0;

  switch(len & 15)
  {
  case 15: k2 ^= ((uint64_t)tail[14]) << 48;
  case 14: k2 ^= ((uint64_t)tail[13]) << 40;
  case 13: k2 ^= ((uint64_t)tail[12]) << 32;
  case 12: k2 ^= ((uint64_t)tail[11]) << 24;
  case 11: k2 ^= ((uint64_t)tail[10]) << 16;
  case 10: k2 ^= ((uint64_t)tail[ 9]) << 8;
  case  9: k2 ^= ((uint64_t)tail[ 8]) << 0;
           k2 *= c2; k2  = ROTL64(k2,33); k2 *= c1; h2 ^= k2;

  case  8: k1 ^= ((uint64_t)tail[ 7]) << 56;
  case  7: k1 ^= ((uint64_t)tail[ 6]) << 48;
  case  6: k1 ^= ((uint64_t)tail[ 5]) << 40;
  case  5: k1 ^= ((uint64_t)tail[ 4]) << 32;
  case  4: k1 ^= ((uint64_t)tail[ 3]) << 24;
  case  3: k1 ^= ((uint64_t)tail[ 2]) << 16;
  case  2: k1 ^= ((uint64_t)tail[ 1]) << 8;
  case  1: k1 ^= ((uint64_t)tail[ 0]) << 0;
           k1 *= c1; k1  = ROTL64(k1,31); k1 *= c2; h1 ^= k1;
  };

  //----------
  // finalization

  h1 ^= len; h2 ^= len;

  h1 += h2;
  h2 += h1;

  h1 = fmix64(h1);
  h2 = fmix64(h2);

  h1 += h2;
  h2 += h1;

  ((uint64_t*)out)[0] = h1;
  ((uint64_t*)out)[1] = h2;
}


