//
// Copyright (c) 2017-2025, Manticore Software LTD (https://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "accumulator.h"
#include "sphinxrt.h"
#include "columnarmisc.h"
#include "memio.h"
#include "tracer.h"

#include <memory>

std::unique_ptr<ReplicationCommand_t> MakeReplicationCommand ( ReplCmd_e eCommand, CSphString sIndex, CSphString sCluster )
{
	auto pCmd = std::make_unique<ReplicationCommand_t>();
	pCmd->m_eCommand = eCommand;
	pCmd->m_sCluster = std::move ( sCluster );
	pCmd->m_sIndex = std::move ( sIndex );
	return pCmd;
}

ReplicationCommand_t* RtAccum_t::AddCommand ( ReplCmd_e eCmd, CSphString sIndex, CSphString sCluster )
{
	// all writes to RT index go as single command to serialize accumulator
	if ( eCmd == ReplCmd_e::RT_TRX && !m_dCmd.IsEmpty() && m_dCmd.Last()->m_eCommand == ReplCmd_e::RT_TRX )
		return m_dCmd.Last().get();

	m_dCmd.Add ( MakeReplicationCommand ( eCmd, std::move ( sIndex ), std::move ( sCluster ) ) );
	return m_dCmd.Last().get();
}

void RtAccum_t::SetupDict ( const RtIndex_i* pIndex, const DictRefPtr_c& pDict, bool bKeywordDict )
{
	if ( pIndex == m_pIndex && pDict.Ptr() == m_pRefDict && bKeywordDict == m_bKeywordDict )
		return;

	m_bKeywordDict = bKeywordDict;
	m_pRefDict = pDict.Ptr();
	m_pDict = GetStatelessDict ( pDict );
	if ( m_bKeywordDict )
	{
		m_pDict = m_pDictRt = sphCreateRtKeywordsDictionaryWrapper ( m_pDict, pIndex->NeedStoreWordID() );
	}
}

void RtAccum_t::ResetDict()
{
	assert ( !m_bKeywordDict || m_pDictRt );
	if ( m_pDictRt )
		m_pDictRt->ResetKeywords();

	m_dPackedKeywords.Reset ( 0 );
}

const BYTE* RtAccum_t::GetPackedKeywords() const
{
	return m_dPackedKeywords.IsEmpty() ? m_pDictRt->GetPackedKeywords() : m_dPackedKeywords.begin();
}

int RtAccum_t::GetPackedLen() const
{
	return m_dPackedKeywords.IsEmpty() ? m_pDictRt->GetPackedLen() : m_dPackedKeywords.GetLength();
}


void RtAccum_t::Sort()
{
	TRACE_CONN ( "conn", "RtAccum_t::Sort" );
	if ( !m_bKeywordDict )
		m_dAccum.Sort ( Lesser ( [] ( const CSphWordHit& a, const CSphWordHit& b )
		{
			return 	( a.m_uWordID<b.m_uWordID ) ||
				( a.m_uWordID==b.m_uWordID && a.m_tRowID<b.m_tRowID ) ||
				( a.m_uWordID==b.m_uWordID && a.m_tRowID==b.m_tRowID && HITMAN::GetPosWithField ( a.m_uWordPos )<HITMAN::GetPosWithField ( b.m_uWordPos ) );
		}));
	else
	{
		assert ( m_pDictRt );
		m_dAccum.Sort ( Lesser ( [pPackedKeywords = GetPackedKeywords()] ( const CSphWordHit& a, const CSphWordHit& b )
		{
			const BYTE* pPackedA = pPackedKeywords + a.m_uWordID;
			const BYTE* pPackedB = pPackedKeywords + b.m_uWordID;
			int iCmp = sphDictCmpStrictly ( (const char*)pPackedA + 1, *pPackedA, (const char*)pPackedB + 1, *pPackedB );
			return ( iCmp < 0 ) || ( iCmp == 0 && a.m_tRowID < b.m_tRowID ) || ( iCmp == 0 && a.m_tRowID == b.m_tRowID && HITMAN::GetPosWithField ( a.m_uWordPos ) < HITMAN::GetPosWithField ( b.m_uWordPos ) );
		}));
	}
}


static bool RepackBlob ( const CSphColumnInfo & tAttr, const CSphColumnInfo & tBlobLoc, int iBlobAttr, const CSphRowitem * pRow, const BYTE * pBlobPool, std::unique_ptr<BlobRowBuilder_i> & pBlobWriter, CSphString & sError )
{
	SphAttr_t tBlobRowOffset = sphGetRowAttr ( pRow, tBlobLoc.m_tLocator );
	const BYTE * pBlobRow = pBlobPool + tBlobRowOffset;
	ByteBlob_t tBlob = sphGetBlobAttr ( pBlobRow, tAttr.m_tLocator );
	return pBlobWriter->SetAttr ( iBlobAttr, tBlob.first, tBlob.second, sError );
}


