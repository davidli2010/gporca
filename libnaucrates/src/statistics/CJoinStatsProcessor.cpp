//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright 2018 Pivotal, Inc.
//
//	@filename:
//		CJoinStatsProcessor.cpp
//
//	@doc:
//		Statistics helper routines for processing all join types
//---------------------------------------------------------------------------

#include "gpopt/operators/ops.h"
#include "gpopt/optimizer/COptimizerConfig.h"

#include "naucrates/statistics/CStatisticsUtils.h"
#include "naucrates/statistics/CJoinStatsProcessor.h"
#include "naucrates/statistics/CLeftAntiSemiJoinStatsProcessor.h"
#include "naucrates/statistics/CFilterStatsProcessor.h"
#include "naucrates/statistics/CScaleFactorUtils.h"

using namespace gpopt;

// helper for joining histograms
void
CJoinStatsProcessor::JoinHistograms
		(
				CMemoryPool *mp,
				const CHistogram *histogram1,
				const CHistogram *histogram2,
				CStatsPredJoin *join_pred_stats,
				CDouble num_rows1,
				CDouble num_rows2,
				CHistogram **result_hist1, // output: histogram 1 after join
				CHistogram **result_hist2, // output: histogram 2 after join
				CDouble *scale_factor, // output: scale factor based on the join
				BOOL is_input_empty,
				IStatistics::EStatsJoinType join_type,
				BOOL DoIgnoreLASJHistComputation
		)
{
	GPOS_ASSERT(NULL != histogram1);
	GPOS_ASSERT(NULL != histogram2);
	GPOS_ASSERT(NULL != join_pred_stats);
	GPOS_ASSERT(NULL != result_hist1);
	GPOS_ASSERT(NULL != result_hist2);
	GPOS_ASSERT(NULL != scale_factor);

	if (IStatistics::EsjtLeftAntiSemiJoin == join_type)
	{
		CLeftAntiSemiJoinStatsProcessor::JoinHistogramsLASJ
				(
				mp,
				histogram1,
				histogram2,
				join_pred_stats,
				num_rows1,
				num_rows2,
				result_hist1,
				result_hist2,
				scale_factor,
				is_input_empty,
				join_type,
				DoIgnoreLASJHistComputation
				);

		return;
	}

	if (is_input_empty)
	{
		// use Cartesian product as scale factor
		*scale_factor = num_rows1 * num_rows2;
		*result_hist1 = GPOS_NEW(mp) CHistogram(GPOS_NEW(mp) CBucketArray(mp));
		*result_hist2 = GPOS_NEW(mp) CHistogram(GPOS_NEW(mp) CBucketArray(mp));

		return;
	}

	*scale_factor = CScaleFactorUtils::DefaultJoinPredScaleFactor;

	CStatsPred::EStatsCmpType stats_cmp_type = join_pred_stats->GetCmpType();
	BOOL empty_histograms = histogram1->IsEmpty() || histogram2->IsEmpty();

	if (empty_histograms)
	{
		// if one more input has no histograms (due to lack of statistics
		// for table columns or computed columns), we estimate
		// the join cardinality to be the max of the two rows.
		// In other words, the scale factor is equivalent to the
		// min of the two rows.
		*scale_factor = std::min(num_rows1, num_rows2);
	}
	else if (CHistogram::JoinPredCmpTypeIsSupported(stats_cmp_type))
	{
		CHistogram *join_histogram = histogram1->MakeJoinHistogramNormalize
				(
				mp,
				stats_cmp_type,
				num_rows1,
				histogram2,
				num_rows2,
				scale_factor
				);

		if (CStatsPred::EstatscmptEq == stats_cmp_type || CStatsPred::EstatscmptINDF == stats_cmp_type || CStatisticsUtils::IsStatsCmpTypeNdvEq(stats_cmp_type))
		{
			if (histogram1->WereNDVsScaled())
			{
				join_histogram->SetNDVScaled();
			}
			*result_hist1 = join_histogram;
			*result_hist2 = (*result_hist1)->CopyHistogram(mp);
			if (histogram2->WereNDVsScaled())
			{
				(*result_hist2)->SetNDVScaled();
			}
			return;
		}

		// note that for IDF and Not Equality predicate, we do not generate histograms but
		// just the scale factors.

		GPOS_ASSERT(join_histogram->IsEmpty());
		GPOS_DELETE(join_histogram);

		// TODO:  Feb 21 2014, for all join condition except for "=" join predicate
		// we currently do not compute new histograms for the join columns
	}

	// for an unsupported join predicate operator or in the case of
	// missing histograms, copy input histograms and use default scale factor
	*result_hist1 = histogram1->CopyHistogram(mp);
	*result_hist2 = histogram2->CopyHistogram(mp);
}

