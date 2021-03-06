#ifndef _STREAM_H
#define _STREAM_H
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

MarFS uses libaws4c for Amazon S3 object communication. The original version
is at https://aws.amazon.com/code/Amazon-S3/2601 and under the LGPL license.
LANL added functionality to the original work. The original work plus
LANL contributions is found at https://github.com/jti-lanl/aws4c.

GNU licenses can be found at http://www.gnu.org/licenses/.
*/



typedef enum
{
   CREATE_STREAM,
   EDIT_STREAM,
   READ_STREAM,
   ERROR_STREAM
} STREAM_TYPE;

typedef struct streamfile_struct {
   MDAL_FHANDLE    metahandle;
   FTAG            ftag;
   struct timespec times[2];
}* STREAMFILE;

typedef struct datastream_struct {
   STREAM_TYPE     type;
   const marfs_ns  ns;
   MDAL_CTXT       mdalctxt;
   ne_handle   datahandle;
   STREAMFILE  files;
   size_t      filecount;
   size_t      filealloc;
   char*       ftagstr;
   size_t      ftagstrsize;
   size_t      recoveryheaderlen;
   size_t      objno;
   size_t      offset;
   RECOVERY_FINFO finfo;
}* DATASTREAM;


DATASTREAM datastream_init( DATASTREAM stream, const char* path, MDAL mdal, MDAL_CTXT mdalctxt );
int datastream_setrecoverypath( DATASTREAM stream, const char* recovpath );
int datastream_extend( DATASTREAM stream, off_t length );
int datastream_chunkbounds( DATASTREAM stream, int chunknum, off_t* offset, size_t* size );
off_t datastream_seek( DATASTREAM stream, off_t offset, int whence );
int datastream_truncate( DATASTREAM stream, off_t length );
int datastream_utimens( DATASTREAM stream, const struct timespec times[2] );
size_t datastream_write( DATASTREAM stream, const void* buff, size_t size );
size_t datastream_read( DATASTREAM stream, void* buffer, size_t size );
int datastream_flush( DATASTREAM stream );
int datastream_close( DATASTREAM stream );
int datastream_release( DATASTREAM stream );

#endif // _STREAM_H

