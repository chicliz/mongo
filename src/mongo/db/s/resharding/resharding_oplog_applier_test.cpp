/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <fmt/format.h>

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator_interface.h"
#include "mongo/db/s/resharding/resharding_oplog_applier.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/sharding_mongod_test_fixture.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

using namespace fmt::literals;

class OplogIteratorMock : public ReshardingDonorOplogIteratorInterface {
public:
    OplogIteratorMock(std::queue<repl::OplogEntry> oplogToReturn)
        : _oplogToReturn(std::move(oplogToReturn)) {}

    Future<boost::optional<repl::OplogEntry>> getNext(OperationContext* opCtx) override {
        boost::optional<repl::OplogEntry> ret;
        if (!_oplogToReturn.empty()) {
            if (_oplogToReturn.size() <= 1 && _doThrow) {
                uasserted(ErrorCodes::InternalError, "OplogIteratorMock simulating error");
            }

            ret = _oplogToReturn.front();
            _oplogToReturn.pop();
        }

        return Future<boost::optional<repl::OplogEntry>>::makeReady(ret);
    }

    /**
     * Makes this iterator throw an error when calling getNext with only a single item left in the
     * buffer. This allows simulating an exception being thrown at different points in time.
     */
    void setThrowWhenSingleItem() {
        _doThrow = true;
    }

    bool hasMore() const override {
        return !_oplogToReturn.empty();
    }

private:
    std::queue<repl::OplogEntry> _oplogToReturn;
    bool _doThrow{false};
};

class ReshardingOplogApplierTest : public ShardingMongodTestFixture {
public:
    void setUp() override {
        ShardingMongodTestFixture::setUp();

        serverGlobalParams.clusterRole = ClusterRole::ShardServer;

        auto mockNetwork = std::make_unique<executor::NetworkInterfaceMock>();
        _executor = executor::makeThreadPoolTestExecutor(std::move(mockNetwork));
        _executor->startup();

        _writerPool = repl::makeReplWriterPool(kWriterPoolSize);

        uassertStatusOK(createCollection(operationContext(),
                                         kAppliedToNs.db().toString(),
                                         BSON("create" << kAppliedToNs.coll())));
    }

    ThreadPool* writerPool() {
        return _writerPool.get();
    }

    repl::OplogEntry makeOplog(const repl::OpTime& opTime,
                               repl::OpTypeEnum opType,
                               const BSONObj& obj1,
                               const boost::optional<BSONObj> obj2) {
        return makeOplog(opTime, opType, obj1, obj2, {}, boost::none);
    }

    repl::OplogEntry makeOplog(const repl::OpTime& opTime,
                               repl::OpTypeEnum opType,
                               const BSONObj& obj1,
                               const boost::optional<BSONObj> obj2,
                               const OperationSessionInfo& sessionInfo,
                               const boost::optional<StmtId>& statementId) {
        ReshardingDonorOplogId id(opTime.getTimestamp(), opTime.getTimestamp());
        return repl::OplogEntry(opTime,
                                boost::none /* hash */,
                                opType,
                                kCrudNs,
                                kCrudUUID,
                                false /* fromMigrate */,
                                0 /* version */,
                                obj1,
                                obj2,
                                sessionInfo,
                                boost::none /* upsert */,
                                {} /* date */,
                                statementId,
                                boost::none /* prevWrite */,
                                boost::none /* preImage */,
                                boost::none /* postImage */,
                                kMyShardId,
                                Value(id.toBSON()));
    }

    const NamespaceString& oplogNs() {
        return kOplogNs;
    }

    const NamespaceString& crudNs() {
        return kCrudNs;
    }

    const UUID& crudUUID() {
        return kCrudUUID;
    }

    const NamespaceString& appliedToNs() {
        return kAppliedToNs;
    }

    executor::ThreadPoolTaskExecutor* getExecutor() {
        return _executor.get();
    }

