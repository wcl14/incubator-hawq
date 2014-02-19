//---------------------------------------------------------------------------
//  Greenplum Database
//	Copyright (C) 2011 EMC Corp.
//
//	@filename:
//		CTranslatorQueryToDXL.cpp
//
//	@doc:
//		Implementation of the methods used to translate a query into DXL tree.
//		All translator methods allocate memory in the provided memory pool, and
//		the caller is responsible for freeing it
//
//	@owner:
//		raghav
//
//	@test:
//
//---------------------------------------------------------------------------

#include "postgres.h"

#include "access/sysattr.h"
#include "nodes/plannodes.h"
#include "nodes/parsenodes.h"
#include "nodes/makefuncs.h"
#include "optimizer/walkers.h"

#include "gpos/base.h"
#include "gpos/common/CAutoTimer.h"

#include "exception.h"

#include "dxl/CDXLUtils.h"
#include "dxl/operators/dxlops.h"
#include "dxl/operators/CDXLScalarBooleanTest.h"
#include "dxl/operators/CDXLDatumInt8.h"

#include "dxl/xml/dxltokens.h"

#include "gpopt/base/CUtils.h"
#include "gpopt/mdcache/CMDAccessor.h"
#include "gpopt/translate/CCTEListEntry.h"
#include "gpopt/translate/CQueryMutators.h"
#include "gpopt/translate/CTranslatorUtils.h"
#include "gpopt/translate/CTranslatorQueryToDXL.h"
#include "gpopt/translate/CTranslatorPlStmtToDXL.h"
#include "gpopt/translate/CTranslatorDXLToPlStmt.h"
#include "gpopt/translate/CTranslatorRelcacheToDXL.h"

#include "md/IMDScalarOp.h"
#include "md/IMDAggregate.h"
#include "md/IMDTypeBool.h"
#include "md/IMDTypeInt8.h"

#include "traceflags/traceflags.h"

#include "gpopt/gpdbwrappers.h"

using namespace gpdxl;
using namespace gpos;
using namespace gpopt;
using namespace gpnaucrates;
using namespace gpmd;

// OIDs of variants of LEAD window function
const OID rgOIDLead[] =
	{
	7011, 7074, 7075, 7310, 7312,
	7314, 7316, 7318,
	7320, 7322, 7324, 7326, 7328,
	7330, 7332, 7334, 7336, 7338,
	7340, 7342, 7344, 7346, 7348,
	7350, 7352, 7354, 7356, 7358,
	7360, 7362, 7364, 7366, 7368,
	7370, 7372, 7374, 7376, 7378,
	7380, 7382, 7384, 7386, 7388,
	7390, 7392, 7394, 7396, 7398,
	7400, 7402, 7404, 7406, 7408,
	7410, 7412, 7414, 7416, 7418,
	7420, 7422, 7424, 7426, 7428,
	7430, 7432, 7434, 7436, 7438,
	7440, 7442, 7444, 7446, 7448,
	7450, 7452, 7454, 7456, 7458,
	7460, 7462, 7464, 7466, 7468,
	7470, 7472, 7474, 7476, 7478,
	7480, 7482, 7484, 7486, 7488,
	7214, 7215, 7216, 7220, 7222,
	7224, 7244, 7246, 7248, 7260,
	7262, 7264
	};

// OIDs of variants of LAG window function
const OID rgOIDLag[] =
	{
	7675, 7491, 7493, 7495, 7497, 7499,
	7501, 7503, 7505, 7507, 7509,
	7511, 7513, 7515, 7517, 7519,
	7521, 7523, 7525, 7527, 7529,
	7531, 7533, 7535, 7537, 7539,
	7541, 7543, 7545, 7547, 7549,
	7551, 7553, 7555, 7557, 7559,
	7561, 7563, 7565, 7567, 7569,
	7571, 7573, 7575, 7577, 7579,
	7581, 7583, 7585, 7587, 7589,
	7591, 7593, 7595, 7597, 7599,
	7601, 7603, 7605, 7607, 7609,
	7611, 7613, 7615, 7617, 7619,
	7621, 7623, 7625, 7627, 7629,
	7631, 7633, 7635, 7637, 7639,
	7641, 7643, 7645, 7647, 7649,
	7651, 7653, 7655, 7657, 7659,
	7661, 7663, 7665, 7667, 7669,
	7671, 7673,
	7211, 7212, 7213, 7226, 7228,
	7230, 7250, 7252, 7254, 7266,
	7268, 7270
	};

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::CTranslatorQueryToDXL
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CTranslatorQueryToDXL::CTranslatorQueryToDXL
	(
	IMemoryPool *pmp,
	CMDAccessor *pmda,
	CIdGenerator *pidgtorColId,
	CIdGenerator *pidgtorCTE,
	CMappingVarColId *pmapvarcolid,
	Query *pquery,
	ULONG ulQueryLevel,
	HMUlCTEListEntry *phmulCTEEntries
	)
	:
	m_pmp(pmp),
	m_sysid(IMDId::EmdidGPDB, GPMD_GPDB_SYSID),
	m_pmda(pmda),
	m_pidgtorCol(pidgtorColId),
	m_pidgtorCTE(pidgtorCTE),
	m_pmapvarcolid(pmapvarcolid),
	m_ulQueryLevel(ulQueryLevel),
	m_phmulCTEEntries(NULL),
	m_pdrgpdxlnQueryOutput(NULL),
	m_pdrgpdxlnCTE(NULL),
	m_phmulfCTEProducers(NULL)
{
	GPOS_ASSERT(NULL != pquery);
	CheckSupportedCmdType(pquery);
	
	m_phmulCTEEntries = New(m_pmp) HMUlCTEListEntry(m_pmp);
	m_pdrgpdxlnCTE = New(m_pmp) DrgPdxln(m_pmp);
	m_phmulfCTEProducers = New(m_pmp) HMUlF(m_pmp);
	
	if (NULL != phmulCTEEntries)
	{
		HMIterUlCTEListEntry hmiterullist(phmulCTEEntries);

		while (hmiterullist.FAdvance())
		{
			ULONG ulCTEQueryLevel = *(hmiterullist.Pk());

			CCTEListEntry *pctelistentry =  const_cast<CCTEListEntry *>(hmiterullist.Pt());

			// CTE's that have been defined before the m_ulQueryLevel
			// should only be inserted into the hash map
			// For example:
			// WITH ab as (SELECT a as a, b as b from foo)
			// SELECT *
			// FROM
			// 	(WITH aEq10 as (SELECT b from ab ab1 where ab1.a = 10)
			//  	SELECT *
			//  	FROM (WITH aEq20 as (SELECT b from ab ab2 where ab2.a = 20)
			// 		      SELECT * FROM aEq10 WHERE b > (SELECT min(b) from aEq20)
			// 		      ) dtInner
			// 	) dtOuter
			// When translating the from expression containing "aEq10" in the derived table "dtInner"
			// we have already seen three CTE namely: "ab", "aEq10" and "aEq20". BUT when we expand aEq10
			// in the dt1, we should only have access of CTE's defined prior to its level namely "ab".

			if (ulCTEQueryLevel < ulQueryLevel && NULL != pctelistentry)
			{
				pctelistentry->AddRef();
#ifdef GPOS_DEBUG
				BOOL fRes =
#endif
				m_phmulCTEEntries->FInsert(New(pmp) ULONG(ulCTEQueryLevel), pctelistentry);
				GPOS_ASSERT(fRes);
			}
		}
	}

	// check if the query has any unsupported node types
	CheckUnsupportedNodeTypes(pquery);

	// check if the query has SIRV functions in the targetlist without a FROM clause
	CheckSirvFuncsWithoutFromClause(pquery);

	// first normalize the query
	m_pquery = CQueryMutators::PqueryNormalize(m_pmp, m_pmda, pquery, ulQueryLevel);

	if (NULL != m_pquery->cteList)
	{
		ConstructCTEProducerList(m_pquery->cteList, ulQueryLevel);
	}

	m_psctranslator = New(m_pmp) CTranslatorScalarToDXL
									(
									m_pmp,
									m_pmda,
									m_pidgtorCol,
									m_pidgtorCTE,
									m_ulQueryLevel,
									true, /* m_fQuery */
									NULL, /* m_pplstmt */
									NULL /* m_pmappv */,
									m_phmulCTEEntries,
									m_pdrgpdxlnCTE
									);

}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::~CTranslatorQueryToDXL
//
//	@doc:
//		Dtor
//
//---------------------------------------------------------------------------
CTranslatorQueryToDXL::~CTranslatorQueryToDXL()
{
	delete m_psctranslator;
	delete m_pmapvarcolid;
	gpdb::GPDBFree(m_pquery);
	m_phmulCTEEntries->Release();
	m_pdrgpdxlnCTE->Release();
	m_phmulfCTEProducers->Release();
	CRefCount::SafeRelease(m_pdrgpdxlnQueryOutput);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::CheckUnsupportedNodeTypes
//
//	@doc:
//		Check for unsupported node types, and throws an exception when found
//
//---------------------------------------------------------------------------
void
CTranslatorQueryToDXL::CheckUnsupportedNodeTypes
	(
	Query *pquery
	)
{
	SUnsupportedFeature rgUnsupported[] =
	{
		{T_RowExpr, GPOS_WSZ_LIT("ROW EXPRESSION")},
		{T_RowCompareExpr, GPOS_WSZ_LIT("ROW COMPARE")},
		{T_FieldSelect, GPOS_WSZ_LIT("FIELDSELECT")},
		{T_FieldStore, GPOS_WSZ_LIT("FIELDSTORE")},
		{T_ArrayRef, GPOS_WSZ_LIT("ARRAYREF")},
		{T_CoerceToDomain, GPOS_WSZ_LIT("COERCETODOMAIN")},
		{T_GroupId, GPOS_WSZ_LIT("GROUPID")},
		{T_PercentileExpr, GPOS_WSZ_LIT("PERCENTILE")},
		{T_CurrentOfExpr, GPOS_WSZ_LIT("CURRENT OF")},
	};

	List *plUnsupported = NIL;
	for (ULONG ul = 0; ul < GPOS_ARRAY_SIZE(rgUnsupported); ul++)
	{
		plUnsupported = gpdb::PlAppendInt(plUnsupported, rgUnsupported[ul].ent);
	}

	INT iUnsupported = gpdb::IFindNodes((Node *) pquery, plUnsupported);
	gpdb::GPDBFree(plUnsupported);

	if (0 <= iUnsupported)
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, rgUnsupported[iUnsupported].m_wsz);
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::CheckSirvFuncsWithoutFromClause
//
//	@doc:
//		Check for SIRV functions in the target list without a FROM clause, and
//		throw an exception when found
//
//---------------------------------------------------------------------------
void
CTranslatorQueryToDXL::CheckSirvFuncsWithoutFromClause
	(
	Query *pquery
	)
{
	// if there is a FROM clause or if target list is empty, look no further
	if ((NULL != pquery->jointree && 0 < gpdb::UlListLength(pquery->jointree->fromlist))
		|| NIL == pquery->targetList)
	{
		return;
	}

	// see if we have SIRV functions in the target list
	List *plFunctions =	gpdb::PlExtractNodesExpression((Node *)pquery->targetList, T_FuncExpr, true /*descendIntoSubqueries*/);
	ListCell *plc = NULL;

	ForEach (plc, plFunctions)
	{
		FuncExpr *pfuncexpr = (FuncExpr *) lfirst(plc);
		if (CTranslatorUtils::FSirvFunc(m_pmp, m_pmda, pfuncexpr->funcid))
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("SIRV functions"));
		}
	}

	gpdb::FreeList(plFunctions);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::CheckSupportedCmdType
