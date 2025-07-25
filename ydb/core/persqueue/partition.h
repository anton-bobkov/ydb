#pragma once

#include "blob.h"
#include "header.h"
#include "key.h"
#include "partition_init.h"
#include "partition_sourcemanager.h"
#include "partition_types.h"
#include "quota_tracker.h"
#include "sourceid.h"
#include "subscriber.h"
#include "user_info.h"
#include "utils.h"
#include "read_quoter.h"
#include "partition_blob_encoder.h"

#include <ydb/core/keyvalue/keyvalue_events.h>
#include <ydb/library/persqueue/counter_time_keeper/counter_time_keeper.h>

#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/core/hfunc.h>
#include <ydb/library/actors/core/log.h>
#include <library/cpp/sliding_window/sliding_window.h>

#include <util/generic/set.h>

#include <variant>

namespace NKikimr::NPQ {

static const ui32 MAX_BLOB_PART_SIZE = 500_KB;
static const ui32 DEFAULT_BUCKET_COUNTER_MULTIPLIER = 20;
static const ui32 MAX_USER_ACTS = 1000;
// Extra amount of time YDB should wait before deleting supportive partition for kafka transaction
// after transaction timeout has passed.
//
// Total time till kafka supportive partition deletion = AppData.KafkaProxyConfig.TransactionTimeoutMs + KAFKA_TRANSACTION_DELETE_DELAY_MS
static const ui32 KAFKA_TRANSACTION_DELETE_DELAY_MS = TDuration::Hours(1).MilliSeconds(); // 1 hour;
static const ui32 BATCH_UNPACK_SIZE_BORDER = 500_KB;
static const ui32 MAX_INLINE_SIZE = 1000;

using TPartitionLabeledCounters = TProtobufTabletLabeledCounters<EPartitionLabeledCounters_descriptor>;

ui64 GetOffsetEstimate(const std::deque<TDataKey>& container, TInstant timestamp, ui64 headOffset);


class TKeyLevel;
struct TMirrorerInfo;

enum class ECommitState {
    Pending,
    Committed,
    Aborted
};

struct TTransaction {

    explicit TTransaction(TSimpleSharedPtr<TEvPQ::TEvTxCalcPredicate> tx,
                          TMaybe<bool> predicate = Nothing())
        : Tx(tx)
        , Predicate(predicate)
        , SupportivePartitionActor(tx->SupportivePartitionActor)
    {
        Y_ABORT_UNLESS(Tx);
    }

    TTransaction(TSimpleSharedPtr<TEvPQ::TEvChangePartitionConfig> changeConfig,
                 bool sendReply)
        : ChangeConfig(changeConfig)
        , SendReply(sendReply)
    {
        Y_ABORT_UNLESS(ChangeConfig);
    }

    explicit TTransaction(TSimpleSharedPtr<TEvPQ::TEvProposePartitionConfig> proposeConfig)
        : ProposeConfig(proposeConfig)
    {
        Y_ABORT_UNLESS(ProposeConfig);
    }

    explicit TTransaction(TSimpleSharedPtr<TEvPersQueue::TEvProposeTransaction> proposeTx)
        : ProposeTransaction(proposeTx)
        , State(ECommitState::Committed)
    {
        const auto& record = proposeTx->GetRecord();
        if (record.HasSupportivePartitionActor()) {
            SupportivePartitionActor = ActorIdFromProto(record.GetSupportivePartitionActor());
        }
        Y_ABORT_UNLESS(ProposeTransaction);
    }

    TMaybe<ui64> GetTxId() const {
        if (Tx) {
            return Tx->TxId;
        } else if (ProposeConfig) {
            return ProposeConfig->TxId;
        }
        return {};
    }

    TSimpleSharedPtr<TEvPQ::TEvTxCalcPredicate> Tx;
    TMaybe<bool> Predicate;
    TActorId SupportivePartitionActor;


    TSimpleSharedPtr<TEvPQ::TEvChangePartitionConfig> ChangeConfig;
    bool SendReply;
    TSimpleSharedPtr<TEvPQ::TEvProposePartitionConfig> ProposeConfig;
    TSimpleSharedPtr<TEvPersQueue::TEvProposeTransaction> ProposeTransaction;

    //Data Tx
    THolder<TEvPQ::TEvGetWriteInfoResponse> WriteInfo;
    bool WriteInfoApplied = false;
    TString Message;
    ECommitState State = ECommitState::Pending;
};

class TPartition : public TActorBootstrapped<TPartition> {
    friend TInitializer;
    friend TInitializerStep;
    friend TInitConfigStep;
    friend TInitInternalFieldsStep;
    friend TInitDiskStatusStep;
    friend TInitMetaStep;
    friend TInitInfoRangeStep;
    friend TInitDataRangeStep;
    friend TInitDataStep;
    friend TInitEndWriteTimestampStep;

    friend TPartitionSourceManager;

    friend class TPartitionTestWrapper;

public:
    const TString& TopicName() const;

    ui64 GetUsedStorage(const TInstant& ctx);

private:
    static const ui32 MAX_ERRORS_COUNT_TO_STORE = 10;
    static const ui32 SCALE_REQUEST_REPEAT_MIN_SECONDS = 60;

    enum EDeletePartitionState {
        DELETION_NOT_INITED = 0,
        DELETION_INITED = 1,
        DELETION_IN_PROCESS = 2,
    };

private:
    struct THasDataReq;
    struct THasDataDeadline;
    using TMessageQueue = std::deque<TMessage>;
    using TGetWriteInfoResp =
        std::variant<TAutoPtr<TEvPQ::TEvGetWriteInfoResponse>,
                     TAutoPtr<TEvPQ::TEvGetWriteInfoError>>;

    bool IsActive() const;
    bool CanWrite() const;
    bool CanEnqueue() const;

    bool LastOffsetHasBeenCommited(const TUserInfoBase& userInfo) const;

    void ReplyError(const TActorContext& ctx, const ui64 dst, NPersQueue::NErrorCode::EErrorCode errorCode, const TString& error);
    void ReplyError(const TActorContext& ctx, const ui64 dst, NPersQueue::NErrorCode::EErrorCode errorCode, const TString& error, NWilson::TSpan& span);
    void ReplyPropose(const TActorContext& ctx, const NKikimrPQ::TEvProposeTransaction& event, NKikimrPQ::TEvProposeTransactionResult::EStatus statusCode,
                      NKikimrPQ::TError::EKind kind, const TString& reason);
    void ReplyErrorForStoredWrites(const TActorContext& ctx);

    void ReplyGetClientOffsetOk(const TActorContext& ctx, const ui64 dst, const i64 offset, const TInstant writeTimestamp, const TInstant createTimestamp, bool consumerHasAnyCommits, const std::optional<TString>& committedMetadata);
    void ReplyOk(const TActorContext& ctx, const ui64 dst);
    void ReplyOk(const TActorContext& ctx, const ui64 dst, NWilson::TSpan& span);
    void ReplyOwnerOk(const TActorContext& ctx, const ui64 dst, const TString& ownerCookie, ui64 seqNo, NWilson::TSpan& span);