    const ReshardingSourceId& sourceId() {
        return _sourceId;
    }

protected:
    static constexpr int kWriterPoolSize = 4;
    const NamespaceString kOplogNs{"config.localReshardingOplogBuffer.xxx.yyy"};
    const NamespaceString kCrudNs{"foo.bar"};
    const UUID kCrudUUID = UUID::gen();
    const NamespaceString kAppliedToNs{"foo", "system.resharding.{}"_format(kCrudUUID.toString())};
    const ShardId kMyShardId{"shard1"};
    UUID _crudNsUuid = UUID::gen();

    const ReshardingSourceId _sourceId{UUID::gen(), kMyShardId};

    std::unique_ptr<executor::ThreadPoolTaskExecutor> _executor;
    std::unique_ptr<ThreadPool> _writerPool;
};

TEST_F(ReshardingOplogApplierTest, NothingToIterate) {
    std::queue<repl::OplogEntry> crudOps;
    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));

    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(6, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    future = applier.applyUntilDone();
    future.get();
}

TEST_F(ReshardingOplogApplierTest, ApplyBasicCrud) {
    std::queue<repl::OplogEntry> crudOps;
    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 2),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                           repl::OpTypeEnum::kUpdate,
                           BSON("$set" << BSON("x" << 1)),
                           BSON("_id" << 2)));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                           repl::OpTypeEnum::kDelete,
                           BSON("_id" << 1),
                           boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(6, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), doc);

    future = applier.applyUntilDone();
    future.get();

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2 << "x" << 1), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, InsertTypeOplogAppliedInMultipleBatches) {
    std::queue<repl::OplogEntry> crudOps;

    for (int x = 0; x < 20; x++) {
        crudOps.push(makeOplog(repl::OpTime(Timestamp(x, 3), 1),
                               repl::OpTypeEnum::kInsert,
                               BSON("_id" << x),
                               boost::none));
    }

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(8, 3),
                                   std::move(iterator),
                                   3 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    DBDirectClient client(operationContext());

    for (int x = 0; x < 9; x++) {
        auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << x));
        ASSERT_BSONOBJ_EQ(BSON("_id" << x), doc);
    }

    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 9));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getTs());

    future = applier.applyUntilDone();
    future.get();

    for (int x = 0; x < 19; x++) {
        doc = client.findOne(appliedToNs().ns(), BSON("_id" << x));
        ASSERT_BSONOBJ_EQ(BSON("_id" << x), doc);
    }

    progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(19, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(19, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, ErrorDuringBatchApplyCloningPhase) {
    std::queue<repl::OplogEntry> crudOps;
    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                           repl::OpTypeEnum::kUpdate,
                           BSON("$invalidOperator" << BSON("x" << 1)),
                           BSON("_id" << 1)));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(7, 3),
                                   std::move(iterator),
                                   4 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::FailedToParse);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, ErrorDuringBatchApplyCatchUpPhase) {
    std::queue<repl::OplogEntry> crudOps;
    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 2),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 3),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                           repl::OpTypeEnum::kUpdate,
                           BSON("$invalidOperator" << BSON("x" << 1)),
                           BSON("_id" << 1)));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(6, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    future = applier.applyUntilDone();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::FailedToParse);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 3), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingFirstOplogCloningPhase) {
    std::queue<repl::OplogEntry> crudOps;
    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    iterator->setThrowWhenSingleItem();

    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(6, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingFirstOplogCatchUpPhase) {
    std::queue<repl::OplogEntry> crudOps;
    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 2),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 3),
                           boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    iterator->setThrowWhenSingleItem();

    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(5, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    future = applier.applyUntilDone();
    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingFirstBatchCloningPhase) {
    std::queue<repl::OplogEntry> crudOps;
    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 2),
                           boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    iterator->setThrowWhenSingleItem();

    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(8, 3),
                                   std::move(iterator),
                                   4 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingFirstBatchCatchUpPhase) {
    std::queue<repl::OplogEntry> crudOps;
    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 2),
                           boost::none));

    crudOps.push(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 3),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 2),
                           boost::none));
    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    iterator->setThrowWhenSingleItem();

    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(6, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    future = applier.applyUntilDone();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingSecondBatchCloningPhase) {
    std::queue<repl::OplogEntry> crudOps;
    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 2),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 3),
                           boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    iterator->setThrowWhenSingleItem();

    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(7, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingSecondBatchCatchUpPhase) {
    std::queue<repl::OplogEntry> crudOps;
    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 2),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 3),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 4),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(9, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 5),
                           boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    iterator->setThrowWhenSingleItem();

    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(6, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    future = applier.applyUntilDone();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 3), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 4));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 4), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 5));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, ExecutorIsShutDownCloningPhase) {
    std::queue<repl::OplogEntry> crudOps;
    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(5, 3),
                                   std::move(iterator),
                                   4 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    getExecutor()->shutdown();

    auto future = applier.applyUntilCloneFinishedTs();
    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::ShutdownInProgress);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, ExecutorIsShutDownCatchUpPhase) {
    std::queue<repl::OplogEntry> crudOps;
    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 2),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 3),
                           boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(5, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    getExecutor()->shutdown();
    future = applier.applyUntilDone();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::ShutdownInProgress);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, WriterPoolIsShutDownCloningPhase) {
    std::queue<repl::OplogEntry> crudOps;
    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(5, 3),
                                   std::move(iterator),
                                   4 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    writerPool()->shutdown();

    auto future = applier.applyUntilCloneFinishedTs();
    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::ShutdownInProgress);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, WriterPoolIsShutDownCatchUpPhase) {
    std::queue<repl::OplogEntry> crudOps;
    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 2),
                           boost::none));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 3),
                           boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(5, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    writerPool()->shutdown();
    future = applier.applyUntilDone();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::ShutdownInProgress);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