//	derive statistics for the given join's predicate(s)
IStatistics *
CJoinStatsProcessor::CalcAllJoinStats
		(
		CMemoryPool *mp,
		IStatisticsArray *statistics_array,
		CExpression *expr,
		IStatistics::EStatsJoinType join_type
		)
{
	GPOS_ASSERT(NULL != expr);
	GPOS_ASSERT(NULL != statistics_array);
	GPOS_ASSERT(0 < statistics_array->Size());
	BOOL left_outer_join = IStatistics::EsjtLeftOuterJoin == join_type;
	GPOS_ASSERT_IMP(left_outer_join, 2 == statistics_array->Size());


	// create an empty set of outer references for statistics derivation
	CColRefSet *outer_refs = GPOS_NEW(mp) CColRefSet(mp);

	// join statistics objects one by one using relevant predicates in given scalar expression
	const ULONG num_stats = statistics_array->Size();
	IStatistics *stats = (*statistics_array)[0]->CopyStats(mp);
	CDouble num_rows_outer = stats->Rows();

	for (ULONG i = 1; i < num_stats; i++)
	{
		IStatistics *current_stats = (*statistics_array)[i];

		CColRefSetArray *output_colrefsets = GPOS_NEW(mp) CColRefSetArray(mp);
		output_colrefsets->Append(stats->GetColRefSet(mp));
		output_colrefsets->Append(current_stats->GetColRefSet(mp));

		CStatsPred *unsupported_pred_stats = NULL;
		CStatsPredJoinArray *join_preds_stats = CStatsPredUtils::ExtractJoinStatsFromJoinPredArray
				(
						mp,
						expr,
						output_colrefsets,
						outer_refs,
						&unsupported_pred_stats
				);
		IStatistics *new_stats = NULL;
		if (left_outer_join)
		{
			new_stats = stats->CalcLOJoinStats(mp, current_stats, join_preds_stats);
		}
		else
		{
			new_stats = stats->CalcInnerJoinStats(mp, current_stats, join_preds_stats);
		}
		stats->Release();
		stats = new_stats;

		if (NULL != unsupported_pred_stats)
		{
			// apply the unsupported join filters as a filter on top of the join results.
			// TODO,  June 13 2014 we currently only cap NDVs for filters
			// immediately on top of tables.
			IStatistics *stats_after_join_filter = CFilterStatsProcessor::MakeStatsFilter(mp, dynamic_cast<CStatistics *>(stats), unsupported_pred_stats, false /* do_cap_NDVs */);

			// If it is outer join and the cardinality after applying the unsupported join
			// filters is less than the cardinality of outer child, we don't use this stats.
			// Because we need to make sure that Card(LOJ) >= Card(Outer child of LOJ).
			if (left_outer_join && stats_after_join_filter->Rows() < num_rows_outer)
			{
				stats_after_join_filter->Release();
			}
			else
			{
				stats->Release();
				stats = stats_after_join_filter;
			}

			unsupported_pred_stats->Release();
		}

		join_preds_stats->Release();
		output_colrefsets->Release();
	}

	// clean up
	outer_refs->Release();

	return stats;
}