    void ReplyWrite(const TActorContext& ctx, ui64 dst, const TString& sourceId, ui64 seqNo, ui16 partNo, ui16 totalParts, ui64 offset, TInstant writeTimestamp, bool already, ui64 maxSeqNo, TDuration partitionQuotedTime, TDuration topicQuotedTime, TDuration queueTime, TDuration writeTime, NWilson::TSpan& span);
    void SendReadingFinished(const TString& consumer);

    void AddNewWriteBlob(std::pair<TKey, ui32>& res, TEvKeyValue::TEvRequest* request, const TActorContext& ctx);
    void AddNewFastWriteBlob(std::pair<TKey, ui32>& res, TEvKeyValue::TEvRequest* request, const TActorContext& ctx);
    void AddNewCompactionWriteBlob(std::pair<TKey, ui32>& res, TEvKeyValue::TEvRequest* request, ui64 blobCreationUnixTime, const TActorContext& ctx);
    void AnswerCurrentWrites(const TActorContext& ctx);
    void AnswerCurrentReplies(const TActorContext& ctx);
    void CancelOneWriteOnWrite(const TActorContext& ctx,
                               const TString& errorStr,
                               const TWriteMsg& p,
                               NPersQueue::NErrorCode::EErrorCode errorCode);
    void ClearOldHead(const ui64 offset, const ui16 partNo);
    void CreateMirrorerActor();
    void DoRead(TEvPQ::TEvRead::TPtr&& ev, TDuration waitQuotaTime, const TActorContext& ctx);
    void FillReadFromTimestamps(const TActorContext& ctx);
    void FilterDeadlinedWrites(const TActorContext& ctx);
    void FilterDeadlinedWrites(const TActorContext& ctx, TMessageQueue& requests);

    void Handle(NReadQuoterEvents::TEvAccountQuotaCountersUpdated::TPtr& ev, const TActorContext& ctx);
    void Handle(NReadQuoterEvents::TEvQuotaCountersUpdated::TPtr& ev, const TActorContext& ctx);
    void Handle(NReadQuoterEvents::TEvQuotaUpdated::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvApproveReadQuota::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvKeyValue::TEvResponse::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvBlobResponse::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvChangeOwner::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvChangePartitionConfig::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvError::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvGetClientOffset::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvGetMaxSeqNoRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvGetPartitionClientInfo::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvHandleWriteResponse::TPtr&, const TActorContext& ctx);
    void Handle(TEvPQ::TEvMirrorerCounters::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvPartitionOffsets::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvPartitionStatus::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvPipeDisconnected::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvProxyResponse::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvQuotaDeadlineCheck::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvRead::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvReadTimeout::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvReserveBytes::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvSetClientInfo::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvUpdateWriteTimestamp::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPersQueue::TEvHasDataInfo::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPersQueue::TEvProposeTransaction::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvTxCalcPredicate::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvGetWriteInfoRequest::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvGetWriteInfoResponse::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvGetWriteInfoError::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvTxCommit::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvTxRollback::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPersQueue::TEvReportPartitionError::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvApproveWriteQuota::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvents::TEvPoisonPill::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvSubDomainStatus::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvRunCompaction::TPtr& ev);
    void HandleMonitoring(TEvPQ::TEvMonRequest::TPtr& ev, const TActorContext& ctx);
    void HandleOnIdle(TEvPQ::TEvDeregisterMessageGroup::TPtr& ev, const TActorContext& ctx);
    void HandleOnIdle(TEvPQ::TEvRegisterMessageGroup::TPtr& ev, const TActorContext& ctx);
    void HandleOnIdle(TEvPQ::TEvSplitMessageGroup::TPtr& ev, const TActorContext& ctx);
    void HandleOnIdle(TEvPQ::TEvUpdateAvailableSize::TPtr& ev, const TActorContext& ctx);
    void HandleOnIdle(TEvPQ::TEvWrite::TPtr& ev, const TActorContext& ctx);
    void HandleOnInit(TEvPQ::TEvPartitionOffsets::TPtr& ev, const TActorContext& ctx);
    void HandleOnInit(TEvPQ::TEvPartitionStatus::TPtr& ev, const TActorContext& ctx);
    void HandleOnWrite(TEvPQ::TEvDeregisterMessageGroup::TPtr& ev, const TActorContext& ctx);
    void HandleOnWrite(TEvPQ::TEvRegisterMessageGroup::TPtr& ev, const TActorContext& ctx);
    void HandleOnWrite(TEvPQ::TEvSplitMessageGroup::TPtr& ev, const TActorContext& ctx);
    void HandleOnWrite(TEvPQ::TEvUpdateAvailableSize::TPtr& ev, const TActorContext& ctx);
    void HandleOnWrite(TEvPQ::TEvWrite::TPtr& ev, const TActorContext& ctx);
    void HandleWakeup(const TActorContext& ctx);
    void HandleWriteResponse(const TActorContext& ctx);

    void InitComplete(const TActorContext& ctx);
    void InitUserInfoForImportantClients(const TActorContext& ctx);

    void LogAndCollectError(NKikimrServices::EServiceKikimr service, const TString& msg, const TActorContext& ctx);
    void LogAndCollectError(const NKikimrPQ::TStatusResponse::TErrorMessage& error, const TActorContext& ctx);

    void OnReadRequestFinished(ui64 cookie, ui64 answerSize, const TString& consumer, const TActorContext& ctx);

    void ProcessChangeOwnerRequest(TAutoPtr<TEvPQ::TEvChangeOwner> ev, const TActorContext& ctx);
    void ProcessChangeOwnerRequests(const TActorContext& ctx);
    void ProcessHasDataRequests(const TActorContext& ctx);
    bool ProcessHasDataRequest(const THasDataReq& request, const TActorContext& ctx);
    void ProcessRead(const TActorContext& ctx, TReadInfo&& info, const ui64 cookie, bool subscription);
    void ProcessReserveRequests(const TActorContext& ctx);
    void ProcessTimestampRead(const TActorContext& ctx);
    void ProcessTimestampsForNewData(const ui64 prevEndOffset, const TActorContext& ctx);

    void ProcessMaxSeqNoRequest(const TActorContext& ctx);

    void ReadTimestampForOffset(const TString& user, TUserInfo& ui, const TActorContext& ctx);
    void ReportCounters(const TActorContext& ctx, bool force = false);
    bool UpdateCounters(const TActorContext& ctx, bool force = false);
    void ScheduleUpdateAvailableSize(const TActorContext& ctx);
    void SetDeadlinesForWrites(const TActorContext& ctx);

    void SetupStreamCounters(const TActorContext& ctx);
    void SetupTopicCounters(const TActorContext& ctx);

    void SyncMemoryStateWithKVState(const TActorContext& ctx);
    void UpdateAvailableSize(const TActorContext& ctx);

