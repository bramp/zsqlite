/*
** 2011 March 16
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file contains code implements a VFS shim that writes diagnostic
** output for each VFS call, similar to "strace".
**
** USAGE:
**
** This source file exports a single symbol which is the name of a
** function:
**
**   int vfstrace_register(
**     const char *zTraceName,         // Name of the newly constructed VFS
**     const char *zOldVfsName,        // Name of the underlying VFS
**     int (*xOut)(const char*,void*), // Output routine.  ex: fputs
**     void *pOutArg,                  // 2nd argument to xOut.  ex: stderr
**     int makeDefault                 // Make the new VFS the default
**   );
**
** Applications that want to trace their VFS usage must provide a callback
** function with this prototype:
**
**   int traceOutput(const char *zMessage, void *pAppData);
**
** This function will "output" the trace messages, where "output" can
** mean different things to different applications.  The traceOutput function
** for the command-line shell (see shell.c) is "fputs" from the standard
** library, which means that all trace output is written on the stream
** specified by the second argument.  In the case of the command-line shell
** the second argument is stderr.  Other applications might choose to output
** trace information to a file, over a socket, or write it into a buffer.
**
** The vfstrace_register() function creates a new "shim" VFS named by
** the zTraceName parameter.  A "shim" VFS is an SQLite backend that does
** not really perform the duties of a true backend, but simply filters or
** interprets VFS calls before passing them off to another VFS which does
** the actual work.  In this case the other VFS - the one that does the
** real work - is identified by the second parameter, zOldVfsName.  If
** the 2nd parameter is NULL then the default VFS is used.  The common
** case is for the 2nd parameter to be NULL.
**
** The third and fourth parameters are the pointer to the output function
** and the second argument to the output function.  For the SQLite
** command-line shell, when the -vfstrace option is used, these parameters
** are fputs and stderr, respectively.
**
** The fifth argument is true (non-zero) to cause the newly created VFS
** to become the default VFS.  The common case is for the fifth parameter
** to be true.
**
** The call to vfstrace_register() simply creates the shim VFS that does
** tracing.  The application must also arrange to use the new VFS for
** all database connections that are created and for which tracing is 
** desired.  This can be done by specifying the trace VFS using URI filename
** notation, or by specifying the trace VFS as the 4th parameter to
** sqlite3_open_v2() or by making the trace VFS be the default (by setting
** the 5th parameter of vfstrace_register() to 1).
**
**
** ENABLING VFSTRACE IN A COMMAND-LINE SHELL
**
** The SQLite command line shell implemented by the shell.c source file
** can be used with this module.  To compile in -vfstrace support, first
** gather this file (test_vfstrace.c), the shell source file (shell.c),
** and the SQLite amalgamation source files (sqlite3.c, sqlite3.h) into
** the working directory.  Then compile using a command like the following:
**
**    gcc -o sqlite3 -Os -I. -DSQLITE_ENABLE_VFSTRACE \
**        -DSQLITE_THREADSAFE=0 -DSQLITE_ENABLE_FTS3 -DSQLITE_ENABLE_RTREE \
**        -DHAVE_READLINE -DHAVE_USLEEP=1 \
**        shell.c test_vfstrace.c sqlite3.c -ldl -lreadline -lncurses
**
** The gcc command above works on Linux and provides (in addition to the
** -vfstrace option) support for FTS3 and FTS4, RTREE, and command-line
** editing using the readline library.  The command-line shell does not
** use threads so we added -DSQLITE_THREADSAFE=0 just to make the code
** run a little faster.   For compiling on a Mac, you'll probably need
** to omit the -DHAVE_READLINE, the -lreadline, and the -lncurses options.
** The compilation could be simplified to just this:
**
**    gcc -DSQLITE_ENABLE_VFSTRACE \
**         shell.c test_vfstrace.c sqlite3.c -ldl -lpthread
**
** In this second example, all unnecessary options have been removed
** Note that since the code is now threadsafe, we had to add the -lpthread
** option to pull in the pthreads library.
**
** To cross-compile for windows using MinGW, a command like this might
** work:
**
**    /opt/mingw/bin/i386-mingw32msvc-gcc -o sqlite3.exe -Os -I \
**         -DSQLITE_THREADSAFE=0 -DSQLITE_ENABLE_VFSTRACE \
**         shell.c test_vfstrace.c sqlite3.c
**
** Similar compiler commands will work on different systems.  The key
** invariants are (1) you must have -DSQLITE_ENABLE_VFSTRACE so that
** the shell.c source file will know to include the -vfstrace command-line
** option and (2) you must compile and link the three source files
** shell,c, test_vfstrace.c, and sqlite3.c.  
*/
#include <snappy-c.h>

