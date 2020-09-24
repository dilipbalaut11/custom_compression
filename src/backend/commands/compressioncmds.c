/*-------------------------------------------------------------------------
 *
 * compressioncmds.c
 *	  Routines for SQL commands for compression access methods
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/compressioncmds.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "miscadmin.h"

#include "access/cmapi.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_attr_compression_d.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_proc_d.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/snapmgr.h"

/* Set by pg_upgrade_support functions */
Oid			binary_upgrade_next_attr_compression_oid = InvalidOid;

/*
 * When conditions of compression satisfies one if builtin attribute
 * compresssion tuples the compressed attribute will be linked to
 * builtin compression without new record in pg_attr_compression.
 * So the fact that the column has a builtin compression we only can find out
 * by its dependency.
 */
static void
lookup_builtin_dependencies(Oid attrelid, AttrNumber attnum,
							List **amoids)
{
	LOCKMODE	lock = AccessShareLock;
	Oid			amoid = InvalidOid;
	HeapTuple	tup;
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[3];

	rel = table_open(DependRelationId, lock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(attrelid));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum((int32) attnum));

	scan = systable_beginscan(rel, DependDependerIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);

		if (depform->refclassid == AttrCompressionRelationId)
		{
			Assert(IsBuiltinCompression(depform->refobjid));
			amoid = GetAttrCompressionAmOid(depform->refobjid);
			*amoids = list_append_unique_oid(*amoids, amoid);
		}
	}

	systable_endscan(scan);
	table_close(rel, lock);
}

/*
 * Find identical attribute compression for reuse and fill the list with
 * used compression methods.
 */
