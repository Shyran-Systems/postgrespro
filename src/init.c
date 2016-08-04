/* ------------------------------------------------------------------------
 *
 * init.c
 *		Initialization functions
 *
 * Copyright (c) 2015-2016, Postgres Professional
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * ------------------------------------------------------------------------
 */

#include "hooks.h"
#include "init.h"
#include "pathman.h"
#include "relation_info.h"
#include "utils.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/indexing.h"
#include "catalog/pg_inherits_fn.h"
#include "catalog/pg_type.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_constraint.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "utils/inval.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"
#include "utils/builtins.h"
#include "utils/typcache.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"


/* Initial size of 'partitioned_rels' table */
#define PART_RELS_SIZE	10
#define CHILD_FACTOR	500


/* Storage for PartRelationInfos */
HTAB   *partitioned_rels = NULL;

/* Storage for PartParentInfos */
HTAB   *parent_cache = NULL;

bool	initialization_needed = true;


static void read_pathman_config(void);

static Expr *get_partition_constraint_expr(Oid partition, AttrNumber part_attno);

static int cmp_range_entries(const void *p1, const void *p2, void *arg);

static bool validate_range_constraint(const Expr *expr,
									  const PartRelationInfo *prel,
									  Datum *min,
									  Datum *max);

static bool validate_hash_constraint(const Expr *expr,
									 const PartRelationInfo *prel,
									 uint32 *part_hash);

static bool read_opexpr_const(const OpExpr *opexpr, AttrNumber varattno, Datum *val);

static int oid_cmp(const void *p1, const void *p2);


/*
 * Create local PartRelationInfo cache & load pg_pathman's config.
 */
void
load_config()
{
	init_local_config();	/* create 'relations' hash table */
	read_pathman_config();	/* read PATHMAN_CONFIG table & fill cache */

	initialization_needed = false;

	elog(DEBUG2, "pg_pathman's config has been loaded successfully");
}

/*
 * Estimate shmem amount needed for pg_pathman to run.
 */
Size
estimate_pathman_shmem_size(void)
{
	return estimate_dsm_config_size() + MAXALIGN(sizeof(PathmanState));
}

/*
 * Initialize per-process resources.
 */