static bool StoreEmbeddings ( const CSphSchema & tSchema, int iAttr, int iBlobAttr, int iColumnarAttr, DocstoreDoc_t & tDoc, std::unique_ptr<BlobRowBuilder_i> & pNewBlobBuilder, std::unique_ptr<ColumnarBuilderRT_i> & pNewColumnarBuilder, const IntVec_t & dDocstoreRemap, const std::vector<float> & dEmbedding, CSphVector<int64_t> & dTmp, CSphString & sError )
{
	const CSphColumnInfo & tAttr = tSchema.GetAttr(iAttr);

	if ( tAttr.IsColumnar() )
	{
		dTmp.Resize ( dEmbedding.size() );
		ARRAY_FOREACH ( iEmb, dTmp )
			dTmp[iEmb] = sphF2DW ( dEmbedding[iEmb] );

		pNewColumnarBuilder->SetAttr ( iColumnarAttr, dTmp.Begin(), dTmp.GetLength() );
	}
	else
	{
		if ( !pNewBlobBuilder->SetAttr ( iBlobAttr, (const BYTE*)dEmbedding.data(), dEmbedding.size()*sizeof(float), sError ) )
			return false;
	}

	if ( tAttr.IsStored() )
	{
		int iId = dDocstoreRemap[iAttr];
		tDoc.m_dFields[iId].Resize ( dEmbedding.size()*sizeof(DWORD) );
		const BYTE * pStart = tDoc.m_dFields[iId].Begin();
		for ( auto i : dEmbedding )
		{
			*(DWORD*)pStart = sphF2DW ( dEmbedding[i] );
			pStart += sizeof(DWORD);
		}
	}

	return true;
}


bool RtAccum_t::RebuildStoragesForEmbeddings ( RowID_t tRowID, CSphRowitem * pRow, const CSphVector<AttrWithModel_t> & dAttrsWithModels, std::unique_ptr<BlobRowBuilder_i> & pNewBlobBuilder, std::unique_ptr<ColumnarBuilderRT_i> & pNewColumnarBuilder, std::unique_ptr<DocstoreRT_i> & pNewDocstoreBuilder, CSphVector<ScopedTypedIterator_t> & dAllIterators, const IntVec_t & dDocstoreRemap, const CSphColumnInfo * pBlobLoc, std::vector<std::vector<std::vector<float>>> & dAllEmbeddings, CSphVector<int64_t> & dTmp, CSphString & sError )
{
	int iBlobAttr = 0;
	int iColumnarAttr = 0;
	DocstoreDoc_t tDoc;

	// fetch all fields and attributes without model from docstore (they will not be modified)
	if ( pNewDocstoreBuilder )
		tDoc = m_pDocstore->GetDoc ( tRowID, nullptr, -1, false );

	assert(m_pIndex);
	const CSphSchema & tSchema = m_pIndex->GetInternalSchema();

	ARRAY_FOREACH ( i, dAttrsWithModels )
	{
		const CSphColumnInfo & tAttr = tSchema.GetAttr(i);
		const AttrWithModel_t & tAttrWithModel = dAttrsWithModels[i];

		if ( tAttrWithModel.m_pModel )
		{
			if ( !StoreEmbeddings ( tSchema, i, iBlobAttr, iColumnarAttr, tDoc, pNewBlobBuilder, pNewColumnarBuilder, dDocstoreRemap, dAllEmbeddings[i][tRowID], dTmp, sError ) )
				return false;
		}
		else
		{
			if ( pNewBlobBuilder && !tAttr.IsColumnar() && sphIsBlobAttr(tAttr) )
			{
				assert(pBlobLoc);
				if ( !RepackBlob ( tAttr, *pBlobLoc, iBlobAttr, pRow, m_dBlobs.Begin(), pNewBlobBuilder, sError ) )
					return false;
			}

			if ( pNewColumnarBuilder && tAttr.IsColumnar() )
			{
				auto & tIt = dAllIterators[iColumnarAttr];
				SetColumnarAttr ( iColumnarAttr, tIt.second, pNewColumnarBuilder.get(), tIt.first, tRowID, dTmp );
			}
		}

		if ( sphIsBlobAttr(tAttr) )
			iBlobAttr++;

		if ( tAttr.IsColumnar() )
			iColumnarAttr++;
	}

	if ( pNewBlobBuilder )
	{
		assert(pBlobLoc);
		sphSetRowAttr ( pRow, pBlobLoc->m_tLocator, pNewBlobBuilder->Flush().first );
	}

	if ( pNewDocstoreBuilder )
		pNewDocstoreBuilder->AddDoc ( tRowID, tDoc );

	return true;
}