static Oid
lookup_attribute_compression(Oid attrelid, AttrNumber attnum,
							 char cm, List **previous_cms)
{
	Relation	rel;
	HeapTuple	tuple;
	SysScanDesc scan;
	FmgrInfo	arrayeq_info;
	Oid			result = InvalidOid;
	ScanKeyData key[2];

	/* fill FmgrInfo for array_eq function */
	fmgr_info(F_ARRAY_EQ, &arrayeq_info);

	Assert((attrelid > 0 && attnum > 0) || (attrelid == 0 && attnum == 0));

	rel = table_open(AttrCompressionRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_attr_compression_acrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(attrelid));

	ScanKeyInit(&key[1],
				Anum_pg_attr_compression_acattnum,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(attnum));

	scan = systable_beginscan(rel, AttrCompressionRelidAttnumIndexId,
							  true, NULL, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Oid			acoid;
		char		tup_cm;
		Datum		values[Natts_pg_attr_compression];
		bool		nulls[Natts_pg_attr_compression];

		heap_deform_tuple(tuple, RelationGetDescr(rel), values, nulls);
		acoid = DatumGetObjectId(values[Anum_pg_attr_compression_acoid - 1]);
		tup_cm = DatumGetChar(values[Anum_pg_attr_compression_acmethod - 1]);

		if (previous_cms)
			*previous_cms = list_append_unique_oid(*previous_cms, tup_cm);

		if (tup_cm != cm)
			continue;

		result = acoid;

		if (previous_cms == NULL && IsValidCompression(result))
			break;
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return result;
}

/*
 * Link compression with an attribute. Creates a row in pg_attr_compression
 * if needed.
 *
 * When compression is not specified returns default attribute compression.
 * It is possible case for CREATE TABLE and ADD COLUMN commands
 * where COMPRESSION syntax is optional.
 *
 * If any of builtin attribute compression tuples satisfies conditions
 * returns it.
 *
 * For ALTER command check for previous attribute compression record with
 * identical compression options and reuse it if found any.
 *
 * Note we create attribute compression for EXTERNAL storage too, so when
 * storage is changed we can start compression on future tuples right away.
 */
char
CreateAttributeCompression(Form_pg_attribute att,
						   ColumnCompression *compression,
						   bool *need_rewrite, List **preserved_cmids)
{
	Relation	rel;
	HeapTuple	newtup;
	Oid			acoid = InvalidOid,
				amoid;
	Datum		values[Natts_pg_attr_compression];
	bool		nulls[Natts_pg_attr_compression];
	PGCompressionID cmid;

	ObjectAddress myself;
	int i;

	/* No compression for PLAIN storage. */
	if (att->attstorage == TYPSTORAGE_PLAIN)
		return InvalidCompressionMethod;

	/* Fallback to default compression if it's not specified */
	if (compression == NULL)
		return DefaultCompressionMethod;

	cmid = GetCompressionMethodIDFromName(compression->cmname);

	/* no rewrite by default */
	if (need_rewrite != NULL)
		*need_rewrite = false;

	if (IsBinaryUpgrade)
	{
		/* Skip the rewrite checks and searching of identical compression */
		goto add_tuple;
	}

	/*
	 * attrelid will be invalid on CREATE TABLE, no need for table rewrite
	 * check.
	 */
	if (OidIsValid(att->attrelid))
	{
		List	   *previous_cmids = NIL;

		/*
		 * Try to find identical compression from previous tuples, and fill
		 * the list of previous compresssion methods.
		 */
		acoid = lookup_attribute_compression(att->attrelid, att->attnum,
											 cmid, &previous_cmids);

		/*
		 * Determine if the column needs rewrite or not. Rewrite conditions: -
		 * SET COMPRESSION without PRESERVE - SET COMPRESSION with PRESERVE
		 * but not with full list of previous access methods.
		 */
		if (need_rewrite != NULL)
		{
			Assert(preserved_cmids != NULL);

			if (compression->preserve == NIL)
				*need_rewrite = true;
			else
			{
				ListCell   *cell;

				foreach(cell, compression->preserve)
				{
					char   *cmname = strVal(lfirst(cell));
					PGCompressionID cmid_p = GetCompressionMethodIDFromName(cmname);

					if (!list_member_int(previous_cmids, cmid))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("\"%s\" compression access method cannot be preserved", amname_p),
								 errhint("use \"pg_column_compression\" function for list of compression methods")
								 ));

					*preserved_cmids = list_append_unique_int(*preserved_cmids, cmid_p);

					/*
					 * Remove from previous list, also protect from multiple
					 * mentions of one access method in PRESERVE list
					 */
					previous_cmids = list_delete_oid(previous_cmids, cmid_p);
				}

				/*
				 * If the list of previous Oids is not empty after deletions
				 * then we need to rewrite tuples in the table.
				 *
				 * In binary upgrade list will not be free since it contains
				 * Oid of builtin compression access method.
				 */
				if (list_length(previous_cmids) != 0)
					*need_rewrite = true;
			}
		}

		/* Cleanup */
		list_free(previous_cmids);
	}

	/* Return Oid if we already found identical compression on this column */
	if (OidIsValid(acoid))
		return acoid;

add_tuple:
	/* Initialize buffers for new tuple values */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	rel = table_open(AttrCompressionRelationId, RowExclusiveLock);

	if (IsBinaryUpgrade)
	{
		/* acoid should be found in some cases */
		if (binary_upgrade_next_attr_compression_oid < FirstNormalObjectId &&
			(!OidIsValid(acoid) || binary_upgrade_next_attr_compression_oid != acoid))
			elog(ERROR, "could not link to built-in attribute compression");

		acoid = binary_upgrade_next_attr_compression_oid;
	}
	else
	{
		acoid = GetNewOidWithIndex(rel, AttrCompressionIndexId,
									Anum_pg_attr_compression_acoid);

	}

	if (acoid < FirstNormalObjectId)
	{
		/* this is built-in attribute compression */
		table_close(rel, RowExclusiveLock);
		return acoid;
	}


	values[Anum_pg_attr_compression_acoid - 1] = ObjectIdGetDatum(acoid);
	values[Anum_pg_attr_compression_acmethod - 1] = GetCompressionMethod(att, compression->cmname);
	values[Anum_pg_attr_compression_acrelid - 1] = ObjectIdGetDatum(att->attrelid);
	values[Anum_pg_attr_compression_acattnum - 1] = Int32GetDatum(att->attnum);

	newtup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, newtup);
	heap_freetuple(newtup);
	table_close(rel, RowExclusiveLock);

	ObjectAddressSet(myself, AttrCompressionRelationId, acoid);

	/* Make the changes visible */
	CommandCounterIncrement();

	return acoid;
}