// main driver to generate join stats
CStatistics *
CJoinStatsProcessor::SetResultingJoinStats
		(
		CMemoryPool *mp,
		CStatisticsConfig *stats_config,
		const IStatistics *outer_stats_input,
		const IStatistics *inner_stats_input,
										   CStatsPredJoinArray *join_pred_stats_info,
		IStatistics::EStatsJoinType join_type,
		BOOL DoIgnoreLASJHistComputation
		)
{
	GPOS_ASSERT(NULL != mp);
	GPOS_ASSERT(NULL != inner_stats_input);
	GPOS_ASSERT(NULL != outer_stats_input);

	GPOS_ASSERT(NULL != join_pred_stats_info);

	BOOL IsLASJ = (IStatistics::EsjtLeftAntiSemiJoin == join_type);
	BOOL semi_join = IStatistics::IsSemiJoin(join_type);

	// Extract stat objects for inner and outer child.
	// Historically, IStatistics was meant to have multiple derived classes
	// However, currently only CStatistics implements IStatistics
	// Until this changes, the interfaces have been designed to take IStatistics as parameters
	// In the future, IStatistics should be removed, as it is not being utilized as designed
	const CStatistics *outer_stats = dynamic_cast<const CStatistics *> (outer_stats_input);
	const CStatistics *inner_side_stats = dynamic_cast<const CStatistics *> (inner_stats_input);

	// create hash map from colid -> histogram for resultant structure
	UlongToHistogramMap *result_col_hist_mapping = GPOS_NEW(mp) UlongToHistogramMap(mp);

	// build a bitset with all join columns
	CBitSet *join_colids = GPOS_NEW(mp) CBitSet(mp);
	for (ULONG i = 0; i < join_pred_stats_info->Size(); i++)
	{
		CStatsPredJoin *join_stats = (*join_pred_stats_info)[i];

		(void) join_colids->ExchangeSet(join_stats->ColIdOuter());
		if (!semi_join)
		{
			(void) join_colids->ExchangeSet(join_stats->ColIdInner());
		}
	}

	// histograms on columns that do not appear in join condition will
	// be copied over to the result structure
	outer_stats->AddNotExcludedHistograms(mp, join_colids, result_col_hist_mapping);
	if (!semi_join)
	{
		inner_side_stats->AddNotExcludedHistograms(mp, join_colids, result_col_hist_mapping);
	}

	CDoubleArray *join_conds_scale_factors = GPOS_NEW(mp) CDoubleArray(mp);
	const ULONG num_join_conds = join_pred_stats_info->Size();

	BOOL output_is_empty = false;
	CDouble num_join_rows = 0;
	// iterate over join's predicate(s)
	for (ULONG i = 0; i < num_join_conds; i++)
	{
		CStatsPredJoin *pred_info = (*join_pred_stats_info)[i];
		CStatsPred::EStatsCmpType stats_cmp_type = pred_info->GetCmpType();
		ULONG colid1 = pred_info->ColIdOuter();
		ULONG colid2 = pred_info->ColIdInner();
		GPOS_ASSERT(colid1 != colid2);
		// find the histograms corresponding to the two columns
		const CHistogram *outer_histogram = outer_stats->GetHistogram(colid1);
		// are column id1 and 2 always in the order of outer inner?
		const CHistogram *inner_histogram = inner_side_stats->GetHistogram(colid2);
		GPOS_ASSERT(NULL != outer_histogram);
		GPOS_ASSERT(NULL != inner_histogram);
		BOOL is_input_empty = CStatistics::IsEmptyJoin(outer_stats, inner_side_stats, IsLASJ);
		CDouble local_scale_factor(1.0);
		CHistogram *outer_histogram_after = NULL;
		CHistogram *inner_histogram_after = NULL;

		// When we have any form of equi join with join condition of type f(a)=b,
		// we calculate the NDV of such a join as NDV(b) ( from Selinger et al.)
		if (CStatsPred::EstatscmptEqNDVOuter == stats_cmp_type)
		{
			inner_histogram = outer_histogram;
		}
		else if (CStatsPred::EstatscmptEqNDVInner == stats_cmp_type)
		{
			outer_histogram = inner_histogram;
		}

		JoinHistograms
		(
		mp,
		outer_histogram,
		inner_histogram,
		pred_info,
		outer_stats->Rows(),
		inner_side_stats->Rows(),
		&outer_histogram_after,
		&inner_histogram_after,
		&local_scale_factor,
		is_input_empty,
		join_type,
		DoIgnoreLASJHistComputation
		);


		output_is_empty = JoinStatsAreEmpty(outer_stats->IsEmpty(), output_is_empty, outer_histogram, inner_histogram, outer_histogram_after, join_type);
		
		CStatisticsUtils::AddHistogram(mp, colid1, outer_histogram_after, result_col_hist_mapping);
		if (!semi_join)
		{
			CStatisticsUtils::AddHistogram(mp, colid2, inner_histogram_after, result_col_hist_mapping);
		}

		GPOS_DELETE(outer_histogram_after);
		GPOS_DELETE(inner_histogram_after);

		join_conds_scale_factors->Append(GPOS_NEW(mp) CDouble(local_scale_factor));
	}


	num_join_rows = CStatistics::MinRows;
	if (!output_is_empty)
	{
		num_join_rows = CalcJoinCardinality(stats_config, outer_stats->Rows(), inner_side_stats->Rows(), join_conds_scale_factors, join_type);
	}

	// clean up
	join_conds_scale_factors->Release();
	join_colids->Release();

	UlongToDoubleMap *col_width_mapping_result = outer_stats->CopyWidths(mp);
	if (!semi_join)
	{
		inner_side_stats->CopyWidthsInto(mp, col_width_mapping_result);
	}

	// create an output stats object
	CStatistics *join_stats = GPOS_NEW(mp) CStatistics
			(
					mp,
					result_col_hist_mapping,
					col_width_mapping_result,
					num_join_rows,
					output_is_empty,
					outer_stats->GetNumberOfPredicates()
			);

	// In the output statistics object, the upper bound source cardinality of the join column
	// cannot be greater than the upper bound source cardinality information maintained in the input
	// statistics object. Therefore we choose CStatistics::EcbmMin the bounding method which takes
	// the minimum of the cardinality upper bound of the source column (in the input hash map)
	// and estimated join cardinality.

	CStatisticsUtils::ComputeCardUpperBounds(mp, outer_stats, join_stats, num_join_rows, CStatistics::EcbmMin /* card_bounding_method */);
	if (!semi_join)
	{
		CStatisticsUtils::ComputeCardUpperBounds(mp, inner_side_stats, join_stats, num_join_rows, CStatistics::EcbmMin /* card_bounding_method */);
	}

	return join_stats;
}