bool RtAccum_t::GenerateEmbeddings ( int iAttr, int iAttrWithModel, const CSphVector<AttrWithModel_t> & dAttrsWithModels, std::vector<std::vector<std::vector<float>>> & dAllEmbeddings, columnar::Iterator_i * pColumnarIterator, CSphString & sError )
{
	const AttrWithModel_t & tAttrWithModel = dAttrsWithModels[iAttr];
	if ( !tAttrWithModel.m_pModel )
		return true;

	assert(m_pIndex);
	const CSphSchema & tSchema = m_pIndex->GetInternalSchema();
	const auto & tAttr = tSchema.GetAttr(iAttr);
	const CSphColumnInfo * pBlobLoc = tSchema.GetAttr ( sphGetBlobLocatorName() );

	int iRowSize = tSchema.GetRowSize();
	std::vector<std::vector<float>> & dEmbeddingsForAttr = dAllEmbeddings[iAttr];
	std::vector<std::string_view> dTexts;
	bool bHaveSkipped = false;
	IntVec_t dResultIds(m_uAccumDocs);
	CSphRowitem * pRow = m_dAccumRows.Begin();
	for ( RowID_t tRowID = 0; tRowID < m_uAccumDocs; ++tRowID, pRow += iRowSize )
	{
		bool bDefault;
		if ( pColumnarIterator )
		{
			const BYTE * pResult = nullptr;
			int iBytes = pColumnarIterator->Get ( tRowID, pResult );
			assert ( iBytes==sizeof(DWORD) );
			bDefault = *(const DWORD*)pResult;
		}
		else
		{
			assert(pBlobLoc);
			SphAttr_t tBlobRowOffset = sphGetRowAttr ( pRow, pBlobLoc->m_tLocator );
			const BYTE * pBlobRow = m_dBlobs.Begin() + tBlobRowOffset;
			ByteBlob_t tBlob = sphGetBlobAttr ( pBlobRow, tAttr.m_tLocator );
			assert ( tBlob.second==sizeof(DWORD) );
			bDefault = *(const DWORD*)tBlob.first;
		}

		if ( bDefault )
		{
			dResultIds[tRowID] = dTexts.size();
			const auto & dConcat = m_pEmbeddingsSrc->Get ( tRowID, iAttrWithModel );
			dTexts.push_back ( { dConcat.Begin(), (size_t)dConcat.GetLength() } );
		}
		else
		{
			dResultIds[tRowID] = -1;
			bHaveSkipped = true;
		}
	}

	std::string sErrorSTL;
	std::vector<std::vector<float>> dEmbeddingsForAttrTmp;
	if ( !tAttrWithModel.m_pModel->Convert ( dTexts, bHaveSkipped ? dEmbeddingsForAttrTmp : dEmbeddingsForAttr, sErrorSTL ) )
	{
		sError = sErrorSTL.c_str();
		return false;
	}

	if ( bHaveSkipped )
	{
		dEmbeddingsForAttr.resize ( dResultIds.GetLength() );
		ARRAY_FOREACH ( i, dResultIds )
		{
			int iResultId = dResultIds[i];
			if ( iResultId!=-1 )
				dEmbeddingsForAttr[i] = dEmbeddingsForAttrTmp[iResultId];
		}
	}

	return true;
}


bool RtAccum_t::FetchEmbeddings ( TableEmbeddings_c * pEmbeddings, const CSphVector<AttrWithModel_t> & dAttrsWithModels, CSphString & sError )
{
	if ( !pEmbeddings )
		return true;

	assert(m_pIndex);
	const CSphSchema & tSchema = m_pIndex->GetInternalSchema();

	// we need to rebuild our blobs/columnar storage (to replace empty data with fetched floatvectors), but they are immutable by design
	// so we'll have to rebuild the blobs/columnar storage fully
	const CSphColumnInfo * pBlobLoc = tSchema.GetAttr ( sphGetBlobLocatorName() );
	bool bRebuildColumnar = false;
	bool bRebuildBlobs = false;
	bool bRebuildDocstore = false;
	IntVec_t dDocstoreRemap;
	dDocstoreRemap.Resize ( dAttrsWithModels.GetLength() );
	dDocstoreRemap.Fill(-1);
	ARRAY_FOREACH ( i, dAttrsWithModels )
	{
		if ( !dAttrsWithModels[i].m_pModel )
			continue;

		auto & tAttr = tSchema.GetAttr(i);
		assert ( tAttr.m_eAttrType==SPH_ATTR_FLOAT_VECTOR );
		bRebuildColumnar |= tAttr.IsColumnar();
		bRebuildBlobs |= !tAttr.IsColumnar();
		bRebuildDocstore |= tAttr.IsStored();

		dDocstoreRemap[i] = ((DocstoreBuilder_i*)m_pDocstore.get())->GetFieldId ( tAttr.m_sName, DOCSTORE_ATTR );
	}

	CSphTightVector<BYTE> dNewBlobPool;
	std::unique_ptr<BlobRowBuilder_i> pNewBlobBuilder = bRebuildBlobs ? sphCreateBlobRowBuilder ( tSchema, dNewBlobPool ) : nullptr;
	std::unique_ptr<ColumnarBuilderRT_i> pNewColumnarBuilder = bRebuildColumnar ? CreateColumnarBuilderRT(tSchema) : nullptr;
	std::unique_ptr<ColumnarRT_i> pColumnar = bRebuildColumnar ? CreateLightColumnarRT ( tSchema, m_pColumnarBuilder.get() ) : nullptr;
	CSphVector<ScopedTypedIterator_t> dAllIterators;
	if ( bRebuildColumnar )
		dAllIterators = CreateAllColumnarIterators ( pColumnar.get(), tSchema );

	std::unique_ptr<DocstoreRT_i> pNewDocstoreBuilder;
	if ( bRebuildDocstore )
	{
		pNewDocstoreBuilder = CreateDocstoreRT();
		SetupDocstoreFields ( *pNewDocstoreBuilder, tSchema );
	}

	// 1st pass - generate all embeddings for each attribute
	int iRowSize = tSchema.GetRowSize();
	int iAttrWithModel = 0;
	int iColumnarAttr = 0;
	std::vector<std::vector<std::vector<float>>> dAllEmbeddings;
	dAllEmbeddings.resize ( dAttrsWithModels.GetLength() );
	ARRAY_FOREACH ( i, dAttrsWithModels )
	{
		bool bColumnar = tSchema.GetAttr(i).IsColumnar();
		if ( !GenerateEmbeddings( i, iAttrWithModel, dAttrsWithModels, dAllEmbeddings, bColumnar ? dAllIterators[iColumnarAttr].first.get() : nullptr, sError ) )
			return false;

		if ( dAttrsWithModels[i].m_pModel )
			iAttrWithModel++;

		if ( bColumnar )
			iColumnarAttr++;
	}

	// 2nd pass - rebuild attribute storages
	std::vector<float> dEmbedding;
	CSphVector<int64_t> dTmp;
	CSphRowitem * pRow = m_dAccumRows.Begin();
	for ( RowID_t tRowID = 0; tRowID < m_uAccumDocs; ++tRowID, pRow += iRowSize )
		if ( !RebuildStoragesForEmbeddings ( tRowID, pRow, dAttrsWithModels, pNewBlobBuilder, pNewColumnarBuilder, pNewDocstoreBuilder, dAllIterators, dDocstoreRemap, pBlobLoc, dAllEmbeddings, dTmp, sError ) )
			return false;

	if ( bRebuildBlobs )
	{
		m_pBlobWriter = std::move(pNewBlobBuilder);
		m_dBlobs.SwapData(dNewBlobPool);
	}

	if ( bRebuildColumnar )
		m_pColumnarBuilder = std::move(pNewColumnarBuilder);

	if ( bRebuildDocstore )
		m_pDocstore = std::move(pNewDocstoreBuilder);

	return true;
}