/*
 * Remove the attribute compression record from pg_attr_compression.
 */
void
RemoveAttributeCompression(Oid acoid)
{
	Relation	relation;
	HeapTuple	tup;

	tup = SearchSysCache1(ATTCOMPRESSIONOID, ObjectIdGetDatum(acoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for attribute compression %u", acoid);

	/* check we're not trying to remove builtin attribute compression */
	Assert(((Form_pg_attr_compression) GETSTRUCT(tup))->acrelid != 0);

	/* delete the record from catalogs */
	relation = table_open(AttrCompressionRelationId, RowExclusiveLock);
	CatalogTupleDelete(relation, &tup->t_self);
	table_close(relation, RowExclusiveLock);
	ReleaseSysCache(tup);
}

/*
 * CleanupAttributeCompression
 *
 * Remove entries in pg_attr_compression of the column except current
 * attribute compression and related with specified list of access methods.
 */
void
CleanupAttributeCompression(Oid relid, AttrNumber attnum, List *keepAmOids)
{
	Oid			acoid,
				amoid;
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData key[3];
	HeapTuple	tuple,
				attrtuple;
	Form_pg_attribute attform;
	List	   *removed = NIL;
	ListCell   *lc;

	attrtuple = SearchSysCache2(ATTNUM,
								ObjectIdGetDatum(relid),
								Int16GetDatum(attnum));

	if (!HeapTupleIsValid(attrtuple))
		elog(ERROR, "cache lookup failed for attribute %d of relation %u",
			 attnum, relid);
	attform = (Form_pg_attribute) GETSTRUCT(attrtuple);
	acoid = attform->attcompression;
	ReleaseSysCache(attrtuple);

	Assert(relid > 0 && attnum > 0);
	Assert(!IsBinaryUpgrade);

	rel = table_open(AttrCompressionRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_attr_compression_acrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	ScanKeyInit(&key[1],
				Anum_pg_attr_compression_acattnum,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(attnum));

	scan = systable_beginscan(rel, AttrCompressionRelidAttnumIndexId,
							  true, NULL, 2, key);

	/*
	 * Remove attribute compression tuples and collect removed Oids
	 * to list.
	 */
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_attr_compression acform;

		acform = (Form_pg_attr_compression) GETSTRUCT(tuple);
		amoid = get_am_oid(NameStr(acform->acname), false);

		/* skip current compression */
		if (acform->acoid == acoid)
			continue;

		if (!list_member_oid(keepAmOids, amoid))
		{
			removed = lappend_oid(removed, acform->acoid);
			CatalogTupleDelete(rel, &tuple->t_self);
		}
	}

	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);

	/*
	 * Now remove dependencies between attribute compression (dependent)
	 * and column.
	 */
	rel = table_open(DependRelationId, RowExclusiveLock);
	foreach(lc, removed)
	{
		Oid			tup_acoid = lfirst_oid(lc);

		ScanKeyInit(&key[0],
					Anum_pg_depend_classid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(AttrCompressionRelationId));
		ScanKeyInit(&key[1],
					Anum_pg_depend_objid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(tup_acoid));

		scan = systable_beginscan(rel, DependDependerIndexId, true,
								  NULL, 2, key);

		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
			CatalogTupleDelete(rel, &tuple->t_self);

		systable_endscan(scan);
	}
	table_close(rel, RowExclusiveLock);

	/* Now remove dependencies with builtin compressions */
	rel = table_open(DependRelationId, RowExclusiveLock);
	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum((int32) attnum));

	scan = systable_beginscan(rel, DependDependerIndexId, true,
							  NULL, 3, key);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tuple);

		if (depform->refclassid != AttrCompressionRelationId)
			continue;

		/* skip current compression */
		if (depform->refobjid == acoid)
			continue;

		amoid = GetAttrCompressionAmOid(depform->refobjid);
		if (!list_member_oid(keepAmOids, amoid))
			CatalogTupleDelete(rel, &tuple->t_self);
	}

	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);
}