#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

/*
** An instance of this structure is attached to the each trace VFS to
** provide auxiliary information.
*/
typedef struct vfstrace_info vfstrace_info;
struct vfstrace_info {
  sqlite3_vfs *pRootVfs;              /* The underlying real VFS */
  int (*xOut)(const char*, void*);    /* Send output here */
  void *pOutArg;                      /* First argument to xOut */
  const char *zVfsName;               /* Name of this trace-VFS */
  sqlite3_vfs *pTraceVfs;             /* Pointer back to the trace VFS */
};

/*
** The sqlite3_file object for the trace VFS
*/
typedef struct vfstrace_file vfstrace_file;
struct vfstrace_file {
  sqlite3_file base;        /* Base class.  Must be first */
  vfstrace_info *pInfo;     /* The trace-VFS to which this file belongs */
  const char *zFName;       /* Base name of the file */
  sqlite3_file *pReal;      /* The real underlying file */
};

/*
** Method declarations for vfstrace_file.
*/
static int vfstraceClose(sqlite3_file*);
static int vfstraceRead(sqlite3_file*, void*, int iAmt, sqlite3_int64 iOfst);
static int vfstraceWrite(sqlite3_file*,const void*,int iAmt, sqlite3_int64);
static int vfstraceTruncate(sqlite3_file*, sqlite3_int64 size);
static int vfstraceSync(sqlite3_file*, int flags);
static int vfstraceFileSize(sqlite3_file*, sqlite3_int64 *pSize);
static int vfstraceLock(sqlite3_file*, int);
static int vfstraceUnlock(sqlite3_file*, int);
static int vfstraceCheckReservedLock(sqlite3_file*, int *);
static int vfstraceFileControl(sqlite3_file*, int op, void *pArg);
static int vfstraceSectorSize(sqlite3_file*);
static int vfstraceDeviceCharacteristics(sqlite3_file*);
static int vfstraceShmLock(sqlite3_file*,int,int,int);
static int vfstraceShmMap(sqlite3_file*,int,int,int, void volatile **);
static void vfstraceShmBarrier(sqlite3_file*);
static int vfstraceShmUnmap(sqlite3_file*,int);

/*
** Method declarations for vfstrace_vfs.
*/
static int vfstraceOpen(sqlite3_vfs*, const char *, sqlite3_file*, int , int *);
static int vfstraceDelete(sqlite3_vfs*, const char *zName, int syncDir);
static int vfstraceAccess(sqlite3_vfs*, const char *zName, int flags, int *);
static int vfstraceFullPathname(sqlite3_vfs*, const char *zName, int, char *);
static void *vfstraceDlOpen(sqlite3_vfs*, const char *zFilename);
static void vfstraceDlError(sqlite3_vfs*, int nByte, char *zErrMsg);
static void (*vfstraceDlSym(sqlite3_vfs*,void*, const char *zSymbol))(void);
static void vfstraceDlClose(sqlite3_vfs*, void*);
static int vfstraceRandomness(sqlite3_vfs*, int nByte, char *zOut);
static int vfstraceSleep(sqlite3_vfs*, int microseconds);
static int vfstraceCurrentTime(sqlite3_vfs*, double*);
static int vfstraceGetLastError(sqlite3_vfs*, int, char*);
static int vfstraceCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64*);
static int vfstraceSetSystemCall(sqlite3_vfs*,const char*, sqlite3_syscall_ptr);
static sqlite3_syscall_ptr vfstraceGetSystemCall(sqlite3_vfs*, const char *);
static const char *vfstraceNextSystemCall(sqlite3_vfs*, const char *zName);


/*
** Close an vfstrace-file.
*/
static int vfstraceClose(sqlite3_file *pFile){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  return p->pReal->pMethods->xClose(p->pReal);
}