    void AddMetaKey(TEvKeyValue::TEvRequest* request);
    void CheckHeadConsistency() const;
    void HandlePendingRequests(const TActorContext& ctx);
    void HandleQuotaWaitingRequests(const TActorContext& ctx);
    void RequestQuotaForWriteBlobRequest(size_t dataSize, ui64 cookie);
    bool RequestBlobQuota();
    void RequestBlobQuota(size_t quotaSize);
    void ConsumeBlobQuota();
    void UpdateAfterWriteCounters(bool writeComplete);

    void UpdateUserInfoEndOffset(const TInstant& now);
    void UpdateWriteBufferIsFullState(const TInstant& now);

    TInstant GetWriteTimeEstimate(ui64 offset) const;
    bool CleanUp(TEvKeyValue::TEvRequest* request, const TActorContext& ctx);

    // Removes blobs that are no longer required. Blobs are no longer required if the storage time of all messages
    // stored in this blob has expired and they have been read by all important consumers.
    bool CleanUpBlobs(TEvKeyValue::TEvRequest *request, const TActorContext& ctx);
    bool IsQuotingEnabled() const;
    bool WaitingForPreviousBlobQuota() const;
    bool WaitingForSubDomainQuota(const TActorContext& ctx, const ui64 withSize = 0) const;
    size_t GetQuotaRequestSize(const TEvKeyValue::TEvRequest& request);
    std::pair<TInstant, TInstant> GetTime(const TUserInfo& userInfo, ui64 offset) const;
    ui32 NextChannel(bool isHead, ui32 blobSize);
    ui64 GetSizeLag(i64 offset);
    std::pair<TKey, ui32> GetNewFastWriteKey(bool headCleared);
    std::pair<TKey, ui32> GetNewFastWriteKeyImpl(bool headCleared, ui32 headSize);
    std::pair<TKey, ui32> GetNewCompactionWriteKey(bool headCleared);
    std::pair<TKey, ui32> GetNewCompactionWriteKeyImpl(bool headCleared, bool needCompaction, ui32 headSize);
    THashMap<TString, TOwnerInfo>::iterator DropOwner(THashMap<TString, TOwnerInfo>::iterator& it,
                                                      const TActorContext& ctx);
    // will return rcount and rsize also
    void GetReadRequestFromCompactedBody(const ui64 startOffset, const ui16 partNo, const ui32 maxCount,
                                         const ui32 maxSize, ui32* rcount, ui32* rsize, ui64 lastOffset,
                                         TBlobKeyTokens* blobKeyTokens,
                                         TVector<TRequestedBlob>& blobs);
    void GetReadRequestFromFastWriteBody(const ui64 startOffset, const ui16 partNo, const ui32 maxCount,
                                         const ui32 maxSize, ui32* rcount, ui32* rsize, ui64 lastOffset,
                                         TBlobKeyTokens* blobKeyTokens,
                                         TVector<TRequestedBlob>& blobs);
    TVector<TClientBlob>    GetReadRequestFromHead(const ui64 startOffset, const ui16 partNo, const ui32 maxCount,
                                                   const ui32 maxSize, const ui64 readTimestampMs, ui32* rcount,
                                                   ui32* rsize, ui64* insideHeadOffset, ui64 lastOffset);

    template<typename T>
    std::function<void(bool, T& r)> GetResultPostProcessor(const TString& consumer = "");
    TAutoPtr<TEvPersQueue::TEvHasDataInfoResponse> MakeHasDataInfoResponse(ui64 lagSize, const TMaybe<ui64>& cookie, bool readingFinished = false);

    void ProcessTxsAndUserActs(const TActorContext& ctx);
    void ContinueProcessTxsAndUserActs(const TActorContext& ctx);
    void ProcessCommitQueue();
    void RunPersist();

    void MoveUserActOrTxToCommitState();
    void PushBackDistrTx(TSimpleSharedPtr<TEvPQ::TEvTxCalcPredicate> event);
    void PushBackDistrTx(TSimpleSharedPtr<TEvPQ::TEvChangePartitionConfig> event);
    void PushFrontDistrTx(TSimpleSharedPtr<TEvPQ::TEvChangePartitionConfig> event);
    void PushBackDistrTx(TSimpleSharedPtr<TEvPQ::TEvProposePartitionConfig> event);

    void RequestWriteInfoIfRequired();

    void ProcessDistrTxs(const TActorContext& ctx);
    void ProcessDistrTx(const TActorContext& ctx);

    void AddImmediateTx(TSimpleSharedPtr<TEvPersQueue::TEvProposeTransaction> event);
    void ProcessImmediateTxs(const TActorContext& ctx);

    void AddUserAct(TSimpleSharedPtr<TEvPQ::TEvSetClientInfo> act);
    void RemoveUserAct(const TString& consumerId);
    size_t GetUserActCount(const TString& consumer) const;

    void ProcessUserActs(const TActorContext& ctx);

    void EmulatePostProcessUserAct(const TEvPQ::TEvSetClientInfo& act,
                                   TUserInfoBase& userInfo,
                                   const TActorContext& ctx);

    void ReplyToProposeOrPredicate(TSimpleSharedPtr<TTransaction>& tx, bool isPredicate);

    void SendWriteInfoRequest(const TSimpleSharedPtr<TEvPQ::TEvTxCalcPredicate>& event);
    void WriteInfoResponseHandler(const TActorId& sender,
                        TGetWriteInfoResp&& ev,
                        const TActorContext& ctx);


    void ScheduleReplyOk(const ui64 dst);
    void ScheduleReplyGetClientOffsetOk(const ui64 dst,
                                        const i64 offset,
                                        const TInstant writeTimestamp,
                                        const TInstant createTimestamp,
                                        bool consumerHasAnyCommits,
                                        const std::optional<TString>& committedMetadata=std::nullopt);
    void ScheduleReplyError(const ui64 dst,
                            NPersQueue::NErrorCode::EErrorCode errorCode,
                            const TString& error);
    void ScheduleReplyPropose(const NKikimrPQ::TEvProposeTransaction& event,
                              NKikimrPQ::TEvProposeTransactionResult::EStatus statusCode,
                              NKikimrPQ::TError::EKind kind,
                              const TString& reason);
    void ScheduleReplyCommitDone(ui64 step, ui64 txId);
    void ScheduleDropPartitionLabeledCounters(const TString& group);
    void SchedulePartitionConfigChanged();

    void AddCmdWrite(NKikimrClient::TKeyValueRequest& request,
                     const TKeyPrefix& ikey, const TKeyPrefix& ikeyDeprecated,
                     ui64 offset, ui32 gen, ui32 step, const TString& session,
                     ui64 readOffsetRewindSum,
                     ui64 readRuleGeneration,
                     bool anyCommits, const std::optional<TString>& committedMetadata);
    void AddCmdWriteTxMeta(NKikimrClient::TKeyValueRequest& request);
    void AddCmdWriteUserInfos(NKikimrClient::TKeyValueRequest& request);
    void AddCmdWriteConfig(NKikimrClient::TKeyValueRequest& request);
    void AddCmdDeleteRange(NKikimrClient::TKeyValueRequest& request,
                           const TKeyPrefix& ikey, const TKeyPrefix& ikeyDeprecated);

