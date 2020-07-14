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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/migrating_tenant_donor_util.h"

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/migrate_tenant_state_machine_gen.h"
#include "mongo/db/repl/migrating_tenant_access_blocker_by_prefix.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

namespace migrating_tenant_donor_util {

namespace {

const char kThreadNamePrefix[] = "TenantMigrationWorker-";
const char kPoolName[] = "TenantMigrationWorkerThreadPool";
const char kNetName[] = "TenantMigrationWorkerNetwork";

/**
 * Updates the MigratingTenantAccessBlocker when the tenant migration transitions to the blocking
 * state.
 */
void onTransitionToBlocking(OperationContext* opCtx, TenantMigrationDonorDocument& donorDoc) {
    invariant(donorDoc.getState() == TenantMigrationDonorStateEnum::kBlocking);
    invariant(donorDoc.getBlockTimestamp());

    auto& mtabByPrefix = MigratingTenantAccessBlockerByPrefix::get(opCtx->getServiceContext());
    auto mtab = mtabByPrefix.getMigratingTenantBlocker(donorDoc.getDatabasePrefix());

    if (!opCtx->writesAreReplicated()) {
        // A primary must create the MigratingTenantAccessBlocker and call startBlockingWrites on it
        // before reserving the OpTime for the "start blocking" write, so only secondaries create
        // the MigratingTenantAccessBlocker and call startBlockingWrites on it in the op observer.
        invariant(!mtab);

        mtab = std::make_shared<MigratingTenantAccessBlocker>(
            opCtx->getServiceContext(),
            migrating_tenant_donor_util::getTenantMigrationExecutor(opCtx->getServiceContext())
                .get());
        mtabByPrefix.add(donorDoc.getDatabasePrefix(), mtab);
        mtab->startBlockingWrites();
    }

    invariant(mtab);

    // Both primaries and secondaries call startBlockingReadsAfter in the op observer, since
    // startBlockingReadsAfter just needs to be called before the "start blocking" write's oplog
    // hole is filled.
    mtab->startBlockingReadsAfter(donorDoc.getBlockTimestamp().get());
}

}  // namespace

/**
 *   TODO - Implement recipientSyncData command
 */
void dataSync(OperationContext* opCtx, const TenantMigrationDonorDocument& originalDoc) {
    // Send recipientSyncData.

    // Call startBlockingWrites.
    startTenantMigrationBlockOnPrimary(opCtx, originalDoc);
    // Update the on-disk state of the migration to "blocking" state.
    invariant(originalDoc.getState() == TenantMigrationDonorStateEnum::kDataSync);

    uassertStatusOK(writeConflictRetry(
        opCtx,
        "doStartBlockingWrite",
        NamespaceString::kMigrationDonorsNamespace.ns(),
        [&]() -> Status {
            AutoGetCollection autoCollection(
                opCtx, NamespaceString::kMigrationDonorsNamespace, MODE_IX);
            Collection* collection = autoCollection.getCollection();

            if (!collection) {
                return Status(ErrorCodes::NamespaceNotFound,
                              str::stream() << NamespaceString::kMigrationDonorsNamespace.ns()
                                            << " does not exist");
            }

            WriteUnitOfWork wuow(opCtx);

            const auto originalRecordId =
                Helpers::findOne(opCtx, collection, originalDoc.toBSON(), false /* requireIndex */);
            invariant(!originalRecordId.isNull());

            // Reserve an opTime for the write and use it as the blockTimestamp for the migration.
            auto oplogSlot = repl::LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1U)[0];

            TenantMigrationDonorDocument updatedDoc;
            updatedDoc.setId(originalDoc.getId());
            updatedDoc.setDatabasePrefix(originalDoc.getDatabasePrefix());
            updatedDoc.setState(TenantMigrationDonorStateEnum::kBlocking);
            updatedDoc.setBlockTimestamp(oplogSlot.getTimestamp());

            CollectionUpdateArgs args;
            args.update = updatedDoc.toBSON();
            args.criteria = BSON("_id" << originalDoc.getId());
            args.oplogSlot = oplogSlot;

            collection->updateDocument(
                opCtx,
                originalRecordId,
                Snapshotted<BSONObj>(opCtx->recoveryUnit()->getSnapshotId(), originalDoc.toBSON()),
                updatedDoc.toBSON(),
                false,
                nullptr /* OpDebug* */,
                &args);

            wuow.commit();

            return Status::OK();
        }));
}

void startTenantMigrationBlockOnPrimary(OperationContext* opCtx,
                                        const TenantMigrationDonorDocument& donorDoc) {
    invariant(donorDoc.getState() == TenantMigrationDonorStateEnum::kDataSync);
    auto serviceContext = opCtx->getServiceContext();

    executor::TaskExecutor* mtabExecutor = getTenantMigrationExecutor(serviceContext).get();
    auto mtab = std::make_shared<MigratingTenantAccessBlocker>(serviceContext, mtabExecutor);

    mtab->startBlockingWrites();

    auto& mtabByPrefix = MigratingTenantAccessBlockerByPrefix::get(serviceContext);
    mtabByPrefix.add(donorDoc.getDatabasePrefix(), mtab);
}
std::shared_ptr<executor::TaskExecutor> getTenantMigrationExecutor(ServiceContext* serviceContext) {
    ThreadPool::Options tpOptions;
    tpOptions.threadNamePrefix = kThreadNamePrefix;
    tpOptions.poolName = kPoolName;
    tpOptions.maxThreads = ThreadPool::Options::kUnlimited;
    tpOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };

    return std::make_unique<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(tpOptions),
        executor::makeNetworkInterface(kNetName, nullptr, nullptr));
}

void onTenantMigrationDonorStateTransition(OperationContext* opCtx, const BSONObj& doc) {
    auto donorDoc = TenantMigrationDonorDocument::parse(IDLParserErrorContext("donorDoc"), doc);

    switch (donorDoc.getState()) {
        case TenantMigrationDonorStateEnum::kDataSync:
            break;
        case TenantMigrationDonorStateEnum::kBlocking:
            onTransitionToBlocking(opCtx, donorDoc);
            break;
        case TenantMigrationDonorStateEnum::kCommitted:
            break;
        case TenantMigrationDonorStateEnum::kAborted:
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace migrating_tenant_donor_util

}  // namespace mongo