// return join cardinality based on scaling factor and join type
CDouble
CJoinStatsProcessor::CalcJoinCardinality
		(
		CStatisticsConfig *stats_config,
		CDouble left_num_rows,
		CDouble right_num_rows,
		CDoubleArray *join_conds_scale_factors,
		IStatistics::EStatsJoinType join_type
		)
{
	GPOS_ASSERT(NULL != stats_config);
	GPOS_ASSERT(NULL != join_conds_scale_factors);

	CDouble scale_factor = CScaleFactorUtils::CumulativeJoinScaleFactor(stats_config, join_conds_scale_factors);
	CDouble cartesian_product_num_rows = left_num_rows * right_num_rows;

	if (IStatistics::EsjtLeftAntiSemiJoin == join_type || IStatistics::EsjtLeftSemiJoin == join_type)
	{
		CDouble rows = left_num_rows;

		if (IStatistics::EsjtLeftAntiSemiJoin == join_type)
		{
			rows = left_num_rows / scale_factor;
		}
		else
		{
			// semi join results cannot exceed size of outer side
			rows = std::min(left_num_rows.Get(), (cartesian_product_num_rows / scale_factor).Get());
		}

		return std::max(DOUBLE(1.0), rows.Get());
	}

	GPOS_ASSERT(CStatistics::MinRows <= scale_factor);

	return std::max(CStatistics::MinRows.Get(), (cartesian_product_num_rows / scale_factor).Get());
}