/*
 * Construct ColumnCompression node by attribute compression Oid.
 */
ColumnCompression *
MakeColumnCompression(Oid acoid)
{
	HeapTuple	tuple;
	Form_pg_attr_compression acform;
	ColumnCompression *node;

	if (!OidIsValid(acoid))
		return NULL;

	tuple = SearchSysCache1(ATTCOMPRESSIONOID, ObjectIdGetDatum(acoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for attribute compression %u", acoid);

	acform = (Form_pg_attr_compression) GETSTRUCT(tuple);
	node = makeNode(ColumnCompression);
	node->amname = pstrdup(NameStr(acform->acname));
	ReleaseSysCache(tuple);

	/*
	 * fill attribute compression options too. We could've do it above but
	 * it's easier to call this helper.
	 */
	node->options = GetAttrCompressionOptions(acoid);

	return node;
}

/*
 * Compare compression options for two columns.
 */
void
CheckCompressionMismatch(ColumnCompression *c1, ColumnCompression *c2,
						 const char *attributeName)
{
	if (strcmp(c1->amname, c2->amname))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" has a compression method conflict",
						attributeName),
				 errdetail("%s versus %s", c1->amname, c2->amname)));

	if (!equal(c1->options, c2->options))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" has a compression options conflict",
						attributeName),
				 errdetail("(%s) versus (%s)",
						   formatRelOptions(c1->options),
						   formatRelOptions(c2->options))));
}

/*
 * Return list of compression methods used in specified column.
 */
Datum
pg_column_compression(PG_FUNCTION_ARGS)
{
	Oid			relOid = PG_GETARG_OID(0);
	char	   *attname = TextDatumGetCString(PG_GETARG_TEXT_P(1));
	Relation	rel;
	HeapTuple	tuple;
	AttrNumber	attnum;
	List	   *amoids = NIL;
	Oid			amoid;
	ListCell   *lc;

	ScanKeyData key[2];
	SysScanDesc scan;
	StringInfoData result;

	attnum = get_attnum(relOid, attname);
	if (attnum == InvalidAttrNumber)
		PG_RETURN_NULL();

	/* Collect related builtin compression access methods */
	lookup_builtin_dependencies(relOid, attnum, &amoids);

	/* Collect other related access methods */
	rel = table_open(AttrCompressionRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_attr_compression_acrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relOid));
	ScanKeyInit(&key[1],
				Anum_pg_attr_compression_acattnum,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(attnum));

	scan = systable_beginscan(rel, AttrCompressionRelidAttnumIndexId,
							  true, NULL, 2, key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_attr_compression acform;

		acform = (Form_pg_attr_compression) GETSTRUCT(tuple);
		amoid = get_am_oid(NameStr(acform->acname), false);
		amoids = list_append_unique_oid(amoids, amoid);
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	if (!list_length(amoids))
		PG_RETURN_NULL();

	/* Construct the list separated by comma */
	amoid = InvalidOid;
	initStringInfo(&result);
	foreach(lc, amoids)
	{
		if (OidIsValid(amoid))
			appendStringInfoString(&result, ", ");

		amoid = lfirst_oid(lc);
		appendStringInfoString(&result, get_am_name(amoid));
	}

	PG_RETURN_TEXT_P(CStringGetTextDatum(result.data));
}
