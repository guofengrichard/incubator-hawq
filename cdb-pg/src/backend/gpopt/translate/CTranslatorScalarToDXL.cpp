//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2011 Greenplum, Inc.
//
//	@filename:
//		CTranslatorScalarToDXL.cpp
//
//	@doc:
//		Implementing the methods needed to translate a GPDB Scalar Operation (in a Query / PlStmt object)
//		into a DXL trees
//	@owner:
//		raghav
//
//	@test:
//
//---------------------------------------------------------------------------

#define ALLOW_DatumGetPointer
#define ALLOW_ntohl
#define ALLOW_memset
#define ALLOW_printf

#include "postgres.h"
#include "gpopt/translate/CTranslatorScalarToDXL.h"
#include "gpopt/translate/CTranslatorPlStmtToDXL.h"
#include "gpopt/translate/CTranslatorQueryToDXL.h"
#include "gpopt/translate/CTranslatorUtils.h"
#include "gpopt/translate/CCTEListEntry.h"

#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "utils/datum.h"
#include "utils/numeric.h"

#include "gpos/base.h"
#include "gpos/string/CWStringDynamic.h"

#include "dxl/operators/CDXLDatumBool.h"
#include "dxl/operators/CDXLDatumInt4.h"
#include "dxl/operators/CDXLDatumInt8.h"
#include "dxl/operators/CDXLDatumOid.h"

#include "dxl/CDXLUtils.h"
#include "dxl/xml/dxltokens.h"

#include "gpopt/base/CUtils.h"
#include "gpopt/mdcache/CMDAccessor.h"

#include "md/IMDAggregate.h"
#include "md/IMDScalarOp.h"
#include "md/IMDType.h"
#include "md/CMDTypeGenericGPDB.h"

#include "gpopt/gpdbwrappers.h"

using namespace gpdxl;
using namespace gpopt;

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::CTranslatorScalarToDXL
//
//	@doc:
//		Ctor
//---------------------------------------------------------------------------
CTranslatorScalarToDXL::CTranslatorScalarToDXL
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	CIdGenerator *pulidgtorCol,
	CIdGenerator *pulidgtorCTE,
	ULONG ulQueryLevel,
	BOOL fQuery,
	PlannedStmt *pplstmt,
	CMappingParamIdScalarId *pmapps,
	HMUlCTEListEntry *phmulCTEEntries,
	DrgPdxln *pdrgpdxlnCTE
	)
	:
	m_pmp(pmp),
	m_pmda(pmda),
	m_pidgtorCol(pulidgtorCol),
	m_pidgtorCTE(pulidgtorCTE),
	m_ulQueryLevel(ulQueryLevel),
	m_fQuery(fQuery),
	m_pplstmt(pplstmt),
	m_pparammapping(pmapps),
	m_eplsphoptype(EpspotNone),
	m_phmulCTEEntries(phmulCTEEntries),
	m_pdrgpdxlnCTE(pdrgpdxlnCTE)
{
	GPOS_ASSERT_IMP(!fQuery, pplstmt);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::EdxlbooltypeFromGPDBBoolType
//
//	@doc:
//		Return the EdxlBoolExprType for a given GPDB BoolExprType
//---------------------------------------------------------------------------
EdxlBoolExprType
CTranslatorScalarToDXL::EdxlbooltypeFromGPDBBoolType
	(
	BoolExprType boolexprtype
	)
	const
{
	ULONG rgrgulMapping[][2] =
		{
		{NOT_EXPR, Edxlnot},
		{AND_EXPR, Edxland},
		{OR_EXPR, Edxlor},
		};

	EdxlBoolExprType edxlbt = EdxlBoolExprTypeSentinel;

	const ULONG ulArity = GPOS_ARRAY_SIZE(rgrgulMapping);
	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		ULONG *pulElem = rgrgulMapping[ul];
		if ((ULONG) boolexprtype == pulElem[0])
		{
			edxlbt = (EdxlBoolExprType) pulElem[1];
			break;
		}
	}

	GPOS_ASSERT(EdxlBoolExprTypeSentinel != edxlbt && "Invalid bool expr type");

	return edxlbt;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScIdFromVar
//
//	@doc:
//		Create a DXL node for a scalar ident expression from a GPDB Var expression.
//		This function can be used for constructing a scalar ident operator appearing in a
//		base node (e.g. a scan node) or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScIdFromVar
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
    const
{
	GPOS_ASSERT(IsA(pexpr, Var));
	const Var * pvar = (Var *) pexpr;

	if (pvar->varattno == 0)
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Whole-row variable"));
	}

	// column name
	const CWStringBase *pstr = pmapvarcolid->PstrColName(m_ulQueryLevel, pvar, m_eplsphoptype);

	// column id
	ULONG ulId;

	if(pvar->varattno != 0 || EpspotIndexScan == m_eplsphoptype || EpspotIndexOnlyScan == m_eplsphoptype)
	{
		ulId = pmapvarcolid->UlColId(m_ulQueryLevel, pvar, m_eplsphoptype);
	}
	else
	{
		ulId = m_pidgtorCol->UlNextId();
	}
	CMDName *pmdname = New(m_pmp) CMDName(m_pmp, pstr);

	// create a column reference for the given var
	CDXLColRef *pdxlcr = New(m_pmp) CDXLColRef(m_pmp, pmdname, ulId);

	// create the scalar ident operator
	CDXLScalarIdent *pdxlopIdent = New(m_pmp) CDXLScalarIdent(m_pmp, pdxlcr, New(m_pmp) CMDIdGPDB(pvar->vartype));

	// create the DXL node holding the scalar ident operator
	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlopIdent);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScIdFromParam
//
//	@doc:
//		Create a DXL node for a scalar ident expression from a GPDB Param expression.
//		If no mapping can be found, return NULL
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScIdFromParam
	(
	const Param * pparam
	)
    const
{
	GPOS_ASSERT(IsA(pparam, Param));

	if (NULL == m_pparammapping)
	{
		return NULL;
	}

	CDXLScalarIdent *pdxlopIdent = m_pparammapping->Pscid(pparam->paramid);
	if (NULL == pdxlopIdent)
	{
		return NULL;
	}

	return New(m_pmp) CDXLNode(m_pmp, pdxlopIdent);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnSubPlanFromSubPlan
//
//	@doc:
//		Create a DXL SubPlan node for a from a GPDB SubPlan
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnSubPlanFromSubPlan
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
    const
{
	GPOS_ASSERT(IsA(pexpr, SubPlan));
	GPOS_ASSERT(NULL != m_pparammapping);

	const SubPlan * psubplan = (SubPlan *)pexpr;

	// outer references
	DrgPdxlcr *pdrgdxlcr = New(m_pmp) DrgPdxlcr(m_pmp);
	DrgPmdid *pdrgmdid = New(m_pmp) DrgPmdid(m_pmp);

	ListCell *plc = NULL;
	ULONG ul;

	ForEachWithCount (plc, psubplan->args, ul)
	{
		Var *pvar = (Var*) lfirst(plc);

		// get the param id
		ULONG ulParamId = gpdb::IListNth(psubplan->parParam, ul);

		// insert mapping
		if (NULL == m_pparammapping->Pscid(ulParamId))
		{
			CDXLNode *pdxln = PdxlnScIdFromVar((Expr *)pvar, pmapvarcolid);
			CDXLScalarIdent *pdxlopIdent = CDXLScalarIdent::PdxlopConvert(pdxln->Pdxlop());
			pdxlopIdent->AddRef();
			m_pparammapping->FInsertMapping(ulParamId, pdxlopIdent);
			pdxln->Release();
		}

		// column name
		const CWStringBase *pstr = pmapvarcolid->PstrColName(m_ulQueryLevel, pvar, m_eplsphoptype);
		// column id
		ULONG ulId = 0;

		if(0 != pvar->varattno)
		{
			ulId = pmapvarcolid->UlColId(m_ulQueryLevel, pvar, m_eplsphoptype);
		}
		else
		{
			ulId = m_pidgtorCol->UlNextId();
		}

		CMDName *pmdname = New(m_pmp) CMDName(m_pmp, pstr);

		// create a column reference for the given var
		CDXLColRef *pdxlcr = New(m_pmp) CDXLColRef(m_pmp, pmdname, ulId);

		pdrgdxlcr->Append(pdxlcr);
		pdrgmdid->Append(New(m_pmp) CMDIdGPDB(pvar->vartype));
	}

	IMDId *pmdid = New(m_pmp) CMDIdGPDB(psubplan->firstColType);

	// create the DXL node holding the scalar subplan
	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarSubPlan(m_pmp, pmdid, pdrgdxlcr, pdrgmdid));

	// get the actual sub plan from the plstmt
	Plan *pplanChild = (Plan *) gpdb::PvListNth(m_pplstmt->subplans, psubplan->plan_id - 1);

	// Since a sub plan has relational children, we create a new translator to handle its translation
	CTranslatorPlStmtToDXL trplstmttodxl(m_pmp, m_pmda, m_pidgtorCol, m_pplstmt, m_pparammapping);

	CDXLNode *pdxlnSubPlan = trplstmttodxl.PdxlnFromPlan(pplanChild);

	pdxln->AddChild(pdxlnSubPlan);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScOpFromExpr
//
//	@doc:
//		Create a DXL node for a scalar expression from a GPDB expression node.
//		This function can be used for constructing a scalar operator appearing in a
//		base node (e.g. a scan node), or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScOpFromExpr
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	STranslatorElem rgTranslators[] =
	{
		{T_Var, &CTranslatorScalarToDXL::PdxlnScIdFromVar},
		{T_OpExpr, &CTranslatorScalarToDXL::PdxlnScOpExprFromExpr},
		{T_ScalarArrayOpExpr, &CTranslatorScalarToDXL::PdxlnArrayOpExpr},
		{T_DistinctExpr, &CTranslatorScalarToDXL::PdxlnScDistCmpFromDistExpr},
		{T_Const, &CTranslatorScalarToDXL::PdxlnScConstFromExpr},
		{T_BoolExpr, &CTranslatorScalarToDXL::PdxlnScBoolExprFromExpr},
		{T_BooleanTest, &CTranslatorScalarToDXL::PdxlnScBooleanTestFromExpr},
		{T_CaseExpr, &CTranslatorScalarToDXL::PdxlnScCaseStmtFromExpr},
		{T_CaseTestExpr, &CTranslatorScalarToDXL::PdxlnScCaseTestFromExpr},
		{T_CoalesceExpr, &CTranslatorScalarToDXL::PdxlnScCoalesceFromExpr},
		{T_FuncExpr, &CTranslatorScalarToDXL::PdxlnScFuncExprFromFuncExpr},
		{T_Aggref, &CTranslatorScalarToDXL::PdxlnScAggrefFromAggref},
		{T_WindowRef, &CTranslatorScalarToDXL::PdxlnScWindowref},
		{T_NullTest, &CTranslatorScalarToDXL::PdxlnScNullTestFromNullTest},
		{T_NullIfExpr, &CTranslatorScalarToDXL::PdxlnScNullIfFromExpr},
		{T_RelabelType, &CTranslatorScalarToDXL::PdxlnScCastFromRelabelType},
		{T_Param, &CTranslatorScalarToDXL::PdxlnPlanFromParam},
		{T_SubPlan, &CTranslatorScalarToDXL::PdxlnSubPlanFromSubPlan},
		{T_SubLink, &CTranslatorScalarToDXL::PdxlnFromSublink},
		{T_ArrayExpr, &CTranslatorScalarToDXL::PdxlnArray},
	};

	const ULONG ulTranslators = GPOS_ARRAY_SIZE(rgTranslators);
	NodeTag ent = pexpr->type;

	// find translator for the expression type
	PfPdxln pf = NULL;
	for (ULONG ul = 0; ul < ulTranslators; ul++)
	{
		STranslatorElem elem = rgTranslators[ul];
		if (ent == elem.ent)
		{
			pf = elem.pf;
			break;
		}
	}

	if (NULL == pf)
	{
		CHAR *sz = (CHAR*) gpdb::SzNodeToString(const_cast<Expr*>(pexpr));
		CWStringDynamic *pstr = CDXLUtils::PstrFromSz(m_pmp, sz);
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiPlStmt2DXLConversion, pstr->Wsz());
	}

	return (this->*pf)(pexpr, pmapvarcolid);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScDistCmpFromDistExpr
//
//	@doc:
//		Create a DXL node for a scalar distinct comparison expression from a GPDB
//		DistinctExpr.
//		This function can be used for constructing a scalar comparison operator appearing in a
//		base node (e.g. a scan node), or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScDistCmpFromDistExpr
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, DistinctExpr));
	const DistinctExpr *pdistexpr = (DistinctExpr *) pexpr;

	// process arguments of op expr
	GPOS_ASSERT(2 == gpdb::UlListLength(pdistexpr->args));

	CDXLNode *pdxlnLeft = PdxlnScOpFromExpr
							(
							(Expr *) gpdb::PvListNth(pdistexpr->args, 0),
							pmapvarcolid
							);

	CDXLNode *pdxlnRight = PdxlnScOpFromExpr
							(
							(Expr *) gpdb::PvListNth(pdistexpr->args, 1),
							pmapvarcolid
							);

	GPOS_ASSERT(NULL != pdxlnLeft);
	GPOS_ASSERT(NULL != pdxlnRight);

	CDXLScalarDistinctComp *pdxlop = New(m_pmp) CDXLScalarDistinctComp(m_pmp, New(m_pmp) CMDIdGPDB(pdistexpr->opno));

	// create the DXL node holding the scalar distinct comparison operator
	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlop);

	// add children in the right order
	pdxln->AddChild(pdxlnLeft);
	pdxln->AddChild(pdxlnRight);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScCmpFromOpExpr
//
//	@doc:
//		Create a DXL node for a scalar comparison expression from a GPDB OpExpr.
//		This function can be used for constructing a scalar comparison operator appearing in a
//		base node (e.g. a scan node), or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScCmpFromOpExpr
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, OpExpr));
	const OpExpr *popexpr = (OpExpr *) pexpr;

	if(0 == popexpr->opfuncid)
	{
		// In a GPDB query object, the Funcid is set to 0. Therefore, we set the
		// funcoid from the meta data (CMetadataContext)

		CMDIdGPDB *pmdidOp = New(m_pmp) CMDIdGPDB(popexpr->opno);
		const IMDScalarOp *pmdscop = m_pmda->Pmdscop(pmdidOp);
		pmdidOp->Release();

		const_cast<OpExpr*>(popexpr)->opfuncid = CMDIdGPDB::PmdidConvert(pmdscop->PmdidFunc())->OidObjectId();
	}

	// process arguments of op expr
	GPOS_ASSERT(2 == gpdb::UlListLength(popexpr->args));

	Expr *pexprLeft = (Expr *) gpdb::PvListNth(popexpr->args, 0);
	Expr *pexprRight = (Expr *) gpdb::PvListNth(popexpr->args, 1);

	CDXLNode *pdxlnLeft = PdxlnScOpFromExpr(pexprLeft, pmapvarcolid);
	CDXLNode *pdxlnRight = PdxlnScOpFromExpr(pexprRight, pmapvarcolid);;

	GPOS_ASSERT(NULL != pdxlnLeft);
	GPOS_ASSERT(NULL != pdxlnRight);

	CMDIdGPDB *pmdid = New(m_pmp) CMDIdGPDB(popexpr->opno);

	// get operator name
	const CWStringConst *pstr = PstrOpName(pmdid);

	CDXLScalarComp *pdxlop = New(m_pmp) CDXLScalarComp(m_pmp, pmdid, New(m_pmp) CWStringConst(pstr->Wsz()));

	// create the DXL node holding the scalar comparison operator
	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlop);

	// add children in the right order
	pdxln->AddChild(pdxlnLeft);
	pdxln->AddChild(pdxlnRight);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScOpExprFromExpr
//
//	@doc:
//		Create a DXL node for a scalar opexpr from a GPDB OpExpr.
//		This function can be used for constructing a scalar opexpr operator appearing in a
//		base node (e.g. a scan node), or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScOpExprFromExpr
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, OpExpr));

	// check if this is a scalar comparison
	CMDIdGPDB *pmdidReturnType = New(m_pmp) CMDIdGPDB(((OpExpr *) pexpr)->opresulttype);
	const IMDType *pmdtype= m_pmda->Pmdtype(pmdidReturnType);

	if (IMDType::EtiBool ==  pmdtype->Eti())
	{
		pmdidReturnType->Release();
		return PdxlnScCmpFromOpExpr(pexpr, pmapvarcolid);
	}

	const OpExpr *popexpr = (OpExpr *) pexpr;

	if(0 == popexpr->opfuncid)
	{
		CMDIdGPDB *pmdidOp = New(m_pmp) CMDIdGPDB(popexpr->opno);
		const IMDScalarOp *pmdscop = m_pmda->Pmdscop(pmdidOp);
		pmdidOp->Release();

		// In a GPDB query object, the Funcid is set to 0;
		const_cast<OpExpr*>(popexpr)->opfuncid = CMDIdGPDB::PmdidConvert(pmdscop->PmdidFunc())->OidObjectId();
	}

	GPOS_ASSERT(1 == gpdb::UlListLength(popexpr->args) || 2 == gpdb::UlListLength(popexpr->args));

	// get operator name and id
	IMDId *pmdid = New(m_pmp) CMDIdGPDB(popexpr->opno);
	
	const CWStringConst *pstr = PstrOpName(pmdid);

	CDXLScalarOpExpr *pdxlop = New(m_pmp) CDXLScalarOpExpr(m_pmp, pmdid, pmdidReturnType, New(m_pmp) CWStringConst(pstr->Wsz()));

	// create the DXL node holding the scalar opexpr
	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlop);

	// process arguments of op expr
	CDXLNode *pdxlnLeft = PdxlnScOpFromExpr((Expr*) gpdb::PvListNth(popexpr->args, 0), pmapvarcolid);
	GPOS_ASSERT(NULL != pdxlnLeft);

	// add children in the right order
	pdxln->AddChild(pdxlnLeft);

	if (2 == gpdb::UlListLength(popexpr->args))
	{
		CDXLNode *pdxlnRight = PdxlnScOpFromExpr((Expr *) gpdb::PvListNth(popexpr->args, 1), pmapvarcolid);
		GPOS_ASSERT(NULL != pdxlnRight);
		pdxln->AddChild(pdxlnRight);
	}

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScNullIfFromExpr
//
//	@doc:
//		Create a DXL node for a scalar nullif from a GPDB Expr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScNullIfFromExpr
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, NullIfExpr));
	const NullIfExpr *pnullifexpr = (NullIfExpr *) pexpr;

	GPOS_ASSERT(2 == gpdb::UlListLength(pnullifexpr->args));

	CDXLScalarNullIf *pdxlop = New(m_pmp) CDXLScalarNullIf(m_pmp, New(m_pmp) CMDIdGPDB(pnullifexpr->opno), New(m_pmp) CMDIdGPDB(gpdb::OidExprType((Node *)pnullifexpr)));

	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlop);

	// process arguments
	TranslateScalarChildren(pdxln, pnullifexpr->args, pmapvarcolid);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnArrayOpExpr
//
//	@doc:
//		Create a DXL node for a scalar array expression from a GPDB OpExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnArrayOpExpr
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	return PdxlnScArrayCompFromExpr(pexpr, pmapvarcolid);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScArrayCompFromExpr
//
//	@doc:
//		Create a DXL node for a scalar array comparison from a GPDB OpExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScArrayCompFromExpr
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, ScalarArrayOpExpr));
	const ScalarArrayOpExpr *pscarrayopexpr = (ScalarArrayOpExpr *) pexpr;

	// process arguments of op expr
	GPOS_ASSERT(2 == gpdb::UlListLength(pscarrayopexpr->args));

	Expr *pexprLeft = (Expr*) gpdb::PvListNth(pscarrayopexpr->args, 0);
	CDXLNode *pdxlnLeft = PdxlnScOpFromExpr(pexprLeft, pmapvarcolid);

	Expr *pexprRight = (Expr*) gpdb::PvListNth(pscarrayopexpr->args, 1);
	CDXLNode *pdxlnRight = PdxlnScOpFromExpr(pexprRight, pmapvarcolid);

	GPOS_ASSERT(NULL != pdxlnLeft);
	GPOS_ASSERT(NULL != pdxlnRight);

	// get operator name
	CMDIdGPDB *pmdidOp = New(m_pmp) CMDIdGPDB(pscarrayopexpr->opno);
	const IMDScalarOp *pmdscop = m_pmda->Pmdscop(pmdidOp);
	pmdidOp->Release();

	const CWStringConst *pstr = pmdscop->Mdname().Pstr();
	GPOS_ASSERT(NULL != pstr);

	EdxlArrayCompType edxlarraycomptype = Edxlarraycomptypeany;

	if(!pscarrayopexpr->useOr)
	{
		edxlarraycomptype = Edxlarraycomptypeall;
	}

	CDXLScalarArrayComp *pdxlop = New(m_pmp) CDXLScalarArrayComp(m_pmp, New(m_pmp) CMDIdGPDB(pscarrayopexpr->opno), New(m_pmp) CWStringConst(pstr->Wsz()), edxlarraycomptype);

	// create the DXL node holding the scalar opexpr
	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlop);

	// add children in the right order
	pdxln->AddChild(pdxlnLeft);
	pdxln->AddChild(pdxlnRight);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScConstFromExpr
//
//	@doc:
//		Create a DXL node for a scalar const value from a GPDB Const
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScConstFromExpr
	(
	const Expr *pexpr,
	const CMappingVarColId * // pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, Const));
	const Const *pconst = (Const *) pexpr;

	CDXLNode *pdxln = New(m_pmp) CDXLNode
									(
									m_pmp,
									New(m_pmp) CDXLScalarConstValue
												(
												m_pmp,
												Pdxldatum(pconst)
												)
									);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::Pdxldatum
//
//	@doc:
//		Create a DXL datum from a GPDB Const
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::Pdxldatum
	(
	const Const *pconst
	)
	const
{
	CMDIdGPDB *pmdid = New(m_pmp) CMDIdGPDB(pconst->consttype);
	const IMDType *pmdtype= m_pmda->Pmdtype(pmdid);
	pmdid->Release();

 	// translate gpdb datum into a DXL datum and create the scalar constant operator holding the datum
	CDXLDatum *pdxldatum = CTranslatorScalarToDXL::Pdxldatum(m_pmp, pmdtype, pconst->constisnull, pconst->constlen, pconst->constvalue);

	return pdxldatum;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScBoolExprFromExpr
//
//	@doc:
//		Create a DXL node for a scalar boolean expression from a GPDB OpExpr.
//		This function can be used for constructing a scalar boolexpr operator appearing in a
//		base node (e.g. a scan node), or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScBoolExprFromExpr
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, BoolExpr));
	const BoolExpr *pboolexpr = (BoolExpr *) pexpr;
	GPOS_ASSERT(0 < gpdb::UlListLength(pboolexpr->args));

	EdxlBoolExprType boolexptype = EdxlbooltypeFromGPDBBoolType(pboolexpr->boolop);
	GPOS_ASSERT(EdxlBoolExprTypeSentinel != boolexptype);

	// create the DXL node holding the scalar boolean operator
	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarBoolExpr(m_pmp, boolexptype));

	ULONG ulCount = gpdb::UlListLength(pboolexpr->args);

	if ((NOT_EXPR != pboolexpr->boolop) && (2 > ulCount))
	{
		GPOS_RAISE
			(
			gpdxl::ExmaDXL,
			gpdxl::ExmiPlStmt2DXLConversion,
			GPOS_WSZ_LIT("Boolean Expression (OR / AND): Incorrect Number of Children ")
			);
	}
	else if ((NOT_EXPR == pboolexpr->boolop) && (1 != ulCount))
	{
		GPOS_RAISE
			(
			gpdxl::ExmaDXL,
			gpdxl::ExmiPlStmt2DXLConversion,
			GPOS_WSZ_LIT("Boolean Expression (NOT): Incorrect Number of Children ")
			);
	}

	TranslateScalarChildren(pdxln, pboolexpr->args, pmapvarcolid);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScBooleanTestFromExpr
//
//	@doc:
//		Create a DXL node for a scalar boolean test from a GPDB OpExpr.
//		This function can be used for constructing a scalar boolexpr operator appearing in a
//		base node (e.g. a scan node), or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScBooleanTestFromExpr
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, BooleanTest));

	const BooleanTest *pbooleantest = (BooleanTest *) pexpr;

	GPOS_ASSERT(NULL != pbooleantest->arg);

	ULONG rgrgulMapping[][2] =
		{
		{IS_TRUE, EdxlbooleantestIsTrue},
		{IS_NOT_TRUE, EdxlbooleantestIsNotTrue},
		{IS_FALSE, EdxlbooleantestIsFalse},
		{IS_NOT_FALSE, EdxlbooleantestIsNotFalse},
		{IS_UNKNOWN, EdxlbooleantestIsUnknown},
		{IS_NOT_UNKNOWN, EdxlbooleantestIsNotUnknown},
		};

	EdxlBooleanTestType edxlbt = EdxlbooleantestSentinel;
	const ULONG ulArity = GPOS_ARRAY_SIZE(rgrgulMapping);
	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		ULONG *pulElem = rgrgulMapping[ul];
		if ((ULONG) pbooleantest->booltesttype == pulElem[0])
		{
			edxlbt = (EdxlBooleanTestType) pulElem[1];
			break;
		}
	}
	GPOS_ASSERT(EdxlbooleantestSentinel != edxlbt && "Invalid boolean test type");

	// create the DXL node holding the scalar boolean test operator
	CDXLNode *pdxln = New(m_pmp) CDXLNode
									(
									m_pmp,
									New(m_pmp) CDXLScalarBooleanTest(m_pmp,edxlbt)
									);

	CDXLNode *pdxlnArg = PdxlnScOpFromExpr(pbooleantest->arg, pmapvarcolid);
	GPOS_ASSERT(NULL != pdxlnArg);

	pdxln->AddChild(pdxlnArg);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScNullTestFromNullTest
//
//	@doc:
//		Create a DXL node for a scalar nulltest expression from a GPDB OpExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScNullTestFromNullTest
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, NullTest));
	const NullTest *pnulltest = (NullTest *) pexpr;

	GPOS_ASSERT(NULL != pnulltest->arg);
	CDXLNode *pdxlnChild = PdxlnScOpFromExpr(pnulltest->arg, pmapvarcolid);

	GPOS_ASSERT(NULL != pdxlnChild);
	GPOS_ASSERT(IS_NULL == pnulltest->nulltesttype || IS_NOT_NULL == pnulltest->nulltesttype);

	BOOL fIsNull = false;
	if (IS_NULL == pnulltest->nulltesttype)
	{
		fIsNull = true;
	}

	// create the DXL node holding the scalar NullTest operator
	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarNullTest(m_pmp, fIsNull));
	pdxln->AddChild(pdxlnChild);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScCoalesceFromExpr
//
//	@doc:
//		Create a DXL node for a coalesce function from a GPDB OpExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScCoalesceFromExpr
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, CoalesceExpr));

	CoalesceExpr *pcoalesceexpr = (CoalesceExpr *) pexpr;
	GPOS_ASSERT(NULL != pcoalesceexpr->args);

	CDXLScalarCoalesce *pdxlop = New(m_pmp) CDXLScalarCoalesce
											(
											m_pmp,
											New(m_pmp) CMDIdGPDB(pcoalesceexpr->coalescetype)
											);

	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlop);

	TranslateScalarChildren(pdxln, pcoalesceexpr->args, pmapvarcolid);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::TranslateScalarChildren
//
//	@doc:
//		Translate list elements and add them as children of the DXL node
//---------------------------------------------------------------------------
void
CTranslatorScalarToDXL::TranslateScalarChildren
	(
	CDXLNode *pdxln,
	List *plist,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	ListCell *plc = NULL;
	ForEach (plc, plist)
	{
		Expr *pexprChild = (Expr *) lfirst(plc);
		CDXLNode *pdxlnChild = PdxlnScOpFromExpr(pexprChild, pmapvarcolid);
		GPOS_ASSERT(NULL != pdxlnChild);
		pdxln->AddChild(pdxlnChild);
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScCaseStmtFromExpr
//
//	@doc:
//		Create a DXL node for a case statement from a GPDB OpExpr.
//		This function can be used for constructing a scalar opexpr operator appearing in a
//		base node (e.g. a scan node), or in an intermediate plan nodes.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScCaseStmtFromExpr
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, CaseExpr));

	const CaseExpr *pcaseexpr = (CaseExpr *) pexpr;

	if (NULL == pcaseexpr->args)
	{
			GPOS_RAISE
				(
				gpdxl::ExmaDXL,
				gpdxl::ExmiPlStmt2DXLConversion,
				GPOS_WSZ_LIT("Do not support SIMPLE CASE STATEMENT")
				);
			return NULL;
	}

	if (NULL == pcaseexpr->arg)
	{
		return PdxlnScIfStmtFromCaseExpr(pcaseexpr, pmapvarcolid);
	}

	return PdxlnScSwitchFromCaseExpr(pcaseexpr, pmapvarcolid);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScSwitchFromCaseExpr
//
//	@doc:
//		Create a DXL Switch node from a GPDB CaseExpr.
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScSwitchFromCaseExpr
	(
	const CaseExpr *pcaseexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT (NULL != pcaseexpr->arg);

	CDXLScalarSwitch *pdxlop = New(m_pmp) CDXLScalarSwitch
												(
												m_pmp,
												New(m_pmp) CMDIdGPDB(pcaseexpr->casetype)
												);
	CDXLNode *pdxlnSwitch = New(m_pmp) CDXLNode(m_pmp, pdxlop);

	// translate the switch expression
	CDXLNode *pdxlnArg = PdxlnScOpFromExpr(pcaseexpr->arg, pmapvarcolid);
	pdxlnSwitch->AddChild(pdxlnArg);

	// translate the cases
	ListCell *plc = NULL;
	ForEach (plc, pcaseexpr->args)
	{
		CaseWhen *pexpr = (CaseWhen *) lfirst(plc);

		CDXLScalarSwitchCase *pdxlopCase = New(m_pmp) CDXLScalarSwitchCase(m_pmp);
		CDXLNode *pdxlnCase = New(m_pmp) CDXLNode(m_pmp, pdxlopCase);

		CDXLNode *pdxlnCmpExpr = PdxlnScOpFromExpr(pexpr->expr, pmapvarcolid);
		GPOS_ASSERT(NULL != pdxlnCmpExpr);

		CDXLNode *pdxlnResult = PdxlnScOpFromExpr(pexpr->result, pmapvarcolid);
		GPOS_ASSERT(NULL != pdxlnResult);

		pdxlnCase->AddChild(pdxlnCmpExpr);
		pdxlnCase->AddChild(pdxlnResult);

		// add current case to switch node
		pdxlnSwitch->AddChild(pdxlnCase);
	}

	// translate the "else" clause
	if (NULL != pcaseexpr->defresult)
	{
		CDXLNode *pdxlnDefaultResult = PdxlnScOpFromExpr(pcaseexpr->defresult, pmapvarcolid);
		GPOS_ASSERT(NULL != pdxlnDefaultResult);

		pdxlnSwitch->AddChild(pdxlnDefaultResult);

	}

	return pdxlnSwitch;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScCaseTestFromExpr
//
//	@doc:
//		Create a DXL node for a case test from a GPDB Expr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScCaseTestFromExpr
	(
	const Expr *pexpr,
	const CMappingVarColId* //pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, CaseTestExpr));
	const CaseTestExpr *pcasetestexpr = (CaseTestExpr *) pexpr;
	CDXLScalarCaseTest *pdxlop = New(m_pmp) CDXLScalarCaseTest
												(
												m_pmp,
												New(m_pmp) CMDIdGPDB(pcasetestexpr->typeId)
												);

	return New(m_pmp) CDXLNode(m_pmp, pdxlop);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScIfStmtFromCaseExpr
//
//	@doc:
//		Create a DXL If node from a GPDB CaseExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScIfStmtFromCaseExpr
	(
	const CaseExpr *pcaseexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT (NULL == pcaseexpr->arg);
	const ULONG ulWhenClauseCount = gpdb::UlListLength(pcaseexpr->args);

	CDXLNode *pdxlnRootIfTree = NULL;
	CDXLNode *pdxlnCurr = NULL;

	for (ULONG ul = 0; ul < ulWhenClauseCount; ul++)
	{
		CDXLScalarIfStmt *pdxlopIfstmtNew = New(m_pmp) CDXLScalarIfStmt
															(
															m_pmp,
															New(m_pmp) CMDIdGPDB(pcaseexpr->casetype)
															);

		CDXLNode *pdxlnIfStmtNew = New(m_pmp) CDXLNode(m_pmp, pdxlopIfstmtNew);

		CaseWhen *pexpr = (CaseWhen *) gpdb::PvListNth(pcaseexpr->args, ul);
		GPOS_ASSERT(IsA(pexpr, CaseWhen));

		CDXLNode *pdxlnCond = PdxlnScOpFromExpr(pexpr->expr, pmapvarcolid);
		CDXLNode *pdxlnResult = PdxlnScOpFromExpr(pexpr->result, pmapvarcolid);

		GPOS_ASSERT(NULL != pdxlnCond);
		GPOS_ASSERT(NULL != pdxlnResult);

		pdxlnIfStmtNew->AddChild(pdxlnCond);
		pdxlnIfStmtNew->AddChild(pdxlnResult);

		if(NULL == pdxlnRootIfTree)
		{
			pdxlnRootIfTree = pdxlnIfStmtNew;
		}
		else
		{
			pdxlnCurr->AddChild(pdxlnIfStmtNew);
		}
		pdxlnCurr = pdxlnIfStmtNew;
	}

	if (NULL != pcaseexpr->defresult)
	{
		CDXLNode *pdxlnDefaultResult = PdxlnScOpFromExpr(pcaseexpr->defresult, pmapvarcolid);
		GPOS_ASSERT(NULL != pdxlnDefaultResult);
		pdxlnCurr->AddChild(pdxlnDefaultResult);
	}

	return pdxlnRootIfTree;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScCastFromRelabelType
//
//	@doc:
//		Create a DXL node for a scalar RelabelType expression from a GPDB RelabelType
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScCastFromRelabelType
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, RelabelType));

	const RelabelType *prelabeltype = (RelabelType *) pexpr;

	GPOS_ASSERT(NULL != prelabeltype->arg);

	CDXLNode *pdxlnChild = PdxlnScOpFromExpr
								(
								prelabeltype->arg,
								pmapvarcolid
								);

	GPOS_ASSERT(NULL != pdxlnChild);

	// create the DXL node holding the scalar boolean operator
	CDXLNode *pdxln = New(m_pmp) CDXLNode
									(
									m_pmp,
									New(m_pmp) CDXLScalarCast
												(
												m_pmp,
												New(m_pmp) CMDIdGPDB(prelabeltype->resulttype),
												New(m_pmp) CMDIdGPDB(0) // casting function oid
												)
									);
	pdxln->AddChild(pdxlnChild);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScFuncExprFromFuncExpr
//
//	@doc:
//		Create a DXL node for a scalar funcexpr from a GPDB FuncExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScFuncExprFromFuncExpr
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, FuncExpr));
	const FuncExpr *pfuncexpr = (FuncExpr *) pexpr;

	if (pg_stat_get_activity_oid == pfuncexpr->funcid)
	{
		// TODO 07/19/2013; solimm1: remove exception when pg_stat_get_activity() properties are fixed in the catalog
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("pg_stat_get_activity() function"));
	}

	CMDIdGPDB *pmdidFunc = New(m_pmp) CMDIdGPDB(pfuncexpr->funcid);

	// In the planner, scalar functions that are volatile (SIRV) or read or modify SQL
	// data get patched into an InitPlan. This is not supported in the optimizer
	if (CTranslatorUtils::FSirvFunc(m_pmda, pmdidFunc))
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("SIRV functions"));
	}

	// create the DXL node holding the scalar funcexpr
	CDXLNode *pdxln = New(m_pmp) CDXLNode
									(
									m_pmp,
									New(m_pmp) CDXLScalarFuncExpr
												(
												m_pmp,
												pmdidFunc,
												New(m_pmp) CMDIdGPDB(pfuncexpr->funcresulttype),
												pfuncexpr->funcretset
												)
									);

	if (CTranslatorUtils::FReadsOrModifiesData(m_pmda, pmdidFunc))
	{
		ListCell *plc = NULL;
		ForEach (plc, pfuncexpr->args)
		{
			Node *pnodeArg = (Node *) lfirst(plc);
			if (CTranslatorUtils::FHasSubquery(pnodeArg))
			{
				GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature,
						GPOS_WSZ_LIT("Functions which read or modify data with subqueries in arguments"));
			}
		}
	}

	TranslateScalarChildren(pdxln, pfuncexpr->args, pmapvarcolid);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScAggrefFromAggref
//
//	@doc:
//		Create a DXL node for a scalar aggref from a GPDB FuncExpr
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScAggrefFromAggref
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, Aggref));
	const Aggref *paggref = (Aggref *) pexpr;

	ULONG rgrgulMapping[][2] =
		{
		{AGGSTAGE_NORMAL, EdxlaggstageNormal},
		{AGGSTAGE_PARTIAL, EdxlaggstagePartial},
		{AGGSTAGE_INTERMEDIATE, EdxlaggstageIntermediate},
		{AGGSTAGE_FINAL, EdxlaggstageFinal},
		};

	EdxlAggrefStage edxlaggstage = EdxlaggstageSentinel;
	const ULONG ulArity = GPOS_ARRAY_SIZE(rgrgulMapping);
	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		ULONG *pulElem = rgrgulMapping[ul];
		if ((ULONG) paggref->aggstage == pulElem[0])
		{
			edxlaggstage = (EdxlAggrefStage) pulElem[1];
			break;
		}
	}
	GPOS_ASSERT(EdxlaggstageSentinel != edxlaggstage && "Invalid agg stage");

	CMDIdGPDB *pmdidAgg = New(m_pmp) CMDIdGPDB(paggref->aggfnoid);
	const IMDAggregate *pmdagg = m_pmda->Pmdagg(pmdidAgg);

	if (pmdagg->FOrdered())
	{
		GPOS_ASSERT_IMP(NULL == paggref->aggorder, pmdagg->FOrdered());
		GPOS_RAISE
			(
			gpdxl::ExmaDXL,
			gpdxl::ExmiPlStmt2DXLConversion,
			GPOS_WSZ_LIT("Ordered aggregates")
			);
	}

	GPOS_ASSERT(0 == paggref->agglevelsup);
	CDXLScalarAggref *pdxlopAggref = New(m_pmp) CDXLScalarAggref(m_pmp, pmdidAgg, paggref->aggdistinct, edxlaggstage);

	// create the DXL node holding the scalar aggref
	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlopAggref);

	TranslateScalarChildren(pdxln, paggref->args, pmapvarcolid);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::Edxlfb
//
//	@doc:
//		Return the DXL representation of window frame boundary
//---------------------------------------------------------------------------
EdxlFrameBoundary
CTranslatorScalarToDXL::Edxlfb
	(
	WindowBoundingKind kind,
	Node *pnode
	)
	const
{
	ULONG rgrgulMapping[][2] =
			{
			{WINDOW_UNBOUND_PRECEDING, EdxlfbUnboundedPreceding},
			{WINDOW_BOUND_PRECEDING, EdxlfbBoundedPreceding},
			{WINDOW_CURRENT_ROW, EdxlfbCurrentRow},
			{WINDOW_BOUND_FOLLOWING, EdxlfbBoundedFollowing},
			{WINDOW_UNBOUND_FOLLOWING, EdxlfbUnboundedFollowing},
			{WINDOW_DELAYED_BOUND_PRECEDING, EdxlfbDelayedBoundedPreceding},
		    {WINDOW_DELAYED_BOUND_FOLLOWING, EdxlfbDelayedBoundedFollowing}
			};

	const ULONG ulArity = GPOS_ARRAY_SIZE(rgrgulMapping);
	EdxlFrameBoundary edxlfb = EdxlfbSentinel;
	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		ULONG *pulElem = rgrgulMapping[ul];
		if ((ULONG) kind == pulElem[0])
		{
			edxlfb = (EdxlFrameBoundary) pulElem[1];

			if ((WINDOW_BOUND_FOLLOWING == kind) && ((NULL == pnode) || !IsA(pnode, Const)))
			{
				edxlfb = EdxlfbDelayedBoundedFollowing;
			}

			if ((WINDOW_BOUND_PRECEDING == kind) && ((NULL == pnode) || !IsA(pnode, Const)))
			{
				edxlfb = EdxlfbDelayedBoundedPreceding;
			}

			break;
		}
	}
	GPOS_ASSERT(EdxlfbSentinel != edxlfb && "Invalid window frame boundary");

	return edxlfb;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::Pdxlwf
//
//	@doc:
//		Create a DXL window frame from a GPDB WindowFrame
//---------------------------------------------------------------------------
CDXLWindowFrame *
CTranslatorScalarToDXL::Pdxlwf
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid,
	CDXLNode *pdxlnNewChildScPrL
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, WindowFrame));
	const WindowFrame *pwindowframe = (WindowFrame *) pexpr;

	EdxlFrameSpec edxlfs = EdxlfsRow;
	if (!pwindowframe->is_rows)
	{
		edxlfs = EdxlfsRange;
	}

	EdxlFrameBoundary edxlfbLead = Edxlfb(pwindowframe->lead->kind, pwindowframe->lead->val);
	EdxlFrameBoundary edxlfbTrail = Edxlfb(pwindowframe->trail->kind, pwindowframe->trail->val);

	ULONG rgrgulExclusionMapping[][2] =
			{
			{WINDOW_EXCLUSION_NULL, EdxlfesNulls},
			{WINDOW_EXCLUSION_CUR_ROW, EdxlfesCurrentRow},
			{WINDOW_EXCLUSION_GROUP, EdxlfesGroup},
			{WINDOW_EXCLUSION_TIES, EdxlfesTies},
			{WINDOW_EXCLUSION_NO_OTHERS, EdxlfesNone},
			};

	const ULONG ulArityExclusion = GPOS_ARRAY_SIZE(rgrgulExclusionMapping);
	EdxlFrameExclusionStrategy edxlfes = EdxlfesSentinel;
	for (ULONG ul = 0; ul < ulArityExclusion; ul++)
	{
		ULONG *pulElem = rgrgulExclusionMapping[ul];
		if ((ULONG) pwindowframe->exclude == pulElem[0])
		{
			edxlfes = (EdxlFrameExclusionStrategy) pulElem[1];
			break;
		}
	}
	GPOS_ASSERT(EdxlfesSentinel != edxlfes && "Invalid window frame exclusion");

	CDXLNode *pdxlnLeadEdge = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarWindowFrameEdge(m_pmp, true /* fLeading */, edxlfbLead));
	CDXLNode *pdxlnTrailEdge = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarWindowFrameEdge(m_pmp, false /* fLeading */, edxlfbTrail));

	// translate the lead and trail value
	if (NULL != pwindowframe->lead->val)
	{
		pdxlnLeadEdge->AddChild(PdxlnWindowFrameEdgeVal(pwindowframe->lead->val, pmapvarcolid, pdxlnNewChildScPrL));
	}

	if (NULL != pwindowframe->trail->val)
	{
		pdxlnTrailEdge->AddChild(PdxlnWindowFrameEdgeVal(pwindowframe->trail->val, pmapvarcolid, pdxlnNewChildScPrL));
	}

	CDXLWindowFrame *pdxlWf = New(m_pmp) CDXLWindowFrame(m_pmp, edxlfs, edxlfes, pdxlnLeadEdge, pdxlnTrailEdge);

	return pdxlWf;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnWindowFrameEdgeVal
//
//	@doc:
//		Translate the window frame edge, if the column used in the edge is a
// 		computed column then add it to the project list
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnWindowFrameEdgeVal
	(
	const Node *pnode,
	const CMappingVarColId* pmapvarcolid,
	CDXLNode *pdxlnNewChildScPrL
	)
	const
{
	CDXLNode *pdxlnVal = PdxlnScOpFromExpr((Expr *) pnode, pmapvarcolid);

	if (m_fQuery && !IsA(pnode, Var) && !IsA(pnode, Const))
	{
		GPOS_ASSERT(NULL != pdxlnNewChildScPrL);
		CWStringConst strUnnamedCol(GPOS_WSZ_LIT("?column?"));
		CMDName *pmdnameAlias = New(m_pmp) CMDName(m_pmp, &strUnnamedCol);
		ULONG ulPrElId = m_pidgtorCol->UlNextId();

		// construct a projection element
		CDXLNode *pdxlnPrEl = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjElem(m_pmp, ulPrElId, pmdnameAlias));
		pdxlnPrEl->AddChild(pdxlnVal);

		// add it to the computed columns project list
		pdxlnNewChildScPrL->AddChild(pdxlnPrEl);

		// construct a new scalar ident
		CDXLScalarIdent *pdxlopIdent = New(m_pmp) CDXLScalarIdent
													(
													m_pmp,
													New(m_pmp) CDXLColRef
																(
																m_pmp,
																New(m_pmp) CMDName(m_pmp, &strUnnamedCol),
																ulPrElId
																),
													New(m_pmp) CMDIdGPDB(gpdb::OidExprType(const_cast<Node*>(pnode)))
													);

		pdxlnVal = New(m_pmp) CDXLNode(m_pmp, pdxlopIdent);
	}

	return pdxlnVal;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScWindowref
//
//	@doc:
//		Create a DXL node for a scalar window ref from a GPDB WindowRef
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScWindowref
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, WindowRef));

	const WindowRef *pwindowref = (WindowRef *) pexpr;

	ULONG rgrgulMapping[][2] =
		{
		{WINSTAGE_IMMEDIATE, EdxlwinstageImmediate},
		{WINSTAGE_PRELIMINARY, EdxlwinstagePreliminary},
		{WINSTAGE_ROWKEY, EdxlwinstageRowKey},
		};

	const ULONG ulArity = GPOS_ARRAY_SIZE(rgrgulMapping);
	EdxlWinStage edxlwinstage = EdxlwinstageSentinel;

	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		ULONG *pulElem = rgrgulMapping[ul];
		if ((ULONG) pwindowref->winstage == pulElem[0])
		{
			edxlwinstage = (EdxlWinStage) pulElem[1];
			break;
		}
	}

	ULONG ulWinSpecPos = (ULONG) pwindowref->winlevel;
	if (m_fQuery)
	{
		ulWinSpecPos = (ULONG) pwindowref->winspec;
	}

	GPOS_ASSERT(EdxlwinstageSentinel != edxlwinstage && "Invalid window stage");
	if (0 != pwindowref->winlevelsup)
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Window functions with outer references"));
	}

	CDXLScalarWindowRef *pdxlopWinref = New(m_pmp) CDXLScalarWindowRef
													(
													m_pmp,
													New(m_pmp) CMDIdGPDB(pwindowref->winfnoid),
													New(m_pmp) CMDIdGPDB(pwindowref->restype),
													pwindowref->windistinct,
													edxlwinstage,
													ulWinSpecPos
													);

	// create the DXL node holding the scalar aggref
	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlopWinref);

	TranslateScalarChildren(pdxln, pwindowref->args, pmapvarcolid);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScCondFromQual
//
//	@doc:
//		Create a DXL scalar boolean operator node from a GPDB qual list.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScCondFromQual
	(
	List *plQual,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	if (NULL == plQual || 0 == gpdb::UlListLength(plQual))
	{
		return NULL;
	}

	if (1 == gpdb::UlListLength(plQual))
	{
		Expr *pexpr = (Expr *) gpdb::PvListNth(plQual, 0);
		return PdxlnScOpFromExpr(pexpr, pmapvarcolid);
	}
	else
	{
		// GPDB assumes that if there are a list of qual conditions then it is an implicit AND operation
		// Here we build the left deep AND tree
		CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarBoolExpr(m_pmp, Edxland));

		TranslateScalarChildren(pdxln, plQual, pmapvarcolid);

		return pdxln;
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnFilterFromQual
//
//	@doc:
//		Create a DXL scalar filter node from a GPDB qual list.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnFilterFromQual
	(
	List *plQual,
	const CMappingVarColId* pmapvarcolid,
	Edxlopid edxlopFilterType
	)
	const
{
	CDXLScalarFilter *pdxlop = NULL;

	switch (edxlopFilterType)
	{
		case EdxlopScalarFilter:
			pdxlop = New(m_pmp) CDXLScalarFilter(m_pmp);
			break;
		case EdxlopScalarJoinFilter:
			pdxlop = New(m_pmp) CDXLScalarJoinFilter(m_pmp);
			break;
		case EdxlopScalarOneTimeFilter:
			pdxlop = New(m_pmp) CDXLScalarOneTimeFilter(m_pmp);
			break;
		default:
			GPOS_ASSERT(!"Unrecognized filter type");
	}


	CDXLNode *pdxlnFilter = New(m_pmp) CDXLNode(m_pmp, pdxlop);

	CDXLNode *pdxlnCond = PdxlnScCondFromQual(plQual, pmapvarcolid);

	if (NULL != pdxlnCond)
	{
		pdxlnFilter->AddChild(pdxlnCond);
	}

	return pdxlnFilter;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnFromSublink
//
//	@doc:
//		Create a DXL node from a GPDB sublink node.
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnFromSublink
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	const SubLink *psublink = (SubLink *) pexpr;

	switch (psublink->subLinkType)
	{
		case EXPR_SUBLINK:
			return PdxlnScSubqueryFromSublink(psublink, pmapvarcolid);

		case ALL_SUBLINK:
		case ANY_SUBLINK:
			return PdxlnQuantifiedSubqueryFromSublink(psublink, pmapvarcolid);

		case EXISTS_SUBLINK:
			return PdxlnExistSubqueryFromSublink(psublink, pmapvarcolid);

		default:
			{
				GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Non-Scalar Subquery"));
				return NULL;
			}

	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnQuantifiedSubqueryFromSublink
//
//	@doc:
//		Create ANY / ALL quantified sub query from a GPDB sublink node
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnQuantifiedSubqueryFromSublink
	(
	const SubLink *psublink,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	CMappingVarColId *pmapvarcolidCopy = pmapvarcolid->PmapvarcolidCopy(m_pmp);

	CTranslatorQueryToDXL trquerytodxl
							(
							m_pmp,
							m_pmda,
							m_pidgtorCol,
							m_pidgtorCTE,
							pmapvarcolidCopy,
							(Query *) psublink->subselect,
							m_ulQueryLevel + 1,
							m_phmulCTEEntries
							);

	CDXLNode *pdxlnInner = trquerytodxl.PdxlnFromQueryInternal();

	DrgPdxln *pdrgpdxlnQueryOutput = trquerytodxl.PdrgpdxlnQueryOutput();
	DrgPdxln *pdrgpdxlnCTE = trquerytodxl.PdrgpdxlnCTE();
	CUtils::AddRefAppend(m_pdrgpdxlnCTE, pdrgpdxlnCTE);

	if (1 != pdrgpdxlnQueryOutput->UlLength())
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Non-Scalar Subquery"));
	}

	CDXLNode *pdxlnIdent = (*pdrgpdxlnQueryOutput)[0];
	GPOS_ASSERT(NULL != pdxlnIdent);

	// get dxl scalar identifier
	CDXLScalarIdent *pdxlopIdent = dynamic_cast<CDXLScalarIdent *>(pdxlnIdent->Pdxlop());

	// get the dxl column reference
	const CDXLColRef *pdxlcr = pdxlopIdent->Pdxlcr();
	const ULONG ulColId = pdxlcr->UlID();

	// get the test expression
	GPOS_ASSERT(IsA(psublink->testexpr, OpExpr));
	OpExpr *popexpr = (OpExpr*) psublink->testexpr;

	IMDId *pmdid = New(m_pmp) CMDIdGPDB(popexpr->opno);

	// get operator name
	const CWStringConst *pstr = PstrOpName(pmdid);

	// translate left hand side of the expression
	GPOS_ASSERT(NULL != popexpr->args);
	Expr* pexprLHS = (Expr*) gpdb::PvListNth(popexpr->args, 0);

	CDXLNode *pdxlnOuter = PdxlnScOpFromExpr(pexprLHS, pmapvarcolid);

	CDXLNode *pdxln = NULL;
	CDXLScalar *pdxlopSubquery = NULL;

	GPOS_ASSERT(ALL_SUBLINK == psublink->subLinkType || ANY_SUBLINK == psublink->subLinkType);
	if (ALL_SUBLINK == psublink->subLinkType)
	{
		pdxlopSubquery = New (m_pmp) CDXLScalarSubqueryAll
								(
								m_pmp,
								pmdid,
								New(m_pmp) CMDName(m_pmp, pstr),
								ulColId
								);

	}
	else
	{
		pdxlopSubquery = New (m_pmp) CDXLScalarSubqueryAny
								(
								m_pmp,
								pmdid,
								New(m_pmp) CMDName(m_pmp, pstr),
								ulColId
								);
	}

	pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlopSubquery);

	pdxln->AddChild(pdxlnOuter);
	pdxln->AddChild(pdxlnInner);

#ifdef GPOS_DEBUG
	pdxln->Pdxlop()->AssertValid(pdxln, false /* fValidateChildren */);
#endif

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnScSubqueryFromSublink
//
//	@doc:
//		Create a scalar subquery from a GPDB sublink node
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnScSubqueryFromSublink
	(
	const SubLink *psublink,
	const CMappingVarColId *pmapvarcolid
	)
	const
{
	CMappingVarColId *pmapvarcolidCopy = pmapvarcolid->PmapvarcolidCopy(m_pmp);

	Query *pquerySublink = (Query *) psublink->subselect;
	CTranslatorQueryToDXL trquerytodxl
							(
							m_pmp,
							m_pmda,
							m_pidgtorCol,
							m_pidgtorCTE,
							pmapvarcolidCopy,
							pquerySublink,
							m_ulQueryLevel + 1,
							m_phmulCTEEntries
							);
	CDXLNode *pdxlnSubQuery = trquerytodxl.PdxlnFromQueryInternal();

	DrgPdxln *pdrgpdxlnQueryOutput = trquerytodxl.PdrgpdxlnQueryOutput();

	GPOS_ASSERT(1 == pdrgpdxlnQueryOutput->UlLength());

	DrgPdxln *pdrgpdxlnCTE = trquerytodxl.PdrgpdxlnCTE();
	CUtils::AddRefAppend(m_pdrgpdxlnCTE, pdrgpdxlnCTE);

	// get dxl scalar identifier
	CDXLNode *pdxlnIdent = (*pdrgpdxlnQueryOutput)[0];
	GPOS_ASSERT(NULL != pdxlnIdent);

	CDXLScalarIdent *pdxlopIdent = CDXLScalarIdent::PdxlopConvert(pdxlnIdent->Pdxlop());

	// get the dxl column reference
	const CDXLColRef *pdxlcr = pdxlopIdent->Pdxlcr();
	const ULONG ulColId = pdxlcr->UlID();

	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, New (m_pmp) CDXLScalarSubquery(m_pmp, ulColId));

	pdxln->AddChild(pdxlnSubQuery);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnArray
//
//	@doc:
//		Translate array
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnArray
	(
	const Expr *pexpr,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, ArrayExpr));

	const ArrayExpr *parrayexpr = (ArrayExpr *) pexpr;

	CDXLScalarArray *pdxlop =
			New(m_pmp) CDXLScalarArray
						(
						m_pmp,
						New(m_pmp) CMDIdGPDB(parrayexpr->element_typeid),
						New(m_pmp) CMDIdGPDB(parrayexpr->array_typeid),
						parrayexpr->multidims
						);

	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlop);

	TranslateScalarChildren(pdxln, parrayexpr->elements, pmapvarcolid);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PstrOpName