/*
** Read data from an vfstrace-file.
*/
static int vfstraceRead(
  sqlite3_file *pFile, 
  void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;

  int buf_size = 4096;
  sqlite_int64 index[];
  char * zBufPtr = (char *) zBuf;
  int block = (iOfst / buf_size);
  char tmp[ block_len ];

  while (iAmt > 0) {
    sqlite_int64 realOfst = index[block];
    int block_len = index[block + 1] - index[block];

    int rc = p->pReal->pMethods->xRead(p->pReal, tmp, block_len, realOfst);
    if (rc != SQLITE_OK) {
      return rc;
    }

    size_t zBufAmt;

    if (iAmt < buf_size) {
      // The calle's buffer doesn't have enough space, so we decompress into our own space
      // and copy back
      char tmp2[ buf_size ];
      zBufAmt = buf_size;

      snappy_status status = snappy_uncompress(tmp, block_len, tmp2, &zBufAmt);
      if (status != SNAPPY_OK || zBufAmt != buf_size) {
        return SQLITE_CORRUPT;
      }

      zBufAmt = iAmt;
      memcpy(zBufPtr, tmp2, iAmt);
    } else {
      // Uncompress directly into calle's buffer
      zBufAmt = iAmt;
      snappy_status status = snappy_uncompress(tmp, block_len, zBufPtr, &zBufAmt);

      if (status != SNAPPY_OK || zBufAmt != buf_size) {
        return SQLITE_CORRUPT;
      }
    }

    zBufPtr += zBufAmt;
    iAmt    -= zBufAmt;

    block++;
  }

  return SQLITE_OK;
}

/*
** Write data to an vfstrace-file.
*/
static int vfstraceWrite(
  sqlite3_file *pFile, 
  const void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  return SQLITE_READONLY;
}

/*
** Truncate an vfstrace-file.
*/
static int vfstraceTruncate(sqlite3_file *pFile, sqlite_int64 size){
  return SQLITE_READONLY;
}

/*
** Sync an vfstrace-file.
*/
static int vfstraceSync(sqlite3_file *pFile, int flags){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  return p->pReal->pMethods->xSync(p->pReal, flags);
}

/*
** Return the current file-size of an vfstrace-file.
*/
static int vfstraceFileSize(sqlite3_file *pFile, sqlite_int64 *pSize){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  return p->pReal->pMethods->xFileSize(p->pReal, pSize);
}

/*
** Lock an vfstrace-file.
*/
static int vfstraceLock(sqlite3_file *pFile, int eLock){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  return p->pReal->pMethods->xLock(p->pReal, eLock);
}

/*
** Unlock an vfstrace-file.
*/
static int vfstraceUnlock(sqlite3_file *pFile, int eLock){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  return p->pReal->pMethods->xUnlock(p->pReal, eLock);
}

/*
** Check if another file-handle holds a RESERVED lock on an vfstrace-file.
*/
static int vfstraceCheckReservedLock(sqlite3_file *pFile, int *pResOut){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  return p->pReal->pMethods->xCheckReservedLock(p->pReal, pResOut);
}

/*
** File control method. For custom operations on an vfstrace-file.
*/
static int vfstraceFileControl(sqlite3_file *pFile, int op, void *pArg){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  return p->pReal->pMethods->xFileControl(p->pReal, op, pArg);
}

/*
** Return the sector-size in bytes for an vfstrace-file.
*/
static int vfstraceSectorSize(sqlite3_file *pFile){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  return p->pReal->pMethods->xSectorSize(p->pReal);
}

/*
** Return the device characteristic flags supported by an vfstrace-file.
*/
static int vfstraceDeviceCharacteristics(sqlite3_file *pFile){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  return p->pReal->pMethods->xDeviceCharacteristics(p->pReal);
}

/*
** Shared-memory operations.
*  TODO Figure out how to support
*/
static int vfstraceShmLock(sqlite3_file *pFile, int ofst, int n, int flags){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  return p->pReal->pMethods->xShmLock(p->pReal, ofst, n, flags);
}
static int vfstraceShmMap(
  sqlite3_file *pFile, 
  int iRegion, 
  int szRegion, 
  int isWrite, 
  void volatile **pp
){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  return p->pReal->pMethods->xShmMap(p->pReal, iRegion, szRegion, isWrite, pp);
}
static void vfstraceShmBarrier(sqlite3_file *pFile){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  p->pReal->pMethods->xShmBarrier(p->pReal);
}
static int vfstraceShmUnmap(sqlite3_file *pFile, int delFlag){
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = p->pInfo;
  return p->pReal->pMethods->xShmUnmap(p->pReal, delFlag);
}



