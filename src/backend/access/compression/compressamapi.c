/*-------------------------------------------------------------------------
 *
 * compression/compressamapi.c
 *	  Functions for compression methods
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/compression/compressamapi.c
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/compressamapi.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"

/*
 * CompressionOidtoId - Convert compression Oid to built-in compression id.
 *
 * For more details refer comment atop CompressionId in compressamapi.h
 */
CompressionId
CompressionOidToId(Oid cmoid)
{
	switch (cmoid)
	{
		case PGLZ_COMPRESSION_AM_OID:
			return PGLZ_COMPRESSION_ID;
		case LZ4_COMPRESSION_AM_OID:
			return LZ4_COMPRESSION_ID;
		default:
			elog(ERROR, "Invalid compression method oid %u", cmoid);
	}
}

/*
 * CompressionIdtoOid - Convert built-in compression id to Oid
 *
 * For more details refer comment atop CompressionId in compressamapi.h
 */
Oid
CompressionIdToOid(Oid cmid)
{
	switch (cmid)
	{
		case PGLZ_COMPRESSION_ID:
			return PGLZ_COMPRESSION_AM_OID;
		case LZ4_COMPRESSION_ID:
			return LZ4_COMPRESSION_AM_OID;
		default:
			elog(ERROR, "Invalid compression method id %d", cmid);
	}
}