void RtAccum_t::CleanupPart()
{
	m_dAccumRows.Resize ( 0 );
	m_dBlobs.Resize ( 0 );
	m_pColumnarBuilder.reset();
	m_dPerDocHitsCount.Resize ( 0 );
	m_dAccum.Resize ( 0 );
	m_pDocstore.reset();
	m_pEmbeddingsSrc.reset();

	ResetDict();
	ResetRowID();
}

void RtAccum_t::Cleanup()
{
	CleanupPart();

	m_pIndex = nullptr;
	m_pBlobWriter.reset();
	m_uAccumDocs = 0;
	m_iAccumBytes = 0;
	m_dAccumKlist.Reset();
	m_sIndexName = CSphString();
	m_iIndexId = 0;

	m_dCmd.Reset();
}

void RtAccum_t::CleanReplicated()
{
	m_tCmdReplicated = ReplicatedCommand_t();
}

void RtAccum_t::SetupDocstore()
{
	if ( m_pDocstore )
		return;

	m_pDocstore = CreateDocstoreRT();
	assert ( m_pDocstore );
	SetupDocstoreFields ( *m_pDocstore, m_pIndex->GetInternalSchema() );
}

bool RtAccum_t::SetupDocstore ( const RtIndex_i& tIndex, CSphString& sError )
{
	const CSphSchema& tSchema = tIndex.GetInternalSchema();
	if ( !m_pDocstore && !tSchema.HasStoredFields() && !tSchema.HasStoredAttrs() )
		return true;

	// might be a case when replicated trx was wo docstore but index has docstore
	if ( !m_pDocstore )
		m_pDocstore = CreateDocstoreRT();

	assert ( m_pDocstore );
	SetupDocstoreFields ( *m_pDocstore, tSchema );
	return m_pDocstore->CheckFieldsLoaded ( sError );
}


[[nodiscard]] bool RtAccum_t::IsClusterCommand() const noexcept
{
	return ( m_dCmd.GetLength () && !m_dCmd[0]->m_sCluster.IsEmpty () );
}


[[nodiscard]] bool RtAccum_t::IsUpdateCommand() const noexcept
{
	return ( m_dCmd.GetLength () &&
			( m_dCmd[0]->m_eCommand==ReplCmd_e::UPDATE_API
					|| m_dCmd[0]->m_eCommand==ReplCmd_e::UPDATE_QL
					|| m_dCmd[0]->m_eCommand==ReplCmd_e::UPDATE_JSON ) );
}

static void ResetTailHit ( CSphWordHit * pHit )
{
	if ( pHit->m_tRowID!=pHit[1].m_tRowID || pHit->m_uWordID!=pHit[1].m_uWordID )
		return;

	if ( HITMAN::GetField ( pHit->m_uWordPos )==HITMAN::GetField ( pHit[1].m_uWordPos ) && HITMAN::IsEnd ( pHit[1].m_uWordPos ) )
		pHit->m_uWordPos = HITMAN::GetPosWithField ( pHit->m_uWordPos );
}