// check if the join statistics object is empty output based on the input
// histograms and the join histograms
BOOL
CJoinStatsProcessor::JoinStatsAreEmpty
		(
		BOOL outer_is_empty,
		BOOL output_is_empty,
		const CHistogram *outer_histogram,
		const CHistogram *inner_histogram,
		CHistogram *join_histogram,
		IStatistics::EStatsJoinType join_type
		)
{
	GPOS_ASSERT(NULL != outer_histogram);
	GPOS_ASSERT(NULL != inner_histogram);
	GPOS_ASSERT(NULL != join_histogram);
	BOOL IsLASJ = IStatistics::EsjtLeftAntiSemiJoin == join_type;
	return output_is_empty ||
		   (!IsLASJ && outer_is_empty) ||
		   (!outer_histogram->IsEmpty() && !inner_histogram->IsEmpty() && join_histogram->IsEmpty());
}

// Derive statistics for join operation given array of statistics object
IStatistics *
CJoinStatsProcessor::DeriveJoinStats
		(
		CMemoryPool *mp,
		CExpressionHandle &exprhdl,
		IStatisticsArray *stats_ctxt
		)
{
	GPOS_ASSERT(CLogical::EspNone < CLogical::PopConvert(exprhdl.Pop())->Esp(exprhdl));

	IStatisticsArray *statistics_array = GPOS_NEW(mp) IStatisticsArray(mp);
	const ULONG arity = exprhdl.Arity();
	for (ULONG i = 0; i < arity - 1; i++)
	{
		IStatistics *child_stats = exprhdl.Pstats(i);
		child_stats->AddRef();
		statistics_array->Append(child_stats);
	}

	CExpression *join_pred_expr = NULL;
	if (exprhdl.GetDrvdScalarProps(arity - 1)->FHasSubquery())
	{
		// in case of subquery in join predicate, assume join condition is True
		join_pred_expr = CUtils::PexprScalarConstBool(mp, true /*value*/);
	}
	else
	{
		// remove implied predicates from join condition to avoid cardinality under-estimation
		join_pred_expr = CPredicateUtils::PexprRemoveImpliedConjuncts(mp, exprhdl.PexprScalarChild(arity - 1), exprhdl);
	}

	// split join predicate into local predicate and predicate involving outer references
	CExpression *local_expr = NULL;
	CExpression *expr_with_outer_refs = NULL;

	// get outer references from expression handle
	CColRefSet *outer_refs = exprhdl.GetRelationalProperties()->PcrsOuter();

	CPredicateUtils::SeparateOuterRefs(mp, join_pred_expr, outer_refs, &local_expr, &expr_with_outer_refs);
	join_pred_expr->Release();

	COperator::EOperatorId op_id = exprhdl.Pop()->Eopid();
	GPOS_ASSERT(COperator::EopLogicalLeftOuterJoin == op_id ||
				COperator::EopLogicalInnerJoin == op_id ||
				COperator::EopLogicalNAryJoin == op_id);

	// we use Inner Join semantics here except in the case of Left Outer Join
	IStatistics::EStatsJoinType join_type = IStatistics::EsjtInnerJoin;
	if (COperator::EopLogicalLeftOuterJoin == op_id)
	{
		join_type = IStatistics::EsjtLeftOuterJoin;
	}

	// derive stats based on local join condition
	IStatistics *join_stats = CJoinStatsProcessor::CalcAllJoinStats(mp, statistics_array, local_expr, join_type);

	if (exprhdl.HasOuterRefs() && 0 < stats_ctxt->Size())
	{
		// derive stats based on outer references
		IStatistics *stats = DeriveStatsWithOuterRefs(mp, exprhdl, expr_with_outer_refs, join_stats, stats_ctxt, join_type);
		join_stats->Release();
		join_stats = stats;
	}

	local_expr->Release();
	expr_with_outer_refs->Release();

	statistics_array->Release();

	return join_stats;
}