    TUserInfoBase& GetOrCreatePendingUser(const TString& user, TMaybe<ui64> readRuleGeneration = {});
    TUserInfoBase* GetPendingUserIfExists(const TString& user);

    THolder<TEvPQ::TEvProxyResponse> MakeReplyOk(const ui64 dst);
    THolder<TEvPQ::TEvProxyResponse> MakeReplyGetClientOffsetOk(const ui64 dst,
                                                                const i64 offset,
                                                                const TInstant writeTimestamp,
                                                                const TInstant createTimestamp,
                                                                bool consumerHasAnyCommits,
                                                                const std::optional<TString>& committedMetadata);
    THolder<TEvPQ::TEvError> MakeReplyError(const ui64 dst,
                                            NPersQueue::NErrorCode::EErrorCode errorCode,
                                            const TString& error);
    THolder<TEvPersQueue::TEvProposeTransactionResult> MakeReplyPropose(const NKikimrPQ::TEvProposeTransaction& event,
                                                                        NKikimrPQ::TEvProposeTransactionResult::EStatus statusCode,
                                                                        NKikimrPQ::TError::EKind kind,
                                                                        const TString& reason);
    THolder<TEvPQ::TEvTxCommitDone> MakeCommitDone(ui64 step, ui64 txId);

    bool BeginTransaction(const TEvPQ::TEvProposePartitionConfig& event);

    void CommitTransaction(TSimpleSharedPtr<TTransaction>& t);
    void RollbackTransaction(TSimpleSharedPtr<TTransaction>& t);


    void BeginChangePartitionConfig(const NKikimrPQ::TPQTabletConfig& config,
                                    const TActorContext& ctx);
    void ExecChangePartitionConfig();

    void OnProcessTxsAndUserActsWriteComplete(const TActorContext& ctx);

    void EndChangePartitionConfig(NKikimrPQ::TPQTabletConfig&& config,
                                  const TEvPQ::TMessageGroupsPtr& explicitMessageGroups,
                                  NPersQueue::TTopicConverterPtr topicConverter,
                                  const TActorContext& ctx);
    TString GetKeyConfig() const;

    void InitPendingUserInfoForImportantClients(const NKikimrPQ::TPQTabletConfig& config,
                                                const TActorContext& ctx);

    void Initialize(const TActorContext& ctx);
    void InitSplitMergeSlidingWindow();

    template <typename T>
    void EmplacePendingRequest(T&& body, NWilson::TSpan&& span, const TActorContext& ctx) {
        const auto now = ctx.Now();
        auto& msg = PendingRequests.emplace_back(body, std::move(span), now - TInstant::Zero());
        AttachPersistRequestSpan(msg.Span);
     }

    void EmplaceResponse(TMessage&& message, const TActorContext& ctx);

    void Handle(TEvPQ::TEvProposePartitionConfig::TPtr& ev, const TActorContext& ctx);

    void HandleOnInit(TEvPQ::TEvTxCalcPredicate::TPtr& ev, const TActorContext& ctx);
    void HandleOnInit(TEvPQ::TEvTxCommit::TPtr& ev, const TActorContext& ctx);
    void HandleOnInit(TEvPQ::TEvTxRollback::TPtr& ev, const TActorContext& ctx);
    void HandleOnInit(TEvPQ::TEvProposePartitionConfig::TPtr& ev, const TActorContext& ctx);
    void HandleOnInit(TEvPQ::TEvGetWriteInfoRequest::TPtr& ev, const TActorContext& ctx);
    void HandleOnInit(TEvPQ::TEvGetWriteInfoResponse::TPtr& ev, const TActorContext& ctx);
    void HandleOnInit(TEvPQ::TEvGetWriteInfoError::TPtr& ev, const TActorContext& ctx);

    void ChangePlanStepAndTxId(ui64 step, ui64 txId);

    void SendReadPreparedProxyResponse(const TReadAnswer& answer, const TReadInfo& readInfo, TUserInfo& user);

    void CheckIfSessionExists(TUserInfoBase& userInfo, const TActorId& newPipe);
    // void DestroyReadSession(const TReadSessionKey& key);

    void Handle(TEvPQ::TEvCheckPartitionStatusRequest::TPtr& ev, const TActorContext& ctx);

    NKikimrPQ::EScaleStatus CheckScaleStatus(const TActorContext& ctx);
    void ChangeScaleStatusIfNeeded(NKikimrPQ::EScaleStatus scaleStatus);

    TString LogPrefix() const;

    void Handle(TEvPQ::TEvProcessChangeOwnerRequests::TPtr& ev, const TActorContext& ctx);
    void StartProcessChangeOwnerRequests(const TActorContext& ctx);

    void CommitWriteOperations(TTransaction& t);

    void HandleOnInit(TEvPQ::TEvDeletePartition::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvDeletePartition::TPtr& ev, const TActorContext& ctx);

    ui64 GetReadOffset(ui64 offset, TMaybe<TInstant> readTimestamp) const;

    TConsumerSnapshot CreateSnapshot(TUserInfo& userInfo) const;

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::PERSQUEUE_PARTITION_ACTOR;
    }

    TPartition(ui64 tabletId, const TPartitionId& partition, const TActorId& tablet, ui32 tabletGeneration, const TActorId& blobCache,
               const NPersQueue::TTopicConverterPtr& topicConverter, TString dcId, bool isServerless,
               const NKikimrPQ::TPQTabletConfig& config, const TTabletCountersBase& counters, bool SubDomainOutOfSpace, ui32 numChannels,
               const TActorId& writeQuoterActorId, bool newPartition = false,
               TVector<TTransaction> distrTxs = {});

    void Bootstrap(const TActorContext& ctx);

    ui64 Size() const {
        return CompactionBlobEncoder.GetSize();
    }

    // The size of the data realy was persisted in the storage by the partition
    ui64 UserDataSize() const;
    // The size of the data was metered to user
    ui64 MeteringDataSize(TInstant now) const;
    // The size of the storage that was reserved by the partition
    ui64 ReserveSize() const;
    // The size of the storage that usud by the partition. That included combination of the reserver and realy persisted data.
    ui64 StorageSize(const TActorContext& ctx) const;
    ui64 UsedReserveSize(const TActorContext& ctx) const;
    // Minimal offset, the data from which cannot be deleted, because it is required by an important consumer
    ui64 ImportantClientsMinOffset() const;

    TInstant GetEndWriteTimestamp() const; // For tests only

    //Bootstrap sends kvRead
    //Become StateInit
    //StateInit
    //wait for correct result, cache all
    //Become StateIdle
    //StateIdle
    //got read - make kvRead
    //got kvReadResult - answer read
    //got write - make kvWrite, Become StateWrite
    //StateWrite
    // got read - ...
    // got kwReadResult - ...
    //got write - store it inflight
    //got kwWriteResult - check it, become StateIdle of StateWrite(and write inflight)

private:
    template <typename TEv>
    TString EventStr(const char * func, const TEv& ev) {
        TStringStream ss;
        ss << func << " event# " << ev->GetTypeRewrite() << " (" << ev->GetTypeName() << "), Tablet " << Tablet << ", Partition " << Partition
           << ", Sender " << ev->Sender.ToString() << ", Recipient " << ev->Recipient.ToString() << ", Cookie: " << ev->Cookie;
        return ss.Str();
    }