static const char * FetchStringFromDoc ( int iAttr, const InsertDocData_c & tDoc, const ISphSchema & tSchema )
{
	const char ** ppStr = tDoc.m_dStrings.Begin();
	int iStrAttr = 0;

	for ( int i=0; i < tSchema.GetAttrsCount(); ++i )
	{
		const CSphColumnInfo & tAttr = tSchema.GetAttr(i);

		if ( tAttr.m_eAttrType!=SPH_ATTR_STRING )
			continue;

		if ( iAttr==i )
			return ppStr[iStrAttr];

		iStrAttr++;
	}

	return nullptr;
}


static CSphVector<char> ConcatFromFields ( const InsertDocData_c & tDoc, const AttrWithModel_t & tAttr, const ISphSchema & tSchema )
{
	CSphVector<char> dTmp;
	ARRAY_FOREACH ( i, tAttr.m_dFrom )
	{
		auto tFrom = tAttr.m_dFrom[i];
		int iAttrFieldId = tFrom.first;
		VecTraits_T<const char> dSrc;
		if ( tFrom.second )
			dSrc = { tDoc.m_dFields[iAttrFieldId].Begin(), tDoc.m_dFields[iAttrFieldId].GetLength() };
		else
		{
			const char * szString = FetchStringFromDoc ( iAttrFieldId, tDoc, tSchema );
			dSrc = { szString, (int64_t)( szString ? strlen(szString) : 0 ) };
		}

		int iOldSize = dTmp.GetLength();
		dTmp.Resize ( iOldSize + dSrc.GetLength() + 1 );
		dTmp[iOldSize] = ' ';
		memcpy ( dTmp.Begin() + iOldSize + 1, dSrc.Begin(), dSrc.GetLength() );
	}

	return dTmp;
}


void RtAccum_t::FetchEmbeddingsSrc ( InsertDocData_c & tDoc, const CSphVector<AttrWithModel_t> & dAttrsWithModels )
{
	if ( !m_pEmbeddingsSrc )
	{
		int iAttrsWithModels = dAttrsWithModels.count_of ( []( auto & tData ){ return !!tData.m_pModel; } );
		m_pEmbeddingsSrc = std::make_unique<EmbeddingsSrc_c>(iAttrsWithModels);
	}

	assert(m_pIndex);
	const CSphSchema & tSchema = m_pIndex->GetInternalSchema();

	CSphVector<char> dTmp;
	int iAttrWithModel = 0;
	ARRAY_FOREACH ( i, dAttrsWithModels )
	{
		const AttrWithModel_t & tAttrWithModel = dAttrsWithModels[i];
		if ( !tAttrWithModel.m_pModel )
			continue;

		auto dConcat = ConcatFromFields ( tDoc, tAttrWithModel, tSchema );
		m_pEmbeddingsSrc->Add ( iAttrWithModel, dConcat );
		iAttrWithModel++;
	}
}