/*
** Open an vfstrace file handle.
*/
static int vfstraceOpen(
  sqlite3_vfs *pVfs,
  const char *zName,
  sqlite3_file *pFile,
  int flags,
  int *pOutFlags
){
  int rc;
  vfstrace_file *p = (vfstrace_file *)pFile;
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  p->pInfo = pInfo;
  p->zFName = zName ? fileTail(zName) : "<temp>";
  p->pReal = (sqlite3_file *)&p[1];
  rc = pRoot->xOpen(pRoot, zName, p->pReal, flags, pOutFlags);
  vfstrace_printf(pInfo, "%s.xOpen(%s,flags=0x%x)",
                  pInfo->zVfsName, p->zFName, flags);
  if( p->pReal->pMethods ){
    sqlite3_io_methods *pNew = sqlite3_malloc( sizeof(*pNew) );
    const sqlite3_io_methods *pSub = p->pReal->pMethods;
    memset(pNew, 0, sizeof(*pNew));
    pNew->iVersion = pSub->iVersion;
    pNew->xClose = vfstraceClose;
    pNew->xRead = vfstraceRead;
    pNew->xWrite = vfstraceWrite;
    pNew->xTruncate = vfstraceTruncate;
    pNew->xSync = vfstraceSync;
    pNew->xFileSize = vfstraceFileSize;
    pNew->xLock = vfstraceLock;
    pNew->xUnlock = vfstraceUnlock;
    pNew->xCheckReservedLock = vfstraceCheckReservedLock;
    pNew->xFileControl = vfstraceFileControl;
    pNew->xSectorSize = vfstraceSectorSize;
    pNew->xDeviceCharacteristics = vfstraceDeviceCharacteristics;
    if( pNew->iVersion>=2 ){
      pNew->xShmMap = pSub->xShmMap ? vfstraceShmMap : 0;
      pNew->xShmLock = pSub->xShmLock ? vfstraceShmLock : 0;
      pNew->xShmBarrier = pSub->xShmBarrier ? vfstraceShmBarrier : 0;
      pNew->xShmUnmap = pSub->xShmUnmap ? vfstraceShmUnmap : 0;
    }
    pFile->pMethods = pNew;
  }
  vfstrace_print_errcode(pInfo, " -> %s", rc);
  if( pOutFlags ){
    vfstrace_printf(pInfo, ", outFlags=0x%x\n", *pOutFlags);
  }else{
    vfstrace_printf(pInfo, "\n");
  }
  return rc;
}

/*
** Delete the file located at zPath. If the dirSync argument is true,
** ensure the file-system modifications are synced to disk before
** returning.
*/
static int vfstraceDelete(sqlite3_vfs *pVfs, const char *zPath, int dirSync){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xDelete(pRoot, zPath, dirSync);
}

/*
** Test for access permissions. Return true if the requested permission
** is available, or false otherwise.
*/
static int vfstraceAccess(
  sqlite3_vfs *pVfs,
  const char *zPath,
  int flags,
  int *pResOut
){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xAccess(pRoot, zPath, flags, pResOut);
}

/*
** Populate buffer zOut with the full canonical pathname corresponding
** to the pathname in zPath. zOut is guaranteed to point to a buffer
** of at least (DEVSYM_MAX_PATHNAME+1) bytes.
*/
static int vfstraceFullPathname(
  sqlite3_vfs *pVfs,
  const char *zPath,
  int nOut,
  char *zOut
){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xFullPathname(pRoot, zPath, nOut, zOut);
}

/*
** Open the dynamic library located at zPath and return a handle.
*/
static void *vfstraceDlOpen(sqlite3_vfs *pVfs, const char *zPath){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xDlOpen(pRoot, zPath);
}

/*
** Populate the buffer zErrMsg (size nByte bytes) with a human readable
** utf-8 string describing the most recent error encountered associated
** with dynamic libraries.
*/
static void vfstraceDlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  pRoot->xDlError(pRoot, nByte, zErrMsg);
}

/*
** Return a pointer to the symbol zSymbol in the dynamic library pHandle.
*/
static void (*vfstraceDlSym(sqlite3_vfs *pVfs,void *p,const char *zSym))(void){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xDlSym(pRoot, p, zSym);
}

/*
** Close the dynamic library handle pHandle.
*/
static void vfstraceDlClose(sqlite3_vfs *pVfs, void *pHandle){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  pRoot->xDlClose(pRoot, pHandle);
}

/*
** Populate the buffer pointed to by zBufOut with nByte bytes of 
** random data.
*/
static int vfstraceRandomness(sqlite3_vfs *pVfs, int nByte, char *zBufOut){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xRandomness(pRoot, nByte, zBufOut);
}

/*
** Sleep for nMicro microseconds. Return the number of microseconds 
** actually slept.
*/
static int vfstraceSleep(sqlite3_vfs *pVfs, int nMicro){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xSleep(pRoot, nMicro);
}