//
//	@doc:
//		Get the operator name
//---------------------------------------------------------------------------
const CWStringConst *
CTranslatorScalarToDXL::PstrOpName
	(
	IMDId *pmdid
	)
	const
{
	// get operator name
	const IMDScalarOp *pmdscop = m_pmda->Pmdscop(pmdid);

	const CWStringConst *pstr = pmdscop->Mdname().Pstr();

	return pstr;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnExistSubqueryFromSublink
//
//	@doc:
//		Create a DXL EXISTS subquery node from the respective GPDB
//		sublink node
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnExistSubqueryFromSublink
	(
	const SubLink *psublink,
	const CMappingVarColId* pmapvarcolid
	)
	const
{
	GPOS_ASSERT(NULL != psublink);
	CMappingVarColId *pmapvarcolidCopy = pmapvarcolid->PmapvarcolidCopy(m_pmp);

	CTranslatorQueryToDXL trquerytodxl
							(
							m_pmp,
							m_pmda,
							m_pidgtorCol,
							m_pidgtorCTE,
							pmapvarcolidCopy,
							(Query *) psublink->subselect,
							m_ulQueryLevel + 1,
							m_phmulCTEEntries
							);
	CDXLNode *pdxlnRoot = trquerytodxl.PdxlnFromQueryInternal();
	
	DrgPdxln *pdrgpdxlnCTE = trquerytodxl.PdrgpdxlnCTE();
	CUtils::AddRefAppend(m_pdrgpdxlnCTE, pdrgpdxlnCTE);
	
	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, New (m_pmp) CDXLScalarSubqueryExists(m_pmp));
	pdxln->AddChild(pdxlnRoot);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnPlanFromParam
//
//	@doc:
//		Create a DXL InitPlan/Var node from a GPDB PARAM node
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnPlanFromParam
	(
	const Expr *pexpr,
	const CMappingVarColId * // pmapvarcolid
	)
	const
{
	const Param *pparam = (Param *) pexpr;
	// check first if this param can be translated into a scalar id (from a subplan)
	CDXLNode *pdxln = PdxlnScIdFromParam((Param *) pexpr);
	if (NULL != pdxln)
	{
		return pdxln;
	}

	// TODO: Venky; Need to handle when parameters that are passed to the plan

	if (PARAM_EXEC == pparam->paramkind)
	{
		return PdxlnInitPlanFromParam(pparam);
	}
	else
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("External Parameters"));
	}
	return NULL;
}
//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxlnInitPlanFromParam
//
//	@doc:
//		Create a DXL InitPlan node from a GPDB PARAM node
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorScalarToDXL::PdxlnInitPlanFromParam
	(
	const Param *pparam
	)
	const
{
	GPOS_ASSERT(NULL != pparam);

	// TODO: Venky; Need to handle when parameters that are passed to the plan
	CDXLNode *pdxlnInitPlan = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarInitPlan(m_pmp));

	GPOS_ASSERT(NULL != m_pplstmt->subplans && "We only support parameters that are generated by initplans");

	// Get the plan defining the param
	GPOS_ASSERT(gpdb::UlListLength(m_pplstmt->subplans) >= pparam->paramid && "Parameter ID not found");

	Plan *ppl = (Plan *) gpdb::PvListNth(m_pplstmt->subplans, pparam->paramid);

	GPOS_ASSERT(NULL != ppl);

	// Since an init plan is not a scalar node, we create a new PlStmt translator to handle its translation
	CTranslatorPlStmtToDXL trplstmttodxl(m_pmp, m_pmda, m_pidgtorCol, m_pplstmt, m_pparammapping);
	CDXLNode *pdxlnSubPlan = trplstmttodxl.PdxlnFromPlan(ppl);
	GPOS_ASSERT(NULL != pdxlnSubPlan);
	pdxlnInitPlan->AddChild(pdxlnSubPlan);

	return pdxlnInitPlan;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::Pdatum