//
//	@doc:
//		Check for supported command types, throws an exception when command
//		type not yet supported
//---------------------------------------------------------------------------
void
CTranslatorQueryToDXL::CheckSupportedCmdType
	(
	Query *pquery
	)
{
	if (NULL != pquery->utilityStmt)
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("UTILITY command"));
	}

	if (CMD_SELECT == pquery->commandType && NULL != pquery->intoClause)
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("CTAS"));
	}
	
	if (CMD_SELECT == pquery->commandType)
	{
		return;
	}

	if (CMD_INSERT == pquery->commandType || CMD_DELETE == pquery->commandType || CMD_UPDATE == pquery->commandType)
	{
		if (NULL != pquery->resultRelations)
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("DML on partitioned tables"));
		}
		return;
	}

	SCmdNameElem rgStrMap[] =
		{
		{CMD_UTILITY, GPOS_WSZ_LIT("UTILITY command")}
		};

	const ULONG ulLen = GPOS_ARRAY_SIZE(rgStrMap);
	for (ULONG ul = 0; ul < ulLen; ul++)
	{
		SCmdNameElem mapelem = rgStrMap[ul];
		if (mapelem.m_cmdtype == pquery->commandType)
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, mapelem.m_wsz);
		}
	}

	GPOS_ASSERT(!"Unrecognized command type");
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdrgpdxlnQueryOutput
//
//	@doc:
//		Return the list of query output columns
//
//---------------------------------------------------------------------------
DrgPdxln *
CTranslatorQueryToDXL::PdrgpdxlnQueryOutput() const
{
	return m_pdrgpdxlnQueryOutput;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdrgpdxlnCTE
//
//	@doc:
//		Return the list of CTEs
//
//---------------------------------------------------------------------------
DrgPdxln *
CTranslatorQueryToDXL::PdrgpdxlnCTE() const
{
	return m_pdrgpdxlnCTE;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnFromQueryInternal
//
//	@doc:
//		Translates a Query into a DXL tree. The function allocates memory in
//		the translator memory pool, and caller is responsible for freeing it.
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnFromQueryInternal()
{
	CTranslatorUtils::CheckRTEPermissions(m_pquery->rtable);
	
	CDXLNode *pdxlnChild = NULL;
	HMIUl *phmiulSortGroupColsColId =  New(m_pmp) HMIUl(m_pmp);
	HMIUl *phmiulOutputCols = New(m_pmp) HMIUl(m_pmp);

	// construct CTEAnchor operators for the CTEs defined at the top level
	CDXLNode *pdxlnCTEAnchorTop = NULL;
	CDXLNode *pdxlnCTEAnchorBottom = NULL;
	ConstructCTEAnchors(m_pdrgpdxlnCTE, &pdxlnCTEAnchorTop, &pdxlnCTEAnchorBottom);
	GPOS_ASSERT_IMP(0 < m_pdrgpdxlnCTE->UlSafeLength(),
					NULL != pdxlnCTEAnchorTop && NULL != pdxlnCTEAnchorBottom);
	
	GPOS_ASSERT_IMP(NULL != m_pquery->setOperations, 0 == gpdb::UlListLength(m_pquery->windowClause));
	if (NULL != m_pquery->setOperations)
	{
		List *plTargetList = m_pquery->targetList;
		// translate set operations
		pdxlnChild = PdxlnFromSetOp(m_pquery->setOperations, plTargetList, phmiulOutputCols);

		CDXLLogicalSetOp *pdxlop = CDXLLogicalSetOp::PdxlopConvert(pdxlnChild->Pdxlop());
		const DrgPdxlcd *pdrgpdxlcd = pdxlop->Pdrgpdxlcd();
		ListCell *plcTE = NULL;
		ULONG ulResNo = 1;
		ForEach (plcTE, plTargetList)
		{
			TargetEntry *pte = (TargetEntry *) lfirst(plcTE);
			if (0 < pte->ressortgroupref)
			{
				ULONG ulColId = ((*pdrgpdxlcd)[ulResNo - 1])->UlID();
				AddSortingGroupingColumn(pte, phmiulSortGroupColsColId, ulColId);
			}
			ulResNo++;
		}
	}
	else if (0 != gpdb::UlListLength(m_pquery->windowClause)) // translate window clauses
	{
		CDXLNode *pdxln = PdxlnFromGPDBFromExpr(m_pquery->jointree);
		GPOS_ASSERT(NULL == m_pquery->groupClause);
		pdxlnChild = PdxlnWindow
						(
						pdxln,
						m_pquery->targetList,
						m_pquery->windowClause,
						m_pquery->sortClause,
						phmiulSortGroupColsColId,
						phmiulOutputCols
						);
	}
	else
	{
		pdxlnChild = PdxlnGroupingSets(m_pquery->jointree, m_pquery->targetList, m_pquery->groupClause, m_pquery->hasAggs, phmiulSortGroupColsColId, phmiulOutputCols);
	}

	// translate limit clause
	CDXLNode *pdxlnLimit = PdxlnLgLimit(m_pquery->sortClause, m_pquery->limitCount, m_pquery->limitOffset, pdxlnChild, phmiulSortGroupColsColId);


	if (NULL == m_pquery->targetList)
	{
		m_pdrgpdxlnQueryOutput = New(m_pmp) DrgPdxln(m_pmp);
	}
	else
	{
		m_pdrgpdxlnQueryOutput = PdrgpdxlnConstructOutputCols(m_pquery->targetList, phmiulOutputCols);
	}

	// cleanup
	CRefCount::SafeRelease(phmiulSortGroupColsColId);

	phmiulOutputCols->Release();
	
	// add CTE anchors if needed
	CDXLNode *pdxlnResult = pdxlnLimit;
	
	if (NULL != pdxlnCTEAnchorTop)
	{
		GPOS_ASSERT(NULL != pdxlnCTEAnchorBottom);
		pdxlnCTEAnchorBottom->AddChild(pdxlnResult);
		pdxlnResult = pdxlnCTEAnchorTop;
	}
	
	return pdxlnResult;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnSPJ
//
//	@doc:
//		Construct a DXL SPJ tree from the given query parts
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnSPJ
	(
	List *plTargetList,
	FromExpr *pfromexpr,
	HMIUl *phmiulSortGroupColsColId,
	HMIUl *phmiulOutputCols,
	List *plGroupClause
	)
{
	CDXLNode *pdxlnJoinTree = PdxlnFromGPDBFromExpr(pfromexpr);

	// translate target list entries into a logical project
	return PdxlnLgProjectFromGPDBTL(plTargetList, pdxlnJoinTree, phmiulSortGroupColsColId, phmiulOutputCols, plGroupClause);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnSPJForGroupingSets
//
//	@doc:
//		Construct a DXL SPJ tree from the given query parts, and keep variables
//		appearing in aggregates in the project list
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnSPJForGroupingSets
	(
	List *plTargetList,
	FromExpr *pfromexpr,
	HMIUl *phmiulSortGroupColsColId,
	HMIUl *phmiulOutputCols,
	List *plGroupClause
	)
{
	CDXLNode *pdxlnJoinTree = PdxlnFromGPDBFromExpr(pfromexpr);

	// translate target list entries into a logical project
	return PdxlnLgProjectFromGPDBTL(plTargetList, pdxlnJoinTree, phmiulSortGroupColsColId, phmiulOutputCols, plGroupClause, true /*fExpandAggrefExpr*/);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnFromQuery
//
//	@doc:
//		Main driver
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnFromQuery()
{
	CAutoTimer at("\n[OPT]: Query To DXL Translation Time", GPOS_FTRACE(EopttracePrintOptStats));

	switch (m_pquery->commandType)
	{
		case CMD_SELECT:
			return PdxlnFromQueryInternal();

		case CMD_INSERT:
			return PdxlnInsert();

		case CMD_DELETE:
			return PdxlnDelete();

		case CMD_UPDATE:
			return PdxlnUpdate();

		default:
			GPOS_ASSERT(!"Statement type not supported");
			return NULL;
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnInsert
//
//	@doc:
//		Translate an insert stmt
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnInsert()
{
	GPOS_ASSERT(CMD_INSERT == m_pquery->commandType);
	GPOS_ASSERT(0 < m_pquery->resultRelation);

	CDXLNode *pdxlnQuery = PdxlnFromQueryInternal();
	const RangeTblEntry *prte = (RangeTblEntry *) gpdb::PvListNth(m_pquery->rtable, m_pquery->resultRelation - 1);

	CDXLTableDescr *pdxltabdesc = CTranslatorUtils::Pdxltabdesc(m_pmp, m_pmda, m_pidgtorCol, prte);
	const IMDRelation *pmdrel = m_pmda->Pmdrel(pdxltabdesc->Pmdid());

	const ULONG ulLenTblCols = pmdrel->UlColumns() - pmdrel->UlSystemColumns();
	const ULONG ulLenTL = gpdb::UlListLength(m_pquery->targetList);
	GPOS_ASSERT(ulLenTblCols >= ulLenTL);
	GPOS_ASSERT(ulLenTL == m_pdrgpdxlnQueryOutput->UlLength());

	CDXLNode *pdxlnPrL = NULL;
	
	const ULONG ulLenNonDroppedCols = pmdrel->UlNonDroppedCols() - pmdrel->UlSystemColumns();
	if (ulLenNonDroppedCols > ulLenTL)
	{
		// missing target list entries
		pdxlnPrL = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjList(m_pmp));
	}

	DrgPul *pdrgpulSource = New(m_pmp) DrgPul(m_pmp);

	ULONG ulPosTL = 0;
	for (ULONG ul = 0; ul < ulLenTblCols; ul++)
	{
		const IMDColumn *pmdcol = pmdrel->Pmdcol(ul);
		GPOS_ASSERT(!pmdcol->FSystemColumn());
		
		if (pmdcol->FDropped())
		{
			continue;
		}
		
		if (ulPosTL < ulLenTL)
		{
			INT iAttno = pmdcol->IAttno();
			
			TargetEntry *pte = (TargetEntry *) gpdb::PvListNth(m_pquery->targetList, ulPosTL);
			AttrNumber iResno = pte->resno;

			if (iAttno == iResno)
			{
				CDXLNode *pdxlnCol = (*m_pdrgpdxlnQueryOutput)[ulPosTL];
				CDXLScalarIdent *pdxlopIdent = CDXLScalarIdent::PdxlopConvert(pdxlnCol->Pdxlop());
				pdrgpulSource->Append(New(m_pmp) ULONG(pdxlopIdent->Pdxlcr()->UlID()));
				ulPosTL++;
				continue;
			}
		}

		// target entry corresponding to the tables column not found, therefore
		// add a project element with null value scalar child
		CDXLNode *pdxlnPrE = CTranslatorUtils::PdxlnPrElNull(m_pmp, m_pmda, m_pidgtorCol, pmdcol);
		ULONG ulColId = CDXLScalarProjElem::PdxlopConvert(pdxlnPrE->Pdxlop())->UlId();
 	 	pdxlnPrL->AddChild(pdxlnPrE);
	 	pdrgpulSource->Append(New(m_pmp) ULONG(ulColId));
	}

	CDXLLogicalInsert *pdxlopInsert = New(m_pmp) CDXLLogicalInsert(m_pmp, pdxltabdesc, pdrgpulSource);

	if (NULL != pdxlnPrL)
	{
		GPOS_ASSERT(0 < pdxlnPrL->UlArity());
		
		CDXLNode *pdxlnProject = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalProject(m_pmp));
		pdxlnProject->AddChild(pdxlnPrL);
		pdxlnProject->AddChild(pdxlnQuery);
		pdxlnQuery = pdxlnProject;
	}

	return New(m_pmp) CDXLNode(m_pmp, pdxlopInsert, pdxlnQuery);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::GetCtidAndSegmentId
//
//	@doc:
//		Obtains the ids of the ctid and segmentid columns for the target
//		table of a DML query
//
//---------------------------------------------------------------------------
void
CTranslatorQueryToDXL::GetCtidAndSegmentId
	(
	ULONG *pulCtid,
	ULONG *pulSegmentId
	)
{
	// ctid column id
	IMDId *pmdid = CTranslatorUtils::PmdidSystemColType(m_pmp, SelfItemPointerAttributeNumber);
	*pulCtid = CTranslatorUtils::UlColId(m_ulQueryLevel, m_pquery->resultRelation, SelfItemPointerAttributeNumber, pmdid, m_pmapvarcolid);
	pmdid->Release();

	// segmentid column id
	pmdid = CTranslatorUtils::PmdidSystemColType(m_pmp, GpSegmentIdAttributeNumber);
	*pulSegmentId = CTranslatorUtils::UlColId(m_ulQueryLevel, m_pquery->resultRelation, GpSegmentIdAttributeNumber, pmdid, m_pmapvarcolid);
	pmdid->Release();
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::UlTupleOidColId
//
//	@doc:
//		Obtains the id of the tuple oid column for the target table of a DML
//		update
//
//---------------------------------------------------------------------------
ULONG
CTranslatorQueryToDXL::UlTupleOidColId()
{
	IMDId *pmdid = CTranslatorUtils::PmdidSystemColType(m_pmp, ObjectIdAttributeNumber);
	ULONG ulTupleOidColId = CTranslatorUtils::UlColId(m_ulQueryLevel, m_pquery->resultRelation, ObjectIdAttributeNumber, pmdid, m_pmapvarcolid);
	pmdid->Release();
	return ulTupleOidColId;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnDelete
//
//	@doc:
//		Translate a delete stmt
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnDelete()
{
	GPOS_ASSERT(CMD_DELETE == m_pquery->commandType);
	GPOS_ASSERT(0 < m_pquery->resultRelation);

	CDXLNode *pdxlnQuery = PdxlnFromQueryInternal();
	const RangeTblEntry *prte = (RangeTblEntry *) gpdb::PvListNth(m_pquery->rtable, m_pquery->resultRelation - 1);

	CDXLTableDescr *pdxltabdesc = CTranslatorUtils::Pdxltabdesc(m_pmp, m_pmda, m_pidgtorCol, prte);

	ULONG ulCtid = 0;
	ULONG ulSegmentId = 0;
	GetCtidAndSegmentId(&ulCtid, &ulSegmentId);

	const IMDRelation *pmdrel = m_pmda->Pmdrel(pdxltabdesc->Pmdid());
	DrgPul *pdrgpulDelete = New(m_pmp) DrgPul(m_pmp);

	const ULONG ulRelColumns = pmdrel->UlColumns();
	for (ULONG ul = 0; ul < ulRelColumns; ul++)
	{
		const IMDColumn *pmdcol = pmdrel->Pmdcol(ul);
		if (pmdcol->FSystemColumn() || pmdcol->FDropped())
		{
			continue;
		}

		ULONG ulColId = CTranslatorUtils::UlColId(m_ulQueryLevel, m_pquery->resultRelation, pmdcol->IAttno(), pmdcol->PmdidType(), m_pmapvarcolid);
		pdrgpulDelete->Append(New(m_pmp) ULONG(ulColId));
	}

	CDXLLogicalDelete *pdxlopdelete = New(m_pmp) CDXLLogicalDelete(m_pmp, pdxltabdesc, ulCtid, ulSegmentId, pdrgpulDelete);

	return New(m_pmp) CDXLNode(m_pmp, pdxlopdelete, pdxlnQuery);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnUpdate
//
//	@doc:
//		Translate an update stmt
//
//---------------------------------------------------------------------------

CDXLNode *
CTranslatorQueryToDXL::PdxlnUpdate()
{
	GPOS_ASSERT(CMD_UPDATE == m_pquery->commandType);
	GPOS_ASSERT(0 < m_pquery->resultRelation);

	CDXLNode *pdxlnQuery = PdxlnFromQueryInternal();
	const RangeTblEntry *prte = (RangeTblEntry *) gpdb::PvListNth(m_pquery->rtable, m_pquery->resultRelation - 1);

	CDXLTableDescr *pdxltabdesc = CTranslatorUtils::Pdxltabdesc(m_pmp, m_pmda, m_pidgtorCol, prte);

	ULONG ulCtidColId = 0;
	ULONG ulSegmentIdColId = 0;
	GetCtidAndSegmentId(&ulCtidColId, &ulSegmentIdColId);
	
	ULONG ulTupleOidColId = 0;
	
	const IMDRelation *pmdrel = m_pmda->Pmdrel(pdxltabdesc->Pmdid());

	BOOL fHasOids = pmdrel->FHasOids();
	if (fHasOids)
	{
		ulTupleOidColId = UlTupleOidColId();
	}

	// get (resno -> colId) mapping of columns to be updated
	HMIUl *phmiulUpdateCols = PhmiulUpdateCols();

	const ULONG ulRelColumns = pmdrel->UlColumns();
	DrgPul *pdrgpulInsert = New(m_pmp) DrgPul(m_pmp);
	DrgPul *pdrgpulDelete = New(m_pmp) DrgPul(m_pmp);

	for (ULONG ul = 0; ul < ulRelColumns; ul++)
	{
		const IMDColumn *pmdcol = pmdrel->Pmdcol(ul);
		if (pmdcol->FSystemColumn() || pmdcol->FDropped())
		{
			continue;
		}

		INT iAttno = pmdcol->IAttno();
		ULONG *pulColId = phmiulUpdateCols->PtLookup(&iAttno);

		ULONG ulColId = CTranslatorUtils::UlColId(m_ulQueryLevel, m_pquery->resultRelation, iAttno, pmdcol->PmdidType(), m_pmapvarcolid);

		// if the column is in the query outputs then use it
		// otherwise get the column id created by the child query
		if (NULL != pulColId)
		{
			pdrgpulInsert->Append(New(m_pmp) ULONG(*pulColId));
		}
		else
		{
			pdrgpulInsert->Append(New(m_pmp) ULONG(ulColId));
		}

		pdrgpulDelete->Append(New(m_pmp) ULONG(ulColId));
	}

	phmiulUpdateCols->Release();
	CDXLLogicalUpdate *pdxlopupdate = New(m_pmp) CDXLLogicalUpdate(m_pmp, pdxltabdesc, ulCtidColId, ulSegmentIdColId, pdrgpulDelete, pdrgpulInsert, fHasOids, ulTupleOidColId);

	return New(m_pmp) CDXLNode(m_pmp, pdxlopupdate, pdxlnQuery);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PhmiulUpdateCols
//
//	@doc:
// 		Return resno -> colId mapping of columns to be updated
//
//---------------------------------------------------------------------------
HMIUl *
CTranslatorQueryToDXL::PhmiulUpdateCols()
{
	GPOS_ASSERT((ULONG)gpdb::UlListLength(m_pquery->targetList) == m_pdrgpdxlnQueryOutput->UlLength());
	HMIUl *phmiulUpdateCols = New(m_pmp) HMIUl(m_pmp);

	ListCell *plc = NULL;
	ULONG ul = 0;
	ForEach (plc, m_pquery->targetList)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plc);
		GPOS_ASSERT(IsA(pte, TargetEntry));
		ULONG ulResno = pte->resno;
		GPOS_ASSERT(0 < ulResno);

		CDXLNode *pdxlnCol = (*m_pdrgpdxlnQueryOutput)[ul];
		CDXLScalarIdent *pdxlopIdent = CDXLScalarIdent::PdxlopConvert(pdxlnCol->Pdxlop());
		ULONG ulColId = pdxlopIdent->Pdxlcr()->UlID();

		StoreAttnoColIdMapping(phmiulUpdateCols, ulResno, ulColId);
		ul++;
	}

	return phmiulUpdateCols;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::FOIDFound
//
//	@doc:
// 		Helper to check if OID is included in given array of OIDs
//
//---------------------------------------------------------------------------
BOOL
CTranslatorQueryToDXL::FOIDFound
	(
	OID oid,
	const OID rgOID[],
	ULONG ulSize
	)
{
	BOOL fFound = false;
	for (ULONG ul = 0; !fFound && ul < ulSize; ul++)
	{
		fFound = (rgOID[ul] == oid);
	}

	return fFound;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::FLeadWindowFunc
//
//	@doc:
// 		Check if given operator is LEAD window function
//
//---------------------------------------------------------------------------
BOOL
CTranslatorQueryToDXL::FLeadWindowFunc
	(
	CDXLOperator *pdxlop
	)
{
	BOOL fLead = false;
	if (EdxlopScalarWindowRef == pdxlop->Edxlop())
	{
		CDXLScalarWindowRef *pdxlopWinref = CDXLScalarWindowRef::PdxlopConvert(pdxlop);
		const CMDIdGPDB *pmdidgpdb = CMDIdGPDB::PmdidConvert(pdxlopWinref->PmdidFunc());
		OID oid = pmdidgpdb->OidObjectId();
		fLead =  FOIDFound(oid, rgOIDLead, GPOS_ARRAY_SIZE(rgOIDLead));
	}

	return fLead;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::FLagWindowFunc
//
//	@doc:
// 		Check if given operator is LAG window function
//
//---------------------------------------------------------------------------
BOOL
CTranslatorQueryToDXL::FLagWindowFunc
	(
	CDXLOperator *pdxlop
	)
{
	BOOL fLag = false;
	if (EdxlopScalarWindowRef == pdxlop->Edxlop())
	{
		CDXLScalarWindowRef *pdxlopWinref = CDXLScalarWindowRef::PdxlopConvert(pdxlop);
		const CMDIdGPDB *pmdidgpdb = CMDIdGPDB::PmdidConvert(pdxlopWinref->PmdidFunc());
		OID oid = pmdidgpdb->OidObjectId();
		fLag =  FOIDFound(oid, rgOIDLag, GPOS_ARRAY_SIZE(rgOIDLag));
	}

	return fLag;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlwfLeadLag
//
//	@doc:
// 		Manufacture window frame for lead/lag functions
//
//---------------------------------------------------------------------------
CDXLWindowFrame *
CTranslatorQueryToDXL::PdxlwfLeadLag
	(
	BOOL fLead,
	CDXLNode *pdxlnOffset
	)
	const
{
	EdxlFrameBoundary edxlfbLead = EdxlfbBoundedFollowing;
	EdxlFrameBoundary edxlfbTrail = EdxlfbBoundedFollowing;
	if (!fLead)
	{
		edxlfbLead = EdxlfbBoundedPreceding;
		edxlfbTrail = EdxlfbBoundedPreceding;
	}

	CDXLNode *pdxlnLeadEdge = NULL;
	CDXLNode *pdxlnTrailEdge = NULL;
	if (NULL == pdxlnOffset)
	{
		pdxlnLeadEdge = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarWindowFrameEdge(m_pmp, true /* fLeading */, edxlfbLead));
		pdxlnTrailEdge = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarWindowFrameEdge(m_pmp, false /* fLeading */, edxlfbTrail));

		pdxlnLeadEdge->AddChild(CTranslatorUtils::PdxlnInt4Const(m_pmp, m_pmda, 1 /*iVal*/));
		pdxlnTrailEdge->AddChild(CTranslatorUtils::PdxlnInt4Const(m_pmp, m_pmda, 1 /*iVal*/));
	}
	else
	{
		// overwrite frame edge types based on specified offset type
		if (EdxlopScalarConstValue != pdxlnOffset->Pdxlop()->Edxlop())
		{
			if (fLead)
			{
				edxlfbLead = EdxlfbDelayedBoundedFollowing;
				edxlfbTrail = EdxlfbDelayedBoundedFollowing;
			}
			else
			{
				edxlfbLead = EdxlfbDelayedBoundedPreceding;
				edxlfbTrail = EdxlfbDelayedBoundedPreceding;
			}
		}
		pdxlnLeadEdge = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarWindowFrameEdge(m_pmp, true /* fLeading */, edxlfbLead));
		pdxlnTrailEdge = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarWindowFrameEdge(m_pmp, false /* fLeading */, edxlfbTrail));

		pdxlnOffset->AddRef();
		pdxlnLeadEdge->AddChild(pdxlnOffset);
		pdxlnOffset->AddRef();
		pdxlnTrailEdge->AddChild(pdxlnOffset);
	}

	// manufacture a frame for LEAD/LAG function
	return New(m_pmp) CDXLWindowFrame
							(
							m_pmp,
							EdxlfsRow, // frame specification
							EdxlfesNulls, // frame exclusion strategy is set to exclude NULLs in GPDB
							pdxlnLeadEdge,
							pdxlnTrailEdge
							);
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::UpdateLeadLagWinSpecPos
//
//	@doc:
// 		LEAD/LAG window functions need special frames to get executed correctly;
//		these frames are system-generated and cannot be specified in query text;
//		this function adds new entries to the list of window specs holding these
//		manufactured frames, and updates window spec references of LEAD/LAG
//		functions accordingly
//
//
//---------------------------------------------------------------------------
void
CTranslatorQueryToDXL::UpdateLeadLagWinSpecPos
	(
	CDXLNode *pdxlnPrL, // project list holding WinRef nodes
	DrgPdxlws *pdrgpdxlws // original list of window spec
	)
	const
{
	GPOS_ASSERT(NULL != pdxlnPrL);
	GPOS_ASSERT(NULL != pdrgpdxlws);

	const ULONG ulArity = pdxlnPrL->UlArity();
	for (ULONG ul = 0; ul < ulArity; ul++)
	{
		CDXLNode *pdxlnChild = (*(*pdxlnPrL)[ul])[0];
		CDXLOperator *pdxlop = pdxlnChild->Pdxlop();
		BOOL fLead = FLeadWindowFunc(pdxlop);
		BOOL fLag = FLagWindowFunc(pdxlop);
		if (fLead || fLag)
		{
			CDXLScalarWindowRef *pdxlopWinref = CDXLScalarWindowRef::PdxlopConvert(pdxlop);
			CDXLWindowSpec *pdxlws = (*pdrgpdxlws)[pdxlopWinref->UlWinSpecPos()];
			CMDName *pmdname = NULL;
			if (NULL != pdxlws->Pmdname())
			{
				pmdname = New(m_pmp) CMDName(m_pmp, pdxlws->Pmdname()->Pstr());
			}

			// find if an offset is specified
			CDXLNode *pdxlnOffset = NULL;
			if (1 < pdxlnChild->UlArity())
			{
				pdxlnOffset = (*pdxlnChild)[1];
			}

			// create LEAD/LAG frame
			CDXLWindowFrame *pdxlwf = PdxlwfLeadLag(fLead, pdxlnOffset);

			// create new window spec object
			pdxlws->PdrgulPartColList()->AddRef();
			pdxlws->PdxlnSortColList()->AddRef();
			CDXLWindowSpec *pdxlwsNew =
				New(m_pmp) CDXLWindowSpec
					(
					m_pmp,
					pdxlws->PdrgulPartColList(),
					pmdname,
					pdxlws->PdxlnSortColList(),
					pdxlwf
					);
			pdrgpdxlws->Append(pdxlwsNew);

			// update win spec pos of LEAD/LAG function
			pdxlopWinref->SetWinSpecPos(pdrgpdxlws->UlLength() - 1);
		}
	}
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::Pdrgpdxlws
//
//	@doc:
//		Translate window specs
//
//---------------------------------------------------------------------------
DrgPdxlws *
CTranslatorQueryToDXL::Pdrgpdxlws
	(
	List *plWindowClause,
	HMIUl *phmiulSortColsColId,
	CDXLNode *pdxlnScPrL
	)
	const
{
	GPOS_ASSERT(NULL != plWindowClause);
	GPOS_ASSERT(NULL != phmiulSortColsColId);
	GPOS_ASSERT(NULL != pdxlnScPrL);

	DrgPdxlws *pdrgpdxlws = New(m_pmp) DrgPdxlws(m_pmp);

	// translate window specification
	ListCell *plcWindowSpec = NULL;
	ForEach (plcWindowSpec, plWindowClause)
	{
		WindowSpec *pwindowspec = (WindowSpec *) lfirst(plcWindowSpec);
		DrgPul *pdrgppulPartCol = PdrgpulPartCol(pwindowspec->partition, phmiulSortColsColId);

		CDXLNode *pdxlnSortColList = NULL;
		CMDName *pmdname = NULL;
		CDXLWindowFrame *pdxlwf = NULL;

		if (NULL != pwindowspec->name)
		{
			CWStringDynamic *pstrAlias = CDXLUtils::PstrFromSz(m_pmp, pwindowspec->name);
			pmdname = New(m_pmp) CMDName(m_pmp, pstrAlias);
			delete pstrAlias;
		}

		if (0 < gpdb::UlListLength(pwindowspec->order))
		{
			// create a sorting col list
			pdxlnSortColList = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarSortColList(m_pmp));

			DrgPdxln *pdrgpdxlnSortCol = PdrgpdxlnSortCol(pwindowspec->order, phmiulSortColsColId);
			const ULONG ulSize = pdrgpdxlnSortCol->UlLength();
			for (ULONG ul = 0; ul < ulSize; ul++)
			{
				CDXLNode *pdxlnSortClause = (*pdrgpdxlnSortCol)[ul];
				pdxlnSortClause->AddRef();
				pdxlnSortColList->AddChild(pdxlnSortClause);
			}
			pdrgpdxlnSortCol->Release();
		}

		if (NULL != pwindowspec->frame)
		{
			pdxlwf = m_psctranslator->Pdxlwf((Expr *) pwindowspec->frame, m_pmapvarcolid, pdxlnScPrL);
		}

		CDXLWindowSpec *pdxlws = New(m_pmp) CDXLWindowSpec(m_pmp, pdrgppulPartCol, pmdname, pdxlnSortColList, pdxlwf);
		pdrgpdxlws->Append(pdxlws);
	}

	return pdrgpdxlws;
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnWindow
//
//	@doc:
//		Translate a window operator
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnWindow
	(
	CDXLNode *pdxlnChild,
	List *plTargetList,
	List *plWindowClause,
	List *plSortClause,
	HMIUl *phmiulSortColsColId,
	HMIUl *phmiulOutputCols
	)
	const
{
	if (0 == gpdb::UlListLength(plWindowClause))
	{
		return pdxlnChild;
	}

	// translate target list entries
	CDXLNode *pdxlnPrL = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjList(m_pmp));

	CDXLNode *pdxlnNewChildScPrL = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjList(m_pmp));
	ListCell *plcTE = NULL;
	ULONG ulResno = 1;

	// target entries that are result of flattening join alias and 
	// are equivalent to a defined Window specs target entry
	List *plTEOmitted = NIL; 
	List *plResno = NIL;
	
	ForEach (plcTE, plTargetList)
	{
		BOOL fInsertSortInfo = true;
		TargetEntry *pte = (TargetEntry *) lfirst(plcTE);
		GPOS_ASSERT(IsA(pte, TargetEntry));

		// create the DXL node holding the target list entry
		CDXLNode *pdxlnPrEl =  PdxlnPrEFromGPDBExpr(pte->expr, pte->resname);
		ULONG ulColId = CDXLScalarProjElem::PdxlopConvert(pdxlnPrEl->Pdxlop())->UlId();

		if (!pte->resjunk)
		{
			if (IsA(pte->expr, Var) || IsA(pte->expr, WindowRef))
			{
				// add window functions and non-computed columns to the project list of the window operator
				pdxlnPrL->AddChild(pdxlnPrEl);

				StoreAttnoColIdMapping(phmiulOutputCols, ulResno, ulColId);
			}
			else if (CTranslatorUtils::FWindowSpec(pte, plWindowClause))
			{
				// add computed column used in window specification needed in the output columns
				// to the child's project list
				pdxlnNewChildScPrL->AddChild(pdxlnPrEl);

				// construct a scalar identifier that points to the computed column and
				// add it to the project list of the window operator
				CMDName *pmdnameAlias = New(m_pmp) CMDName
													(
													m_pmp,
													CDXLScalarProjElem::PdxlopConvert(pdxlnPrEl->Pdxlop())->PmdnameAlias()->Pstr()
													);
				CDXLNode *pdxlnPrElNew = New(m_pmp) CDXLNode
													(
													m_pmp,
													New(m_pmp) CDXLScalarProjElem(m_pmp, ulColId, pmdnameAlias)
													);
				CDXLNode *pdxlnPrElNewChild = New(m_pmp) CDXLNode
															(
															m_pmp,
															New(m_pmp) CDXLScalarIdent
																		(
																		m_pmp,
																		New(m_pmp) CDXLColRef
																					(
																					m_pmp,
																					New(m_pmp) CMDName(m_pmp, pmdnameAlias->Pstr()), ulColId
																					),
																		New(m_pmp) CMDIdGPDB(gpdb::OidExprType((Node*) pte->expr))
																		)
															);
				pdxlnPrElNew->AddChild(pdxlnPrElNewChild);
				pdxlnPrL->AddChild(pdxlnPrElNew);

				StoreAttnoColIdMapping(phmiulOutputCols, ulResno, ulColId);
			}
			else
			{
				fInsertSortInfo = false;
				plTEOmitted = gpdb::PlAppendElement(plTEOmitted, pte);
				plResno = gpdb::PlAppendInt(plResno, ulResno);

				pdxlnPrEl->Release();
			}
		}
		else if (IsA(pte->expr, WindowRef))
		{
			// computed columns used in the order by clause
			pdxlnPrL->AddChild(pdxlnPrEl);
		}
		else if (!IsA(pte->expr, Var))
		{
			GPOS_ASSERT(CTranslatorUtils::FWindowSpec(pte, plWindowClause));
			// computed columns used in the window specification
			pdxlnNewChildScPrL->AddChild(pdxlnPrEl);
		}
		else
		{
			pdxlnPrEl->Release();
		}

		if (fInsertSortInfo)
		{
			AddSortingGroupingColumn(pte, phmiulSortColsColId, ulColId);
		}

		ulResno++;
	}

	plcTE = NULL;

	// process target entries that are a result of flattening join alias
	ListCell *plcResno = NULL;
	ForBoth (plcTE, plTEOmitted,
			plcResno, plResno)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plcTE);
		INT iResno = (INT) lfirst_int(plcResno);

		INT iSortGroupRef = (INT) pte->ressortgroupref;

		TargetEntry *pteWindowSpec = CTranslatorUtils::PteWindowSpec( (Node*) pte->expr, plWindowClause, plTargetList);
		if (NULL != pteWindowSpec)
		{
			const ULONG ulColId = CTranslatorUtils::UlColId( (INT) pteWindowSpec->ressortgroupref, phmiulSortColsColId);
			StoreAttnoColIdMapping(phmiulOutputCols, iResno, ulColId);
			AddSortingGroupingColumn(pte, phmiulSortColsColId, ulColId);
		}
	}
	if (NIL != plTEOmitted)
	{
		gpdb::GPDBFree(plTEOmitted);
	}

	// translate window spec
	DrgPdxlws *pdrgpdxlws = Pdrgpdxlws(plWindowClause, phmiulSortColsColId, pdxlnNewChildScPrL);

	CDXLNode *pdxlnNewChild = NULL;

	if (0 < pdxlnNewChildScPrL->UlArity())
	{
		// create a project list for the computed columns used in the window specification
		pdxlnNewChild = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalProject(m_pmp));
		pdxlnNewChild->AddChild(pdxlnNewChildScPrL);
		pdxlnNewChild->AddChild(pdxlnChild);
		pdxlnChild = pdxlnNewChild;
	}
	else
	{
		// clean up
		pdxlnNewChildScPrL->Release();
	}

	if (0 == pdxlnPrL->UlArity())
	{
		pdxlnPrL->Release();
		pdrgpdxlws->Release();

		return pdxlnChild;
	}

	// update window spec positions of LEAD/LAG functions
	UpdateLeadLagWinSpecPos(pdxlnPrL, pdrgpdxlws);

	CDXLLogicalWindow *pdxlopWindow = New(m_pmp) CDXLLogicalWindow(m_pmp, pdrgpdxlws);
	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlopWindow);

	pdxln->AddChild(pdxlnPrL);
	pdxln->AddChild(pdxlnChild);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdrgpulPartCol
//
//	@doc:
//		Translate the list of partition-by column identifiers
//
//---------------------------------------------------------------------------
DrgPul *
CTranslatorQueryToDXL::PdrgpulPartCol
	(
	List *plPartCl,
	HMIUl *phmiulColColId
	)
	const
{
	DrgPul *pdrgpul = New(m_pmp) DrgPul(m_pmp);

	ListCell *plcPartCl = NULL;
	ForEach (plcPartCl, plPartCl)
	{
		Node *pnodePartCl = (Node*) lfirst(plcPartCl);
		GPOS_ASSERT(NULL != pnodePartCl);

		GPOS_ASSERT(IsA(pnodePartCl, SortClause));
		SortClause *psortcl = (SortClause*) pnodePartCl;

		// get the colid of the partition-by column
		ULONG ulColId = CTranslatorUtils::UlColId((INT) psortcl->tleSortGroupRef, phmiulColColId);

		pdrgpul->Append(New(m_pmp) ULONG(ulColId));
	}

	return pdrgpul;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdrgpdxlnSortCol
//
//	@doc:
//		Translate the list of sorting columns
//
//---------------------------------------------------------------------------
DrgPdxln *
CTranslatorQueryToDXL::PdrgpdxlnSortCol
	(
	List *plSortCl,
	HMIUl *phmiulColColId
	)
	const
{
	DrgPdxln *pdrgpdxln = New(m_pmp) DrgPdxln(m_pmp);

	ListCell *plcSortCl = NULL;
	ForEach (plcSortCl, plSortCl)
	{
		Node *pnodeSortCl = (Node*) lfirst(plcSortCl);
		GPOS_ASSERT(NULL != pnodeSortCl);

		GPOS_ASSERT(IsA(pnodeSortCl, SortClause));

		SortClause *psortcl = (SortClause*) pnodeSortCl;

		// get the colid of the sorting column
		const ULONG ulColId = CTranslatorUtils::UlColId((INT) psortcl->tleSortGroupRef, phmiulColColId);

		OID oid = psortcl->sortop;

		// get operator name
		CMDIdGPDB *pmdidScOp = New(m_pmp) CMDIdGPDB(oid);
		const IMDScalarOp *pmdscop = m_pmda->Pmdscop(pmdidScOp);

		const CWStringConst *pstr = pmdscop->Mdname().Pstr();
		GPOS_ASSERT(NULL != pstr);

		CDXLScalarSortCol *pdxlop = New(m_pmp) CDXLScalarSortCol
												(
												m_pmp,
												ulColId,
												pmdidScOp,
												New(m_pmp) CWStringConst(pstr->Wsz()),
												false
												);

		// create the DXL node holding the sorting col
		CDXLNode *pdxlnSortCol = New(m_pmp) CDXLNode(m_pmp, pdxlop);

		pdrgpdxln->Append(pdxlnSortCol);
	}

	return pdrgpdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnLgLimit
//
//	@doc:
//		Translate the list of sorting columns, limit offset and limit count
//		into a CDXLLogicalGroupBy node
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnLgLimit
	(
	List *plSortCl,
	Node *pnodeLimitCount,
	Node *pnodeLimitOffset,
	CDXLNode *pdxlnChild,
	HMIUl *phmiulGrpColsColId
	)
	const
{
	if (0 == gpdb::UlListLength(plSortCl) && NULL == pnodeLimitCount && NULL == pnodeLimitOffset)
	{
		return pdxlnChild;
	}

	// create the logical limit operator
	CDXLNode *pdxlnLimit = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalLimit(m_pmp));

	// create a sorting col list
	CDXLNode *pdxlnSortColList = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarSortColList(m_pmp));

	DrgPdxln *pdrgpdxlnSortCol = PdrgpdxlnSortCol(plSortCl, phmiulGrpColsColId);
	const ULONG ulSize = pdrgpdxlnSortCol->UlLength();
	for (ULONG ul = 0; ul < ulSize; ul++)
	{
		CDXLNode *pdxlnSortCol = (*pdrgpdxlnSortCol)[ul];
		pdxlnSortCol->AddRef();
		pdxlnSortColList->AddChild(pdxlnSortCol);
	}
	pdrgpdxlnSortCol->Release();

	// create limit count
	CDXLNode *pdxlnLimitCount = New(m_pmp) CDXLNode
										(
										m_pmp,
										New(m_pmp) CDXLScalarLimitCount(m_pmp)
										);

	if (NULL != pnodeLimitCount)
	{
		pdxlnLimitCount->AddChild(PdxlnScFromGPDBExpr((Expr*) pnodeLimitCount));
	}

	// create limit offset
	CDXLNode *pdxlnLimitOffset = New(m_pmp) CDXLNode
										(
										m_pmp,
										New(m_pmp) CDXLScalarLimitOffset(m_pmp)
										);

	if (NULL != pnodeLimitOffset)
	{
		pdxlnLimitOffset->AddChild(PdxlnScFromGPDBExpr((Expr*) pnodeLimitOffset));
	}

	pdxlnLimit->AddChild(pdxlnSortColList);
	pdxlnLimit->AddChild(pdxlnLimitCount);
	pdxlnLimit->AddChild(pdxlnLimitOffset);
	pdxlnLimit->AddChild(pdxlnChild);

	return pdxlnLimit;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::AddSortingGroupingColumn
//
//	@doc:
//		Add sorting and grouping column into the hash map
//
//---------------------------------------------------------------------------
void
CTranslatorQueryToDXL::AddSortingGroupingColumn
	(
	TargetEntry *pte,
	HMIUl *phmiulSortgrouprefColId,
	ULONG ulColId
	)
	const
{
	if (0 < pte->ressortgroupref)
	{
		INT *piKey = New(m_pmp) INT(pte->ressortgroupref);
		ULONG *pulValue = New(m_pmp) ULONG(ulColId);

		// insert idx-colid mapping in the hash map
#ifdef GPOS_DEBUG
		BOOL fRes =
#endif // GPOS_DEBUG
				phmiulSortgrouprefColId->FInsert(piKey, pulValue);

		GPOS_ASSERT(fRes);
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnSimpleGroupBy
//
//	@doc:
//		Translate a query with grouping clause into a CDXLLogicalGroupBy node
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnSimpleGroupBy
	(
	List *plTargetList,
	List *plGroupClause,
	CBitSet *pbsGroupByCols,
	BOOL fHasAggs,
	BOOL fGroupingSets,
	CDXLNode *pdxlnChild,
	HMIUl *phmiulSortgrouprefColId,
	HMIUl *phmiulChild,
	HMIUl *phmiulOutputCols
	)
	const
{
	if (NULL == pbsGroupByCols && !fHasAggs)
	{ 
		if (!fGroupingSets)
		{
			// no group by needed and not part of a grouping sets query: 
			// propagate child columns to output columns
			HMIUlIter mi(phmiulChild);
			while (mi.FAdvance())
			{
	#ifdef GPOS_DEBUG
				BOOL fResult =
	#endif // GPOS_DEBUG
				phmiulOutputCols->FInsert(New(m_pmp) INT(*(mi.Pk())), New(m_pmp) ULONG(*(mi.Pt())));
				GPOS_ASSERT(fResult);
			}
		}
		// else:
		// in queries with grouping sets we may generate a branch corresponding to GB grouping sets ();
		// in that case do not propagate the child columns to the output hash map, as later
		// processing may introduce NULLs for those

		return pdxlnChild;
	}

	List *plDQA = NIL;
	// construct the project list of the group-by operator
	CDXLNode *pdxlnPrLGrpBy = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjList(m_pmp));

	ListCell *plcTE = NULL;
	ULONG ulDQAs = 0;
	ForEach (plcTE, plTargetList)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plcTE);
		GPOS_ASSERT(IsA(pte, TargetEntry));
		GPOS_ASSERT(0 < pte->resno);
		ULONG ulResNo = pte->resno;

		TargetEntry *pteEquivalent = CTranslatorUtils::PteGroupingColumn( (Node *) pte->expr, plGroupClause, plTargetList);

		BOOL fGroupingCol = pbsGroupByCols->FBit(pte->ressortgroupref) || (NULL != pteEquivalent && pbsGroupByCols->FBit(pteEquivalent->ressortgroupref));
		ULONG ulColId = 0;

		if (fGroupingCol)
		{
			// find colid for grouping column
			ulColId = CTranslatorUtils::UlColId(ulResNo, phmiulChild);
		}
		else if (IsA(pte->expr, Aggref) || IsA(pte->expr, PercentileExpr))
		{
			if (IsA(pte->expr, Aggref) && ((Aggref *) pte->expr)->aggdistinct && !FDuplicateDqaArg(plDQA, (Aggref *) pte->expr))
			{
				plDQA = gpdb::PlAppendElement(plDQA, gpdb::PvCopyObject(pte->expr));
				ulDQAs++;
			}

			// create a project element for aggregate
			CDXLNode *pdxlnPrEl = PdxlnPrEFromGPDBExpr(pte->expr, pte->resname);
			pdxlnPrLGrpBy->AddChild(pdxlnPrEl);
			ulColId = CDXLScalarProjElem::PdxlopConvert(pdxlnPrEl->Pdxlop())->UlId();
			AddSortingGroupingColumn(pte, phmiulSortgrouprefColId, ulColId);
		}

		if (fGroupingCol || IsA(pte->expr, Aggref) || IsA(pte->expr, PercentileExpr))
		{
			// add to the list of output columns
			StoreAttnoColIdMapping(phmiulOutputCols, ulResNo, ulColId);
		}
		else if (0 == pbsGroupByCols->CElements() && !fGroupingSets && !fHasAggs)
		{
			StoreAttnoColIdMapping(phmiulOutputCols, ulResNo, ulColId);
		}
	}

	// TODO: elhela - May 30, 2013; remove the following check when MDQAs are supported
	if (1 < ulDQAs)
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Multiple Distinct Qualified Aggregates"));
	}

	// initialize the array of grouping columns
	DrgPul *pdrgpul = CTranslatorUtils::PdrgpulGroupingCols(m_pmp, pbsGroupByCols, phmiulSortgrouprefColId);

	// clean up
	if (NIL != plDQA)
	{
		gpdb::FreeList(plDQA);
	}

	return New(m_pmp) CDXLNode
						(
						m_pmp,
						New(m_pmp) CDXLLogicalGroupBy(m_pmp, pdrgpul),
						pdxlnPrLGrpBy,
						pdxlnChild
						);
}


//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::FDuplicateDqaArg
//
//	@doc:
//		Check if the argument of a DQA has already being used by another DQA
//---------------------------------------------------------------------------
BOOL
CTranslatorQueryToDXL::FDuplicateDqaArg
	(
	List *plDQA,
	Aggref *paggref
	)
{
	GPOS_ASSERT(NULL != paggref);

	if (NIL == plDQA || 0 == gpdb::UlListLength(plDQA))
	{
		return false;
	}

	ListCell *plc = NULL;
	ForEach (plc, plDQA)
	{
		Node *pnode = (Node *) lfirst(plc);
		GPOS_ASSERT(IsA(pnode, Aggref));

		if (gpdb::FEqual(paggref->args, ((Aggref *) pnode)->args))
		{
			return true;
		}
	}

	return false;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnGroupingSets
//
//	@doc:
//		Translate a query with grouping sets
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnGroupingSets
	(
	FromExpr *pfromexpr,
	List *plTargetList,
	List *plGroupClause,
	BOOL fHasAggs,
	HMIUl *phmiulSortgrouprefColId,
	HMIUl *phmiulOutputCols
	)
{
	const ULONG ulCols = gpdb::UlListLength(plTargetList) + 1;

	if (NULL == plGroupClause)
	{
		HMIUl *phmiulChild = New(m_pmp) HMIUl(m_pmp);

		CDXLNode *pdxlnSPJ = PdxlnSPJ(plTargetList, pfromexpr, phmiulSortgrouprefColId, phmiulChild, plGroupClause);

		CBitSet *pbs = NULL;
		if (fHasAggs)
		{ 
			pbs = New(m_pmp) CBitSet(m_pmp); 
		}
		
		// in case of aggregates, construct a group by operator
		CDXLNode *pdxlnResult = PdxlnSimpleGroupBy
								(
								plTargetList,
								plGroupClause,
								pbs,
								fHasAggs,
								false, // fGroupingSets
								pdxlnSPJ,
								phmiulSortgrouprefColId,
								phmiulChild,
								phmiulOutputCols
								);

		// cleanup
		phmiulChild->Release();
		CRefCount::SafeRelease(pbs);
		return pdxlnResult;
	}

	// grouping functions refer to grouping col positions, so construct a map pos->grouping column
	// while processing the grouping clause
	HMUlUl *phmululGrpColPos = New(m_pmp) HMUlUl(m_pmp);
	CBitSet *pbsUniqueueGrpCols = New(m_pmp) CBitSet(m_pmp, ulCols);
	DrgPbs *pdrgpbs = CTranslatorUtils::PdrgpbsGroupBy(m_pmp, plGroupClause, ulCols, phmululGrpColPos, pbsUniqueueGrpCols);

	const ULONG ulGroupingSets = pdrgpbs->UlLength();

	if (1 == ulGroupingSets)
	{
		// simple group by
		HMIUl *phmiulChild = New(m_pmp) HMIUl(m_pmp);
		CDXLNode *pdxlnSPJ = PdxlnSPJ(plTargetList, pfromexpr, phmiulSortgrouprefColId, phmiulChild, plGroupClause);

		// translate the groupby clauses into a logical group by operator
		CBitSet *pbs = (*pdrgpbs)[0];


		CDXLNode *pdxlnGroupBy = PdxlnSimpleGroupBy
								(
								plTargetList,
								plGroupClause,
								pbs,
								fHasAggs,
								false, // fGroupingSets
								pdxlnSPJ,
								phmiulSortgrouprefColId,
								phmiulChild,
								phmiulOutputCols
								);
		
		CDXLNode *pdxlnResult = PdxlnProjectGroupingFuncs
									(
									plTargetList,
									pdxlnGroupBy,
									pbs,
									phmiulOutputCols,
									phmululGrpColPos,
									phmiulSortgrouprefColId
									);

		phmiulChild->Release();
		pdrgpbs->Release();
		pbsUniqueueGrpCols->Release();
		phmululGrpColPos->Release();
		
		return pdxlnResult;
	}
	
	CDXLNode *pdxlnResult = PdxlnUnionAllForGroupingSets
			(
			pfromexpr,
			plTargetList,
			plGroupClause,
			fHasAggs,
			pdrgpbs,
			phmiulSortgrouprefColId,
			phmiulOutputCols,
			phmululGrpColPos
			);

	pbsUniqueueGrpCols->Release();
	phmululGrpColPos->Release();
	
	return pdxlnResult;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnUnionAllForGroupingSets
//
//	@doc:
//		Construct a union all for the given grouping sets
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnUnionAllForGroupingSets
	(
	FromExpr *pfromexpr,
	List *plTargetList,
	List *plGroupClause,
	BOOL fHasAggs,
	DrgPbs *pdrgpbs,
	HMIUl *phmiulSortgrouprefColId,
	HMIUl *phmiulOutputCols,
	HMUlUl *phmululGrpColPos		// mapping pos->unique grouping columns for grouping func arguments
	)
{
	GPOS_ASSERT(NULL != pdrgpbs);
	GPOS_ASSERT(1 < pdrgpbs->UlLength());

	const ULONG ulGroupingSets = pdrgpbs->UlLength();
	CDXLNode *pdxlnUnionAll = NULL;
	DrgPul *pdrgpulColIdsInner = NULL;

	const ULONG ulCTEId = m_pidgtorCTE->UlNextId();
	
	// construct a CTE producer on top of the SPJ query
	HMIUl *phmiulSPJ = New(m_pmp) HMIUl(m_pmp);
	HMIUl *phmiulSortgrouprefColIdProducer = New(m_pmp) HMIUl(m_pmp);
	CDXLNode *pdxlnSPJ = PdxlnSPJForGroupingSets(plTargetList, pfromexpr, phmiulSortgrouprefColIdProducer, phmiulSPJ, plGroupClause);

	// construct output colids
	DrgPul *pdrgpulCTEProducer = PdrgpulExtractColIds(m_pmp, phmiulSPJ);

	GPOS_ASSERT (NULL != m_pdrgpdxlnCTE);
	
	CDXLLogicalCTEProducer *pdxlopCTEProducer = New(m_pmp) CDXLLogicalCTEProducer(m_pmp, ulCTEId, pdrgpulCTEProducer);
	CDXLNode *pdxlnCTEProducer = New(m_pmp) CDXLNode(m_pmp, pdxlopCTEProducer, pdxlnSPJ);
	m_pdrgpdxlnCTE->Append(pdxlnCTEProducer);
	
	CMappingVarColId *pmapvarcolidOriginal = m_pmapvarcolid->PmapvarcolidCopy(m_pmp);
	
	for (ULONG ul = 0; ul < ulGroupingSets; ul++)
	{
		CBitSet *pbsGroupingSet = (*pdrgpbs)[ul];

		// remap columns
		DrgPul *pdrgpulCTEConsumer = PdrgpulGenerateColIds(m_pmp, pdrgpulCTEProducer->UlLength());
		
		// reset col mapping with new consumer columns
		delete m_pmapvarcolid;
		m_pmapvarcolid = pmapvarcolidOriginal->PmapvarcolidRemap(m_pmp, pdrgpulCTEProducer, pdrgpulCTEConsumer);
		
		HMIUl *phmiulSPJConsumer = PhmiulRemapColIds(m_pmp, phmiulSPJ, pdrgpulCTEProducer, pdrgpulCTEConsumer);
		HMIUl *phmiulSortgrouprefColIdConsumer = PhmiulRemapColIds(m_pmp, phmiulSortgrouprefColIdProducer, pdrgpulCTEProducer, pdrgpulCTEConsumer);

		// construct a CTE consumer
		CDXLNode *pdxlnCTEConsumer = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalCTEConsumer(m_pmp, ulCTEId, pdrgpulCTEConsumer));

		HMIUl *phmiulGroupBy = New(m_pmp) HMIUl(m_pmp);
		CDXLNode *pdxlnGroupBy = PdxlnSimpleGroupBy
					(
					plTargetList,
					plGroupClause,
					pbsGroupingSet,
					fHasAggs,
					true, // fGroupingSets
					pdxlnCTEConsumer,
					phmiulSortgrouprefColIdConsumer,
					phmiulSPJConsumer,
					phmiulGroupBy
					);

		// add a project list for the NULL values
		CDXLNode *pdxlnProject = PdxlnProjectNullsForGroupingSets(plTargetList, pdxlnGroupBy, pbsGroupingSet, phmiulSortgrouprefColIdConsumer, phmiulGroupBy, phmululGrpColPos);

		DrgPul *pdrgpulColIdsOuter = CTranslatorUtils::PdrgpulColIds(m_pmp, plTargetList, phmiulGroupBy);
		if (NULL != pdxlnUnionAll)
		{
			GPOS_ASSERT(NULL != pdrgpulColIdsInner);
			DrgPdxlcd *pdrgpdxlcd = CTranslatorUtils::Pdrgpdxlcd(m_pmp, plTargetList, pdrgpulColIdsOuter, true /* fKeepResjunked */);

			pdrgpulColIdsOuter->AddRef();

			DrgPdrgPul *pdrgpdrgulInputColIds = New(m_pmp) DrgPdrgPul(m_pmp);
			pdrgpdrgulInputColIds->Append(pdrgpulColIdsOuter);
			pdrgpdrgulInputColIds->Append(pdrgpulColIdsInner);

			CDXLLogicalSetOp *pdxlopSetop = New(m_pmp) CDXLLogicalSetOp(m_pmp, EdxlsetopUnionAll, pdrgpdxlcd, pdrgpdrgulInputColIds, false);
			pdxlnUnionAll = New(m_pmp) CDXLNode(m_pmp, pdxlopSetop, pdxlnProject, pdxlnUnionAll);
		}
		else
		{
			pdxlnUnionAll = pdxlnProject;
		}

		pdrgpulColIdsInner = pdrgpulColIdsOuter;
		
		if (ul == ulGroupingSets - 1)
		{
			// add the sortgroup columns to output map of the last column
			ULONG ulTargetEntryPos = 0;
			ListCell *plcTE = NULL;
			ForEach (plcTE, plTargetList)
			{
				TargetEntry *pte = (TargetEntry *) lfirst(plcTE);

				INT iSortGrpRef = INT (pte->ressortgroupref);
				if (0 < iSortGrpRef && NULL != phmiulSortgrouprefColIdConsumer->PtLookup(&iSortGrpRef))
				{
					// add the mapping information for sorting columns
					AddSortingGroupingColumn(pte, phmiulSortgrouprefColId, *(*pdrgpulColIdsInner)[ulTargetEntryPos]);
				}

				ulTargetEntryPos++;
			}
		}

		// cleanup
		phmiulGroupBy->Release();
		phmiulSPJConsumer->Release();
		phmiulSortgrouprefColIdConsumer->Release();
	}

	// cleanup
	phmiulSPJ->Release();
	phmiulSortgrouprefColIdProducer->Release();
	delete pmapvarcolidOriginal;
	pdrgpulColIdsInner->Release();

	// compute output columns
	CDXLLogicalSetOp *pdxlopUnion = CDXLLogicalSetOp::PdxlopConvert(pdxlnUnionAll->Pdxlop());

	ListCell *plcTE = NULL;
	ULONG ulOutputColIndex = 0;
	ForEach (plcTE, plTargetList)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plcTE);
		GPOS_ASSERT(IsA(pte, TargetEntry));
		GPOS_ASSERT(0 < pte->resno);
		ULONG ulResNo = pte->resno;

		// note that all target list entries are kept in union all's output column
		// this is achieved by the fKeepResjunked flag in CTranslatorUtils::Pdrgpdxlcd
		const CDXLColDescr *pdxlcd = pdxlopUnion->Pdxlcd(ulOutputColIndex);
		const ULONG ulColId = pdxlcd->UlID();
		ulOutputColIndex++;

		if (!pte->resjunk)
		{
			// add non-resjunk columns to the hash map that maintains the output columns
			StoreAttnoColIdMapping(phmiulOutputCols, ulResNo, ulColId);
		}
	}

	// cleanup
	pdrgpbs->Release();

	// construct a CTE anchor operator on top of the union all
	return New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalCTEAnchor(m_pmp, ulCTEId), pdxlnUnionAll);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnConstTableGet
//
//	@doc:
//		Create a dummy constant table get (CTG) with a boolean true value
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnConstTableGet() const
{

	// construct the schema of the const table
	DrgPdxlcd *pdrgpdxlcd = New(m_pmp) DrgPdxlcd(m_pmp);

	const CMDTypeBoolGPDB *pmdtypeBool = dynamic_cast<const CMDTypeBoolGPDB *>(m_pmda->PtMDType<IMDTypeBool>(m_sysid));
	const CMDIdGPDB *pmdid = CMDIdGPDB::PmdidConvert(pmdtypeBool->Pmdid());

	// empty column name
	CWStringConst strUnnamedCol(GPOS_WSZ_LIT(""));
	CMDName *pmdname = New(m_pmp) CMDName(m_pmp, &strUnnamedCol);
	CDXLColDescr *pdxlcd = New(m_pmp) CDXLColDescr
										(
										m_pmp,
										pmdname,
										m_pidgtorCol->UlNextId(),
										1 /* iAttno */,
										New(m_pmp) CMDIdGPDB(pmdid->OidObjectId()),
										false /* fDropped */
										);
	pdrgpdxlcd->Append(pdxlcd);

	// create the array of datum arrays
	DrgPdrgPdxldatum *pdrgpdrgpdxldatum = New(m_pmp) DrgPdrgPdxldatum(m_pmp);
	
	// create a datum array
	DrgPdxldatum *pdrgpdxldatum = New(m_pmp) DrgPdxldatum(m_pmp);

	Const *pconst = (Const*) gpdb::PnodeMakeBoolConst(true /*value*/, false /*isnull*/);
	CDXLDatum *pdxldatum = m_psctranslator->Pdxldatum(pconst);
	gpdb::GPDBFree(pconst);

	pdrgpdxldatum->Append(pdxldatum);
	pdrgpdrgpdxldatum->Append(pdrgpdxldatum);

	CDXLLogicalConstTable *pdxlop = New(m_pmp) CDXLLogicalConstTable(m_pmp, pdrgpdxlcd, pdrgpdrgpdxldatum);

	return New(m_pmp) CDXLNode(m_pmp, pdxlop);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnFromSetOp
//
//	@doc:
//		Translate a set operation
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnFromSetOp
	(
	Node *pnodeSetOp,
	List *plTargetList,
	HMIUl *phmiulOutputCols
	)
	const
{
	GPOS_ASSERT(IsA(pnodeSetOp, SetOperationStmt));
	SetOperationStmt *psetopstmt = (SetOperationStmt*) pnodeSetOp;
	GPOS_ASSERT(SETOP_NONE != psetopstmt->op);

	EdxlSetOpType edxlsetop = CTranslatorUtils::Edxlsetop(psetopstmt->op, psetopstmt->all);

	// translate the left and right child
	DrgPul *pdrgpulLeft = New (m_pmp) DrgPul(m_pmp);
	DrgPul *pdrgpulRight = New (m_pmp) DrgPul(m_pmp);
	DrgPmdid *pdrgpmdidLeft = New (m_pmp) DrgPmdid(m_pmp);
	DrgPmdid *pdrgpmdidRight = New (m_pmp) DrgPmdid(m_pmp);

	CDXLNode *pdxlnLeftChild = PdxlnSetOpChild(psetopstmt->larg, pdrgpulLeft, pdrgpmdidLeft, plTargetList);
	CDXLNode *pdxlnRightChild = PdxlnSetOpChild(psetopstmt->rarg, pdrgpulRight, pdrgpmdidRight, plTargetList);

	DrgPdrgPul *pdrgpdrgulInputColIds = New(m_pmp) DrgPdrgPul(m_pmp);
	pdrgpdrgulInputColIds->Append(pdrgpulLeft);
	pdrgpdrgulInputColIds->Append(pdrgpulRight);
	
	DrgPul *pdrgpulOutput = NULL;

	BOOL fCastAcrossInput = FCast(plTargetList, pdrgpmdidLeft) || FCast(plTargetList, pdrgpmdidRight);
	
	if (fCastAcrossInput)
	{
		// set operation may generate new output column since we need to add casting functions
		pdrgpulOutput = CTranslatorUtils::PdrgpulGenerateColIds(m_pmp, plTargetList, pdrgpmdidLeft, pdrgpulLeft, m_pidgtorCol);
		GPOS_ASSERT(pdrgpulOutput->UlLength() == pdrgpulLeft->UlLength());
	}
	else
	{
		// the set operation outputs the column from the left child
		pdrgpulLeft->AddRef();
		pdrgpulOutput = pdrgpulLeft;
	}

	DrgPdxln *pdrgpdxlnChildren  = New(m_pmp) DrgPdxln(m_pmp);
	pdrgpdxlnChildren->Append(pdxlnLeftChild);
	pdrgpdxlnChildren->Append(pdxlnRightChild);

	CDXLNode *pdxln = PdxlnSetOp
						(
						edxlsetop,
						plTargetList,
						pdrgpulOutput,
						pdrgpdrgulInputColIds,
						pdrgpdxlnChildren,
						fCastAcrossInput,
						false /* fKeepResjunked */
						);

	CDXLLogicalSetOp *pdxlop = CDXLLogicalSetOp::PdxlopConvert(pdxln->Pdxlop());
	const DrgPdxlcd *pdrgpdxlcd = pdxlop->Pdrgpdxlcd();

	ULONG ulOutputColIndex = 0;
	ListCell *plcTE = NULL;
	ForEach (plcTE, plTargetList)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plcTE);
		GPOS_ASSERT(IsA(pte, TargetEntry));
		GPOS_ASSERT(0 < pte->resno);
		ULONG ulResNo = pte->resno;

		if (!pte->resjunk)
		{
			const CDXLColDescr *pdxlcdNew = (*pdrgpdxlcd)[ulOutputColIndex];
			ULONG ulColId = pdxlcdNew->UlID();
			StoreAttnoColIdMapping(phmiulOutputCols, ulResNo, ulColId);
			ulOutputColIndex++;
		}
	}

	// clean up
	pdrgpulOutput->Release();
	pdrgpmdidLeft->Release();
	pdrgpmdidRight->Release();

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlSetOp
//
//	@doc:
//		Create a set op after adding dummy cast on input columns where needed
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnSetOp
	(
	EdxlSetOpType edxlsetop,
	List *plTargetListOutput,
	DrgPul *pdrgpulOutput,
	DrgPdrgPul *pdrgpdrgulInputColIds,
	DrgPdxln *pdrgpdxlnChildren,
	BOOL fCastAcrossInput,
	BOOL fKeepResjunked
	)
	const
{
	GPOS_ASSERT(NULL != plTargetListOutput);
	GPOS_ASSERT(NULL != pdrgpulOutput);
	GPOS_ASSERT(NULL != pdrgpdrgulInputColIds);
	GPOS_ASSERT(NULL != pdrgpdxlnChildren);
	GPOS_ASSERT(1 < pdrgpdrgulInputColIds->UlLength());
	GPOS_ASSERT(1 < pdrgpdxlnChildren->UlLength());

	// positions of output columns in the target list
	DrgPul *pdrgpulTLPos = CTranslatorUtils::PdrgpulPosInTargetList(m_pmp, plTargetListOutput, fKeepResjunked);

	const ULONG ulCols = pdrgpulOutput->UlLength();
	DrgPul *pdrgpulInputFirstChild = (*pdrgpdrgulInputColIds)[0];
	GPOS_ASSERT(ulCols == pdrgpulInputFirstChild->UlLength());
	GPOS_ASSERT(ulCols == pdrgpulOutput->UlLength());

	CBitSet *pbs = New(m_pmp) CBitSet(m_pmp);

	// project list to maintain the casting of the duplicate input columns
	CDXLNode *pdxlnNewChildScPrL = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjList(m_pmp));

	DrgPul *pdrgpulInputFirstChildNew = New(m_pmp) DrgPul (m_pmp);
	DrgPdxlcd *pdrgpdxlcdOutput = New(m_pmp) DrgPdxlcd(m_pmp);
	for (ULONG ul = 0; ul < ulCols; ul++)
	{
		ULONG ulColIdOutput = *(*pdrgpulOutput)[ul];
		ULONG ulColIdInput = *(*pdrgpulInputFirstChild)[ul];

		BOOL fColExists = pbs->FBit(ulColIdInput);
		BOOL fCastedCol = (ulColIdOutput != ulColIdInput);

		ULONG ulTLPos = *(*pdrgpulTLPos)[ul];
		TargetEntry *pte = (TargetEntry*) gpdb::PvListNth(plTargetListOutput, ulTLPos);
		GPOS_ASSERT(NULL != pte);

		CDXLColDescr *pdxlcdOutput = NULL;
		if (!fColExists)
		{
			pbs->FExchangeSet(ulColIdInput);
			pdrgpulInputFirstChildNew->Append(New(m_pmp) ULONG(ulColIdInput));

			pdxlcdOutput = CTranslatorUtils::Pdxlcd(m_pmp, pte, ulColIdOutput, ul + 1);
		}
		else
		{
			// we add a dummy-cast to distinguish between the output columns of the union
			ULONG ulColIdNew = m_pidgtorCol->UlNextId();
			pdrgpulInputFirstChildNew->Append(New(m_pmp) ULONG(ulColIdNew));

			ULONG ulColIdUnionOutput = ulColIdNew;
			if (fCastedCol)
			{
				// create new output column id since current colid denotes its duplicate
				ulColIdUnionOutput = m_pidgtorCol->UlNextId();
			}

			pdxlcdOutput = CTranslatorUtils::Pdxlcd(m_pmp, pte, ulColIdUnionOutput, ul + 1);
			CDXLNode *pdxlnPrEl = CTranslatorUtils::PdxlnDummyPrElem(m_pmp, ulColIdInput, ulColIdNew, pdxlcdOutput);

			pdxlnNewChildScPrL->AddChild(pdxlnPrEl);
		}

		pdrgpdxlcdOutput->Append(pdxlcdOutput);
	}

	pdrgpdrgulInputColIds->Replace(0, pdrgpulInputFirstChildNew);

	if (0 < pdxlnNewChildScPrL->UlArity())
	{
		// create a project node for the dummy casted columns
		CDXLNode *pdxlnFirstChild = (*pdrgpdxlnChildren)[0];
		pdxlnFirstChild->AddRef();
		CDXLNode *pdxlnNewChild = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalProject(m_pmp));
		pdxlnNewChild->AddChild(pdxlnNewChildScPrL);
		pdxlnNewChild->AddChild(pdxlnFirstChild);

		pdrgpdxlnChildren->Replace(0, pdxlnNewChild);
	}
	else
	{
		pdxlnNewChildScPrL->Release();
	}

	CDXLLogicalSetOp *pdxlop = New(m_pmp) CDXLLogicalSetOp
											(
											m_pmp,
											edxlsetop,
											pdrgpdxlcdOutput,
											pdrgpdrgulInputColIds,
											fCastAcrossInput
											);
	CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlop, pdrgpdxlnChildren);

	pbs->Release();
	pdrgpulTLPos->Release();

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::FCast
//
//	@doc:
//		Check if the set operation need to cast any of its input columns
//
//---------------------------------------------------------------------------
BOOL
CTranslatorQueryToDXL::FCast
	(
	List *plTargetList,
	DrgPmdid *pdrgpmdid
	)
	const
{
	GPOS_ASSERT(NULL != pdrgpmdid);
	GPOS_ASSERT(NULL != plTargetList);
	GPOS_ASSERT(pdrgpmdid->UlLength() <= gpdb::UlListLength(plTargetList)); // there may be resjunked columns

	ULONG ulColPos = 0;
	ListCell *plcTE = NULL;
	ForEach (plcTE, plTargetList)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plcTE);
		OID oidExprType = gpdb::OidExprType((Node*) pte->expr);
		if (!pte->resjunk)
		{
			IMDId *pmdid = (*pdrgpmdid)[ulColPos];
			if (CMDIdGPDB::PmdidConvert(pmdid)->OidObjectId() != oidExprType)
			{
				return true;
			}
			ulColPos++;
		}
	}

	return false;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnSetOpChild
//
//	@doc:
//		Translate the child of a set operation
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnSetOpChild
	(
	Node *pnodeChild,
	DrgPul *pdrgpul,
	DrgPmdid *pdrgpmdid,
	List *plTargetList
	)
	const
{
	GPOS_ASSERT(NULL != pdrgpul);
	GPOS_ASSERT(NULL != pdrgpmdid);

	if (IsA(pnodeChild, RangeTblRef))
	{
		RangeTblRef *prtref = (RangeTblRef*) pnodeChild;
		const ULONG ulRTIndex = prtref->rtindex;
		const RangeTblEntry *prte = (RangeTblEntry *) gpdb::PvListNth(m_pquery->rtable, ulRTIndex - 1);

		if (RTE_SUBQUERY == prte->rtekind)
		{
			Query *pqueryDerTbl = CTranslatorUtils::PqueryFixUnknownTypeConstant(prte->subquery, plTargetList);
			GPOS_ASSERT(NULL != pqueryDerTbl);

			CMappingVarColId *pmapvarcolid = m_pmapvarcolid->PmapvarcolidCopy(m_pmp);
			CTranslatorQueryToDXL trquerytodxl(m_pmp, m_pmda, m_pidgtorCol, m_pidgtorCTE, pmapvarcolid, pqueryDerTbl, m_ulQueryLevel + 1, m_phmulCTEEntries);

			// translate query representing the derived table to its DXL representation
			CDXLNode *pdxln = trquerytodxl.PdxlnFromQueryInternal();
			GPOS_ASSERT(NULL != pdxln);

			DrgPdxln *pdrgpdxlnCTE = trquerytodxl.PdrgpdxlnCTE();
			CUtils::AddRefAppend(m_pdrgpdxlnCTE, pdrgpdxlnCTE);

			// get the output columns of the derived table
			DrgPdxln *pdrgpdxln = trquerytodxl.PdrgpdxlnQueryOutput();
			GPOS_ASSERT(pdrgpdxln != NULL);
			const ULONG ulLen = pdrgpdxln->UlLength();
			for (ULONG ul = 0; ul < ulLen; ul++)
			{
				CDXLNode *pdxlnCurr = (*pdrgpdxln)[ul];
				CDXLScalarIdent *pdxlnIdent = CDXLScalarIdent::PdxlopConvert(pdxlnCurr->Pdxlop());
				ULONG *pulColId = New(m_pmp) ULONG(pdxlnIdent->Pdxlcr()->UlID());
				pdrgpul->Append(pulColId);

				IMDId *pmdidCol = pdxlnIdent->PmdidType();
				GPOS_ASSERT(NULL != pmdidCol);
				pmdidCol->AddRef();
				pdrgpmdid->Append(pmdidCol);
			}

			return pdxln;
		}
	}
	else if (IsA(pnodeChild, SetOperationStmt))
	{
		HMIUl *phmiulOutputCols = New(m_pmp) HMIUl(m_pmp);
		CDXLNode *pdxln = PdxlnFromSetOp(pnodeChild, plTargetList, phmiulOutputCols);

		// cleanup
		phmiulOutputCols->Release();

		const DrgPdxlcd *pdrgpdxlcd = CDXLLogicalSetOp::PdxlopConvert(pdxln->Pdxlop())->Pdrgpdxlcd();
		GPOS_ASSERT(NULL != pdrgpdxlcd);
		const ULONG ulLen = pdrgpdxlcd->UlLength();
		for (ULONG ul = 0; ul < ulLen; ul++)
		{
			const CDXLColDescr *pdxlcd = (*pdrgpdxlcd)[ul];
			ULONG *pulColId = New(m_pmp) ULONG(pdxlcd->UlID());
			pdrgpul->Append(pulColId);

			IMDId *pmdidCol = pdxlcd->PmdidType();
			GPOS_ASSERT(NULL != pmdidCol);
			pmdidCol->AddRef();
			pdrgpmdid->Append(pmdidCol);
		}

		return pdxln;
	}

	CHAR *sz = (CHAR*) gpdb::SzNodeToString(const_cast<Node*>(pnodeChild));
	CWStringDynamic *pstr = CDXLUtils::PstrFromSz(m_pmp, sz);

	GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, pstr->Wsz());
	return NULL;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnFromGPDBFromExpr
//
//	@doc:
//		Translate the FromExpr on a GPDB query into either a CDXLLogicalJoin
//		or a CDXLLogicalGet
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnFromGPDBFromExpr
	(
	FromExpr *pfromexpr
	)
	const
{
	CDXLNode *pdxln = NULL;

	if (0 == gpdb::UlListLength(pfromexpr->fromlist))
	{
		pdxln = PdxlnConstTableGet();
	}
	else
	{
		if (1 == gpdb::UlListLength(pfromexpr->fromlist))
		{
			Node *pnode = (Node*) gpdb::PvListNth(pfromexpr->fromlist, 0);
			GPOS_ASSERT(NULL != pnode);
			pdxln = PdxlnFromGPDBFromClauseEntry(pnode);
		}
		else
		{
			// In DXL, we represent an n-ary join (where n>2) by an inner join with condition true.
			// The join conditions represented in the FromExpr->quals is translated
			// into a CDXLLogicalSelect on top of the CDXLLogicalJoin

			pdxln = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalJoin(m_pmp, EdxljtInner));

			ListCell *plc = NULL;
			ForEach (plc, pfromexpr->fromlist)
			{
				Node *pnode = (Node*) lfirst(plc);
				CDXLNode *pdxlnChild = PdxlnFromGPDBFromClauseEntry(pnode);
				pdxln->AddChild(pdxlnChild);
			}
		}
	}

	// translate the quals
	Node *pnodeQuals = pfromexpr->quals;
	CDXLNode *pdxlnCond = NULL;
	if (NULL != pnodeQuals)
	{
		pdxlnCond = PdxlnScFromGPDBExpr( (Expr*) pnodeQuals);
	}

	if (1 >= gpdb::UlListLength(pfromexpr->fromlist))
	{
		if (NULL != pdxlnCond)
		{
			CDXLNode *pdxlnSelect = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalSelect(m_pmp));
			pdxlnSelect->AddChild(pdxlnCond);
			pdxlnSelect->AddChild(pdxln);

			pdxln = pdxlnSelect;
		}
	}
	else //n-ary joins
	{
		if (NULL == pdxlnCond)
		{
			// A cross join (the scalar condition is true)
			pdxlnCond = PdxlnScConstValueTrue();
		}

		pdxln->AddChild(pdxlnCond);
	}

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnFromGPDBFromClauseEntry
//
//	@doc:
//		Returns a CDXLNode representing a from clause entry which can either be
//		(1) a fromlist entry in the FromExpr or (2) left/right child of a JoinExpr
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnFromGPDBFromClauseEntry
	(
	Node *pnode
	)
	const
{
	GPOS_ASSERT(NULL != pnode);

	if (IsA(pnode, RangeTblRef))
	{
		RangeTblRef *prtref = (RangeTblRef *) pnode;
		ULONG ulRTIndex = prtref->rtindex ;
		const RangeTblEntry *prte = (RangeTblEntry *) gpdb::PvListNth(m_pquery->rtable, ulRTIndex - 1);
		GPOS_ASSERT(NULL != prte);

		if (prte->forceDistRandom)
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("gp_dist_random"));
		}

		SRTETranslator rgTranslators[] =
		{
			{RTE_RELATION, &CTranslatorQueryToDXL::PdxlnFromRelation},
			{RTE_VALUES, &CTranslatorQueryToDXL::PdxlnFromValues},
			{RTE_CTE, &CTranslatorQueryToDXL::PdxlnFromCTE},
			{RTE_SUBQUERY, &CTranslatorQueryToDXL::PdxlnFromDerivedTable},
			{RTE_FUNCTION, &CTranslatorQueryToDXL::PdxlnFromTVF},
		};
		
		const ULONG ulTranslators = GPOS_ARRAY_SIZE(rgTranslators);
		
		// find translator for the rtekind
		PfPdxlnLogical pf = NULL;
		for (ULONG ul = 0; ul < ulTranslators; ul++)
		{
			SRTETranslator elem = rgTranslators[ul];
			if (prte->rtekind == elem.m_rtekind)
			{
				pf = elem.pf;
				break;
			}
		}
		
		if (NULL == pf)
		{
			UnsupportedRTEKind(prte->rtekind);
		}
		
		return (this->*pf)(prte, ulRTIndex, m_ulQueryLevel);
	}

	if (IsA(pnode, JoinExpr))
	{
		return PdxlnLgJoinFromGPDBJoinExpr((JoinExpr*) pnode);
	}

	CHAR *sz = (CHAR*) gpdb::SzNodeToString(const_cast<Node*>(pnode));
	CWStringDynamic *pstr = CDXLUtils::PstrFromSz(m_pmp, sz);

	GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, pstr->Wsz());

	return NULL;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::UnsupportedRTEKind