void RtAccum_t::AddDocument ( ISphHits* pHits, const InsertDocData_c& tDoc, bool bReplace, int iRowSize, const DocstoreBuilder_i::Doc_t* pStoredDoc )
{
	MEMORY ( MEM_RT_ACCUM );

	// FIXME? what happens on mixed insert/replace?
	m_bReplace = bReplace;

	DocID_t tDocID = tDoc.GetID();

	// schedule existing copies for deletion
	m_dAccumKlist.Add ( tDocID );

	// reserve some hit space on first use
	if ( pHits && pHits->GetLength() && !m_dAccum.GetLength() )
		m_dAccum.Reserve ( 128 * 1024 );

	// accumulate row data; expect fully dynamic rows
	assert ( !tDoc.m_tDoc.m_pStatic );
	assert ( !( !tDoc.m_tDoc.m_pDynamic && iRowSize != 0 ) );
	assert ( !( tDoc.m_tDoc.m_pDynamic && (int)tDoc.m_tDoc.m_pDynamic[-1] != iRowSize ) );

	CSphRowitem* pRow = nullptr;
	if ( iRowSize )
	{
		m_dAccumRows.Append ( tDoc.m_tDoc.m_pDynamic, iRowSize );
		pRow = &m_dAccumRows[m_dAccumRows.GetLength() - iRowSize];
	}

	CSphString sError;

	int iStrAttr = 0;
	int iBlobAttr = 0;
	int iColumnarAttr = 0;
	int iMva = 0;

	const char** ppStr = tDoc.m_dStrings.Begin();
	const CSphSchema& tSchema = m_pIndex->GetInternalSchema();
	for ( int i = 0; i < tSchema.GetAttrsCount(); ++i )
	{
		const CSphColumnInfo & tColumn = tSchema.GetAttr(i);

		switch ( tColumn.m_eAttrType )
		{
		case SPH_ATTR_STRING:
		case SPH_ATTR_JSON:
			{
				const BYTE* pStr = ppStr ? (const BYTE*)ppStr[iStrAttr++] : nullptr;
				ByteBlob_t dStr;
				if ( tColumn.m_eAttrType == SPH_ATTR_STRING )
					dStr = { pStr, pStr ? (int)strlen ( (const char*)pStr ) : 0 };
				else // SPH_ATTR_JSON - packed len + data
					dStr = sphUnpackPtrAttr ( pStr );

				if ( tColumn.IsColumnar() )
					m_pColumnarBuilder->SetAttr ( iColumnarAttr, dStr.first, dStr.second );
				else
					m_pBlobWriter->SetAttr ( iBlobAttr, dStr.first, dStr.second, sError );
			}
			break;

		case SPH_ATTR_UINT32SET:
		case SPH_ATTR_INT64SET:
		case SPH_ATTR_FLOAT_VECTOR:
			{
				int iNumValues = 0;
				bool bDefault = false;
				const int64_t * pMva = tDoc.GetMVA(iMva);
				std::tie ( iNumValues, bDefault ) = tDoc.ReadMVALength(pMva);
				iMva += iNumValues + 1;

				int64_t iTmpMVA = 0;
				if ( !tColumn.m_tKNNModel.m_sModelName.empty() )
				{
					// store a temporary flag for later checks in the accumulator
					assert(!iNumValues);
					iNumValues = 1;
					iTmpMVA = (int64_t)bDefault;
					pMva = &iTmpMVA;
				}

				if ( tColumn.IsColumnar() )
					m_pColumnarBuilder->SetAttr ( iColumnarAttr, pMva, iNumValues );
				else
					m_pBlobWriter->SetAttr ( iBlobAttr, (const BYTE*)pMva, iNumValues * sizeof ( int64_t ), sError );
			}
			break;

		default:
			if ( tColumn.IsColumnar() )
				m_pColumnarBuilder->SetAttr ( iColumnarAttr, tDoc.m_dColumnarAttrs[iColumnarAttr] );

			break;
		}

		if ( tColumn.IsColumnar() )
			++iColumnarAttr;
		else if ( sphIsBlobAttr ( tColumn ) )
			++iBlobAttr;
	}

	if ( m_pBlobWriter )
	{
		const CSphColumnInfo* pBlobLoc = tSchema.GetAttr ( sphGetBlobLocatorName() );
		assert ( pBlobLoc );

		sphSetRowAttr ( pRow, pBlobLoc->m_tLocator, m_pBlobWriter->Flush().first );
	}

	// handle index_field_lengths
	DWORD* pFieldLens = nullptr;
	if ( m_pIndex->GetSettings().m_bIndexFieldLens )
	{
		int iFirst = tSchema.GetAttrId_FirstFieldLen();
		assert ( tSchema.GetAttr ( iFirst ).m_eAttrType == SPH_ATTR_TOKENCOUNT );
		assert ( tSchema.GetAttr ( iFirst + tSchema.GetFieldsCount() - 1 ).m_eAttrType == SPH_ATTR_TOKENCOUNT );
		pFieldLens = pRow + ( tSchema.GetAttr ( iFirst ).m_tLocator.m_iBitOffset / 32 );
		memset ( pFieldLens, 0, sizeof ( int ) * tSchema.GetFieldsCount() ); // NOLINT
	}

	// accumulate hits
	int iHits = 0;
	if ( pHits && !pHits->IsEmpty() )
	{
		CSphWordHit tLastHit;
		tLastHit.m_tRowID = INVALID_ROWID;
		tLastHit.m_uWordID = 0;
		tLastHit.m_uWordPos = 0;

		Hitpos_t uFieldLastHit = pHits->Begin()->m_uWordPos;
		DWORD uFieldLastCount = 1;

		m_dAccum.ReserveGap ( pHits->GetLength() );
		iHits = 0;
		for ( CSphWordHit* pHit = pHits->Begin(); pHit < pHits->End(); ++pHit )
		{
			// ignore duplicate hits
			if ( *pHit == tLastHit )
				continue;

			// update field lengths
			if ( pFieldLens )
			{
				if ( HITMAN::GetField ( uFieldLastHit ) != HITMAN::GetField ( pHit->m_uWordPos ) )
				{
					pFieldLens[HITMAN::GetField ( uFieldLastHit )] += uFieldLastCount;
					uFieldLastCount = 1;
					uFieldLastHit = pHit->m_uWordPos;
				}

				// skip blended part, lemmas and duplicates
				if ( HITMAN::GetPos ( pHit->m_uWordPos ) > HITMAN::GetPos ( uFieldLastHit ) )
				{
					uFieldLastHit = pHit->m_uWordPos;
					uFieldLastCount++;
				}
			}

			// need original hit for duplicate removal
			tLastHit = *pHit;
			// reset field end for not very last position in this field
			if ( HITMAN::IsEnd ( pHit->m_uWordPos ) && pHit!=&pHits->Last() )
				ResetTailHit ( pHit );

			// accumulate
			m_dAccum.Add ( *pHit );
			++iHits;
		}
		if ( pFieldLens && uFieldLastCount )
		{
			pFieldLens[HITMAN::GetField ( uFieldLastHit )] += uFieldLastCount;
		}
	}
	// make sure to get real count without duplicated hits
	m_dPerDocHitsCount.Add ( iHits );

	if ( pStoredDoc )
	{
		SetupDocstore();
		m_pDocstore->AddDoc ( m_uAccumDocs, *pStoredDoc );
	}

	++m_uAccumDocs;
	m_iAccumBytes += tDoc.m_iTotalBytes;
}