class ReshardingOplogApplierRetryableTest : public ReshardingOplogApplierTest {
public:
    void setUp() override {
        ReshardingOplogApplierTest::setUp();

        repl::StorageInterface::set(operationContext()->getServiceContext(),
                                    std::make_unique<repl::StorageInterfaceImpl>());
        MongoDSessionCatalog::onStepUp(operationContext());
    }

    static repl::OpTime insertRetryableOplog(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             UUID uuid,
                                             const LogicalSessionId& lsid,
                                             TxnNumber txnNumber,
                                             StmtId stmtId,
                                             repl::OpTime prevOpTime) {
        repl::MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        oplogEntry.setNss(nss);
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(BSON("TestValue" << 0));
        oplogEntry.setWallClockTime(Date_t::now());
        if (stmtId != kUninitializedStmtId) {
            oplogEntry.setSessionId(lsid);
            oplogEntry.setTxnNumber(txnNumber);
            oplogEntry.setStatementId(stmtId);
            oplogEntry.setPrevWriteOpTimeInTransaction(prevOpTime);
        }
        return repl::logOp(opCtx, &oplogEntry);
    }

    void writeTxnRecord(const LogicalSessionId& lsid,
                        const TxnNumber& txnNum,
                        StmtId stmtId,
                        repl::OpTime prevOpTime,
                        boost::optional<DurableTxnStateEnum> txnState) {
        auto newClient = operationContext()->getServiceContext()->makeClient("testWriteTxnRecord");
        AlternativeClientRegion acr(newClient);
        auto scopedOpCtx = cc().makeOperationContext();
        auto opCtx = scopedOpCtx.get();

        opCtx->setLogicalSessionId(lsid);
        opCtx->setTxnNumber(txnNum);
        OperationContextSession scopedSession(opCtx);

        const auto session = OperationContextSession::get(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.refreshFromStorageIfNeeded(opCtx);
        txnParticipant.beginOrContinue(opCtx, txnNum, boost::none, boost::none);

        AutoGetCollection autoColl(opCtx, kCrudNs, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        const auto opTime = insertRetryableOplog(
            opCtx, kCrudNs, kCrudUUID, session->getSessionId(), txnNum, stmtId, prevOpTime);

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setSessionId(session->getSessionId());
        sessionTxnRecord.setTxnNum(txnNum);
        sessionTxnRecord.setLastWriteOpTime(opTime);
        sessionTxnRecord.setLastWriteDate(Date_t::now());
        sessionTxnRecord.setState(txnState);
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx, {stmtId}, sessionTxnRecord);
        wuow.commit();
    }

    bool isWriteAlreadyExecuted(const OperationSessionInfo& session, StmtId stmtId) {
        auto newClient =
            operationContext()->getServiceContext()->makeClient("testCheckStmtExecuted");
        AlternativeClientRegion acr(newClient);
        auto scopedOpCtx = cc().makeOperationContext();
        auto opCtx = scopedOpCtx.get();

        opCtx->setLogicalSessionId(*session.getSessionId());
        OperationContextSession scopedSession(opCtx);

        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.refreshFromStorageIfNeeded(opCtx);
        txnParticipant.beginOrContinue(opCtx, *session.getTxnNumber(), boost::none, boost::none);

        return txnParticipant.checkStatementExecuted(opCtx, stmtId).is_initialized();
    }
};

TEST_F(ReshardingOplogApplierRetryableTest, CrudWithEmptyConfigTransactions) {
    std::queue<repl::OplogEntry> crudOps;

    OperationSessionInfo session1;
    session1.setSessionId(makeLogicalSessionIdForTest());
    session1.setTxnNumber(1);

    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none,
                           session1,
                           1));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 2),
                           boost::none,
                           session1,
                           2));

    OperationSessionInfo session2;
    session2.setSessionId(makeLogicalSessionIdForTest());
    session2.setTxnNumber(1);

    crudOps.push(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                           repl::OpTypeEnum::kUpdate,
                           BSON("$set" << BSON("x" << 1)),
                           BSON("_id" << 2),
                           session2,
                           1));

    OperationSessionInfo session3;
    session3.setSessionId(makeLogicalSessionIdForTest());
    session3.setTxnNumber(1);

    crudOps.push(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                           repl::OpTypeEnum::kDelete,
                           BSON("_id" << 1),
                           boost::none,
                           session3,
                           1));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(6, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    future = applier.applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2 << "x" << 1), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getTs());

    ASSERT_TRUE(isWriteAlreadyExecuted(session1, 1));
    ASSERT_TRUE(isWriteAlreadyExecuted(session1, 2));
    ASSERT_TRUE(isWriteAlreadyExecuted(session2, 1));
    ASSERT_TRUE(isWriteAlreadyExecuted(session3, 1));

    ASSERT_FALSE(isWriteAlreadyExecuted(session2, 2));
    ASSERT_FALSE(isWriteAlreadyExecuted(session3, 2));
}

