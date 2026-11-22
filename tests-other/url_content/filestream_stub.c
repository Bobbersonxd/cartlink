/* Link stubs: lrc_hash.c's sha1_calculate() references the file stream
 * API, which these pure-logic tests never call. */

#include <stddef.h>
#include <stdint.h>
#include <streams/file_stream.h>

RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
   return NULL;
}

int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{
   return -1;
}

int filestream_close(RFILE *stream)
{
   return -1;
}