    TInitializer Initializer;

    STFUNC(StateInit)
    {
        NPersQueue::TCounterTimeKeeper keeper(TabletCounters.Cumulative()[COUNTER_PQ_TABLET_CPU_USAGE]);

        ALOG_TRACE(NKikimrServices::PERSQUEUE, EventStr("StateInit", ev));

        TRACE_EVENT(NKikimrServices::PERSQUEUE);
        switch (ev->GetTypeRewrite()) {
            CFunc(TEvents::TSystem::Wakeup, HandleWakeup);
            HFuncTraced(TEvents::TEvPoisonPill, Handle);
            HFuncTraced(TEvPQ::TEvMonRequest, HandleMonitoring);
            HFuncTraced(TEvPQ::TEvChangePartitionConfig, Handle);
            HFuncTraced(TEvPQ::TEvPartitionOffsets, HandleOnInit);
            HFuncTraced(TEvPQ::TEvPartitionStatus, HandleOnInit);
            HFuncTraced(TEvPersQueue::TEvReportPartitionError, Handle);
            HFuncTraced(TEvPersQueue::TEvHasDataInfo, Handle);
            HFuncTraced(TEvPQ::TEvMirrorerCounters, Handle);
            HFuncTraced(TEvPQ::TEvGetPartitionClientInfo, Handle);
            HFuncTraced(TEvPQ::TEvTxCalcPredicate, HandleOnInit);
            HFuncTraced(TEvPQ::TEvProposePartitionConfig, HandleOnInit);
            HFuncTraced(TEvPQ::TEvTxCommit, HandleOnInit);
            HFuncTraced(TEvPQ::TEvTxRollback, HandleOnInit);
            HFuncTraced(TEvPQ::TEvSubDomainStatus, Handle);
            HFuncTraced(NReadQuoterEvents::TEvQuotaUpdated, Handle);
            HFuncTraced(NReadQuoterEvents::TEvAccountQuotaCountersUpdated, Handle);
            HFuncTraced(NReadQuoterEvents::TEvQuotaCountersUpdated, Handle);
            HFuncTraced(TEvPQ::TEvGetWriteInfoRequest, HandleOnInit);
            HFuncTraced(TEvPQ::TEvGetWriteInfoResponse, HandleOnInit);
            HFuncTraced(TEvPQ::TEvGetWriteInfoError, HandleOnInit);
            HFuncTraced(TEvPQ::TEvDeletePartition, HandleOnInit);
            IgnoreFunc(TEvPQ::TEvTxBatchComplete);
            hFuncTraced(TEvPQ::TEvRunCompaction, Handle);
        default:
            if (!Initializer.Handle(ev)) {
                ALOG_ERROR(NKikimrServices::PERSQUEUE, "Unexpected " << EventStr("StateInit", ev));
            }
            break;
        };
    }

    STFUNC(StateIdle)
    {
        NPersQueue::TCounterTimeKeeper keeper(TabletCounters.Cumulative()[COUNTER_PQ_TABLET_CPU_USAGE]);

        ALOG_TRACE(NKikimrServices::PERSQUEUE, EventStr("StateIdle", ev));

        TRACE_EVENT(NKikimrServices::PERSQUEUE);
        switch (ev->GetTypeRewrite()) {
            CFunc(TEvents::TSystem::Wakeup, HandleWakeup);
            HFuncTraced(TEvKeyValue::TEvResponse, Handle);
            HFuncTraced(TEvPQ::TEvHandleWriteResponse, Handle);
            HFuncTraced(TEvPQ::TEvBlobResponse, Handle);
            HFuncTraced(TEvPQ::TEvWrite, HandleOnIdle);
            HFuncTraced(TEvPQ::TEvRead, Handle);
            HFuncTraced(TEvPQ::TEvApproveReadQuota, Handle);
            HFuncTraced(TEvPQ::TEvReadTimeout, Handle);
            HFuncTraced(TEvents::TEvPoisonPill, Handle);
            HFuncTraced(TEvPQ::TEvMonRequest, HandleMonitoring);
            HFuncTraced(TEvPQ::TEvGetMaxSeqNoRequest, Handle);
            HFuncTraced(TEvPQ::TEvChangePartitionConfig, Handle);
            HFuncTraced(TEvPQ::TEvGetClientOffset, Handle);
            HFuncTraced(TEvPQ::TEvUpdateWriteTimestamp, Handle);
            HFuncTraced(TEvPQ::TEvSetClientInfo, Handle);
            HFuncTraced(TEvPQ::TEvPartitionOffsets, Handle);
            HFuncTraced(TEvPQ::TEvPartitionStatus, Handle);
            HFuncTraced(TEvPersQueue::TEvReportPartitionError, Handle);
            HFuncTraced(TEvPQ::TEvChangeOwner, Handle);
            HFuncTraced(TEvPersQueue::TEvHasDataInfo, Handle);
            HFuncTraced(TEvPQ::TEvMirrorerCounters, Handle);
            HFuncTraced(TEvPQ::TEvProxyResponse, Handle);
            HFuncTraced(TEvPQ::TEvError, Handle);
            HFuncTraced(TEvPQ::TEvGetPartitionClientInfo, Handle);
            HFuncTraced(TEvPQ::TEvUpdateAvailableSize, HandleOnIdle);
            HFuncTraced(TEvPQ::TEvReserveBytes, Handle);
            HFuncTraced(TEvPQ::TEvPipeDisconnected, Handle);
            HFuncTraced(TEvPQ::TEvApproveWriteQuota, Handle);
            HFuncTraced(TEvPQ::TEvQuotaDeadlineCheck, Handle);
            HFuncTraced(TEvPQ::TEvRegisterMessageGroup, HandleOnIdle);
            HFuncTraced(TEvPQ::TEvDeregisterMessageGroup, HandleOnIdle);
            HFuncTraced(TEvPQ::TEvSplitMessageGroup, HandleOnIdle);
            HFuncTraced(TEvPersQueue::TEvProposeTransaction, Handle);
            HFuncTraced(TEvPQ::TEvTxCalcPredicate, Handle);
            HFuncTraced(TEvPQ::TEvGetWriteInfoRequest, Handle);
            HFuncTraced(TEvPQ::TEvGetWriteInfoResponse, Handle);
            HFuncTraced(TEvPQ::TEvGetWriteInfoError, Handle);
            HFuncTraced(TEvPQ::TEvProposePartitionConfig, Handle);
            HFuncTraced(TEvPQ::TEvTxCommit, Handle);
            HFuncTraced(TEvPQ::TEvTxRollback, Handle);
            HFuncTraced(TEvPQ::TEvSubDomainStatus, Handle);
            HFuncTraced(TEvPQ::TEvCheckPartitionStatusRequest, Handle);
            HFuncTraced(NReadQuoterEvents::TEvQuotaUpdated, Handle);
            HFuncTraced(NReadQuoterEvents::TEvAccountQuotaCountersUpdated, Handle);
            HFuncTraced(NReadQuoterEvents::TEvQuotaCountersUpdated, Handle);
            HFuncTraced(TEvPQ::TEvProcessChangeOwnerRequests, Handle);
            HFuncTraced(TEvPQ::TEvDeletePartition, Handle);
            IgnoreFunc(TEvPQ::TEvTxBatchComplete);
            hFuncTraced(TEvPQ::TEvRunCompaction, Handle);
        default:
            ALOG_ERROR(NKikimrServices::PERSQUEUE, "Unexpected " << EventStr("StateIdle", ev));
            break;
        };
    }

private:
    enum class EProcessResult {
        Continue,
        ContinueDrop,
        Break,
        NotReady,
        Blocked,
    };

