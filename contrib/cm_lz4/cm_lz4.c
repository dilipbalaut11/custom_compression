/*-------------------------------------------------------------------------
 *
 * cm_lz4.c
 *		lz4 compression method.
 *
 * Portions Copyright (c) 2016-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1990-1993, Regents of the University of California
 *
 * IDENTIFICATION
 *	  contrib/cm_lz4/cm_lz4.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "postgres.h"
#include "access/compressionapi.h"
#include "access/toast_internals.h"

#include "fmgr.h"
#include "lz4.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(lz4handler);

void		_PG_init(void);

//typedef int int32;
/*
 * Module initialize function: initialize info about lz4
 */
void
_PG_init(void)
{

}

/*
 * lz4_cmcompress - compression routine for lz4 compression method
 *
 * Compresses source into dest using the default strategy. Returns the
 * compressed varlena, or NULL if compression fails.
 */
static struct varlena *
lz4_cmcompress(const struct varlena *value, int32 header_size)
{
	int32		valsize;
	int32		len;
	int32		max_size;
	struct varlena *tmp = NULL;

	valsize = VARSIZE_ANY_EXHDR(value);

	max_size = LZ4_compressBound(VARSIZE_ANY_EXHDR(value));
	tmp = (struct varlena *) palloc(max_size + header_size);

	len = LZ4_compress_default(VARDATA_ANY(value),
				   (char *) tmp + header_size,
				   valsize, max_size);

	if (len >= 0)
	{
		SET_VARSIZE_COMPRESSED(tmp, len + header_size);
		return tmp;
	}

	pfree(tmp);

	return NULL;
}

/*
 * lz4_cmdecompress - decompression routine for lz4 compression method
 *
 * Returns the decompressed varlena.
 */
static struct varlena *
lz4_cmdecompress(const struct varlena *value, int32 header_size)
{
	int32		rawsize;
	struct varlena *result;

	result = (struct varlena *) palloc(TOAST_COMPRESS_RAWSIZE(value) + VARHDRSZ);
	SET_VARSIZE(result, TOAST_COMPRESS_RAWSIZE(value) + VARHDRSZ);

	rawsize = LZ4_decompress_safe((char *)value + header_size,
								  VARDATA(result),
								  VARSIZE(value) - header_size,
								  TOAST_COMPRESS_RAWSIZE(value));
	if (rawsize < 0)
		elog(ERROR, "lz4: compressed data is corrupted");

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
}

/*
 * lz4_cmdecompress_slice - slice decompression routine for lz4 compression
 *
 * Decompresses part of the data. Returns the decompressed varlena.
 */
static struct varlena *
lz4_cmdecompress_slice(const struct varlena *value, int32 header_size,
					   int32 slicelength)
{
	int32		rawsize;
	struct varlena *result;

	result = (struct varlena *)palloc(slicelength + VARHDRSZ);
	SET_VARSIZE(result, TOAST_COMPRESS_RAWSIZE(value) + VARHDRSZ);

	rawsize = LZ4_decompress_safe_partial((char *)value + header_size,
										  VARDATA(result),
										  VARSIZE(value) - header_size,
										  slicelength,
										  slicelength);
	if (rawsize < 0)
		elog(ERROR, "lz4: compressed data is corrupted");

	SET_VARSIZE(result, rawsize + VARHDRSZ);

	return result;
}

/* lz4 is the default compression method */
Datum
lz4handler(PG_FUNCTION_ARGS)
{
	CompressionRoutine *routine = makeNode(CompressionRoutine);

	routine->cmcompress = lz4_cmcompress;
	routine->cmdecompress = lz4_cmdecompress;
	routine->cmdecompress_slice = lz4_cmdecompress_slice;

	PG_RETURN_POINTER(routine);
}