void
init_local_config(void)
{
	HASHCTL ctl;

	if (partitioned_rels)
	{
		elog(DEBUG2, "pg_pathman's partitioned relations table already exists");
		return;
	}

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(PartRelationInfo);
	ctl.hcxt = TopMemoryContext; /* place data to persistent mcxt */

	partitioned_rels = hash_create("pg_pathman's partitioned relations cache",
								   PART_RELS_SIZE, &ctl, HASH_ELEM | HASH_BLOBS);

	memset(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(PartParentInfo);
	ctl.hcxt = TopMemoryContext; /* place data to persistent mcxt */

	parent_cache = hash_create("pg_pathman's partition parents cache",
							   PART_RELS_SIZE * CHILD_FACTOR,
							   &ctl, HASH_ELEM | HASH_BLOBS);

	CacheRegisterRelcacheCallback(pathman_relcache_hook,
								  PointerGetDatum(NULL));
}

/*
 * Initializes pg_pathman's global state (PathmanState) & locks.
 */
void
init_shmem_config(void)
{
	bool found;

	/* Check if module was initialized in postmaster */
	pmstate = ShmemInitStruct("pg_pathman's global state",
							  sizeof(PathmanState), &found);
	if (!found)
	{
		/*
		 * Initialize locks in postmaster
		 */
		if (!IsUnderPostmaster)
		{
			/* Initialize locks */
			pmstate->load_config_lock		= LWLockAssign();
			pmstate->dsm_init_lock			= LWLockAssign();
			pmstate->edit_partitions_lock	= LWLockAssign();
		}
	}
}

/*
 * Fill PartRelationInfo with partition-related info.
 */
void
fill_prel_with_partitions(const Oid *partitions,
						  const uint32 parts_count,
						  PartRelationInfo *prel)
{
	uint32			i;
	Expr		   *con_expr;
	MemoryContext	mcxt = TopMemoryContext;

	/* Allocate memory for 'prel->children' & 'prel->ranges' (if needed) */
	prel->children = MemoryContextAllocZero(mcxt, parts_count * sizeof(Oid));
	if (prel->parttype == PT_RANGE)
		prel->ranges = MemoryContextAllocZero(mcxt, parts_count * sizeof(RangeEntry));
	prel->children_count = parts_count;

	for (i = 0; i < PrelChildrenCount(prel); i++)
	{
		con_expr = get_partition_constraint_expr(partitions[i], prel->attnum);

		/* Perform a partitioning_type-dependent task */
		switch (prel->parttype)
		{
			case PT_HASH:
				{
					uint32	hash; /* hash value < parts_count */

					if (validate_hash_constraint(con_expr, prel, &hash))
						prel->children[hash] = partitions[i];
					else
						elog(ERROR,
							 "Wrong constraint format for HASH partition %u",
							 partitions[i]);
				}
				break;

			case PT_RANGE:
				{
					Datum	range_min, range_max;

					if (validate_range_constraint(con_expr, prel,
												  &range_min, &range_max))
					{
						prel->ranges[i].child_oid	= partitions[i];
						prel->ranges[i].min			= range_min;
						prel->ranges[i].max			= range_max;
					}
					else
						elog(ERROR,
							 "Wrong constraint format for RANGE partition %u",
							 partitions[i]);
				}
				break;

			default:
				elog(ERROR, "Unknown partitioning type for relation %u", prel->key);
		}
	}

	/* Finalize 'prel' for a RANGE-partitioned table */
	if (prel->parttype == PT_RANGE)
	{
		TypeCacheEntry *tce = lookup_type_cache(prel->atttype,
												TYPECACHE_CMP_PROC_FINFO);

		/* Sort partitions by RangeEntry->min asc */
		qsort_arg((void *) prel->ranges, PrelChildrenCount(prel),
				  sizeof(RangeEntry), cmp_range_entries,
				  (void *) &tce->cmp_proc_finfo);

		/* Initialize 'prel->children' array */
		for (i = 0; i < PrelChildrenCount(prel); i++)
			prel->children[i] = prel->ranges[i].child_oid;
	}

#ifdef USE_ASSERT_CHECKING
	/* Check that each partition Oid has been assigned properly */
	if (prel->parttype == PT_HASH)
		for (i = 0; i < PrelChildrenCount(prel); i++)
		{
			if (prel->children[i] == InvalidOid)
				elog(ERROR, "pg_pathman's cache for relation %u "
							"has not been properly initialized", prel->key);
		}
#endif
}

/*
 * find_inheritance_children
 *
 * Returns an array containing the OIDs of all relations which
 * inherit *directly* from the relation with OID 'parentrelId'.
 *
 * The specified lock type is acquired on each child relation (but not on the
 * given rel; caller should already have locked it).  If lockmode is NoLock
 * then no locks are acquired, but caller must beware of race conditions
 * against possible DROPs of child relations.
 *
 * borrowed from pg_inherits.c
 */
Oid *
find_inheritance_children_array(Oid parentrelId, LOCKMODE lockmode, uint32 *size)
{
	Relation	relation;
	SysScanDesc scan;
	ScanKeyData key[1];
	HeapTuple	inheritsTuple;
	Oid			inhrelid;
	Oid		   *oidarr;
	uint32		maxoids,
				numoids,
				i;

	/*
	 * Can skip the scan if pg_class shows the relation has never had a
	 * subclass.
	 */
	if (!has_subclass(parentrelId))
	{
		*size = 0;
		return NULL;
	}

	/*
	 * Scan pg_inherits and build a working array of subclass OIDs.
	 */
	maxoids = 32;
	oidarr = (Oid *) palloc(maxoids * sizeof(Oid));
	numoids = 0;

	relation = heap_open(InheritsRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(parentrelId));

	scan = systable_beginscan(relation, InheritsParentIndexId, true,
							  NULL, 1, key);

	while ((inheritsTuple = systable_getnext(scan)) != NULL)
	{
		inhrelid = ((Form_pg_inherits) GETSTRUCT(inheritsTuple))->inhrelid;
		if (numoids >= maxoids)
		{
			maxoids *= 2;
			oidarr = (Oid *) repalloc(oidarr, maxoids * sizeof(Oid));
		}
		oidarr[numoids++] = inhrelid;
	}

	systable_endscan(scan);

	heap_close(relation, AccessShareLock);

	/*
	 * If we found more than one child, sort them by OID.  This ensures
	 * reasonably consistent behavior regardless of the vagaries of an
	 * indexscan.  This is important since we need to be sure all backends
	 * lock children in the same order to avoid needless deadlocks.
	 */
	if (numoids > 1)
		qsort(oidarr, numoids, sizeof(Oid), oid_cmp);

	/*
	 * Acquire locks and build the result list.
	 */
	for (i = 0; i < numoids; i++)
	{
		inhrelid = oidarr[i];

		if (lockmode != NoLock)
		{
			/* Get the lock to synchronize against concurrent drop */
			LockRelationOid(inhrelid, lockmode);

			/*
			 * Now that we have the lock, double-check to see if the relation
			 * really exists or not.  If not, assume it was dropped while we
			 * waited to acquire lock, and ignore it.
			 */
			if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(inhrelid)))
			{
				/* Release useless lock */
				UnlockRelationOid(inhrelid, lockmode);
				/* And ignore this relation */
				continue;
			}
		}
	}

	*size = numoids;
	return oidarr;
}