    struct TProcessParametersBase {
        ui64 CurOffset;
        bool HeadCleared;
    };

    struct ProcessParameters : TProcessParametersBase {
        ProcessParameters(TPartitionSourceManager::TModificationBatch& sourceIdBatch)
                : SourceIdBatch(sourceIdBatch) {
            }

        TPartitionSourceManager::TModificationBatch& SourceIdBatch;
        bool OldPartsCleared;
        bool FirstCommitWriteOperations = true;
    };

    static void RemoveMessages(TMessageQueue& src, TMessageQueue& dst);
    void RemovePendingRequests(TMessageQueue& requests);
    void RemoveMessagesToQueue(TMessageQueue& requests);
    static TString GetConsumerDeletedMessage(TStringBuf consumerName);

private:
    ui64 TabletID;
    ui32 TabletGeneration;
    TPartitionId Partition;
    NKikimrPQ::TPQTabletConfig Config;
    NKikimrPQ::TPQTabletConfig TabletConfig;
    const NKikimrPQ::TPQTabletConfig::TPartition* PartitionConfig = nullptr;
    const NKikimrPQ::TPQTabletConfig::TPartition* PendingPartitionConfig = nullptr;

    const TTabletCountersBase& Counters;
    NPersQueue::TTopicConverterPtr TopicConverter;
    bool IsLocalDC;
    TString DCId;

    TPartitionGraph PartitionGraph;
    TPartitionSourceManager SourceManager;

    struct TSourceIdPostPersistInfo {
        ui64 SeqNo = 0;
        ui64 Offset = 0;

        // KafkaProducerEpoch field is only used for Kafka API,
        // in particular for idempotent or transactional producer.
        // The value is always Nothing() for Topic API.
        TMaybe<i16> KafkaProducerEpoch = 0;
    };

    struct TSeqNoProducerEpoch {
        ui64 SeqNo = 0;

        // KafkaProducerEpoch field is only used for Kafka API,
        // in particular for idempotent or transactional producer.
        // The value is always Nothing() for Topic API.
        TMaybe<i16> KafkaProducerEpoch = 0;
    };

    THashSet<TString> TxAffectedSourcesIds;
    THashSet<TString> WriteAffectedSourcesIds;
    THashSet<TString> TxAffectedConsumers;
    THashSet<TString> SetOffsetAffectedConsumers;
    THashMap<TString, TSourceIdPostPersistInfo> TxSourceIdForPostPersist;
    THashMap<TString, TSeqNoProducerEpoch> TxInflightMaxSeqNoPerSourceId;


    ui32 MaxBlobSize;
    const ui32 TotalLevels = 4;
    TVector<ui32> CompactLevelBorder;
    ui32 TotalMaxCount;
    ui32 MaxSizeCheck;

//                           [ 8+Mb][ 8+Mb ][not compacted data    ] [ data sended to KV but not yet confirmed]
//ofsets in partition:       101 102|103 104|105 106 107 108 109 110|111 112 113
//                            ^               ^                       ^
//                          StartOffset     HeadOffset                EndOffset
//                          [DataKeysBody  ][DataKeysHead                      ]
    TInstant EndWriteTimestamp;
    TInstant PendingWriteTimestamp;

    ui64 WriteInflightSize;
    TActorId Tablet;
    TActorId BlobCache;

    TMessageQueue PendingRequests;
    TMessageQueue QuotaWaitingRequests;

    std::shared_ptr<std::deque<TString>> DeletedKeys;
    std::deque<TBlobKeyTokenPtr> DefferedKeysForDeletion;

    TPartitionBlobEncoder CompactionBlobEncoder; // Compaction zone
    TPartitionBlobEncoder BlobEncoder;           // FastWrite zone

    std::deque<std::pair<ui64, ui64>> GapOffsets;
    ui64 GapSize;

    TString CloudId;
    TString DbId;
    TString DbPath;
    bool IsServerless;
    TString FolderId;

    TMaybe<TUsersInfoStorage> UsersInfoStorage;

    // template <class T> T& GetUserActionAndTransactionEventsFront();
    // template <class T> T& GetCurrentEvent();
    //TSimpleSharedPtr<TTransaction>& GetCurrentTransaction();

    EProcessResult PreProcessUserActionOrTransaction(TSimpleSharedPtr<TEvPQ::TEvSetClientInfo>& event);
    EProcessResult PreProcessUserActionOrTransaction(TSimpleSharedPtr<TEvPersQueue::TEvProposeTransaction>& event);
    EProcessResult PreProcessUserActionOrTransaction(TSimpleSharedPtr<TTransaction>& tx);
    EProcessResult PreProcessUserActionOrTransaction(TMessage& msg);

    bool ExecUserActionOrTransaction(TSimpleSharedPtr<TEvPQ::TEvSetClientInfo>& event, TEvKeyValue::TEvRequest* request);

    bool ExecUserActionOrTransaction(TSimpleSharedPtr<TEvPersQueue::TEvProposeTransaction>& event,
                                     TEvKeyValue::TEvRequest* request);
    bool ExecUserActionOrTransaction(TSimpleSharedPtr<TTransaction>& tx, TEvKeyValue::TEvRequest* request);
    bool ExecUserActionOrTransaction(TMessage& msg, TEvKeyValue::TEvRequest* request);

    [[nodiscard]] EProcessResult PreProcessUserAct(TEvPQ::TEvSetClientInfo& act, const TActorContext& ctx);
    void CommitUserAct(TEvPQ::TEvSetClientInfo& act);


    [[nodiscard]] EProcessResult PreProcessImmediateTx(const NKikimrPQ::TEvProposeTransaction& tx);
    void ExecImmediateTx(TTransaction& tx);

    EProcessResult PreProcessRequest(TRegisterMessageGroupMsg& msg);
    EProcessResult PreProcessRequest(TDeregisterMessageGroupMsg& msg);
    EProcessResult PreProcessRequest(TSplitMessageGroupMsg& msg);
    EProcessResult PreProcessRequest(TWriteMsg& msg);

    void ExecRequest(TRegisterMessageGroupMsg& msg, ProcessParameters& parameters);
    void ExecRequest(TDeregisterMessageGroupMsg& msg, ProcessParameters& parameters);
    void ExecRequest(TSplitMessageGroupMsg& msg, ProcessParameters& parameters);
    bool ExecRequest(TWriteMsg& msg, ProcessParameters& parameters, TEvKeyValue::TEvRequest* request);