//
//	@doc:
//		Raise exception for unsupported RangeTblEntries of a particular kind
//---------------------------------------------------------------------------
void
CTranslatorQueryToDXL::UnsupportedRTEKind
	(
    RTEKind rtekind
	)
	const
{
	GPOS_ASSERT(!(RTE_RELATION == rtekind || RTE_CTE == rtekind
				|| RTE_FUNCTION == rtekind || RTE_SUBQUERY == rtekind
				|| RTE_VALUES == rtekind));

	SRTENameElem rgStrMap[] =
		{
		{RTE_JOIN, GPOS_WSZ_LIT("RangeTableEntry of type Join")},
		{RTE_SPECIAL, GPOS_WSZ_LIT("RangeTableEntry of type Special")},
		{RTE_VOID, GPOS_WSZ_LIT("RangeTableEntry of type Void")},
		{RTE_TABLEFUNCTION, GPOS_WSZ_LIT("RangeTableEntry of type Table Function")}
		};

	const ULONG ulLen = GPOS_ARRAY_SIZE(rgStrMap);
	for (ULONG ul = 0; ul < ulLen; ul++)
	{
		SRTENameElem mapelem = rgStrMap[ul];

		if (mapelem.m_rtekind == rtekind)
		{
			GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, mapelem.m_wsz);
		}
	}

	GPOS_ASSERT(!"Unrecognized RTE kind");
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnFromRelation
//
//	@doc:
//		Returns a CDXLNode representing a from relation range table entry
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnFromRelation
	(
	const RangeTblEntry *prte,
	ULONG ulRTIndex,
	ULONG //ulCurrQueryLevel 
	)
	const
{
	// construct table descriptor for the scan node from the range table entry
	CDXLTableDescr *pdxltabdesc = CTranslatorUtils::Pdxltabdesc(m_pmp, m_pmda, m_pidgtorCol, prte);

	CDXLLogicalGet *pdxlop = NULL;
	const IMDRelation *pmdrel = m_pmda->Pmdrel(pdxltabdesc->Pmdid());
	if (IMDRelation::ErelstorageExternal == pmdrel->Erelstorage())
	{
		pdxlop = New(m_pmp) CDXLLogicalExternalGet(m_pmp, pdxltabdesc);
	}
	else
	{
		pdxlop = New(m_pmp) CDXLLogicalGet(m_pmp, pdxltabdesc);
	}

	CDXLNode *pdxlnGet = New(m_pmp) CDXLNode(m_pmp, pdxlop);

	// make note of new columns from base relation
	m_pmapvarcolid->LoadTblColumns(m_ulQueryLevel, ulRTIndex, pdxltabdesc);

	return pdxlnGet;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnFromValues
//
//	@doc:
//		Returns a CDXLNode representing a range table entry of values
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnFromValues
	(
	const RangeTblEntry *prte,
	ULONG ulRTIndex,
	ULONG ulCurrQueryLevel
	)
	const
{
	List *plTuples = prte->values_lists;
	GPOS_ASSERT(NULL != plTuples);
	
	const ULONG ulValues = gpdb::UlListLength(plTuples);
	GPOS_ASSERT(0 < ulValues);

	// children of the UNION ALL
	DrgPdxln *pdrgpdxln = New(m_pmp) DrgPdxln(m_pmp);
	
	// array of input colid arrays
	DrgPdrgPul *pdrgpdrgulInputColIds = New(m_pmp) DrgPdrgPul(m_pmp);

	// array of column descriptor for the UNION ALL operator
	DrgPdxlcd *pdrgpdxlcd = New(m_pmp) DrgPdxlcd(m_pmp);
	
	// translate the tuples in the value scan
	ULONG ulTuplePos = 0;
	ListCell *plcTuple = NULL;
	GPOS_ASSERT(NULL != prte->eref);
	ForEach (plcTuple, plTuples)
	{
		List *plTuple = (List *) lfirst(plcTuple);
		GPOS_ASSERT(IsA(plTuple, List));

		// array of column colids  
		DrgPul *pdrgpulColIds = New(m_pmp) DrgPul(m_pmp);

		// array of project elements (for expression elements)
		DrgPdxln *pdrgpdxlnPrEl = New(m_pmp) DrgPdxln(m_pmp);
		
		// array of datum (for datum constant values)
		DrgPdxldatum *pdrgpdxldatum = New(m_pmp) DrgPdxldatum(m_pmp);
		
		// array of column descriptors for the CTG containing the datum array
		DrgPdxlcd *pdrgpdxlcdCTG = New(m_pmp) DrgPdxlcd(m_pmp);
		
		List *plColnames = prte->eref->colnames;
		GPOS_ASSERT(NULL != plColnames);
		GPOS_ASSERT(gpdb::UlListLength(plTuple) == gpdb::UlListLength(plColnames));
		
		// translate the columns
		ULONG ulColPos = 0;
		ListCell *plcColumn = NULL;
		ForEach (plcColumn, plTuple)
		{
			Expr *pexpr = (Expr *) lfirst(plcColumn);
			
			CHAR *szColName = (CHAR *) strVal(gpdb::PvListNth(plColnames, ulColPos));
			ULONG ulColId = ULONG_MAX;	
			if (IsA(pexpr, Const))
			{
				// extract the datum
				Const *pconst = (Const *) pexpr;
				CDXLDatum *pdxldatum = m_psctranslator->Pdxldatum(pconst);
				pdrgpdxldatum->Append(pdxldatum);
				
				ulColId = m_pidgtorCol->UlNextId();
				
				CWStringDynamic *pstrAlias = CDXLUtils::PstrFromSz(m_pmp, szColName);
				CMDName *pmdname = New(m_pmp) CMDName(m_pmp, pstrAlias);
				delete pstrAlias;
				
				CDXLColDescr *pdxlcd = New(m_pmp) CDXLColDescr
													(
													m_pmp,
													pmdname,
													ulColId,
													ulColPos + 1 /* iAttno */,
													New(m_pmp) CMDIdGPDB(pconst->consttype),
													false /* fDropped */
													);

				if (0 == ulTuplePos)
				{
					pdxlcd->AddRef();
					pdrgpdxlcd->Append(pdxlcd);
				}
				pdrgpdxlcdCTG->Append(pdxlcd);
			}
			else
			{
				// translate the scalar expression into a project element
				CDXLNode *pdxlnPrE = PdxlnPrEFromGPDBExpr(pexpr, szColName, true /* fInsistNewColIds */ );
				pdrgpdxlnPrEl->Append(pdxlnPrE);
				ulColId = CDXLScalarProjElem::PdxlopConvert(pdxlnPrE->Pdxlop())->UlId();
				
				if (0 == ulTuplePos)
				{
					CWStringDynamic *pstrAlias = CDXLUtils::PstrFromSz(m_pmp, szColName);
					CMDName *pmdname = New(m_pmp) CMDName(m_pmp, pstrAlias);
					delete pstrAlias;
					
					CDXLColDescr *pdxlcd = New(m_pmp) CDXLColDescr
														(
														m_pmp,
														pmdname,
														ulColId,
														ulColPos + 1 /* iAttno */,
														New(m_pmp) CMDIdGPDB(gpdb::OidExprType((Node*) pexpr)),
														false /* fDropped */
														);
					pdrgpdxlcd->Append(pdxlcd);
				}
			} 

			GPOS_ASSERT(ULONG_MAX != ulColId);

			pdrgpulColIds->Append(New(m_pmp) ULONG(ulColId));
			ulColPos++;
		}
		
		pdrgpdxln->Append(PdxlnFromColumnValues(pdrgpdxldatum, pdrgpdxlcdCTG, pdrgpdxlnPrEl));
		pdrgpdrgulInputColIds->Append(pdrgpulColIds);
		ulTuplePos++;
		
		// cleanup
		pdrgpdxldatum->Release();
		pdrgpdxlnPrEl->Release();
		pdrgpdxlcdCTG->Release();
	}
	
	GPOS_ASSERT(NULL != pdrgpdxlcd);

	if (1 < ulValues)
	{
		// create a UNION ALL operator
		CDXLLogicalSetOp *pdxlop = New(m_pmp) CDXLLogicalSetOp(m_pmp, EdxlsetopUnionAll, pdrgpdxlcd, pdrgpdrgulInputColIds, false);
		CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlop, pdrgpdxln);
		
		// make note of new columns from UNION ALL
		m_pmapvarcolid->LoadColumns(m_ulQueryLevel, ulRTIndex, pdxlop->Pdrgpdxlcd());
		
		return pdxln;
	}

	
	GPOS_ASSERT(1 == pdrgpdxln->UlLength());
			
	CDXLNode *pdxln = (*pdrgpdxln)[0];
	pdxln->AddRef();
		
	// make note of new columns
	m_pmapvarcolid->LoadColumns(m_ulQueryLevel, ulRTIndex, pdrgpdxlcd);	
	
	//cleanup
	pdrgpdxln->Release();
	pdrgpdrgulInputColIds->Release();
	pdrgpdxlcd->Release();
	
	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnFromColumnValues
//
//	@doc:
//		Generate a DXL node from column values, where each column value is 
//		either a datum or scalar expression represented as project element.
//		Each datum is associated with a column descriptors used by the CTG
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnFromColumnValues
	(
	DrgPdxldatum *pdrgpdxldatumCTG,
	DrgPdxlcd *pdrgpdxlcdCTG,
	DrgPdxln *pdrgpdxlnPrEl
	)
	const
{
	GPOS_ASSERT(NULL != pdrgpdxldatumCTG);
	GPOS_ASSERT(NULL != pdrgpdxlnPrEl);
	
	CDXLNode *pdxlnCTG = NULL;
	if (0 == pdrgpdxldatumCTG->UlLength())
	{
		// add a dummy CTG
		pdxlnCTG = PdxlnConstTableGet();
	}
	else 
	{
		// create the array of datum arrays
		DrgPdrgPdxldatum *pdrgpdrgpdxldatumCTG = New(m_pmp) DrgPdrgPdxldatum(m_pmp);
		
		pdrgpdxldatumCTG->AddRef();
		pdrgpdrgpdxldatumCTG->Append(pdrgpdxldatumCTG);
		
		pdrgpdxlcdCTG->AddRef();
		CDXLLogicalConstTable *pdxlop = New(m_pmp) CDXLLogicalConstTable(m_pmp, pdrgpdxlcdCTG, pdrgpdrgpdxldatumCTG);
		
		pdxlnCTG = New(m_pmp) CDXLNode(m_pmp, pdxlop);
	}

	if (0 == pdrgpdxlnPrEl->UlLength())
	{
		return pdxlnCTG;
	}

	// create a project node for the list of project elements
	pdrgpdxlnPrEl->AddRef();
	CDXLNode *pdxlnPrL = New(m_pmp) CDXLNode
										(
										m_pmp,
										New(m_pmp) CDXLScalarProjList(m_pmp),
										pdrgpdxlnPrEl
										);
	
	CDXLNode *pdxlnProject = New(m_pmp) CDXLNode
											(
											m_pmp,
											New(m_pmp) CDXLLogicalProject(m_pmp),
											pdxlnPrL,
											pdxlnCTG
											);

	return pdxlnProject;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnFromTVF
//
//	@doc:
//		Returns a CDXLNode representing a from relation range table entry
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnFromTVF
	(
	const RangeTblEntry *prte,
	ULONG ulRTIndex,
	ULONG //ulCurrQueryLevel
	)
	const
{
	GPOS_ASSERT(NULL != prte->funcexpr);

	// if this is a folded function expression, generate a project over a CTG
	if (!IsA(prte->funcexpr, FuncExpr))
	{
		CDXLNode *pdxlnCTG = PdxlnConstTableGet();

		CDXLNode *pdxlnPrL = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjList(m_pmp));

		CDXLNode *pdxlnPrEl =  PdxlnPrEFromGPDBExpr((Expr *) prte->funcexpr, prte->eref->aliasname, true /* fInsistNewColIds */);
		pdxlnPrL->AddChild(pdxlnPrEl);

		CDXLNode *pdxlnProject = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalProject(m_pmp));
		pdxlnProject->AddChild(pdxlnPrL);
		pdxlnProject->AddChild(pdxlnCTG);

		m_pmapvarcolid->LoadProjectElements(m_ulQueryLevel, ulRTIndex, pdxlnPrL);

		return pdxlnProject;
	}

	CDXLLogicalTVF *pdxlopTVF = CTranslatorUtils::Pdxltvf(m_pmp, m_pmda, m_pidgtorCol, prte);
	CDXLNode *pdxlnTVF = New(m_pmp) CDXLNode(m_pmp, pdxlopTVF);

	// make note of new columns from function
	m_pmapvarcolid->LoadColumns(m_ulQueryLevel, ulRTIndex, pdxlopTVF->Pdrgpdxlcd());

	FuncExpr *pfuncexpr = (FuncExpr *) prte->funcexpr;
	BOOL fSubqueryInArgs = false;

	ListCell *plc = NULL;
	ForEach (plc, pfuncexpr->args)
	{
		Node *pnodeArg = (Node *) lfirst(plc);
		fSubqueryInArgs = fSubqueryInArgs || CTranslatorUtils::FHasSubquery(pnodeArg);
		CDXLNode *pdxlnFuncExprArg =
				m_psctranslator->PdxlnScOpFromExpr((Expr *) pnodeArg, m_pmapvarcolid);
		GPOS_ASSERT(NULL != pdxlnFuncExprArg);
		pdxlnTVF->AddChild(pdxlnFuncExprArg);
	}

	CMDIdGPDB *pmdidFunc = New(m_pmp) CMDIdGPDB(pfuncexpr->funcid);
	const IMDFunction *pmdfunc = m_pmda->Pmdfunc(pmdidFunc);
	if (fSubqueryInArgs && IMDFunction::EfsVolatile == pmdfunc->EfsStability())
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Volatile functions with subqueries in arguments"));
	}
	pmdidFunc->Release();

	return pdxlnTVF;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnFromCTE
//
//	@doc:
//		Translate a common table expression into CDXLNode
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnFromCTE
	(
	const RangeTblEntry *prte,
	ULONG ulRTIndex,
	ULONG ulCurrQueryLevel
	) const
{
	const ULONG ulCteQueryLevel = ulCurrQueryLevel - prte->ctelevelsup;
	const CCTEListEntry *pctelistentry = m_phmulCTEEntries->PtLookup(&ulCteQueryLevel);
	if (NULL == pctelistentry)
	{
		// TODO: raghav, Sept 09 2013, remove temporary fix  (revert exception to assert) to avoid crash during algebrization
		GPOS_RAISE
			(
			gpdxl::ExmaDXL,
			gpdxl::ExmiQuery2DXLError,
			GPOS_WSZ_LIT("No CTE")
			);
	}

	const CDXLNode *pdxlnCTEProducer = pctelistentry->PdxlnCTEProducer(prte->ctename);
	const List *plCTEProducerTargetList = pctelistentry->PlCTEProducerTL(prte->ctename);
	
	GPOS_ASSERT(NULL != pdxlnCTEProducer && NULL != plCTEProducerTargetList);

	CDXLLogicalCTEProducer *pdxlopProducer = CDXLLogicalCTEProducer::PdxlopConvert(pdxlnCTEProducer->Pdxlop()); 
	ULONG ulCTEId = pdxlopProducer->UlId();
	DrgPul *pdrgpulCTEProducer = pdxlopProducer->PdrgpulColIds();
	
	// construct output column array
	DrgPul *pdrgpulCTEConsumer = PdrgpulGenerateColIds(m_pmp, pdrgpulCTEProducer->UlLength());
			
	// load the new columns from the CTE
	m_pmapvarcolid->LoadCTEColumns(ulCurrQueryLevel, ulRTIndex, pdrgpulCTEConsumer, const_cast<List *>(plCTEProducerTargetList));

	CDXLLogicalCTEConsumer *pdxlopCTEConsumer = New(m_pmp) CDXLLogicalCTEConsumer(m_pmp, ulCTEId, pdrgpulCTEConsumer);
	CDXLNode *pdxlnCTE = New(m_pmp) CDXLNode(m_pmp, pdxlopCTEConsumer);

	return pdxlnCTE;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnFromDerivedTable
//
//	@doc:
//		Translate a derived table into CDXLNode
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnFromDerivedTable
	(
	const RangeTblEntry *prte,
	ULONG ulRTIndex,
	ULONG ulCurrQueryLevel
	) const
{
	Query *pqueryDerTbl = prte->subquery;
	GPOS_ASSERT(NULL != pqueryDerTbl);

	CMappingVarColId *pmapvarcolid = m_pmapvarcolid->PmapvarcolidCopy(m_pmp);
	CTranslatorQueryToDXL trquerytodxl(m_pmp, m_pmda, m_pidgtorCol, m_pidgtorCTE, pmapvarcolid, pqueryDerTbl, m_ulQueryLevel + 1, m_phmulCTEEntries);

	// translate query representing the derived table to its DXL representation
	CDXLNode *pdxlnDerTbl = trquerytodxl.PdxlnFromQueryInternal();

	// get the output columns of the derived table
	DrgPdxln *pdrgpdxlnQueryOutputDerTbl = trquerytodxl.PdrgpdxlnQueryOutput();
	DrgPdxln *pdrgpdxlnCTE = trquerytodxl.PdrgpdxlnCTE();
	GPOS_ASSERT(NULL != pdxlnDerTbl && pdrgpdxlnQueryOutputDerTbl != NULL);

	CUtils::AddRefAppend(m_pdrgpdxlnCTE, pdrgpdxlnCTE);
	
	// make note of new columns from derived table
	m_pmapvarcolid->LoadDerivedTblColumns(ulCurrQueryLevel, ulRTIndex, pdrgpdxlnQueryOutputDerTbl, trquerytodxl.Pquery()->targetList);

	return pdxlnDerTbl;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnScFromGPDBExpr
//
//	@doc:
//		Translate the Expr into a CDXLScalar node
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnScFromGPDBExpr
	(
	Expr *pexpr
	)
	const
{
	CDXLNode *pdxlnScalar = m_psctranslator->PdxlnScOpFromExpr(pexpr, m_pmapvarcolid);
	GPOS_ASSERT(NULL != pdxlnScalar);

	return pdxlnScalar;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnLgJoinFromGPDBJoinExpr
//
//	@doc:
//		Translate the JoinExpr on a GPDB query into a CDXLLogicalJoin node
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnLgJoinFromGPDBJoinExpr
	(
	JoinExpr *pjoinexpr
	) const
{
	GPOS_ASSERT(NULL != pjoinexpr);

	CDXLNode *pdxlnLeftChild = PdxlnFromGPDBFromClauseEntry(pjoinexpr->larg);
	CDXLNode *pdxlnRightChild = PdxlnFromGPDBFromClauseEntry(pjoinexpr->rarg);
	EdxlJoinType edxljt = CTranslatorUtils::EdxljtFromJoinType(pjoinexpr->jointype);
	CDXLNode *pdxlnJoin = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalJoin(m_pmp, edxljt));

	GPOS_ASSERT(NULL != pdxlnLeftChild && NULL != pdxlnRightChild);

	pdxlnJoin->AddChild(pdxlnLeftChild);
	pdxlnJoin->AddChild(pdxlnRightChild);

	Node* pnode = pjoinexpr->quals;

	// translate the join condition
	if (NULL != pnode)
	{
		pdxlnJoin->AddChild(PdxlnScFromGPDBExpr( (Expr*) pnode));
	}
	else
	{
		// a cross join therefore add a CDXLScalarConstValue representing the value "true"
		pdxlnJoin->AddChild(PdxlnScConstValueTrue());
	}

	// extract the range table entry for the join expr to:
	// 1. Process the alias names of the columns
	// 2. Generate a project list for the join expr and maintain it in our hash map

	const ULONG ulRtIndex = pjoinexpr->rtindex;
	RangeTblEntry *prte = (RangeTblEntry *) gpdb::PvListNth(m_pquery->rtable, ulRtIndex - 1);
	GPOS_ASSERT(NULL != prte);

	Alias *palias = prte->eref;
	GPOS_ASSERT(NULL != palias);
	GPOS_ASSERT(NULL != palias->colnames && 0 < gpdb::UlListLength(palias->colnames));
	GPOS_ASSERT(gpdb::UlListLength(prte->joinaliasvars) == gpdb::UlListLength(palias->colnames));

	CDXLNode *pdxlnPrLComputedColumns = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjList(m_pmp));
	CDXLNode *pdxlnPrL = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjList(m_pmp));

	// construct a proj element node for each entry in the joinaliasvars
	ListCell *plcNode = NULL;
	ListCell *plcColName = NULL;
	ForBoth (plcNode, prte->joinaliasvars,
			plcColName, palias->colnames)
	{
		Node *pnodeJoinAlias = (Node *) lfirst(plcNode);
		GPOS_ASSERT(IsA(pnodeJoinAlias, Var) || IsA(pnodeJoinAlias, CoalesceExpr));
		Value *pvalue = (Value *) lfirst(plcColName);
		CHAR *szColName = strVal(pvalue);

		// create the DXL node holding the target list entry and add it to proj list
		CDXLNode *pdxlnPrEl =  PdxlnPrEFromGPDBExpr( (Expr*) pnodeJoinAlias, szColName);
		pdxlnPrL->AddChild(pdxlnPrEl);

		if (IsA(pnodeJoinAlias, CoalesceExpr))
		{
			// add coalesce expression to the computed columns
			pdxlnPrEl->AddRef();
			pdxlnPrLComputedColumns->AddChild(pdxlnPrEl);
		}
	}
	m_pmapvarcolid->LoadProjectElements(m_ulQueryLevel, ulRtIndex, pdxlnPrL);
	pdxlnPrL->Release();

	if (0 == pdxlnPrLComputedColumns->UlArity())
	{
		pdxlnPrLComputedColumns->Release();
		return pdxlnJoin;
	}

	CDXLNode *pdxlnProject = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalProject(m_pmp));
	pdxlnProject->AddChild(pdxlnPrLComputedColumns);
	pdxlnProject->AddChild(pdxlnJoin);

	return pdxlnProject;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnLgProjectFromGPDBTL
//
//	@doc:
//		Create a DXL project list from the target list. The function allocates
//		memory in the translator memory pool and caller responsible for freeing it.
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnLgProjectFromGPDBTL
	(
	List *plTargetList,
	CDXLNode *pdxlnChild,
	HMIUl *phmiulSortgrouprefColId,
	HMIUl *phmiulOutputCols,
	List *plgrpcl,
	BOOL fExpandAggrefExpr
	)
	const
{
	BOOL fGroupBy = (0 != gpdb::UlListLength(m_pquery->groupClause) || m_pquery->hasAggs);

	CDXLNode *pdxlnPrL = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjList(m_pmp));

	// construct a proj element node for each entry in the target list
	ListCell *plcTE = NULL;
	
	// target entries that are result of flattening join alias
	// and are equivalent to a defined grouping column target entry
	List *plOmittedTE = NIL; 

	// list for all vars used in aggref expressions
	List *plVars = NULL;
	ULONG ulResno = 0;
	ForEach(plcTE, plTargetList)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plcTE);
		GPOS_ASSERT(IsA(pte, TargetEntry));
		GPOS_ASSERT(0 < pte->resno);
		ulResno = pte->resno;

		BOOL fGroupingCol = CTranslatorUtils::FGroupingColumn(pte, plgrpcl);
		if (!fGroupBy || (fGroupBy && fGroupingCol))
		{
			CDXLNode *pdxlnPrEl =  PdxlnPrEFromGPDBExpr(pte->expr, pte->resname);
			ULONG ulColId = CDXLScalarProjElem::PdxlopConvert(pdxlnPrEl->Pdxlop())->UlId();

			AddSortingGroupingColumn(pte, phmiulSortgrouprefColId, ulColId);

			// add column to the list of output columns of the query
			StoreAttnoColIdMapping(phmiulOutputCols, ulResno, ulColId);

			if (!IsA(pte->expr, Var))
			{
				// only add computed columns to the project list
				pdxlnPrL->AddChild(pdxlnPrEl);
			}
			else
			{
				pdxlnPrEl->Release();
			}
		}
		else if (fExpandAggrefExpr && IsA(pte->expr, Aggref))
		{
			plVars = gpdb::PlConcat(plVars, gpdb::PlExtractNodesExpression((Node *) pte->expr, T_Var, false /*descendIntoSubqueries*/));
		}
		else if (!IsA(pte->expr, Aggref))
		{
			plOmittedTE = gpdb::PlAppendElement(plOmittedTE, pte);
		}
	}

	// process target entries that are a result of flattening join alias
	plcTE = NULL;
	ForEach(plcTE, plOmittedTE)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plcTE);
		INT iSortGroupRef = (INT) pte->ressortgroupref;

		TargetEntry *pteGroupingColumn = CTranslatorUtils::PteGroupingColumn( (Node*) pte->expr, plgrpcl, plTargetList);
		if (NULL != pteGroupingColumn)
		{
			const ULONG ulColId = CTranslatorUtils::UlColId((INT) pteGroupingColumn->ressortgroupref, phmiulSortgrouprefColId);
			StoreAttnoColIdMapping(phmiulOutputCols, pte->resno, ulColId);
			if (0 < iSortGroupRef && 0 < ulColId && NULL == phmiulSortgrouprefColId->PtLookup(&iSortGroupRef))
			{
				AddSortingGroupingColumn(pte, phmiulSortgrouprefColId, ulColId);
			}
		}
	}
	if (NIL != plOmittedTE)
	{
		gpdb::GPDBFree(plOmittedTE);
	}

	GPOS_ASSERT_IMP(!fExpandAggrefExpr, NULL == plVars);
	
	// process all additional vars in aggref expressions
	ListCell *plcVar = NULL;
	ForEach (plcVar, plVars)
	{
		ulResno++;
		Var *pvar = (Var *) lfirst(plcVar);

		// TODO: antova - Dec 28, 2012; figure out column's name
		CDXLNode *pdxlnPrEl =  PdxlnPrEFromGPDBExpr((Expr*) pvar, "?col?");
		
		ULONG ulColId = CDXLScalarProjElem::PdxlopConvert(pdxlnPrEl->Pdxlop())->UlId();

		// add column to the list of output columns of the query
		StoreAttnoColIdMapping(phmiulOutputCols, ulResno, ulColId);
		
		pdxlnPrEl->Release();
	}
	
	if (0 < pdxlnPrL->UlArity())
	{
		// create a node with the CDXLLogicalProject operator and add as its children:
		// the CDXLProjectList node and the node representing the input to the project node
		CDXLNode *pdxlnProject = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalProject(m_pmp));
		pdxlnProject->AddChild(pdxlnPrL);
		pdxlnProject->AddChild(pdxlnChild);
		GPOS_ASSERT(NULL != pdxlnProject);
		return pdxlnProject;
	}

	pdxlnPrL->Release();
	return pdxlnChild;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnProjectNullsForGroupingSets
//
//	@doc:
//		Construct a DXL project node projecting NULL values for the columns in the
//		given bitset
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnProjectNullsForGroupingSets
	(
	List *plTargetList,
	CDXLNode *pdxlnChild,
	CBitSet *pbs,					// group by columns
	HMIUl *phmiulSortgrouprefCols,	// mapping of sorting and grouping columns
	HMIUl *phmiulOutputCols,		// mapping of output columns
	HMUlUl *phmululGrpColPos		// mapping of unique grouping col positions
	)
	const
{
	CDXLNode *pdxlnPrL = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjList(m_pmp));

	// construct a proj element node for those non-aggregate entries in the target list which
	// are not included in the grouping set
	ListCell *plcTE = NULL;
	ForEach (plcTE, plTargetList)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plcTE);
		GPOS_ASSERT(IsA(pte, TargetEntry));

		BOOL fGroupingCol = pbs->FBit(pte->ressortgroupref);
		ULONG ulResno = pte->resno;

		ULONG ulColId = 0;
		
		if (IsA(pte->expr, GroupingFunc))
		{
			ulColId = m_pidgtorCol->UlNextId();
			CDXLNode *pdxlnGroupingFunc = PdxlnGroupingFunc(pte->expr, pbs, phmululGrpColPos);
			CMDName *pmdnameAlias = NULL;

			if (NULL == pte->resname)
			{
				CWStringConst strUnnamedCol(GPOS_WSZ_LIT("grouping"));
				pmdnameAlias = New(m_pmp) CMDName(m_pmp, &strUnnamedCol);
			}
			else
			{
				CWStringDynamic *pstrAlias = CDXLUtils::PstrFromSz(m_pmp, pte->resname);
				pmdnameAlias = New(m_pmp) CMDName(m_pmp, pstrAlias);
				delete pstrAlias;
			}
			CDXLNode *pdxlnPrEl = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjElem(m_pmp, ulColId, pmdnameAlias), pdxlnGroupingFunc);
			pdxlnPrL->AddChild(pdxlnPrEl);
			StoreAttnoColIdMapping(phmiulOutputCols, ulResno, ulColId);
		}
		else if (!fGroupingCol && !IsA(pte->expr, Aggref))
		{
			OID oidType = gpdb::OidExprType((Node *) pte->expr);

			ulColId = m_pidgtorCol->UlNextId();
			CMDIdGPDB *pmdid = New(m_pmp) CMDIdGPDB(oidType);

			CDXLNode *pdxlnPrEl = CTranslatorUtils::PdxlnPrElNull(m_pmp, m_pmda, pmdid, ulColId, pte->resname);
			
			pdxlnPrL->AddChild(pdxlnPrEl);
			StoreAttnoColIdMapping(phmiulOutputCols, ulResno, ulColId);
		}
		
		INT iSortGroupRef = INT (pte->ressortgroupref);
		
		GPOS_ASSERT_IMP(0 == phmiulSortgrouprefCols, NULL != phmiulSortgrouprefCols->PtLookup(&iSortGroupRef) && "Grouping column with no mapping");
		
		if (0 < iSortGroupRef && 0 < ulColId && NULL == phmiulSortgrouprefCols->PtLookup(&iSortGroupRef))
		{
			AddSortingGroupingColumn(pte, phmiulSortgrouprefCols, ulColId);
		}
	}

	if (0 == pdxlnPrL->UlArity())
	{
		// no project necessary
		pdxlnPrL->Release();
		return pdxlnChild;
	}

	return New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalProject(m_pmp), pdxlnPrL, pdxlnChild);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnProjectGroupingFuncs
//
//	@doc:
//		Construct a DXL project node projecting values for the grouping funcs in
//		the target list 
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnProjectGroupingFuncs
	(
	List *plTargetList,
	CDXLNode *pdxlnChild,
	CBitSet *pbs,
	HMIUl *phmiulOutputCols,
	HMUlUl *phmululGrpColPos,
	HMIUl *phmiulSortgrouprefColId
	)
	const
{
	CDXLNode *pdxlnPrL = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjList(m_pmp));

	// construct a proj element node for those non-aggregate entries in the target list which
	// are not included in the grouping set
	ListCell *plcTE = NULL;
	ForEach (plcTE, plTargetList)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plcTE);
		GPOS_ASSERT(IsA(pte, TargetEntry));

		BOOL fGroupingCol = pbs->FBit(pte->ressortgroupref);
		ULONG ulResno = pte->resno;

		if (IsA(pte->expr, GroupingFunc))
		{
			ULONG ulColId = m_pidgtorCol->UlNextId();
			CDXLNode *pdxlnGroupingFunc = PdxlnGroupingFunc(pte->expr, pbs, phmululGrpColPos);
			CMDName *pmdnameAlias = NULL;

			if (NULL == pte->resname)
			{
				CWStringConst strUnnamedCol(GPOS_WSZ_LIT("grouping"));
				pmdnameAlias = New(m_pmp) CMDName(m_pmp, &strUnnamedCol);
			}
			else
			{
				CWStringDynamic *pstrAlias = CDXLUtils::PstrFromSz(m_pmp, pte->resname);
				pmdnameAlias = New(m_pmp) CMDName(m_pmp, pstrAlias);
				delete pstrAlias;
			}
			CDXLNode *pdxlnPrEl = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjElem(m_pmp, ulColId, pmdnameAlias), pdxlnGroupingFunc);
			pdxlnPrL->AddChild(pdxlnPrEl);
			StoreAttnoColIdMapping(phmiulOutputCols, ulResno, ulColId);
			AddSortingGroupingColumn(pte, phmiulSortgrouprefColId, ulColId);
		}
	}

	if (0 == pdxlnPrL->UlArity())
	{
		// no project necessary
		pdxlnPrL->Release();
		return pdxlnChild;
	}

	return New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalProject(m_pmp), pdxlnPrL, pdxlnChild);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::StoreAttnoColIdMapping