//
//	@doc:
//		Create CDXLDatum from GPDB datum
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::Pdxldatum
	(
	IMemoryPool *pmp,
	const IMDType *pmdtype,
	BOOL fNull,
	ULONG ulLen,
	Datum datum
	)
{
	SDXLDatumTranslatorElem rgTranslators[] =
		{
			{IMDType::EtiInt4  , &CTranslatorScalarToDXL::PdxldatumInt4},
			{IMDType::EtiInt8 , &CTranslatorScalarToDXL::PdxldatumInt8},
			{IMDType::EtiBool , &CTranslatorScalarToDXL::PdxldatumBool},
			{IMDType::EtiOid  , &CTranslatorScalarToDXL::PdxldatumOid},
		};

	const ULONG ulTranslators = GPOS_ARRAY_SIZE(rgTranslators);
	// find translator for the datum type
	PfPdxldatumFromDatum *pf = NULL;
	for (ULONG ul = 0; ul < ulTranslators; ul++)
	{
		SDXLDatumTranslatorElem elem = rgTranslators[ul];
		if (pmdtype->Eti() == elem.eti)
		{
			pf = elem.pf;
			break;
		}
	}

	if (NULL == pf)
	{
		// generate a datum of generic type
		return PdxldatumGeneric(pmp, pmdtype, fNull, ulLen, datum);
	}
	else
	{
		return (*pf)(pmp, pmdtype, fNull, ulLen, datum);
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxldatumGeneric
//
//	@doc:
//		Translate a datum of generic type
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::PdxldatumGeneric
	(
	IMemoryPool *pmp,
	const IMDType *pmdtype,
	BOOL fNull,
	ULONG ulLen,
	Datum datum
	)
{
	CMDIdGPDB *pmdidMDC = CMDIdGPDB::PmdidConvert(pmdtype->Pmdid());
	CMDIdGPDB *pmdid = New(pmp) CMDIdGPDB(*pmdidMDC);

	BOOL fConstByVal = pmdtype->FByValue();
	BYTE *pba = Pba(pmp, pmdtype, fNull, ulLen, datum);
	ULONG ulLength = 0;
	if (!fNull)
	{
		ulLength = (ULONG) gpdb::SDatumSize(datum, pmdtype->FByValue(), ulLen);
	}

	CDouble dValue(0);
	if (pmdid->FEquals(&CMDIdGPDB::m_mdidNumeric))
	{
		dValue = DValue(pba, fNull);
	}

	LINT lValue = 0;
	if (CMDTypeGenericGPDB::FNeedsByteaLintMapping(pmdid))
	{
		lValue = LValue(pmdid, fNull, pba, ulLength);
	}

	return CMDTypeGenericGPDB::Pdxldatum(pmp, pmdid, fConstByVal, fNull, pba, ulLength, lValue, dValue);
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxldatumBool
//
//	@doc:
//		Translate a datum of type bool
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::PdxldatumBool
	(
	IMemoryPool *pmp,
	const IMDType *pmdtype,
	BOOL fNull,
	ULONG , //ulLen,
	Datum datum
	)
{
	GPOS_ASSERT(pmdtype->FByValue());
	CMDIdGPDB *pmdidMDC = CMDIdGPDB::PmdidConvert(pmdtype->Pmdid());
	CMDIdGPDB *pmdid = New(pmp) CMDIdGPDB(*pmdidMDC);

	return New(pmp) CDXLDatumBool(pmp, pmdid, fNull, gpdb::FBoolFromDatum(datum));
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxldatumOid
//
//	@doc:
//		Translate a datum of type oid
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::PdxldatumOid
	(
	IMemoryPool *pmp,
	const IMDType *pmdtype,
	BOOL fNull,
	ULONG , //ulLen,
	Datum datum
	)
{
	GPOS_ASSERT(pmdtype->FByValue());
	CMDIdGPDB *pmdidMDC = CMDIdGPDB::PmdidConvert(pmdtype->Pmdid());
	CMDIdGPDB *pmdid = New(pmp) CMDIdGPDB(*pmdidMDC);

	return New(pmp) CDXLDatumOid(pmp, pmdid, fNull, gpdb::OidFromDatum(datum));
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxldatumInt4
//
//	@doc:
//		Translate a datum of type int4
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::PdxldatumInt4
	(
	IMemoryPool *pmp,
	const IMDType *pmdtype,
	BOOL fNull,
	ULONG , //ulLen,
	Datum datum
	)
{
	GPOS_ASSERT(pmdtype->FByValue());
	CMDIdGPDB *pmdidMDC = CMDIdGPDB::PmdidConvert(pmdtype->Pmdid());
	CMDIdGPDB *pmdid = New(pmp) CMDIdGPDB(*pmdidMDC);

	return New(pmp) CDXLDatumInt4(pmp, pmdid, fNull, gpdb::IInt32FromDatum(datum));
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::PdxldatumInt8
//
//	@doc:
//		Translate a datum of type int8
//---------------------------------------------------------------------------
CDXLDatum *
CTranslatorScalarToDXL::PdxldatumInt8
	(
	IMemoryPool *pmp,
	const IMDType *pmdtype,
	BOOL fNull,
	ULONG , //ulLen,
	Datum datum
	)
{
	GPOS_ASSERT(pmdtype->FByValue());
	CMDIdGPDB *pmdidMDC = CMDIdGPDB::PmdidConvert(pmdtype->Pmdid());
	CMDIdGPDB *pmdid = New(pmp) CMDIdGPDB(*pmdidMDC);

	return New(pmp) CDXLDatumInt8(pmp, pmdid, fNull, gpdb::LlInt64FromDatum(datum));
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::DValue
//
//	@doc:
//		Extract the double value of the datum
//---------------------------------------------------------------------------
CDouble
CTranslatorScalarToDXL::DValue
	(
	BYTE *pba,
	BOOL fNull
	)
{
	CDouble dValue = 0;
	if (!fNull)
	{
		Numeric num = (Numeric) (pba);
		if (NUMERIC_IS_NAN(num))
		{
			// in GPDB NaN is considered the largest numeric number.
			return CDouble(GPOS_FP_ABS_MAX);
		}

		double d = gpdb::DNumericToDoubleNoOverflow(num);
		dValue = CDouble(d);
	}

	return dValue;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::Pba
//
//	@doc:
//		Extract the byte array value of the datum. The result is NULL if datum is NULL
//---------------------------------------------------------------------------
BYTE *
CTranslatorScalarToDXL::Pba
	(
	IMemoryPool *pmp,
	const IMDType *pmdtype,
	BOOL fNull,
	ULONG ulLen,
	Datum datum
	)
{
	ULONG ulLength = 0;
	BYTE *pba = NULL;

	if (fNull)
	{
		return pba;
	}

	ulLength = (ULONG) gpdb::SDatumSize(datum, pmdtype->FByValue(), ulLen);
	GPOS_ASSERT(ulLength > 0);

	pba = New(pmp) BYTE[ulLength];

	if (pmdtype->FByValue())
	{
		GPOS_ASSERT(ulLength <= ULONG(sizeof(Datum)));
		clib::PvMemCpy(pba, &datum, ulLength);
	}
	else
	{
		clib::PvMemCpy(pba, gpdb::PvPointerFromDatum(datum), ulLength);
	}

	return pba;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::LValue
//
//	@doc:
//		Extract the long int value of a datum
//---------------------------------------------------------------------------
LINT
CTranslatorScalarToDXL::LValue
	(
	IMDId *pmdid,
	BOOL fNull,
	BYTE *pba,
	ULONG ulLength
	)
{
	LINT lValue = 0;
	if (fNull)
	{
		return lValue;
	}

	if (pmdid->FEquals(&CMDIdGPDB::m_mdidDate))
	{
		Datum datumConstVal = (Datum) 0;
		// the value is stored in the datum.
		clib::PvMemCpy(&datumConstVal, pba, ulLength);
		// Date is internally represented as an int32
		lValue = (LINT) (gpdb::IInt32FromDatum(datumConstVal));
		lValue = lValue * LINT(86400000000LL); // microseconds per day
	}
	else if (pmdid->FEquals(&CMDIdGPDB::m_mdidTimestamp))
	{
		Datum datumConstVal = (Datum) 0;
		// the value is stored in the datum.
		clib::PvMemCpy(&datumConstVal, pba, ulLength);
		// Timestamp is internally an int64
		lValue = (LINT) (gpdb::LlInt64FromDatum(datumConstVal));
	}
	else
	{
		// use hash value
		ULONG ulHash = 0;
		if (fNull)
		{
			ulHash = gpos::UlHash<ULONG>(&ulHash);
		}
		else
		{
			ulHash = gpos::UlHash<BYTE>(pba);
			for (ULONG ul = 1; ul < ulLength; ul++)
			{
				ulHash = gpos::UlCombineHashes(ulHash, gpos::UlHash<BYTE>(&pba[ul]));
			}
		}

		lValue = (LINT) (ulHash / 4);
	}

	return lValue;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorScalarToDXL::Pdatum
//
//	@doc:
//		Create IDatum from GPDB datum
//---------------------------------------------------------------------------
IDatum *
CTranslatorScalarToDXL::Pdatum
	(
	IMemoryPool *pmp,
	const IMDType *pmdtype,
	BOOL fNull,
	Datum datum
	)
{
	ULONG ulLength = pmdtype->UlLength();
	if (!pmdtype->FByValue() && !fNull)
	{
		INT iLen = dynamic_cast<const CMDTypeGenericGPDB *>(pmdtype)->ILength();
		ulLength = (ULONG) gpdb::SDatumSize(datum, pmdtype->FByValue(), iLen);
	}
	GPOS_ASSERT(fNull || ulLength > 0);

	CDXLDatum *pdxldatum = CTranslatorScalarToDXL::Pdxldatum(pmp, pmdtype, fNull, ulLength, datum);
	IDatum *pdatum = pmdtype->Pdatum(pmp, pdxldatum);
	pdxldatum->Release();
	return pdatum;
}

// EOF