#ifndef PG_CTBLMGR_H
#define PG_CTBLMGR_H

#include "postgres.h"
#include "miscadmin.h"
#include "replication/logical.h"
#include "replication/output_plugin.h"
#include "access/genam.h"
#include "access/sysattr.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "nodes/parsenodes.h"
#include "utils/typcache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/guc_tables.h"
#include "utils/guc.h"
#include "catalog/pg_am.h" 

// For query parsing
//
#include "fmgr.h"
#include "parser/parser.h"
#include "nodes/print.h"
#include "lib/stringinfo.h"
#include "utils/datum.h"
#include "nodes/plannodes.h"
#include "nodes/pathnodes.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

// XID interface
#include "access/xlog.h"
#include "access/xact.h"
#include "access/transam.h"

// Defined in src/backend/utils/adt/txid.c
/*
typedef uint64 txid;
typedef struct {
    TransactionId last_xid;
    uint32        epoch;
} TxidEpoch;

// GUC Records
typedef struct guc_change {
    const char *    name;
    const char *    value;
    pid_t           backend_pid;
    TransactionId * xid;
};

extern bool should_forward_guc_to_wal( const char * );
*/

enum pg_ctblmgr_wal_level {
    PGC_WAL_FULL,
    PGC_WAL_REDUCED,
    PGC_WAL_MINIMAL
};

extern void _PG_init( void );
extern void PGDLLEXPORT _PG_output_plugin_init( OutputPluginCallbacks * );

const char * transaction_boundary_full = "{\
\"type\":\"%s\",\
\"xid\":\"%u\",\
\"timestamp\":\"%s\"\
}";

const char * transaction_boundary_reduced = "{\
\"type\":\"%s\",\
\"xid\":\"%u\"\
}";

const char * transaction_boundary_minimal = "{\
\"x\":\"%u\",\"b\":\"%s\"}";

const char * dml_preamble_full = "{\
\"type\":\"%s\",\
\"xid\":\"%u\",\
\"timestamp\":\"%s\",\
\"schema_name\":\"%s\",\
\"table_name\":\"%s\"";

const char * dml_preamble_reduced = "{\
\"type\":\"%s\",\
\"xid\":\"%u\",\
\"schema_name\":\"%s\",\
\"table_name\":\"%s\"";
/* {"d":<dml_type>,"x":<xid>,"s":<schema>","t":<table>...} */
const char * dml_preamble_minimal = "{\
\"d\":\"%c\",\
\"x\":\"%u\",\
\"s\":\"%s\",\
\"t\":\"%s\"";

typedef struct {
    MemoryContext               context;
    unsigned long               num_changes;
    bool                        wrote_tx_changes;
    List *                      filter_tables;
    enum pg_ctblmgr_wal_level   wal_level;
    bool                        include_transaction;
    bool                        enable_data_write;
} decode_data;

struct pgc_table {
    char * schema_name;
    char * table_name;
    bool   all_tables;
    bool   all_schemas;
};

static void pg_ctblmgr_decode_startup(
    LogicalDecodingContext *,
    OutputPluginOptions *,
    bool
);

static void pg_ctblmgr_decode_shutdown( LogicalDecodingContext * );
static void pg_ctblmgr_decode_begin_tx(
    LogicalDecodingContext *,
    ReorderBufferTXN *
);

static void pg_ctblmgr_decode_commit_tx(
    LogicalDecodingContext *,
    ReorderBufferTXN *,
    XLogRecPtr
);

#if PG_VERSION_NUM >= 90600
static void pg_ctblmgr_decode_message(
    LogicalDecodingContext *,
    ReorderBufferTXN *,
    XLogRecPtr,
    bool,
    const char *,
    Size,
    const char *
);
#endif // PG_VERSION_NUM