    [[nodiscard]] EProcessResult BeginTransaction(const TEvPQ::TEvTxCalcPredicate& event, TMaybe<bool>& predicate);

    EProcessResult ApplyWriteInfoResponse(TTransaction& tx);

    bool FirstEvent = true;
    bool HaveWriteMsg = false;
    bool HaveCheckDisk = false;
    bool HaveDrop = false;
    TMaybe<TPartitionSourceManager::TModificationBatch> SourceIdBatch;
    TMaybe<ProcessParameters> Parameters;
    THolder<TEvKeyValue::TEvRequest> PersistRequest;
    NWilson::TSpan PersistRequestSpan;
    NWilson::TSpan CurrentPersistRequestSpan;

    void AttachPersistRequestSpan(NWilson::TSpan& span); // create persist request span if we have non empty trace id in input span, then link it

    void BeginHandleRequests(TEvKeyValue::TEvRequest* request, const TActorContext& ctx);
    void EndHandleRequests(TEvKeyValue::TEvRequest* request, const TActorContext& ctx);
    void BeginProcessWrites(const TActorContext& ctx);
    void EndProcessWrites(TEvKeyValue::TEvRequest* request, const TActorContext& ctx);
    void EndProcessWritesForCompaction(TEvKeyValue::TEvRequest* request, ui64 blobCreationUnixTime, const TActorContext& ctx);
    void BeginAppendHeadWithNewWrites(const TActorContext& ctx);
    void EndAppendHeadWithNewWrites(const TActorContext& ctx);

    //
    // user actions and transactions
    //
    struct TUserActionAndTransactionEvent {
        std::variant<TSimpleSharedPtr<TEvPQ::TEvSetClientInfo>,             // user actions
                     TSimpleSharedPtr<TTransaction>,                        // distributed transaction or update config
                     TMessage> Event;
        TUserActionAndTransactionEvent(TSimpleSharedPtr<TTransaction>&& transaction)
            : Event(std::move(transaction))
        {}
        TUserActionAndTransactionEvent(TSimpleSharedPtr<TEvPQ::TEvSetClientInfo>&& userAct)
            : Event(std::move(userAct))
        {}
        TUserActionAndTransactionEvent(TMessage&& message)
            : Event(std::move(message))
        {}
    };

    std::deque<TUserActionAndTransactionEvent> UserActionAndTransactionEvents;
    std::deque<TUserActionAndTransactionEvent> UserActionAndTxPendingCommit;
    TVector<THolder<TEvPQ::TEvGetWriteInfoResponse>> WriteInfosApplied;

    THashMap<ui64, TSimpleSharedPtr<TTransaction>> TransactionsInflight;
    THashMap<TActorId, TSimpleSharedPtr<TTransaction>> WriteInfosToTx;

    size_t ImmediateTxCount = 0;
    THashMap<TString, size_t> UserActCount;
    THashMap<TString, TUserInfoBase> PendingUsersInfo;
    TVector<std::pair<TActorId, std::unique_ptr<IEventBase>>> Replies;
    THashSet<TString> AffectedUsers;
    bool KVWriteInProgress = false;
    TMaybe<ui64> PlanStep;
    TMaybe<ui64> TxId;
    bool TxIdHasChanged = false;
    TSimpleSharedPtr<TEvPQ::TEvChangePartitionConfig> ChangeConfig;
    TEvPQ::TMessageGroupsPtr PendingExplicitMessageGroups;
    TVector<THolder<TEvPQ::TEvSetClientInfo>> ChangeConfigActs;
    bool ChangingConfig = false;
    bool SendChangeConfigReply = true;
    TMessageQueue Responses;
    ui64 CurrentBatchSize = 0;

    enum class ETxBatchingState{
        PreProcessing,
        Executing,
        Finishing
    };
    ETxBatchingState BatchingState = ETxBatchingState::PreProcessing;
    //
    //
    //
    std::deque<std::pair<TString, ui64>> UpdateUserInfoTimestamp;
    bool ReadingTimestamp;
    TString ReadingForUser;
    ui64 ReadingForUserReadRuleGeneration;
    ui64 ReadingForOffset; // log only

    THashMap<ui64, TReadInfo> ReadInfo;    // cookie -> {...}
    ui64 Cookie;
    TInstant CreationTime;
    TDuration InitDuration;
    bool InitDone;
    bool NewPartition;
    ui64 PQRBCookie = 0;

    THashMap<TString, NKikimr::NPQ::TOwnerInfo> Owners;
    THashSet<TActorId> OwnerPipes;

    TSourceIdStorage SourceIdStorage;

    std::deque<THolder<TEvPQ::TEvChangeOwner>> WaitToChangeOwner;

    TTabletCountersBase TabletCounters;
    THolder<TPartitionLabeledCounters> PartitionCountersLabeled;
    TInstant LastCountersUpdate;

    TSubscriber Subscriber;

    TInstant WriteCycleStartTime;
    ui32 WriteCycleSize = 0;
    ui32 WriteCycleSizeEstimate = 0;
    ui32 WriteKeysSizeEstimate = 0;
    ui32 WriteNewSize = 0;
    ui32 WriteNewSizeFull = 0;
    ui32 WriteNewSizeInternal = 0;
    ui64 WriteNewSizeUncompressed = 0;
    ui64 WriteNewSizeUncompressedFull = 0;

    ui32 WriteNewMessages = 0;
    ui32 WriteNewMessagesInternal = 0;

    TInstant CurrentTimestamp;

    bool DiskIsFull;
    bool SubDomainOutOfSpace;

    TSet<THasDataReq> HasDataRequests;
    TSet<THasDataDeadline> HasDataDeadlines;
    ui64 HasDataReqNum;

    TActorId ReadQuotaTrackerActor;
    TActorId WriteQuotaTrackerActor;
    THolder<TPercentileCounter> PartitionWriteQuotaWaitCounter;
    TInstant QuotaDeadline = TInstant::Zero();

    TVector<NSlidingWindow::TSlidingWindow<NSlidingWindow::TSumOperation<ui64>>> AvgWriteBytes;
    NSlidingWindow::TSlidingWindow<NSlidingWindow::TSumOperation<ui64>> AvgReadBytes;
    TVector<NSlidingWindow::TSlidingWindow<NSlidingWindow::TSumOperation<ui64>>> AvgQuotaBytes;

    std::unique_ptr<NSlidingWindow::TSlidingWindow<NSlidingWindow::TSumOperation<ui64>>> SplitMergeAvgWriteBytes;
    TInstant LastScaleRequestTime = TInstant::Zero();
    NKikimrPQ::EScaleStatus ScaleStatus = NKikimrPQ::EScaleStatus::NORMAL;

    ui64 ReservedSize;
    std::deque<THolder<TEvPQ::TEvReserveBytes>> ReserveRequests;

    ui32 Channel;
    ui32 NumChannels;
    TVector<ui32> TotalChannelWritesByHead;