/*
 * Generate check constraint name for a partition.
 *
 * This function does not perform sanity checks at all.
 */
char *
build_check_constraint_name_internal(Oid relid, AttrNumber attno)
{
	return psprintf("pathman_%u_%u_check", relid, attno);
}

/*
 * Check that relation 'relid' is partitioned by pg_pathman.
 *
 * Extract tuple into 'values' and 'isnull' if they're provided.
 */
bool
pathman_config_contains_relation(Oid relid, Datum *values, bool *isnull,
								 TransactionId *xmin)
{
	Oid				pathman_config;
	Relation		rel;
	HeapScanDesc	scan;
	ScanKeyData		key[1];
	Snapshot		snapshot;
	HeapTuple		htup;
	bool			contains_rel = false;

	/* Get PATHMAN_CONFIG table Oid */
	pathman_config = get_relname_relid(PATHMAN_CONFIG, get_pathman_schema());

	ScanKeyInit(&key[0],
				Anum_pathman_config_partrel,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	/* Open relation with latest snapshot available */
	rel = heap_open(pathman_config, AccessShareLock);

	/* Check that 'partrel' column is if regclass type */
	Assert(RelationGetDescr(rel)->
		   attrs[Anum_pathman_config_partrel - 1]->
		   atttypid == REGCLASSOID);

	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = heap_beginscan(rel, snapshot, 1, key);

	while((htup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		contains_rel = true; /* found partitioned table */

		/* Extract data if necessary */
		if (values && isnull)
			heap_deformtuple(htup, RelationGetDescr(rel), values, isnull);

		/* Set xmin if necessary */
		if (xmin)
		{
			Datum	value;
			bool	isnull;

			value = heap_getsysattr(htup,
									MinTransactionIdAttributeNumber,
									RelationGetDescr(rel),
									&isnull);

			Assert(!isnull);
			*xmin = DatumGetTransactionId(value);
		}
	}

	/* Clean resources */
	heap_endscan(scan);
	UnregisterSnapshot(snapshot);
	heap_close(rel, AccessShareLock);

	elog(DEBUG2, "PATHMAN_CONFIG table %s relation %u",
		 (contains_rel ? "contains" : "doesn't contain"), relid);

	return contains_rel;
}

/*
 * Go through the PATHMAN_CONFIG table and create PartRelationInfo entries.
 */
static void
read_pathman_config(void)
{
	Oid				pathman_config;
	Relation		rel;
	HeapScanDesc	scan;
	Snapshot		snapshot;
	HeapTuple		htup;

	/* Get PATHMAN_CONFIG table Oid */
	pathman_config = get_relname_relid(PATHMAN_CONFIG, get_pathman_schema());

	/* Open relation with latest snapshot available */
	rel = heap_open(pathman_config, AccessShareLock);

	/* Check that 'partrel' column is if regclass type */
	Assert(RelationGetDescr(rel)->
		   attrs[Anum_pathman_config_partrel - 1]->
		   atttypid == REGCLASSOID);

	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = heap_beginscan(rel, snapshot, 0, NULL);

	/* Examine each row and create a PartRelationInfo in local cache */
	while((htup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Datum		values[Natts_pathman_config];
		bool		isnull[Natts_pathman_config];
		Oid			relid;		/* partitioned table */
		PartType	parttype;	/* partitioning type */
		text	   *attname;	/* partitioned column name */

		/* Extract Datums from tuple 'htup' */
		heap_deform_tuple(htup, RelationGetDescr(rel), values, isnull);

		/* These attributes are marked as NOT NULL, check anyway */
		Assert(!isnull[Anum_pathman_config_partrel - 1]);
		Assert(!isnull[Anum_pathman_config_parttype - 1]);
		Assert(!isnull[Anum_pathman_config_attname - 1]);

		/* Extract values from Datums */
		relid = DatumGetObjectId(values[Anum_pathman_config_partrel - 1]);
		parttype = DatumGetPartType(values[Anum_pathman_config_parttype - 1]);
		attname = DatumGetTextP(values[Anum_pathman_config_attname - 1]);

		/* Check that relation 'relid' exists */
		if (get_rel_type_id(relid) == InvalidOid)
		{
			DisablePathman();

			ereport(ERROR,
					(errmsg("Table \"%s\" contains nonexistent relation %u",
							PATHMAN_CONFIG, relid),
					 errdetail("pg_pathman will be disabled")));
		}

		/* Create or update PartRelationInfo for this partitioned table */
		refresh_pathman_relation_info(relid, parttype, text_to_cstring(attname));
	}

	/* Clean resources */
	heap_endscan(scan);
	UnregisterSnapshot(snapshot);
	heap_close(rel, AccessShareLock);
}

/*
 * Get constraint expression tree for a partition.
 *
 * build_check_constraint_name_internal() is used to build conname.
 */
static Expr *
get_partition_constraint_expr(Oid partition, AttrNumber part_attno)
{
	Oid			conid;			/* constraint Oid */
	char	   *conname;		/* constraint name */
	HeapTuple	con_tuple;
	Datum		conbin_datum;
	bool		conbin_isnull;
	Expr	   *expr;			/* expression tree for constraint */

	conname = build_check_constraint_name_internal(partition, part_attno);
	conid = get_relation_constraint_oid(partition, conname, false);

	con_tuple = SearchSysCache1(CONSTROID, ObjectIdGetDatum(conid));
	conbin_datum = SysCacheGetAttr(CONSTROID, con_tuple,
								   Anum_pg_constraint_conbin,
								   &conbin_isnull);
	if (conbin_isnull)
	{
		elog(DEBUG2, "conbin is null for constraint %s", conname);
		pfree(conname);
		return NULL;
	}
	pfree(conname);

	/* Finally we get a constraint expression tree */
	expr = (Expr *) stringToNode(TextDatumGetCString(conbin_datum));

	/* Don't foreget to release syscache tuple */
	ReleaseSysCache(con_tuple);

	return expr;
}

/* qsort comparison function for RangeEntries */
static int
cmp_range_entries(const void *p1, const void *p2, void *arg)
{
	const RangeEntry   *v1 = (const RangeEntry *) p1;
	const RangeEntry   *v2 = (const RangeEntry *) p2;
	FmgrInfo		   *cmp_proc = (FmgrInfo *) arg;

	return FunctionCall2(cmp_proc, v1->min, v2->min);
}

/*
 * Validates range constraint. It MUST have this exact format:
 *
 *		VARIABLE >= CONST AND VARIABLE < CONST
 *
 * Writes 'min' & 'max' values on success.
 */
static bool
validate_range_constraint(const Expr *expr,
						  const PartRelationInfo *prel,
						  Datum *min,
						  Datum *max)
{
	const TypeCacheEntry   *tce;
	const BoolExpr		   *boolexpr = (const BoolExpr *) expr;
	const OpExpr		   *opexpr;

	/* it should be an AND operator on top */
	if (!and_clause((Node *) expr))
		return false;

	tce = lookup_type_cache(prel->atttype, TYPECACHE_BTREE_OPFAMILY);

	/* check that left operand is >= operator */
	opexpr = (OpExpr *) linitial(boolexpr->args);
	if (BTGreaterEqualStrategyNumber == get_op_opfamily_strategy(opexpr->opno,
																 tce->btree_opf))
	{
		if (!read_opexpr_const(opexpr, prel->attnum, min))
			return false;
	}
	else
		return false;

	/* check that right operand is < operator */
	opexpr = (OpExpr *) lsecond(boolexpr->args);
	if (BTLessStrategyNumber == get_op_opfamily_strategy(opexpr->opno,
														 tce->btree_opf))
	{
		if (!read_opexpr_const(opexpr, prel->attnum, max))
			return false;
	}
	else
		return false;

	return true;
}

/*
 * Reads const value from expressions of kind: VAR >= CONST or VAR < CONST
 */
static bool
read_opexpr_const(const OpExpr *opexpr, AttrNumber varattno, Datum *val)
{
	const Node *left = linitial(opexpr->args);
	const Node *right = lsecond(opexpr->args);

	if (!IsA(left, Var) || !IsA(right, Const))
		return false;
	if (((Var *) left)->varoattno != varattno)
		return false;
	if (((Const *) right)->constisnull)
		return false;

	*val = ((Const *) right)->constvalue;

	return true;
}

/*
 * Validate hash constraint. It MUST have this exact format:
 *
 *		get_hash(TYPE_HASH_PROC(VALUE), PARTITIONS_COUNT) = CUR_PARTITION_HASH
 *
 * Writes 'part_hash' hash value for this partition on success.
 */
static bool
validate_hash_constraint(const Expr *expr,
						 const PartRelationInfo *prel,
						 uint32 *part_hash)
{
	const TypeCacheEntry   *tce;
	const OpExpr		   *eq_expr;
	const FuncExpr		   *get_hash_expr,
						   *type_hash_proc_expr;
	const Var			   *var; /* partitioned column */

	if (!IsA(expr, OpExpr))
		return false;
	eq_expr = (const OpExpr *) expr;

	/* Check that left expression is a function call */
	if (!IsA(linitial(eq_expr->args), FuncExpr))
		return false;

	get_hash_expr = (FuncExpr *) linitial(eq_expr->args);	/* arg #1: get_hash(...) */

	/* Is 'eqexpr' an equality operator? */
	tce = lookup_type_cache(get_hash_expr->funcresulttype, TYPECACHE_BTREE_OPFAMILY);
	if (BTEqualStrategyNumber != get_op_opfamily_strategy(eq_expr->opno,
														  tce->btree_opf))
		return false;

	if (list_length(get_hash_expr->args) == 2)
	{
		Node   *first = linitial(get_hash_expr->args);	/* arg #1: TYPE_HASH_PROC(VALUE) */
		Node   *second = lsecond(get_hash_expr->args);	/* arg #2: PARTITIONS_COUNT */
		Const  *cur_partition_hash;						/* hash value for this partition */

		if (!IsA(first, FuncExpr) || !IsA(second, Const))
			return false;

		type_hash_proc_expr = (FuncExpr *) first;

		/* Check that function is indeed TYPE_HASH_PROC */
		if (type_hash_proc_expr->funcid != prel->hash_proc ||
				!(IsA(linitial(type_hash_proc_expr->args), Var) ||
				  IsA(linitial(type_hash_proc_expr->args), RelabelType)))
		{
			return false;
		}

		/* Extract argument into 'var' */
		if (IsA(linitial(type_hash_proc_expr->args), RelabelType))
			var = (Var *) ((RelabelType *) linitial(type_hash_proc_expr->args))->arg;
		else
			var = (Var *) linitial(type_hash_proc_expr->args);

		/* Check that 'var' is the partitioning key attribute */
		if (var->varoattno != prel->attnum)
			return false;

		/* Check that PARTITIONS_COUNT is equal to total amount of partitions */
		if (DatumGetUInt32(((Const*) second)->constvalue) != PrelChildrenCount(prel))
			return false;

		/* Check that CUR_PARTITION_HASH is Const */
		if (!IsA(lsecond(eq_expr->args), Const))
			return false;

		cur_partition_hash = lsecond(eq_expr->args);

		/* Check that CUR_PARTITION_HASH is NOT NULL */
		if (cur_partition_hash->constisnull)
			return false;

		*part_hash = DatumGetUInt32(cur_partition_hash->constvalue);
		if (*part_hash >= PrelChildrenCount(prel))
			return false;

		return true; /* everything seems to be ok */
	}

	return false;
}

/* needed for find_inheritance_children_array() function */
static int
oid_cmp(const void *p1, const void *p2)
{
	Oid			v1 = *((const Oid *) p1);
	Oid			v2 = *((const Oid *) p2);

	if (v1 < v2)
		return -1;
	if (v1 > v2)
		return 1;
	return 0;
}
