/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/metadata_consistency_util.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

namespace mongo {
namespace {

class ConfigsvrCheckMetadataConsistencyCommand final
    : public TypedCommand<ConfigsvrCheckMetadataConsistencyCommand> {
public:
    using Request = ConfigsvrCheckMetadataConsistency;
    using Response = CursorInitialReply;

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command. Do not call directly.";
    }

    bool adminOnly() const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            const auto nss = ns();

            std::vector<MetadataInconsistencyItem> inconsistenciesMerged;
            const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();

            if (nss.isCollectionlessCursorNamespace()) {
                // Cluster or database level mode command.
                const auto collections = catalogClient->getCollections(opCtx, nss.db());

                for (const auto& coll : collections) {
                    // Look for chunks inconsistencies and append them to the merged vector.
                    auto chunksInconsistencies =
                        metadata_consistency_util::checkChunksInconsistencies(
                            opCtx, coll, _getCollectionChunks(opCtx, coll));
                    inconsistenciesMerged.insert(
                        inconsistenciesMerged.end(),
                        std::make_move_iterator(chunksInconsistencies.begin()),
                        std::make_move_iterator(chunksInconsistencies.end()));
                }
            } else {
                // Collection level mode command.
                const auto coll = catalogClient->getCollection(opCtx, nss);
                auto chunksInconsistencies = metadata_consistency_util::checkChunksInconsistencies(
                    opCtx, coll, _getCollectionChunks(opCtx, coll));
                inconsistenciesMerged.insert(inconsistenciesMerged.end(),
                                             std::make_move_iterator(chunksInconsistencies.begin()),
                                             std::make_move_iterator(chunksInconsistencies.end()));
            }

            auto exec = metadata_consistency_util::makeQueuedPlanExecutor(
                opCtx, std::move(inconsistenciesMerged), nss);

            ClientCursorParams cursorParams{
                std::move(exec),
                nss,
                AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
                APIParameters::get(opCtx),
                opCtx->getWriteConcern(),
                repl::ReadConcernArgs::get(opCtx),
                ReadPreferenceSetting::get(opCtx),
                request().toBSON({}),
                {Privilege(ResourcePattern::forClusterResource(), ActionType::internal)}};

            const auto batchSize = [&]() -> long long {
                const auto& cursorOpts = request().getCursor();
                if (cursorOpts && cursorOpts->getBatchSize()) {
                    return *cursorOpts->getBatchSize();
                } else {
                    return query_request_helper::kDefaultBatchSize;
                }
            }();

            return metadata_consistency_util::createInitialCursorReplyMongod(
                opCtx, std::move(cursorParams), batchSize);
        }

    private:
        std::vector<ChunkType> _getCollectionChunks(OperationContext* opCtx,
                                                    const CollectionType& coll) {
            const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
            // TODO SERVER-75490: Use kSnapshotReadConcern when getting chunks from the catalog
            return uassertStatusOK(catalogClient->getChunks(
                opCtx,
                BSON(ChunkType::collectionUUID() << coll.getUuid()) /*query*/,
                BSON(ChunkType::min() << 1) /*sort*/,
                boost::none /*limit*/,
                nullptr /*opTime*/,
                coll.getEpoch(),
                coll.getTimestamp(),
                repl::ReadConcernLevel::kMajorityReadConcern));
        }

        NamespaceString ns() const override {
            return request().getNamespace();
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }
    };

} configsvrCheckMetadataConsistencyCommand;

}  // namespace
}  // namespace mongo