struct AccumDocHits_t
{
	DocID_t m_tDocID;
	int m_iDocIndex;
//	int m_iHitIndex;
//	int m_iHitCount;
};


void RtAccum_t::CleanupDuplicates ( int iRowSize )
{
	TRACE_CONN ( "conn", "RtAccum_t::CleanupDuplicates" );

	if ( m_uAccumDocs <= 1 )
		return;

	assert ( m_uAccumDocs == (DWORD)m_dPerDocHitsCount.GetLength() );
	CSphVector<AccumDocHits_t> dDocHits ( m_dPerDocHitsCount.GetLength() );

	assert ( m_pIndex );
	const CSphSchema& tSchema = m_pIndex->GetInternalSchema();
	bool bColumnarId = tSchema.GetAttr ( 0 ).IsColumnar();

	{
		// create temporary columnar accessor; don't take ownership of built attributes
		auto pColumnar = CreateLightColumnarRT ( m_pIndex->GetInternalSchema(), m_pColumnarBuilder.get() );

		std::string sError;
		std::unique_ptr<columnar::Iterator_i> pColumnarIdIterator;
		if ( bColumnarId )
		{
			pColumnarIdIterator = CreateColumnarIterator ( pColumnar.get(), sphGetDocidName(), sError );
			assert ( pColumnarIdIterator );
		}

	//	int iHitIndex = 0;
		CSphRowitem* pRow = m_dAccumRows.Begin();
		for ( DWORD i = 0; i < m_uAccumDocs; ++i, pRow += iRowSize )
		{
			AccumDocHits_t& tElem = dDocHits[i];
			if ( !bColumnarId )
				tElem.m_tDocID = sphGetDocID ( pRow );
			else
				tElem.m_tDocID = pColumnarIdIterator->Get(i);

			tElem.m_iDocIndex = i;
	//		tElem.m_iHitIndex = iHitIndex;
	//		tElem.m_iHitCount = m_dPerDocHitsCount[i];
	//		iHitIndex += m_dPerDocHitsCount[i];
		}
	}

	dDocHits.Sort ( Lesser ( [] ( const AccumDocHits_t& a, const AccumDocHits_t& b )
		{
			return ( a.m_tDocID < b.m_tDocID || ( a.m_tDocID == b.m_tDocID && a.m_iDocIndex < b.m_iDocIndex ) );
		}));

	DocID_t uPrev = 0;
	if ( !dDocHits.any_of ( [&] ( const AccumDocHits_t& dDoc ) {
			 bool bRes = dDoc.m_tDocID == uPrev;
			 uPrev = dDoc.m_tDocID;
			 return bRes;
		 } ) )
		return;

	CSphFixedVector<RowID_t> dRowMap ( m_uAccumDocs );
	for ( auto& i : dRowMap )
		i = 0;

	// identify duplicates to kill
	if ( m_bReplace )
	{
		// replace mode, last value wins, precending values are duplicate
		for ( DWORD i = 0; i < m_uAccumDocs - 1; ++i )
			if ( dDocHits[i].m_tDocID == dDocHits[i + 1].m_tDocID )
				dRowMap[dDocHits[i].m_iDocIndex] = INVALID_ROWID;
	} else
	{
		// insert mode, first value wins, subsequent values are duplicates
		for ( DWORD i = 1; i < m_uAccumDocs; ++i )
			if ( dDocHits[i].m_tDocID == dDocHits[i - 1].m_tDocID )
				dRowMap[dDocHits[i].m_iDocIndex] = INVALID_ROWID;
	}

	RowID_t tNextRowID = 0;
	for ( auto& i : dRowMap )
		if ( i != INVALID_ROWID )
			i = tNextRowID++;

	// remove duplicate hits and compact hit.rowid
	// might be document without hits
	// but hits after that document should be still remapped \ compacted
	// that is why can not use short-cut of
	// if ( tSrcRowID!=INVALID_ROWID ) -> if ( i!=iDstRow )
	int iDstRow = 0;
	for ( int i = 0, iLen = m_dAccum.GetLength(); i < iLen; ++i )
	{
		const auto& dSrcHit = m_dAccum[i];
		RowID_t tSrcRowID = dRowMap[dSrcHit.m_tRowID];
		if ( tSrcRowID != INVALID_ROWID )
		{
			CSphWordHit& tDstHit = m_dAccum[iDstRow];
			tDstHit = dSrcHit;
			tDstHit.m_tRowID = tSrcRowID;
			++iDstRow;
		}
	}

	m_dAccum.Resize ( iDstRow );

	RemoveColumnarDuplicates ( m_pColumnarBuilder, dRowMap, tSchema );
	if ( m_pEmbeddingsSrc )
		m_pEmbeddingsSrc->Remove(dRowMap);

	iDstRow = 0;
	ARRAY_FOREACH ( i, dRowMap )
	{
		if ( dRowMap[i] != INVALID_ROWID )
		{
			if ( i != iDstRow )
			{
				// remove duplicate docinfo
				// but all attributes could be columnar
				if ( iRowSize )
					memcpy ( &m_dAccumRows[iDstRow * iRowSize], &m_dAccumRows[i * iRowSize], iRowSize * sizeof ( CSphRowitem ) );

				// remove duplicate docstore
				if ( m_pDocstore )
					m_pDocstore->SwapRows ( iDstRow, i );
			}
			++iDstRow;
		}
	}

	m_dAccumRows.Resize ( iDstRow * iRowSize );
	m_uAccumDocs = iDstRow;
	if ( m_pDocstore )
		m_pDocstore->DropTail ( iDstRow );
}