TEST_F(ReshardingOplogApplierRetryableTest, MultipleTxnSameLsidInOneBatch) {
    std::queue<repl::OplogEntry> crudOps;

    OperationSessionInfo session1;
    session1.setSessionId(makeLogicalSessionIdForTest());
    session1.setTxnNumber(1);

    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none,
                           session1,
                           1));
    crudOps.push(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 2),
                           boost::none,
                           session1,
                           2));

    OperationSessionInfo session2;
    session2.setSessionId(makeLogicalSessionIdForTest());
    session2.setTxnNumber(1);

    crudOps.push(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 3),
                           boost::none,
                           session2,
                           1));

    session1.setTxnNumber(2);

    crudOps.push(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 4),
                           boost::none,
                           session1,
                           21));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(6, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    future = applier.applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 3), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 4));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 4), doc);

    ASSERT_TRUE(isWriteAlreadyExecuted(session1, 21));
    ASSERT_TRUE(isWriteAlreadyExecuted(session2, 1));
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWithLowerExistingTxn) {
    auto lsid = makeLogicalSessionIdForTest();

    writeTxnRecord(lsid, 2, 1, {}, boost::none);

    std::queue<repl::OplogEntry> crudOps;

    OperationSessionInfo session;
    session.setSessionId(lsid);
    session.setTxnNumber(5);

    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none,
                           session,
                           21));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(6, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    future = applier.applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_TRUE(isWriteAlreadyExecuted(session, 21));
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWithHigherExistingTxnNum) {
    auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber existingTxnNum = 20;
    const StmtId existingStmtId = 1;
    writeTxnRecord(lsid, existingTxnNum, existingStmtId, {}, boost::none);

    OperationSessionInfo session;
    const TxnNumber incomingTxnNum = 15;
    const StmtId incomingStmtId = 21;
    session.setSessionId(lsid);
    session.setTxnNumber(incomingTxnNum);

    std::queue<repl::OplogEntry> crudOps;

    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none,
                           session,
                           incomingStmtId));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(6, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    future = applier.applyUntilDone();
    future.get();

    // Op should always be applied, even if session info was not compatible.
    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_THROWS_CODE(isWriteAlreadyExecuted(session, incomingStmtId),
                       DBException,
                       ErrorCodes::TransactionTooOld);

    // Check that original txn info is intact.
    OperationSessionInfo origSession;
    origSession.setSessionId(lsid);
    origSession.setTxnNumber(existingTxnNum);

    ASSERT_TRUE(isWriteAlreadyExecuted(origSession, existingStmtId));
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWithLowerExistingTxnNum) {
    auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber existingTxnNum = 20;
    const StmtId existingStmtId = 1;
    writeTxnRecord(lsid, existingTxnNum, existingStmtId, {}, boost::none);

    OperationSessionInfo session;
    const TxnNumber incomingTxnNum = 25;
    const StmtId incomingStmtId = 21;
    session.setSessionId(lsid);
    session.setTxnNumber(incomingTxnNum);

    std::queue<repl::OplogEntry> crudOps;

    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none,
                           session,
                           incomingStmtId));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(6, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    future = applier.applyUntilDone();
    future.get();

    // Op should always be applied, even if session info was not compatible.
    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_TRUE(isWriteAlreadyExecuted(session, incomingStmtId));
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWithEqualExistingTxnNum) {
    auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber existingTxnNum = 20;
    const StmtId existingStmtId = 1;
    writeTxnRecord(lsid, existingTxnNum, existingStmtId, {}, boost::none);

    OperationSessionInfo session;
    const TxnNumber incomingTxnNum = existingTxnNum;
    const StmtId incomingStmtId = 21;
    session.setSessionId(lsid);
    session.setTxnNumber(incomingTxnNum);

    std::queue<repl::OplogEntry> crudOps;

    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none,
                           session,
                           incomingStmtId));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(6, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    future = applier.applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_TRUE(isWriteAlreadyExecuted(session, incomingStmtId));
    ASSERT_TRUE(isWriteAlreadyExecuted(session, existingStmtId));
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWithStmtIdAlreadyExecuted) {
    auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber existingTxnNum = 20;
    const StmtId existingStmtId = 1;
    writeTxnRecord(lsid, existingTxnNum, existingStmtId, {}, boost::none);

    OperationSessionInfo session;
    const TxnNumber incomingTxnNum = existingTxnNum;
    const StmtId incomingStmtId = existingStmtId;
    session.setSessionId(lsid);
    session.setTxnNumber(incomingTxnNum);

    std::queue<repl::OplogEntry> crudOps;

    crudOps.push(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                           repl::OpTypeEnum::kInsert,
                           BSON("_id" << 1),
                           boost::none,
                           session,
                           incomingStmtId));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps));
    ReshardingOplogApplier applier(getServiceContext(),
                                   sourceId(),
                                   oplogNs(),
                                   crudNs(),
                                   crudUUID(),
                                   Timestamp(6, 3),
                                   std::move(iterator),
                                   2 /* batchSize */,
                                   getExecutor(),
                                   writerPool());

    auto future = applier.applyUntilCloneFinishedTs();
    future.get();

    future = applier.applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_TRUE(isWriteAlreadyExecuted(session, incomingStmtId));
}

}  // unnamed namespace
}  // namespace mongo