//
//	@doc:
//		Store mapping between attno and generate colid
//
//---------------------------------------------------------------------------
void
CTranslatorQueryToDXL::StoreAttnoColIdMapping
	(
	HMIUl *phmiul,
	INT iAttno,
	ULONG ulColId
	)
	const
{
	GPOS_ASSERT(NULL != phmiul);

#ifdef GPOS_DEBUG
	BOOL fResult =
#endif // GPOS_DEBUG
	phmiul->FInsert(New(m_pmp) INT(iAttno), New(m_pmp) ULONG(ulColId));

	GPOS_ASSERT(fResult);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdrgpdxlnConstructOutputCols
//
//	@doc:
//		Construct an array of DXL nodes representing the query output
//
//---------------------------------------------------------------------------
DrgPdxln *
CTranslatorQueryToDXL::PdrgpdxlnConstructOutputCols
	(
	List *plTargetList,
	HMIUl *phmiulAttnoColId
	)
	const
{
	GPOS_ASSERT(NULL != plTargetList);
	GPOS_ASSERT(NULL != phmiulAttnoColId);

	DrgPdxln *pdrgpdxln = New(m_pmp) DrgPdxln(m_pmp);

	ListCell *plc = NULL;
	ForEach (plc, plTargetList)
	{
		TargetEntry *pte = (TargetEntry *) lfirst(plc);
		GPOS_ASSERT(0 < pte->resno);
		ULONG ulResNo = pte->resno;

		if (pte->resjunk)
		{
			continue;
		}

		GPOS_ASSERT(NULL != pte);
		CMDName *pmdname = NULL;
		if (NULL == pte->resname)
		{
			CWStringConst strUnnamedCol(GPOS_WSZ_LIT("?column?"));
			pmdname = New(m_pmp) CMDName(m_pmp, &strUnnamedCol);
		}
		else
		{
			CWStringDynamic *pstrAlias = CDXLUtils::PstrFromSz(m_pmp, pte->resname);
			pmdname = New(m_pmp) CMDName(m_pmp, pstrAlias);
			// CName constructor copies string
			delete pstrAlias;
		}

		const ULONG ulColId = CTranslatorUtils::UlColId(ulResNo, phmiulAttnoColId);

		// create a column reference
		CDXLColRef *pdxlcr = New(m_pmp) CDXLColRef(m_pmp, pmdname, ulColId);
		CDXLScalarIdent *pdxlopIdent = New(m_pmp) CDXLScalarIdent
												(
												m_pmp,
												pdxlcr,
												New(m_pmp) CMDIdGPDB(gpdb::OidExprType( (Node*) pte->expr))
												);

		// create the DXL node holding the scalar ident operator
		CDXLNode *pdxln = New(m_pmp) CDXLNode(m_pmp, pdxlopIdent);

		pdrgpdxln->Append(pdxln);
	}

	return pdrgpdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnPrEFromGPDBExpr
//
//	@doc:
//		Create a DXL project element node from the target list entry or var.
//		The function allocates memory in the translator memory pool, and the caller
//		is responsible for freeing it.
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnPrEFromGPDBExpr
	(
	Expr *pexpr,
	CHAR *szAliasName,
	BOOL fInsistNewColIds
	) 
	const
{
	GPOS_ASSERT(NULL != pexpr);

	// construct a scalar operator
	CDXLNode *pdxlnChild = PdxlnScFromGPDBExpr(pexpr);

	// get the id and alias for the proj elem
	ULONG ulPrElId;
	CMDName *pmdnameAlias = NULL;

	if (NULL == szAliasName)
	{
		CWStringConst strUnnamedCol(GPOS_WSZ_LIT("?column?"));
		pmdnameAlias = New(m_pmp) CMDName(m_pmp, &strUnnamedCol);
	}
	else
	{
		CWStringDynamic *pstrAlias = CDXLUtils::PstrFromSz(m_pmp, szAliasName);
		pmdnameAlias = New(m_pmp) CMDName(m_pmp, pstrAlias);
		delete pstrAlias;
	}

	if (IsA(pexpr, Var) && !fInsistNewColIds)
	{
		// project elem is a a reference to a column - use the colref id
		GPOS_ASSERT(EdxlopScalarIdent == pdxlnChild->Pdxlop()->Edxlop());
		CDXLScalarIdent *pdxlopIdent = (CDXLScalarIdent *) pdxlnChild->Pdxlop();
		ulPrElId = pdxlopIdent->Pdxlcr()->UlID();
	}
	else
	{
		// project elem is a defined column - get a new id
		ulPrElId = m_pidgtorCol->UlNextId();
	}

	CDXLNode *pdxlnPrEl = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLScalarProjElem(m_pmp, ulPrElId, pmdnameAlias));
	pdxlnPrEl->AddChild(pdxlnChild);

	return pdxlnPrEl;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnScConstValueTrue
//
//	@doc:
//		Returns a CDXLNode representing scalar condition "true"
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnScConstValueTrue() const
{
	Const *pconst = (Const*) gpdb::PnodeMakeBoolConst(true /*value*/, false /*isnull*/);
	CDXLNode *pdxln = PdxlnScFromGPDBExpr((Expr*) pconst);
	gpdb::GPDBFree(pconst);

	return pdxln;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdxlnGroupingFunc
//
//	@doc:
//		Translate grouping func
//
//---------------------------------------------------------------------------
CDXLNode *
CTranslatorQueryToDXL::PdxlnGroupingFunc
	(
	const Expr *pexpr,
	CBitSet *pbs,
	HMUlUl *phmululGrpColPos
	)
	const
{
	GPOS_ASSERT(IsA(pexpr, GroupingFunc));
	GPOS_ASSERT(NULL != phmululGrpColPos);

	const GroupingFunc *pgroupingfunc = (GroupingFunc *) pexpr;

	if (1 < gpdb::UlListLength(pgroupingfunc->args))
	{
		GPOS_RAISE(gpdxl::ExmaDXL, gpdxl::ExmiQuery2DXLUnsupportedFeature, GPOS_WSZ_LIT("Grouping function with multiple arguments"));
	}
	
	Node *pnode = (Node *) gpdb::PvListNth(pgroupingfunc->args, 0);
	ULONG ulGroupingIndex = gpdb::IValue(pnode); 
		
	// generate a constant value for the result of the grouping function as follows:
	// if the grouping function argument is a group-by column, result is 0
	// otherwise, the result is 1 
	LINT lValue = 0;
	
	ULONG *pulSortGrpRef = phmululGrpColPos->PtLookup(&ulGroupingIndex);
	GPOS_ASSERT(NULL != pulSortGrpRef);
	BOOL fGroupingCol = pbs->FBit(*pulSortGrpRef);	
	if (!fGroupingCol)
	{
		// not a grouping column
		lValue = 1; 
	}

	const IMDType *pmdtype = m_pmda->PtMDType<IMDTypeInt8>(m_sysid);
	CMDIdGPDB *pmdidMDC = CMDIdGPDB::PmdidConvert(pmdtype->Pmdid());
	CMDIdGPDB *pmdid = New(m_pmp) CMDIdGPDB(*pmdidMDC);
	
	CDXLDatum *pdxldatum = New(m_pmp) CDXLDatumInt8(m_pmp, pmdid, false /* fNull */, lValue);
	CDXLScalarConstValue *pdxlop = New(m_pmp) CDXLScalarConstValue(m_pmp, pdxldatum);
	return New(m_pmp) CDXLNode(m_pmp, pdxlop);
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::ConstructCTEProducerList
//
//	@doc:
//		Construct a list of CTE producers from the query's CTE list
//
//---------------------------------------------------------------------------
void
CTranslatorQueryToDXL::ConstructCTEProducerList
	(
	List *plCTE,
	ULONG ulCteQueryLevel
	)
{
	GPOS_ASSERT(NULL != m_pdrgpdxlnCTE && "CTE Producer list not initialized"); 
	
	if (NULL == plCTE)
	{
		return;
	}
	
	ListCell *plc = NULL;
	
	ForEach (plc, plCTE)
	{
		CommonTableExpr *pcte = (CommonTableExpr *) lfirst(plc);
		GPOS_ASSERT(IsA(pcte->ctequery, Query));
		
		Query *pqueryCte = CQueryMutators::PqueryNormalize(m_pmp, m_pmda, (Query *) pcte->ctequery, ulCteQueryLevel + 1);
		
		// the query representing the cte can only access variables defined in the current level as well as
		// those defined at prior query levels
		CMappingVarColId *pmapvarcolid = m_pmapvarcolid->PmapvarcolidCopy(ulCteQueryLevel);

		CTranslatorQueryToDXL trquerytodxl(m_pmp, m_pmda, m_pidgtorCol, m_pidgtorCTE, pmapvarcolid, pqueryCte, ulCteQueryLevel + 1, m_phmulCTEEntries);

		// translate query representing the cte table to its DXL representation
		CDXLNode *pdxlnCteChild = trquerytodxl.PdxlnFromQueryInternal();

		// get the output columns of the cte table
		DrgPdxln *pdrgpdxlnQueryOutputCte = trquerytodxl.PdrgpdxlnQueryOutput();
		DrgPdxln *pdrgpdxlnCTE = trquerytodxl.PdrgpdxlnCTE();
		
		GPOS_ASSERT(NULL != pdxlnCteChild && NULL != pdrgpdxlnQueryOutputCte && NULL != pdrgpdxlnCTE);
		
		// append any nested CTE
		CUtils::AddRefAppend(m_pdrgpdxlnCTE, pdrgpdxlnCTE);
		
		DrgPul *pdrgpulColIds = New(m_pmp) DrgPul(m_pmp);
		
		const ULONG ulOutputCols = pdrgpdxlnQueryOutputCte->UlLength();
		for (ULONG ul = 0; ul < ulOutputCols; ul++)
		{
			CDXLNode *pdxlnOutputCol = (*pdrgpdxlnQueryOutputCte)[ul];
			CDXLScalarIdent *pdxlnIdent = CDXLScalarIdent::PdxlopConvert(pdxlnOutputCol->Pdxlop());
			pdrgpulColIds->Append(New(m_pmp) ULONG(pdxlnIdent->Pdxlcr()->UlID()));
		}
		
		CDXLLogicalCTEProducer *pdxlop = New(m_pmp) CDXLLogicalCTEProducer(m_pmp, m_pidgtorCTE->UlNextId(), pdrgpulColIds);
		CDXLNode *pdxlnCTEProducer = New(m_pmp) CDXLNode(m_pmp, pdxlop, pdxlnCteChild);
		
		m_pdrgpdxlnCTE->Append(pdxlnCTEProducer);
#ifdef GPOS_DEBUG
		BOOL fResult =
#endif
		m_phmulfCTEProducers->FInsert(New(m_pmp) ULONG(pdxlop->UlId()), New(m_pmp) BOOL(true));
		GPOS_ASSERT(fResult);
		
		// update CTE producer mappings
		CCTEListEntry *pctelistentry = m_phmulCTEEntries->PtLookup(&ulCteQueryLevel);
		if (NULL == pctelistentry)
		{
			pctelistentry = New (m_pmp) CCTEListEntry (m_pmp, ulCteQueryLevel, pcte, pdxlnCTEProducer);
#ifdef GPOS_DEBUG
		BOOL fRes =
#endif
			m_phmulCTEEntries->FInsert(New(m_pmp) ULONG(ulCteQueryLevel), pctelistentry);
			GPOS_ASSERT(fRes);
		}
		else
		{
			pctelistentry->AddCTEProducer(m_pmp, pcte, pdxlnCTEProducer);
		}
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::ConstructCTEAnchors
//
//	@doc:
//		Construct a stack of CTE anchors for each CTE producer in the given array
//
//---------------------------------------------------------------------------
void
CTranslatorQueryToDXL::ConstructCTEAnchors
	(
	DrgPdxln *pdrgpdxln,
	CDXLNode **ppdxlnCTEAnchorTop,
	CDXLNode **ppdxlnCTEAnchorBottom
	)
{
	GPOS_ASSERT(NULL == *ppdxlnCTEAnchorTop);
	GPOS_ASSERT(NULL == *ppdxlnCTEAnchorBottom);

	if (NULL == pdrgpdxln || 0 == pdrgpdxln->UlLength())
	{
		return;
	}
	
	const ULONG ulCTEs = pdrgpdxln->UlLength();
	
	for (ULONG ul = ulCTEs; ul > 0; ul--)
	{
		// construct a new CTE anchor on top of the previous one
		CDXLNode *pdxlnCTEProducer = (*pdrgpdxln)[ul-1];
		CDXLLogicalCTEProducer *pdxlopCTEProducer = CDXLLogicalCTEProducer::PdxlopConvert(pdxlnCTEProducer->Pdxlop());
		ULONG ulCTEProducerId = pdxlopCTEProducer->UlId();
		
		if (NULL == m_phmulfCTEProducers->PtLookup(&ulCTEProducerId))
		{
			// cte not defined at this level: CTE anchor was already added
			continue;
		}
		
		CDXLNode *pdxlnCTEAnchorNew = New(m_pmp) CDXLNode(m_pmp, New(m_pmp) CDXLLogicalCTEAnchor(m_pmp, ulCTEProducerId));
		
		if (NULL == *ppdxlnCTEAnchorBottom)
		{
			*ppdxlnCTEAnchorBottom = pdxlnCTEAnchorNew;
		}

		if (NULL != *ppdxlnCTEAnchorTop)
		{
			pdxlnCTEAnchorNew->AddChild(*ppdxlnCTEAnchorTop);
		}
		*ppdxlnCTEAnchorTop = pdxlnCTEAnchorNew;
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdrgpulGenerateColIds
//
//	@doc:
//		Generate an array of new column ids of the given size
//
//---------------------------------------------------------------------------
DrgPul *
CTranslatorQueryToDXL::PdrgpulGenerateColIds
	(
	IMemoryPool *pmp,
	ULONG ulSize
	)
	const
{
	DrgPul *pdrgpul = New(pmp) DrgPul(pmp);
	
	for (ULONG ul = 0; ul < ulSize; ul++)
	{
		pdrgpul->Append(New(pmp) ULONG(m_pidgtorCol->UlNextId()));
	}
	
	return pdrgpul;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PdrgpulExtractColIds
//
//	@doc:
//		Extract column ids from the given mapping
//
//---------------------------------------------------------------------------
DrgPul *
CTranslatorQueryToDXL::PdrgpulExtractColIds
	(
	IMemoryPool *pmp,
	HMIUl *phmiul
	)
	const
{
	HMUlUl *phmulul = New(pmp) HMUlUl(pmp);
	
	DrgPul *pdrgpul = New(pmp) DrgPul(pmp);
	
	HMIUlIter mi(phmiul);
	while (mi.FAdvance())
	{
		ULONG ulColId = *(mi.Pt());
		
		// do not insert colid if already inserted
		if (NULL == phmulul->PtLookup(&ulColId))
		{
			pdrgpul->Append(New(m_pmp) ULONG(ulColId));
			phmulul->FInsert(New(m_pmp) ULONG(ulColId), New(m_pmp) ULONG(ulColId));
		}
	}
		
	phmulul->Release();
	return pdrgpul;
}

//---------------------------------------------------------------------------
//	@function:
//		CTranslatorQueryToDXL::PhmiulRemapColIds
//
//	@doc:
//		Construct a new hashmap which replaces the values in the From array
//		with the corresponding value in the To array
//
//---------------------------------------------------------------------------
HMIUl *
CTranslatorQueryToDXL::PhmiulRemapColIds
	(
	IMemoryPool *pmp,
	HMIUl *phmiul,
	DrgPul *pdrgpulFrom,
	DrgPul *pdrgpulTo
	)
	const
{
	GPOS_ASSERT(NULL != phmiul);
	GPOS_ASSERT(NULL != pdrgpulFrom && NULL != pdrgpulTo);
	GPOS_ASSERT(pdrgpulFrom->UlLength() == pdrgpulTo->UlLength());
	
	// compute a map of the positions in the from array
	HMUlUl *phmulul = New(pmp) HMUlUl(pmp);
	const ULONG ulSize = pdrgpulFrom->UlLength();
	for (ULONG ul = 0; ul < ulSize; ul++)
	{
#ifdef GPOS_DEBUG
		BOOL fResult = 
#endif // GPOS_DEBUG
		phmulul->FInsert(New(pmp) ULONG(*((*pdrgpulFrom)[ul])), New(pmp) ULONG(*((*pdrgpulTo)[ul])));
		GPOS_ASSERT(fResult);
	}

	HMIUl *phmiulResult = New(pmp) HMIUl(pmp);
	HMIUlIter mi(phmiul);
	while (mi.FAdvance())
	{
		INT *piKey = New(pmp) INT(*(mi.Pk()));
		const ULONG *pulValue = mi.Pt();
		GPOS_ASSERT(NULL != pulValue);
		
		ULONG *pulValueRemapped = New(pmp) ULONG(*(phmulul->PtLookup(pulValue)));
		phmiulResult->FInsert(piKey, pulValueRemapped);
	}
		
	phmulul->Release();
	
	return phmiulResult;
}


// EOF