void RtAccum_t::GrabLastWarning ( CSphString& sWarning )
{
	if ( m_pDictRt && m_pDictRt->GetLastWarning() )
	{
		sWarning = m_pDictRt->GetLastWarning();
		m_pDictRt->ResetWarning();
	}
}


void RtAccum_t::SetIndex ( RtIndex_i * pIndex )
{
	assert ( pIndex );
	m_iIndexGeneration = pIndex->GetAlterGeneration();
	m_pIndex = pIndex;
	m_pBlobWriter.reset();
	m_sIndexName = pIndex->GetName();
	m_iIndexId = pIndex->GetIndexId();

	const CSphSchema& tSchema = pIndex->GetInternalSchema();
	if ( tSchema.HasBlobAttrs() )
		m_pBlobWriter = sphCreateBlobRowBuilder ( tSchema, m_dBlobs );

	if ( !m_pColumnarBuilder )
		m_pColumnarBuilder = CreateColumnarBuilderRT ( tSchema );

	m_uSchemaHash = pIndex->GetSchemaHash();
}


RowID_t RtAccum_t::GenerateRowID()
{
	return m_tNextRowID++;
}


void RtAccum_t::ResetRowID()
{
	m_tNextRowID = 0;
}

void RtAccum_t::LoadRtTrx ( ByteBlob_t tTrx, DWORD uVer )
{
	MemoryReader_c tReader ( tTrx );
	m_bReplace = !!tReader.GetVal<BYTE>();
	tReader.GetVal ( m_uAccumDocs );
	if ( uVer>=0x106 )
		tReader.GetVal ( m_iAccumBytes );

	// insert and replace
	m_dAccum.Resize ( tReader.GetDword() );
	for ( CSphWordHit& tHit : m_dAccum )
	{
		// such manual serialization is necessary because CSphWordHit is internally aligned by 8,
		// and it's size is 3*8, however actually we have 4+8+4 bytes in members.
		// Sending raw unitialized bytes is not ok, since it may influent crc checking.
		tReader.GetVal ( tHit.m_tRowID );
		tReader.GetVal ( tHit.m_uWordID );
		tReader.GetVal ( tHit.m_uWordPos );
	}
	GetArray ( m_dAccumRows, tReader );
	GetArray ( m_dBlobs, tReader );
	GetArray ( m_dPerDocHitsCount, tReader );

	m_dPackedKeywords.Reset ( tReader.GetDword() );
	tReader.GetBytes ( m_dPackedKeywords.Begin(), (int)m_dPackedKeywords.GetLengthBytes() );

	if ( tReader.GetVal<BYTE>() )
	{
		if ( !m_pDocstore )
			m_pDocstore = CreateDocstoreRT();
		assert ( m_pDocstore );
		m_pDocstore->Load ( tReader );
	}

	if ( tReader.GetVal<BYTE>() )
		m_pColumnarBuilder = CreateColumnarBuilderRT ( tReader );

	// delete
	GetArray ( m_dAccumKlist, tReader );
}

void RtAccum_t::SaveRtTrx ( MemoryWriter_c& tWriter ) const
{
	tWriter.PutByte ( m_bReplace ); // this need only for data sort on commit
	tWriter.PutDword ( m_uAccumDocs );
	tWriter.PutVal ( m_iAccumBytes );

	// insert and replace
	tWriter.PutDword ( m_dAccum.GetLength() );
	for ( const CSphWordHit& tHit : m_dAccum )
	{
		tWriter.PutVal ( tHit.m_tRowID );
		tWriter.PutVal ( tHit.m_uWordID );
		tWriter.PutVal ( tHit.m_uWordPos );
	}
	SaveArray ( m_dAccumRows, tWriter );
	SaveArray ( m_dBlobs, tWriter );
	SaveArray ( m_dPerDocHitsCount, tWriter );

	// packed keywords default length is 1 no need to pass that
	int iLen = ( m_bKeywordDict && m_pDictRt->GetPackedLen() > 1 ? (int)m_pDictRt->GetPackedLen() : 0 );
	tWriter.PutDword ( iLen );
	if ( iLen )
		tWriter.PutBytes ( m_pDictRt->GetPackedKeywords(), iLen );
	tWriter.PutByte ( m_pDocstore != nullptr );
	if ( m_pDocstore )
		m_pDocstore->Save ( tWriter );

	tWriter.PutByte ( m_pColumnarBuilder != nullptr );
	if ( m_pColumnarBuilder )
		m_pColumnarBuilder->Save ( tWriter );

	// delete
	SaveArray ( m_dAccumKlist, tWriter );
}