// Derives statistics when the scalar expression contains one or more outer references.
// This stats derivation mechanism passes around a context array onto which
// operators append their stats objects as they get derived. The context array is
// filled as we derive stats on the children of a given operator. This gives each
// operator access to the stats objects of its previous siblings as well as to the outer
// operators in higher levels.
// For example, in this expression:
//
// JOIN
//   |--Get(R)
//   +--Select(R.r=S.s)
//       +-- Get(S)
//
// We start by deriving stats on JOIN's left child (Get(R)) and append its
// stats to the context. Then, we call stats derivation on JOIN's right child
// (SELECT), passing it the current context.  This gives SELECT access to the
// histogram on column R.r--which is an outer reference in this example. After
// JOIN's children's stats are computed, JOIN combines them into a parent stats
// object, which is passed upwards to JOIN's parent. This mechanism gives any
// operator access to the histograms of outer references defined anywhere in
// the logical tree. For example, we also support the case where outer
// reference R.r is defined two levels upwards:
//
//    JOIN
//      |---Get(R)
//      +--JOIN
//         |--Get(T)
//         +--Select(R.r=S.s)
//               +--Get(S)
//
//
//
// The next step is to combine the statistics objects of the outer references
// with those of the local columns. You can think of this as a correlated
// expression, where for each outer tuple, we need to extract the outer ref
// value and re-execute the inner expression using the current outer ref value.
// This has the same semantics as a Join from a statistics perspective.
//
// We pull statistics for outer references from the passed statistics context,
// using Join statistics derivation in this case.
//
// For example:
//
// 			Join
// 			 |--Get(R)
// 			 +--Join
// 				|--Get(S)
// 				+--Select(T.t=R.r)
// 					+--Get(T)
//
// when deriving statistics on 'Select(T.t=R.r)', we join T with the cross
// product (R x S) based on the condition (T.t=R.r)
IStatistics *
CJoinStatsProcessor::DeriveStatsWithOuterRefs
		(
		CMemoryPool *mp,
		CExpressionHandle &
#ifdef GPOS_DEBUG
		exprhdl // handle attached to the logical expression we want to derive stats for
#endif // GPOS_DEBUG
,
		CExpression *expr, // scalar condition to be used for stats derivation
		IStatistics *stats, // statistics object of the attached expression
		IStatisticsArray *all_outer_stats, // array of stats objects where outer references are defined
		IStatistics::EStatsJoinType join_type
		)
{
	GPOS_ASSERT(exprhdl.HasOuterRefs() && "attached expression does not have outer references");
	GPOS_ASSERT(NULL != expr);
	GPOS_ASSERT(NULL != stats);
	GPOS_ASSERT(NULL != all_outer_stats);
	GPOS_ASSERT(0 < all_outer_stats->Size());
	GPOS_ASSERT(IStatistics::EstiSentinel != join_type);

	// join outer stats object based on given scalar expression,
	// we use inner join semantics here to consider all relevant combinations of outer tuples
	IStatistics *outer_stats = CJoinStatsProcessor::CalcAllJoinStats(mp, all_outer_stats, expr, IStatistics::EsjtInnerJoin);
	CDouble num_rows_outer = outer_stats->Rows();

	// join passed stats object and outer stats based on the passed join type
	IStatisticsArray *statistics_array = GPOS_NEW(mp) IStatisticsArray(mp);
	statistics_array->Append(outer_stats);
	stats->AddRef();
	statistics_array->Append(stats);
	IStatistics *result_join_stats = CJoinStatsProcessor::CalcAllJoinStats(mp, statistics_array, expr, join_type);
	statistics_array->Release();

	// scale result using cardinality of outer stats and set number of rebinds of returned stats
	IStatistics *result_stats = result_join_stats->ScaleStats(mp, CDouble(1.0/num_rows_outer));
	result_stats->SetRebinds(num_rows_outer);
	result_join_stats->Release();

	return result_stats;
}

// EOF
