#include "schemeshard__operation_common.h"
#include "schemeshard__operation_common_external_data_source.h"
#include "schemeshard__operation_part.h"
#include "schemeshard_impl.h"

#include <ydb/core/tx/tiering/tier/object.h>

#include <utility>

namespace {

using namespace NKikimr;
using namespace NSchemeShard;

class TPropose: public TSubOperationState {
private:
    const TOperationId OperationId;

    TString DebugHint() const override {
        return TStringBuilder()
            << "TAlterExternalDataSource TPropose"
            << ", operationId: " << OperationId;
    }

public:
    explicit TPropose(TOperationId id)
        : OperationId(std::move(id))
    {
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        const TStepId step = TStepId(ev->Get()->StepId);

        LOG_I(DebugHint() << "HandleReply TEvOperationPlan"
            << ": step# " << step);

        const TTxState* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxAlterExternalDataSource);

        const auto pathId = txState->TargetPathId;
        const auto path = TPath::Init(pathId, context.SS);
        const TPathElement::TPtr pathPtr = context.SS->PathsById.at(pathId);

        NIceDb::TNiceDb db(context.GetDB());

        IncParentDirAlterVersionWithRepublish(OperationId, path, context);

        context.SS->ClearDescribePathCaches(pathPtr);
        context.OnComplete.PublishToSchemeBoard(OperationId, pathId);

        context.SS->ChangeTxState(db, OperationId, TTxState::Done);
        return true;
    }

    bool ProgressState(TOperationContext& context) override {
        LOG_I(DebugHint() << "ProgressState");

        const TTxState* txState = context.SS->FindTx(OperationId);
        Y_ABORT_UNLESS(txState);
        Y_ABORT_UNLESS(txState->TxType == TTxState::TxAlterExternalDataSource);

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, TStepId(0));
        return false;
    }
};

class TAlterExternalDataSource : public TSubOperation {
    static TTxState::ETxState NextState() { return TTxState::Propose; }

    TTxState::ETxState NextState(TTxState::ETxState state) const override {
        switch (state) {
        case TTxState::Waiting:
        case TTxState::Propose:
            return TTxState::Done;
        default:
            return TTxState::Invalid;
        }
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) override {
        switch (state) {
        case TTxState::Waiting:
        case TTxState::Propose:
            return MakeHolder<TPropose>(OperationId);
        case TTxState::Done:
            return MakeHolder<TDone>(OperationId);
        default:
            return nullptr;
        }
    }

    static bool IsDestinationPathValid(const THolder<TProposeResponse>& result,
                                       const TPath& dstPath) {
        const auto checks = dstPath.Check();
        checks.IsAtLocalSchemeShard()
            .IsResolved()
            .NotUnderDeleting()
            .NotUnderOperation()
            .FailOnWrongType(TPathElement::EPathType::EPathTypeExternalDataSource)
            ;

        if (!checks) {
            result->SetError(checks.GetStatus(), checks.GetError());
            if (dstPath.IsResolved()) {
                result->SetPathCreateTxId(static_cast<ui64>(dstPath.Base()->CreateTxId));
                result->SetPathId(dstPath.Base()->PathId.LocalPathId);
            }
        }

        return static_cast<bool>(checks);
    }

    bool IsApplyIfChecksPassed(const THolder<TProposeResponse>& result,
                               const TOperationContext& context) const {
        TString errorMessage;
        if (!context.SS->CheckApplyIf(Transaction, errorMessage)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errorMessage);
            return false;
        }
        return true;
    }

    static bool IsDescriptionValid(
        const THolder<TProposeResponse>& result,
        const NKikimrSchemeOp::TExternalDataSourceDescription& desc,
        const NExternalSource::IExternalSourceFactory::TPtr& factory) {
        TString errorMessage;
        if (!NExternalDataSource::Validate(desc, factory, errorMessage)) {
            result->SetError(NKikimrScheme::StatusSchemeError, errorMessage);
            return false;
        }
        return true;
    }

    static void AddPathInSchemeShard(
        const THolder<TProposeResponse>& result, const TPath& dstPath) {
        result->SetPathId(dstPath.Base()->PathId.LocalPathId);
    }

    TPathElement::TPtr ReplaceExternalDataSourcePathElement(const TPath& dstPath) const {
        TPathElement::TPtr externalDataSource = dstPath.Base();

        externalDataSource->PathState = TPathElement::EPathState::EPathStateAlter;
        externalDataSource->LastTxId  = OperationId.GetTxId();

        return externalDataSource;
    }

    void CreateTransaction(const TOperationContext& context,
                           const TPathId& externalDataSourcePathId) const {
        TTxState& txState = context.SS->CreateTx(OperationId,
                                                 TTxState::TxAlterExternalDataSource,
                                                 externalDataSourcePathId);
        txState.Shards.clear();
    }

    void RegisterParentPathDependencies(const TOperationContext& context,
                                        const TPath& parentPath) const {
        if (parentPath.Base()->HasActiveChanges()) {
            const TTxId parentTxId = parentPath.Base()->PlannedToCreate()
                                         ? parentPath.Base()->CreateTxId
                                         : parentPath.Base()->LastTxId;
            context.OnComplete.Dependence(parentTxId, OperationId.GetTxId());
        }
    }

    void AdvanceTransactionStateToPropose(const TOperationContext& context,
                                          NIceDb::TNiceDb& db) const {
        context.SS->ChangeTxState(db, OperationId, TTxState::Propose);
        context.OnComplete.ActivateTx(OperationId);
    }

    void PersistExternalDataSource(
        const TOperationContext& context,
        NIceDb::TNiceDb& db,
        const TPathElement::TPtr& externalDataSourcePath,
        const TExternalDataSourceInfo::TPtr& externalDataSourceInfo) const {
        const auto& externalDataSourcePathId = externalDataSourcePath->PathId;

        context.SS->ExternalDataSources[externalDataSourcePathId] = externalDataSourceInfo;

        context.SS->PersistPath(db, externalDataSourcePathId);

        context.SS->PersistExternalDataSource(db,
                                              externalDataSourcePathId,
                                              externalDataSourceInfo);
        context.SS->PersistTxState(db, OperationId);
    }