/*
** Return the current time as a Julian Day number in *pTimeOut.
*/
static int vfstraceCurrentTime(sqlite3_vfs *pVfs, double *pTimeOut){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xCurrentTime(pRoot, pTimeOut);
}
static int vfstraceCurrentTimeInt64(sqlite3_vfs *pVfs, sqlite3_int64 *pTimeOut){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xCurrentTimeInt64(pRoot, pTimeOut);
}

/*
** Return the most recent error code and message
*/
static int vfstraceGetLastError(sqlite3_vfs *pVfs, int iErr, char *zErr){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xGetLastError(pRoot, iErr, zErr);
}

/*
** Override system calls.
*/
static int vfstraceSetSystemCall(
  sqlite3_vfs *pVfs,
  const char *zName,
  sqlite3_syscall_ptr pFunc
){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xSetSystemCall(pRoot, zName, pFunc);
}

static sqlite3_syscall_ptr vfstraceGetSystemCall(
  sqlite3_vfs *pVfs,
  const char *zName
){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xGetSystemCall(pRoot, zName);
}

static const char *vfstraceNextSystemCall(sqlite3_vfs *pVfs, const char *zName){
  vfstrace_info *pInfo = (vfstrace_info*)pVfs->pAppData;
  sqlite3_vfs *pRoot = pInfo->pRootVfs;
  return pRoot->xNextSystemCall(pRoot, zName);
}


/*
** Clients invoke this routine to construct a new trace-vfs shim.
**
** Return SQLITE_OK on success.  
**
** SQLITE_NOMEM is returned in the case of a memory allocation error.
** SQLITE_NOTFOUND is returned if zOldVfsName does not exist.
*/
int vfstrace_register(
   const char *zTraceName,           /* Name of the newly constructed VFS */
   const char *zOldVfsName,          /* Name of the underlying VFS */
   int (*xOut)(const char*,void*),   /* Output routine.  ex: fputs */
   void *pOutArg,                    /* 2nd argument to xOut.  ex: stderr */
   int makeDefault                   /* True to make the new VFS the default */
){
  sqlite3_vfs *pNew;
  sqlite3_vfs *pRoot;
  vfstrace_info *pInfo;
  int nName;
  int nByte;

  pRoot = sqlite3_vfs_find(zOldVfsName);
  if( pRoot==0 ) return SQLITE_NOTFOUND;

  nName = strlen(zTraceName);
  nByte = sizeof(*pNew) + sizeof(*pInfo) + nName + 1;
  pNew = sqlite3_malloc( nByte );
  if( pNew==0 ) return SQLITE_NOMEM;

  memset(pNew, 0, nByte);

  pInfo = (vfstrace_info*)&pNew[1];
  pNew->szOsFile   = pRoot->szOsFile + sizeof(vfstrace_file);
  pNew->mxPathname = pRoot->mxPathname;
  pNew->zName = (char*)&pInfo[1];
  memcpy((char*)&pInfo[1], zTraceName, nName+1);

  pNew->pAppData = pInfo;
  pNew->iVersion = 2; //pRoot->iVersion;

  pNew->xOpen         = vfstraceOpen;
  pNew->xDelete       = vfstraceDelete;
  pNew->xAccess       = vfstraceAccess;
  pNew->xFullPathname = vfstraceFullPathname;
  pNew->xDlOpen       = vfstraceDlOpen;
  pNew->xDlError      = vfstraceDlError;
  pNew->xDlSym        = vfstraceDlSym;
  pNew->xDlClose      = vfstraceDlClose;
  pNew->xRandomness   = vfstraceRandomness;
  pNew->xSleep        = vfstraceSleep;
  pNew->xCurrentTime  = vfstraceCurrentTime;
  pNew->xGetLastError = vfstraceGetLastError;

  if( pNew->iVersion >= 2 ) {
    pNew->xCurrentTimeInt64 = vfstraceCurrentTimeInt64;
    if( pNew->iVersion >= 3 ) {
      pNew->xSetSystemCall  = vfstraceSetSystemCall;
      pNew->xGetSystemCall  = vfstraceGetSystemCall;
      pNew->xNextSystemCall = vfstraceNextSystemCall;
    }
  }

  pInfo->pRootVfs  = pRoot;
  pInfo->xOut      = xOut;
  pInfo->pOutArg   = pOutArg;
  pInfo->zVfsName  = pNew->zName;
  pInfo->pTraceVfs = pNew;
  return sqlite3_vfs_register(pNew, makeDefault);
}