    TWorkingTimeCounter WriteBufferIsFullCounter;

    TInstant WriteTimestamp;
    TInstant WriteTimestampEstimate;
    bool ManageWriteTimestampEstimate = true;
    NSlidingWindow::TSlidingWindow<NSlidingWindow::TMaxOperation<ui64>> WriteLagMs;
    //ToDo - counters.
    THolder<TPercentileCounter> InputTimeLag;
    THolder<TMultiBucketCounter> SupportivePartitionTimeLag;
    TPartitionHistogramWrapper MessageSize;

    TPercentileCounter WriteLatency;

    NKikimr::NPQ::TMultiCounter SLIBigLatency;
    NKikimr::NPQ::TMultiCounter WritesTotal;

    TPartitionCounterWrapper BytesWrittenTotal;

    TPartitionCounterWrapper BytesWrittenGrpc;
    TPartitionCounterWrapper BytesWrittenUncompressed;
    NKikimr::NPQ::TMultiCounter BytesWrittenComp;
    TPartitionCounterWrapper MsgsWrittenTotal;
    TPartitionCounterWrapper MsgsWrittenGrpc;

    NKikimr::NPQ::TMultiCounter MsgsDiscarded;
    NKikimr::NPQ::TMultiCounter BytesDiscarded;

    TMultiCounter CompactionUnprocessedCount;
    TMultiCounter CompactionUnprocessedBytes;
    TMultiCounter CompactionTimeLag;

    // Writing blob with topic quota variables
    ui64 TopicQuotaRequestCookie = 0;
    ui64 NextTopicWriteQuotaRequestCookie = 1;
    ui64 BlobQuotaSize = 0;
    bool NeedDeletePartition = false;

    // Wait topic quota metrics
    ui64 TotalPartitionWriteSpeed = 0;
    THolder<TPercentileCounter> TopicWriteQuotaWaitCounter;
    TInstant WriteStartTime;
    TDuration TopicQuotaWaitTimeForCurrentBlob;
    TDuration PartitionQuotaWaitTimeForCurrentBlob;

    TDeque<NKikimrPQ::TStatusResponse::TErrorMessage> Errors;

    THolder<TMirrorerInfo> Mirrorer;

    TInstant LastUsedStorageMeterTimestamp;

    using TPendingEvent = std::variant<
        std::unique_ptr<TEvPQ::TEvTxCalcPredicate>,
        std::unique_ptr<TEvPQ::TEvTxCommit>,
        std::unique_ptr<TEvPQ::TEvTxRollback>,
        std::unique_ptr<TEvPQ::TEvProposePartitionConfig>,
        std::unique_ptr<TEvPQ::TEvGetWriteInfoRequest>,
        std::unique_ptr<TEvPQ::TEvGetWriteInfoResponse>,
        std::unique_ptr<TEvPQ::TEvGetWriteInfoError>,
        std::unique_ptr<TEvPQ::TEvDeletePartition>
    >;

    TDeque<TPendingEvent> PendingEvents;

    template <class T> void AddPendingEvent(TAutoPtr<TEventHandle<T>>& ev);
    template <class T> void ProcessPendingEvent(std::unique_ptr<T> ev, const TActorContext& ctx);
    template <class T> void ProcessPendingEvent(TAutoPtr<TEventHandle<T>>& ev, const TActorContext& ctx);
    void ProcessPendingEvents(const TActorContext& ctx);

    TRowVersion LastEmittedHeartbeat;

    TLastCounter SourceIdCounter;

    const NKikimrPQ::TPQTabletConfig::TPartition* GetPartitionConfig(const NKikimrPQ::TPQTabletConfig& config);

    bool ClosedInternalPartition = false;

    bool IsSupportive() const;

    EDeletePartitionState DeletePartitionState = DELETION_NOT_INITED;

    void ScheduleDeletePartitionDone();
    void ScheduleNegativeReplies();
    void AddCmdDeleteRangeForAllKeys(TEvKeyValue::TEvRequest& request);

    void ScheduleNegativeReply(const TEvPQ::TEvSetClientInfo& event);
    void ScheduleNegativeReply(const TEvPersQueue::TEvProposeTransaction& event);
    void ScheduleNegativeReply(const TTransaction& tx);
    void ScheduleNegativeReply(const TMessage& msg);

    void OnHandleWriteResponse(const TActorContext& ctx);

    void ScheduleTransactionCompleted(const NKikimrPQ::TEvProposeTransaction& tx);

    void DestroyActor(const TActorContext& ctx);

    TActorId OffloadActor;

    void AddCmdWrite(const std::optional<TPartitionedBlob::TFormedBlobInfo>& newWrite,
                     TEvKeyValue::TEvRequest* request,
                     ui64 creationUnixTime,
                     const TActorContext& ctx);
    void AddCmdWrite(const std::optional<TPartitionedBlob::TFormedBlobInfo>& newWrite,
                     TEvKeyValue::TEvRequest* request,
                     const TActorContext& ctx);
    void RenameFormedBlobs(const std::deque<TPartitionedBlob::TRenameFormedBlobInfo>& formedBlobs,
                           TProcessParametersBase& parameters,
                           ui32 curWrites,
                           TEvKeyValue::TEvRequest* request,
                           TPartitionBlobEncoder& zone,
                           const TActorContext& ctx);
    ui32 RenameTmpCmdWrites(TEvKeyValue::TEvRequest* request);

    void UpdateAvgWriteBytes(ui64 size, const TInstant& now);

    size_t WriteNewSizeFromSupportivePartitions = 0;

    bool TryAddDeleteHeadKeysToPersistRequest();
    void DumpKeyValueRequest(const NKikimrClient::TKeyValueRequest& request);

    TBlobKeyTokenPtr MakeBlobKeyToken(const TString& key);

    void OnReadComplete(TReadInfo& info,
                        TUserInfo* userInfo,
                        const TEvPQ::TEvBlobResponse* blobResponse,
                        const TActorContext& ctx);

    enum ERequestCookie : ui64 {
        ReadBlobsForCompaction = 0,
        WriteBlobsForCompaction,
        End
    };

    void TryRunCompaction();
    void BlobsForCompactionWereRead(const TVector<NPQ::TRequestedBlob>& blobs);
    void BlobsForCompactionWereWrite();
    ui64 NextReadCookie();
    bool ExecRequestForCompaction(TWriteMsg& p, TProcessParametersBase& parameters, TEvKeyValue::TEvRequest* request, ui64 blobCreationUnixTime);

    bool CompactionInProgress = false;
    size_t CompactionBlobsCount = 0;

    void DumpZones(const char* file = nullptr, unsigned line = 0) const;

    const TPartitionBlobEncoder& GetBlobEncoder(ui64 offset) const;

    size_t GetBodyKeysCountLimit() const;
    ui64 GetCumulativeSizeLimit() const;

    void UpdateCompactionCounters();
    bool ThereIsUncompactedData() const;
    TInstant GetFirstUncompactedBlobTimestamp() const;

    void TryCorrectStartOffset(TMaybe<ui64> offset);
};

} // namespace NKikimr::NPQ
