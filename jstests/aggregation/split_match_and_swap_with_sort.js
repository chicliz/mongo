// When a $sort is followed by a $match, the pipeline optimizer should be able to move the match in
// front of the $sort, so as to reduce the number of documents that need to be sorted.
//
// @tags: [
//   # Wrapping the pipeline in a $facet will prevent its $match from getting absorbed by the query
//   # system, which is what we are testing for.
//   do_not_wrap_aggregations_in_facets,
//   # Likewise, splitting up the pipeline for sharding may also prevent the $match from getting
//   # absorbed.
//   assumes_unsharded_collection,
//   # Don't disable the thing we are specifically testing for!
//   requires_pipeline_optimization,
//   sbe_incompatible,
// ]
load('jstests/libs/analyze_plan.js');

(function() {
"use strict";

const coll = db.getSiblingDB("split_match_and_swap_with_sort")["test"];
coll.drop();

assert.commandWorked(
    coll.insert([{_id: 1, a: 1, b: 3}, {_id: 2, a: 2, b: 2}, {_id: 3, a: 3, b: 1}]));

{
    const pipeline = [{$sort: {b: 1}}, {$match: {a: {$ne: 2}}}];

    assert.eq([{_id: 3, a: 3, b: 1}, {_id: 1, a: 1, b: 3}], coll.aggregate(pipeline).toArray());

    const pipelineExplained = coll.explain().aggregate(pipeline);
    const collScanStage = getPlanStage(pipelineExplained, "COLLSCAN");

    // After moving the $match to the front of the pipeline, we expect the pipeline optimizer to
    // push it down into the PlanExecutor, so that it becomes a filter on the COLLSCAN.
    assert.neq(null, collScanStage, pipelineExplained);
    assert.eq({a: {"$not": {"$eq": 2}}}, collScanStage.filter, collScanStage);
}

{
    // Note that a $expr cannot be moved. However, the optimizer can split the non-$expr part of the
    // $match and move that to be in front of the $sort.
    const pipeline =
        [{$sort: {b: 1}}, {$match: {$and: [{a: {$ne: 2}}, {$expr: {$ne: ["$a", "$b"]}}]}}];

    assert.eq([{_id: 3, a: 3, b: 1}, {_id: 1, a: 1, b: 3}], coll.aggregate(pipeline).toArray());

    const pipelineExplained = coll.explain().aggregate(pipeline);
    const collScanStage = getAggPlanStage(pipelineExplained, "COLLSCAN");
    assert.neq(null, collScanStage, pipelineExplained);
    assert.eq({a: {"$not": {"$eq": 2}}}, collScanStage.filter, collScanStage);
}

{
    // SERVER-46233: Normally a $or at the root of a $match expression prevents it from getting
    // split. However, When the $or has only one child, the optimizer should be able to eliminate
    // the $or and then recognize that the simplified result _is_ eligible to be split.
    const pipeline =
        [{$sort: {b: 1}}, {$match: {$or: [{$and: [{a: {$ne: 2}}, {$expr: {$ne: ["$a", "$b"]}}]}]}}];

    assert.eq([{_id: 3, a: 3, b: 1}, {_id: 1, a: 1, b: 3}], coll.aggregate(pipeline).toArray());

    const pipelineExplained = coll.explain().aggregate(pipeline);
    const collScanStage = getAggPlanStage(pipelineExplained, "COLLSCAN");
    assert.neq(null, collScanStage, pipelineExplained);
    assert.eq({a: {"$not": {"$eq": 2}}}, collScanStage.filter, collScanStage);
}
}());