public:
    using TSubOperation::TSubOperation;

    THolder<TProposeResponse> Propose(const TString& owner,
                                      TOperationContext& context) override {
        Y_UNUSED(owner);
        const auto ssId              = context.SS->SelfTabletId();
        const TString& parentPathStr = Transaction.GetWorkingDir();
        const auto& externalDataSourceDescription =
            Transaction.GetCreateExternalDataSource();
        const TString& name = externalDataSourceDescription.GetName();

        LOG_N("TAlterExternalDataSource Propose"
              << ": opId# " << OperationId << ", path# " << parentPathStr << "/" << name);

        auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted,
                                                   static_cast<ui64>(OperationId.GetTxId()),
                                                   static_cast<ui64>(ssId));

        if (context.SS->IsServerlessDomain(TPath::Init(context.SS->RootPathId(), context.SS))) {
            if (!context.SS->EnableExternalDataSourcesOnServerless) {
                result->SetError(NKikimrScheme::StatusPreconditionFailed, "External data sources are disabled for serverless domains. Please contact your system administrator to enable it");
                return result;
            }
        }

        const TPath parentPath = TPath::Resolve(parentPathStr, context.SS);
        RETURN_RESULT_UNLESS(NExternalDataSource::IsParentPathValid(
            result, parentPath, Transaction, /* isCreate */ false));

        const TPath dstPath = parentPath.Child(name);

        RETURN_RESULT_UNLESS(IsDestinationPathValid(result, dstPath));
        RETURN_RESULT_UNLESS(IsApplyIfChecksPassed(result, context));
        RETURN_RESULT_UNLESS(IsDescriptionValid(result, externalDataSourceDescription, context.SS->ExternalSourceFactory));

        const auto oldExternalDataSourceInfo =
        context.SS->ExternalDataSources.Value(dstPath->PathId, nullptr);
        Y_ABORT_UNLESS(oldExternalDataSourceInfo);
        const TExternalDataSourceInfo::TPtr externalDataSourceInfo =
            NExternalDataSource::CreateExternalDataSource(externalDataSourceDescription,
                                     oldExternalDataSourceInfo->AlterVersion + 1);
        Y_ABORT_UNLESS(externalDataSourceInfo);

        {
            bool isTieredStorage = false;
            for (const auto& referrer : externalDataSourceInfo->ExternalTableReferences.GetReferences()) {
                if (TPath::Init(TPathId::FromProto(referrer.GetPathId()), context.SS)->PathType ==
                    NKikimrSchemeOp::EPathType::EPathTypeColumnTable) {
                    isTieredStorage = true;
                    break;
                }
            }
            if (isTieredStorage) {
                if (auto status = NColumnShard::NTiers::TTierConfig().DeserializeFromProto(externalDataSourceDescription); status.IsFail()) {
                    result->SetError(NKikimrScheme::StatusInvalidParameter,
                        "Cannot make this change while the external data source is used as a tiered storage: " + status.GetErrorMessage());
                    return result;
                }
            }
        }

        AddPathInSchemeShard(result, dstPath);
        const TPathElement::TPtr externalDataSource =
            ReplaceExternalDataSourcePathElement(dstPath);
        CreateTransaction(context, externalDataSource->PathId);

        NIceDb::TNiceDb db(context.GetDB());

        RegisterParentPathDependencies(context, parentPath);

        AdvanceTransactionStateToPropose(context, db);

        PersistExternalDataSource(
            context, db, externalDataSource, externalDataSourceInfo);

        IncParentDirAlterVersionWithRepublishSafeWithUndo(OperationId,
                                                          dstPath,
                                                          context.SS,
                                                          context.OnComplete);

        SetState(NextState());
        return result;
    }

    void AbortPropose(TOperationContext& context) override {
        LOG_N("TAlterExternalDataSource AbortPropose"
              << ": opId# " << OperationId);
        Y_ABORT("no AbortPropose for TAlterExternalDataSource");
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        LOG_N("TAlterExternalDataSource AbortUnsafe"
              << ": opId# " << OperationId << ", txId# " << forceDropTxId);
        context.OnComplete.DoneOperation(OperationId);
    }
};

} // namespace

namespace NKikimr::NSchemeShard {

ISubOperation::TPtr CreateAlterExternalDataSource(TOperationId id, const TTxTransaction& tx) {
    return MakeSubOperation<TAlterExternalDataSource>(id, tx);
}

ISubOperation::TPtr CreateAlterExternalDataSource(TOperationId id, TTxState::ETxState state) {
    Y_ABORT_UNLESS(state != TTxState::Invalid);
    return MakeSubOperation<TAlterExternalDataSource>(id, state);
}

}