#if PG_VERSION_NUM >= 110000
static void pg_ctblmgr_decode_truncate(
    LogicalDecodingContext *,
    ReorderBufferTXN *,
    int,
    Relation[],
    ReorderBufferChange *
);
#endif // PG_VERSION_NUM
static void pg_ctblmgr_decode_change(
    LogicalDecodingContext *,
    ReorderBufferTXN *,
    Relation,
    ReorderBufferChange *
);
static void append_tuple_value(
    StringInfo,
    TupleDesc,
    HeapTuple,
    unsigned int
);

Datum get_parse_tree( PG_FUNCTION_ARGS );

static void append_literal_value( StringInfo, Oid, char * );
static void append_tuple( StringInfo, TupleDesc, HeapTuple );
static bool config_to_filter_table( char *, List ** );
static bool parse_table_identifier( List *, List ** );

static char * node_to_json_string( void * );
static char * enum_JoinType( JoinType );
static char * enum_CoercionForm( CoercionForm );
static char * enum_SortByNulls( SortByNulls );
static char * enum_SortByDir( SortByDir );
static char * enum_SubLinkType( SubLinkType );
static char * enum_OnCommitAction( OnCommitAction );
static char * enum_DefElemAction( DefElemAction );
static char * enum_CmdType( CmdType );
static char * enum_MinMaxOp( MinMaxOp );
static char * enum_RTEKind( RTEKind );
static char * enum_ScanDirection( ScanDirection );
static char * enum_NullTestType( NullTestType );
static char * enum_AggStrategy( AggStrategy );
static char * enum_BoolTestType( BoolTestType );
static char * enum_ParamKind( ParamKind );
static char * enum_NodeTag( NodeTag );
static char * enum_SetOperation( SetOperation );
static void Aggref_out( StringInfo, Aggref * );
static void Agg_out( StringInfo, Agg * );
static void Alias_out( StringInfo, Alias * );
static void AlternativeSubPlan_out( StringInfo, AlternativeSubPlan * );
static void AppendPath_out( StringInfo, AppendPath * );
static void AppendRelInfo_out( StringInfo, AppendRelInfo * );
static void Append_out( StringInfo, Append * );
static void ArrayCoerceExpr_out( StringInfo, ArrayCoerceExpr * );
static void ArrayExpr_out( StringInfo, ArrayExpr * );
static void A_ArrayExpr_out( StringInfo, A_ArrayExpr * );
static void A_Const_out( StringInfo, A_Const * );
static void A_Expr_out( StringInfo, A_Expr * );
static void A_Indices_out( StringInfo, A_Indices * );
static void A_Indirection_out( StringInfo, A_Indirection * );
static void A_Star_out( StringInfo, A_Star * );
static void BitmapAndPath_out( StringInfo, BitmapAndPath * );
static void BitmapAnd_out( StringInfo, BitmapAnd * );
static void BitmapHeapPath_out( StringInfo, BitmapHeapPath * );
static void BitmapHeapScan_out( StringInfo, BitmapHeapScan * );
static void BitmapIndexScan_out( StringInfo, BitmapIndexScan * );
static void BitmapOrPath_out( StringInfo, BitmapOrPath * );
static void BitmapOr_out( StringInfo, BitmapOr * );
static void Bitmapset_out( StringInfo, Bitmapset * );
static void BooleanTest_out( StringInfo, BooleanTest * );
static void BoolExpr_out( StringInfo, BoolExpr * );
static void CaseExpr_out( StringInfo, CaseExpr * );
static void CaseTestExpr_out( StringInfo, CaseTestExpr * );
static void CaseWhen_out( StringInfo, CaseWhen * );
static void CoalesceExpr_out( StringInfo, CoalesceExpr * );
static void CoerceToDomainValue_out( StringInfo, CoerceToDomainValue * );
static void CoerceToDomain_out( StringInfo, CoerceToDomain * );
static void CoerceViaIO_out( StringInfo, CoerceViaIO * );
static void CollateClause_out( StringInfo, CollateClause * );
static void CollateExpr_out( StringInfo, CollateExpr * );
static void ColumnDef_out( StringInfo, ColumnDef * );
static void ColumnRef_out( StringInfo, ColumnRef * );
static void CommonTableExpr_out( StringInfo, CommonTableExpr * );
static void Const_out( StringInfo, Const * );
static void ConvertRowtypeExpr_out( StringInfo, ConvertRowtypeExpr * );
static void Constraint_out( StringInfo, Constraint * );
static void CreateForeignTableStmt_out( StringInfo, CreateForeignTableStmt * );
static void CreateStmt_out( StringInfo, CreateStmt * );
static void CteScan_out( StringInfo, CteScan * );
static void CurrentOfExpr_out( StringInfo, CurrentOfExpr * );
static void Datum_out( StringInfo, Datum, int, bool );
static void DeclareCursorStmt_out( StringInfo, DeclareCursorStmt * );
static void DefElem_out( StringInfo, DefElem * );
static void DistinctExpr_out( StringInfo, DistinctExpr * );
static void EquivalenceClass_out( StringInfo, EquivalenceClass * );
static void EquivalenceMember_out( StringInfo, EquivalenceMember * );
static void FieldSelect_out( StringInfo, FieldSelect * );
static void FieldStore_out( StringInfo, FieldStore * );
static void ForeignPath_out( StringInfo, ForeignPath * );
static void ForeignScan_out( StringInfo, ForeignScan * );
static void FromExpr_out( StringInfo, FromExpr * );
static void FuncCall_out( StringInfo, FuncCall * );
static void FuncExpr_out( StringInfo, FuncExpr * );
static void FunctionScan_out( StringInfo, FunctionScan * );
static void Group_out( StringInfo, Group * );
static void HashJoin_out( StringInfo, HashJoin * );
static void HashPath_out( StringInfo, HashPath * );
static void Hash_out( StringInfo, Hash * );
static void IndexElem_out( StringInfo, IndexElem * );
static void IndexOptInfo_out( StringInfo, IndexOptInfo * );
static void IndexPath_out( StringInfo, IndexPath * );
static void IndexScan_out( StringInfo, IndexScan * );
static void IndexStmt_out( StringInfo, IndexStmt * );
static void IntoClause_out( StringInfo, IntoClause * );
static void JoinExpr_out( StringInfo, JoinExpr * );
static void JoinInfo_out( StringInfo, Join * ); // Supplemental info
static void JoinPathInfo_out( StringInfo, JoinPath * ); // Supplemental info
static void Join_out( StringInfo, Join * );
static void Limit_out( StringInfo, Limit * );
static void List_out( StringInfo, List * );
static void LockingClause_out( StringInfo, LockingClause * );
static void LockRows_out( StringInfo, LockRows * );
static void MaterialPath_out( StringInfo, MaterialPath * );
static void Material_out( StringInfo, Material * );
static void MergeAppendPath_out( StringInfo, MergeAppendPath * );
static void MergeAppend_out( StringInfo, MergeAppend * );
static void MergeJoin_out( StringInfo, MergeJoin * );
static void MergePath_out( StringInfo, MergePath * );
static void MinMaxAggInfo_out( StringInfo, MinMaxAggInfo * );
static void MinMaxExpr_out( StringInfo, MinMaxExpr * );
static void ModifyTable_out( StringInfo, ModifyTable * );
static void NamedArgExpr_out( StringInfo, NamedArgExpr * );
static void NestLoopParam_out( StringInfo, NestLoopParam * );
static void NestLoop_out( StringInfo, NestLoop * );
static void NestPath_out( StringInfo, NestPath * );
static void Node_out( StringInfo, void * );
static void NotifyStmt_out( StringInfo, NotifyStmt * );
static void NullIfExpr_out( StringInfo, NullIfExpr * );
static void NullTest_out( StringInfo, NullTest * );
static void OpExpr_out( StringInfo, OpExpr * );
static void ParamRef_out( StringInfo, ParamRef * );
static void Param_out( StringInfo, Param * );
static void PathInfo_out( StringInfo, Path * ); // Supplemental info
static void PathKey_out( StringInfo, PathKey * );
static void Path_out( StringInfo, Path * );
static void PlaceHolderInfo_out( StringInfo, PlaceHolderInfo * );
static void PlaceHolderVar_out( StringInfo, PlaceHolderVar * );
static void PlanInfo_out( StringInfo, Plan * ); // Supplemental info
static void PlanInvalItem_out( StringInfo, PlanInvalItem * );
static void PlannedStmt_out( StringInfo, PlannedStmt * );
static void PlannerGlobal_out( StringInfo, PlannerGlobal * );
static void PlannerInfo_out( StringInfo, PlannerInfo * );
static void PlannerParamItem_out( StringInfo, PlannerParamItem * );
static void PlanRowMark_out( StringInfo, PlanRowMark * );
static void Plan_out( StringInfo, Plan * );
static void Query_out( StringInfo, Query * );
static void RangeFunction_out( StringInfo, RangeFunction * );
static void RangeSubselect_out( StringInfo, RangeSubselect * );
static void RangeTblEntry_out( StringInfo, RangeTblEntry * );
static void RangeTblRef_out( StringInfo, RangeTblRef * );
static void RangeVar_out( StringInfo, RangeVar * );
static void RecursiveUnion_out( StringInfo, RecursiveUnion * );
static void RawStmt_out( StringInfo, RawStmt * );
static void RelabelType_out( StringInfo, RelabelType * );
static void RelOptInfo_out( StringInfo, RelOptInfo * );
static void ResTarget_out( StringInfo, ResTarget * );
static void RestrictInfo_out( StringInfo, RestrictInfo * );
static void Result_out( StringInfo, Result * );
static void RowCompareExpr_out( StringInfo, RowCompareExpr * );
static void RowExpr_out( StringInfo, RowExpr * );
static void RowMarkClause_out( StringInfo, RowMarkClause * );
static void ScalarArrayOpExpr_out( StringInfo, ScalarArrayOpExpr * );
static void ScanInfo_out( StringInfo, Scan * ); // Supplemental info
static void Scan_out( StringInfo, Scan * );
static void SelectStmt_out( StringInfo, SelectStmt * );
static void SeqScan_out( StringInfo, SeqScan * );
static void SetOperationStmt_out( StringInfo, SetOperationStmt * );
static void SetOp_out( StringInfo, SetOp * );
static void SetToDefault_out( StringInfo, SetToDefault * );
static void SortBy_out( StringInfo, SortBy * );
static void SortGroupClause_out( StringInfo, SortGroupClause * );
static void Sort_out( StringInfo, Sort * );
static void SpecialJoinInfo_out( StringInfo, SpecialJoinInfo * );
static void SubLink_out( StringInfo, SubLink * );
static void SubPlan_out( StringInfo, SubPlan * );
static void SubqueryScan_out( StringInfo, SubqueryScan * );
static void TargetEntry_out( StringInfo, TargetEntry * );
static void TidPath_out( StringInfo, TidPath * );
static void TidScan_out( StringInfo, TidScan * );
static void Token_out( StringInfo, char * );
static void TypeCast_out( StringInfo, TypeCast * );
static void TypeName_out( StringInfo, TypeName * );
static void UniquePath_out( StringInfo, UniquePath * );
static void Unique_out( StringInfo, Unique * );
static void ValuesScan_out( StringInfo, ValuesScan * );
static void Value_out( StringInfo, Value * );
static void Var_out( StringInfo, Var * );
static void WindowAgg_out( StringInfo, WindowAgg * );
static void WindowClause_out( StringInfo, WindowClause * );
static void WindowDef_out( StringInfo, WindowDef * );
static void WindowFunc_out( StringInfo, WindowFunc * );
static void WithClause_out( StringInfo, WithClause * );
static void WorkTableScan_out( StringInfo, WorkTableScan * );
static void XmlExpr_out( StringInfo, XmlExpr * );
static void XmlSerialize_out( StringInfo, XmlSerialize * );

#endif // PG_CTBLMGR_H
