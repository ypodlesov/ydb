#include "datashard_impl.h"
#include "datashard__engine_host.h"
#include "read_table_scan.h"

#include <ydb/core/engine/mkql_proto.h>
#include <ydb/core/protos/counters_datashard.pb.h>
#include <ydb/core/protos/ydb_result_set_old.pb.h>

#include <ydb/library/binary_json/read.h>
#include <ydb/library/dynumber/dynumber.h>

//#include <library/cpp/actors/interconnect/interconnect.h>

//#include <util/generic/cast.h>

namespace NKikimr {
namespace NDataShard {

using NTable::EScan;

//YdbOld and Ydb.v1 have same value representation
template<typename TOutValue>
Y_FORCE_INLINE void AddCell(TOutValue& row, NScheme::TTypeId type, const TCell &cell)
{
    auto &val = *row.add_items();

    if (cell.IsNull()) {
        val.set_null_flag_value(::google::protobuf::NULL_VALUE);
        return;
    }

    switch (type) {
    case NUdf::TDataType<bool>::Id:
        val.set_bool_value(cell.AsValue<bool>());
        break;
    case NUdf::TDataType<ui8>::Id:
        val.set_uint32_value(cell.AsValue<ui8>());
        break;
    case NUdf::TDataType<i8>::Id:
        val.set_int32_value(cell.AsValue<i8>());
        break;
    case NUdf::TDataType<ui16>::Id:
        val.set_uint32_value(cell.AsValue<ui16>());
        break;
    case NUdf::TDataType<i16>::Id:
        val.set_int32_value(cell.AsValue<i16>());
        break;
    case NUdf::TDataType<ui32>::Id:
        val.set_uint32_value(cell.AsValue<ui32>());
        break;
    case NUdf::TDataType<i32>::Id:
        val.set_int32_value(cell.AsValue<i32>());
        break;
    case NUdf::TDataType<ui64>::Id:
        val.set_uint64_value(cell.AsValue<ui64>());
        break;
    case NUdf::TDataType<i64>::Id:
        val.set_int64_value(cell.AsValue<i64>());
        break;
    case NUdf::TDataType<float>::Id:
        val.set_float_value(cell.AsValue<float>());
        break;
    case NUdf::TDataType<double>::Id:
        val.set_double_value(cell.AsValue<double>());
        break;
    case NUdf::TDataType<NUdf::TJson>::Id:
    case NUdf::TDataType<NUdf::TYson>::Id:
    case NUdf::TDataType<NUdf::TUtf8>::Id:
        val.set_text_value(cell.Data(), cell.Size());
        break;
    case NUdf::TDataType<NUdf::TDecimal>::Id:
        {
            Y_VERIFY_DEBUG(cell.Size() == 16);
            struct TCellData {
                ui64 Low;
                ui64 High;
            };
            const auto data = cell.AsValue<TCellData>();
            val.set_low_128(data.Low);
            val.set_high_128(data.High);
        }
        break;
    case NUdf::TDataType<NUdf::TDate>::Id:
        val.set_uint32_value(cell.AsValue<ui16>());
        break;
    case NUdf::TDataType<NUdf::TDatetime>::Id:
        val.set_uint32_value(cell.AsValue<ui32>());
        break;
    case NUdf::TDataType<NUdf::TTimestamp>::Id:
        val.set_uint64_value(cell.AsValue<ui64>());
        break;
    case NUdf::TDataType<NUdf::TInterval>::Id:
        val.set_int64_value(cell.AsValue<i64>());
        break;
    case NUdf::TDataType<NUdf::TJsonDocument>::Id: {
        const auto json = NBinaryJson::SerializeToJson(TStringBuf(cell.Data(), cell.Size()));
        val.set_text_value(json);
        break;
    }
    case NUdf::TDataType<NUdf::TDyNumber>::Id: {
        const auto number = NDyNumber::DyNumberToString(TStringBuf(cell.Data(), cell.Size()));
        Y_VERIFY(number.Defined(), "Invalid DyNumber binary representation");
        val.set_text_value(*number);
        break;
    }
    default:
        val.set_bytes_value(cell.Data(), cell.Size());
    }
}

class TRowsToResult {
public:
    TRowsToResult(const NKikimrTxDataShard::TReadTableTransaction &request)
        : CurrentMessageRows(0)
        , ReservedSize(0)
        , ResultStream(ResultString)
    {
        for (auto &col : request.GetColumns())
            ColTypes.push_back(col.GetTypeId());
    }

    virtual ~TRowsToResult() = default;

    ui64 GetMessageSize() const { return ResultString.size(); }
    ui64 GetMessageRows() const { return CurrentMessageRows; }

    void PutRow(const NTable::TRowState& row) {
        RowOffsets.push_back(static_cast<ui32>(ResultString.size()));
        DoPutRow(row);
        ++CurrentMessageRows;
    }

    void Reserve(ui64 size)
    {
        ReservedSize = size;
        ResultStream.Reserve(size);
    }

    void Flush(NKikimrTxDataShard::TEvProposeTransactionResult &res)
    {
        *res.MutableTxResult() = ResultString;
        auto version = GetResultVersion();
        if (version)
            res.SetApiVersion(version);
        for (auto &offset : RowOffsets)
            res.AddRowOffsets(offset);
        StartNewMessage();
    }

protected:
    void StartNewMessage()
    {
        RowOffsets.clear();
        ResultString.clear();

        ResultStream << ResultCommon;
        Reserve(ReservedSize);

        CurrentMessageRows = 0;
    }
private:
    virtual void DoPutRow(const NTable::TRowState &row) = 0;
    virtual ui32 GetResultVersion() const = 0;

    ui64 CurrentMessageRows;
    ui64 ReservedSize;
    TString ResultString;

protected:
    TStringOutput ResultStream;
    TVector<NScheme::TTypeId> ColTypes;
    TVector<ui32> RowOffsets;
    TString ResultCommon;
};

class TRowsToOldResult : public TRowsToResult {
public:
    TRowsToOldResult(const NKikimrTxDataShard::TReadTableTransaction& request)
        : TRowsToResult(request)
    {
        BuildResultCommonPart(request);
        StartNewMessage();
    }

private:
    void DoPutRow(const NTable::TRowState &row) override
    {
        auto &protoRow = *OldResultSet.add_rows();
        auto cells = *row;

        for (size_t col = 0; col < cells.size(); ++col) {
            AddCell(protoRow, ColTypes[col], cells[col]);
        }

        OldResultSet.SerializeToArcadiaStream(&ResultStream);
        OldResultSet.Clear();
    }

    ui32 GetResultVersion() const override { return 0; }

    void BuildResultCommonPart(const NKikimrTxDataShard::TReadTableTransaction &request)
    {
        YdbOld::ResultSet res;

        for (auto &col : request.GetColumns()) {
            auto *meta = res.add_column_meta();
            meta->set_name(col.GetName());
            auto id = static_cast<NYql::NProto::TypeIds>(col.GetTypeId());
            meta->mutable_type()->mutable_optional_type()->mutable_item()->mutable_data_type()->set_id(id);
        }
        res.set_truncated(true);
        Y_PROTOBUF_SUPPRESS_NODISCARD res.SerializeToString(&ResultCommon);
    }

    YdbOld::ResultSet OldResultSet;
};

class TRowsToYdbResult : public TRowsToResult {
public:
    TRowsToYdbResult(const NKikimrTxDataShard::TReadTableTransaction& request)
        : TRowsToResult(request)
    {
        BuildResultCommonPart(request);
        StartNewMessage();
    }

private:
    void DoPutRow(const NTable::TRowState& row) override
    {
        auto &protoRow = *YdbResultSet.add_rows();
        auto cells = *row;

        for (size_t col = 0; col < cells.size(); ++col) {
            AddCell(protoRow, ColTypes[col], cells[col]);
        }

        YdbResultSet.SerializeToArcadiaStream(&ResultStream);
        YdbResultSet.Clear();
    }

    ui32 GetResultVersion() const override { return NKikimrTxUserProxy::TReadTableTransaction::YDB_V1; }

    void BuildResultCommonPart(const NKikimrTxDataShard::TReadTableTransaction &request)
    {
        Ydb::ResultSet res;
        for (auto &col : request.GetColumns()) {
            auto *meta = res.add_columns();
            meta->set_name(col.GetName());
            auto id = static_cast<NYql::NProto::TypeIds>(col.GetTypeId());
            if (id == NYql::NProto::Decimal) {
                auto decimalType = meta->mutable_type()->mutable_optional_type()->mutable_item()->mutable_decimal_type();
                //TODO: Pass decimal params here
                decimalType->set_precision(22);
                decimalType->set_scale(9);
            } else {
                meta->mutable_type()->mutable_optional_type()->mutable_item()
                        ->set_type_id(static_cast<Ydb::Type::PrimitiveTypeId>(id));
            }
        }
        res.set_truncated(true);
        Y_PROTOBUF_SUPPRESS_NODISCARD res.SerializeToString(&ResultCommon);
    }

    Ydb::ResultSet YdbResultSet;
};

class TReadTableScan : public TActor<TReadTableScan>, public NTable::IScan {
public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::TX_READ_TABLE_SCAN;
    }

    TReadTableScan(ui64 txId, ui64 shardId, TUserTable::TCPtr tableInfo,
                   const NKikimrTxDataShard::TReadTableTransaction &tx, TActorId sink,
                   TActorId dataShard)
        : TActor(&TThis::StateWork)
        , Sink(sink)
        , DataShard(dataShard)
        , TxId(txId)
        , ShardId(shardId)
        , Driver(nullptr)
        , MessageQuota(0)
        , MessageSizeLimit(10 << 20)
        , TableInfo(tableInfo)
        , Tx(tx)
        , ScanRange(tx.GetRange())
        , CheckUpper(false)
        , PendingAcks(0)
        , Finished(false)
    {
        if (tx.HasApiVersion() && tx.GetApiVersion() == NKikimrTxUserProxy::TReadTableTransaction::YDB_V1) {
            Writer = MakeHolder<TRowsToYdbResult>(tx);
        } else {
            Writer = MakeHolder<TRowsToOldResult>(tx);
        }

        for (auto &col : tx.GetColumns())
            Tags.push_back(col.GetId());
    }

    ~TReadTableScan() {}

    void Describe(IOutputStream &out) const noexcept override
    {
        out << "TReadTableScan";
    }

    STFUNC(StateWork) {
        switch (ev->GetTypeRewrite()) {
            HFunc(TEvTxProcessing::TEvStreamDataAck, Handle);
            HFunc(TEvTxProcessing::TEvStreamIsDead, Handle);
            HFunc(TEvTxProcessing::TEvStreamQuotaResponse, Handle);
            HFunc(TEvents::TEvUndelivered, Undelivered);
            HFunc(TEvInterconnect::TEvNodeDisconnected, Disconnected);
            HFunc(TEvDataShard::TEvGetReadTableScanStateRequest, Handle);
            IgnoreFunc(TEvInterconnect::TEvNodeConnected);
        default:
            LOG_ERROR(ctx, NKikimrServices::TX_DATASHARD,
                      "TReadTableScan: StateWork unexpected event type: %" PRIx32 " event: %s",
                      ev->GetTypeRewrite(), ev->HasEvent() ? ev->GetBase()->ToString().data() : "serialized?");
        }
    }

private:
    void Die(const TActorContext &ctx) override
    {
        ctx.Send(TActivationContext::InterconnectProxy(Sink.NodeId()),
                 new TEvents::TEvUnsubscribe());

        TActor<TReadTableScan>::Die(ctx);
    }

    void Undelivered(TEvents::TEvUndelivered::TPtr &, const TActorContext &ctx)
    {
        LOG_ERROR(ctx, NKikimrServices::TX_DATASHARD,
                  "TReadTableScan: undelivered event TxId: %" PRIu64, TxId);

        Error = "cannot reach sink actor";
        Driver->Touch(EScan::Final);
    }

    void Disconnected(TEvInterconnect::TEvNodeDisconnected::TPtr &, const TActorContext &ctx)
    {
        LOG_ERROR(ctx, NKikimrServices::TX_DATASHARD,
                  "TReadTableScan: disconnect TxId: %" PRIu64, TxId);

        Error = "cannot reach sink actor";
        Driver->Touch(EScan::Final);
    }

    void Handle(TEvTxProcessing::TEvStreamDataAck::TPtr &, const TActorContext &ctx)
    {
        Y_VERIFY(PendingAcks);
        --PendingAcks;

        LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                    "Got stream data ack ShardId: " << ShardId
                    << ", TxId: " << TxId
                    << ", PendingAcks: " << PendingAcks);

        if (Finished && !PendingAcks)
            Driver->Touch(EScan::Feed);
    }

    void Handle(TEvTxProcessing::TEvStreamIsDead::TPtr &ev, const TActorContext &ctx)
    {
        LOG_INFO(ctx, NKikimrServices::TX_DATASHARD,
                 "TReadTableScan: stream disconnect TxId: %" PRIu64, TxId);

        Error = "got dead stream notification";
        Driver->Touch(EScan::Final);
        ctx.Send(ev->Forward(Sink));
    }

    void Handle(TEvTxProcessing::TEvStreamQuotaResponse::TPtr &ev, const TActorContext &ctx)
    {
        bool touch = !MessageQuota;
        auto &rec = ev->Get()->Record;

        MessageSizeLimit = rec.GetMessageSizeLimit();
        MessageQuota += rec.GetReservedMessages();
        RowLimit = rec.GetRowLimit();

        Writer->Reserve(MessageSizeLimit);

        LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                    "Got quota for read table scan ShardId: " << ShardId
                    << ", TxId: " << TxId
                    << ", MessageQuota: " << MessageQuota);

        CheckQuota(ctx);

        if (MessageQuota && touch)
            Driver->Touch(EScan::Feed);
    }

    void Handle(TEvDataShard::TEvGetReadTableScanStateRequest::TPtr &ev,
                const TActorContext &ctx)
    {
        auto *response = new TEvDataShard::TEvGetReadTableScanStateResponse;
        response->Record.MutableStatus()->SetCode(Ydb::StatusIds::SUCCESS);

        response->Record.SetTxId(TxId);
        response->Record.SetMessageQuota(MessageQuota);
        response->Record.SetMessageSizeLimit(MessageSizeLimit);
        response->Record.SetRowsLimit(RowLimit);
        response->Record.SetPendingAcks(PendingAcks);
        response->Record.SetResultSize(Writer->GetMessageSize());
        response->Record.SetResultRows(Writer->GetMessageRows());
        response->Record.SetHasUpperBorder(CheckUpper);
        response->Record.SetFinished(Finished);
        response->Record.SetError(Error);

        ctx.Send(ev->Sender, response);
    }

    void CheckQuota(const TActorContext &ctx)
    {
        if (MessageQuota)
            return;

        TAutoPtr<TEvTxProcessing::TEvStreamQuotaRequest> request
            = new TEvTxProcessing::TEvStreamQuotaRequest;
        request->Record.SetTxId(TxId);
        request->Record.SetShardId(ShardId);
        ctx.Send(Sink, request.Release(),
                 IEventHandle::FlagTrackDelivery | IEventHandle::FlagSubscribeOnSession);
    }

    THello Prepare(IDriver *driver, TIntrusiveConstPtr<TScheme> scheme) noexcept override
    {
        Driver = driver;

        auto ctx = TActivationContext::AsActorContext();
        auto aid = ctx.RegisterWithSameMailbox(this);

        for (const auto& columnRecord : Tx.GetColumns()) {
            if (!scheme->ColInfo(columnRecord.GetId())) {
                Error = TStringBuilder() << "ReadTable cannot find column "
                    << columnRecord.GetName() << " (" << columnRecord.GetId() << ")";
                SchemaChanged = true;
                return { EScan::Final, { } };
            }
        }

        auto *ev = new TDataShard::TEvPrivate::TEvRegisterScanActor(TxId);
        ctx.MakeFor(aid).Send(DataShard, ev);

        CheckQuota(ctx.MakeFor(aid));

        return { EScan::Sleep, { } };
    }

    EScan Seek(TLead &lead, ui64 seq) noexcept override
    {
        if (seq) {
            MaybeSendResponseMessage(true);
            Finished = true;

            if (PendingAcks)
                return EScan::Sleep;

            return EScan::Final;
        }

        int cmpFrom;
        int cmpTo;
        cmpFrom = CompareBorders<false, false>(ScanRange.From.GetCells(),
                                               TableInfo->Range.From.GetCells(),
                                               ScanRange.FromInclusive,
                                               TableInfo->Range.FromInclusive,
                                               TableInfo->KeyColumnTypes);
        cmpTo = CompareBorders<true, true>(ScanRange.To.GetCells(),
                                           TableInfo->Range.To.GetCells(),
                                           ScanRange.ToInclusive,
                                           TableInfo->Range.ToInclusive,
                                           TableInfo->KeyColumnTypes);

        if (cmpFrom > 0) {
            auto seek = ScanRange.FromInclusive ? NTable::ESeek::Lower : NTable::ESeek::Upper;
            lead.To(Tags, ScanRange.From.GetCells(), seek);
        } else {
            lead.To(Tags, { }, NTable::ESeek::Lower);
        }

        CheckUpper = (cmpTo < 0);

        if (CheckUpper) {
            lead.Until(ScanRange.To.GetCells(), ScanRange.ToInclusive);
        }

        return EScan::Feed;
    }

    EScan MaybeSendResponseMessage(bool last, const TArrayRef<const TCell>& lastKey = { })
    {
        ui64 rows = Writer->GetMessageRows();

        // Nothing to send.
        if (!rows)
            return EScan::Feed;

        // May collect more rows.
        if (Writer->GetMessageSize() < MessageSizeLimit
            && (!RowLimit || RowLimit > rows)
            && !last)
            return EScan::Feed;

        auto ctx = TActivationContext::AsActorContext().MakeFor(SelfId());
        auto result = new TEvDataShard::TEvProposeTransactionResult(
                          NKikimrTxDataShard::TX_KIND_SCAN,
                          ShardId,
                          TxId,
                          NKikimrTxDataShard::TEvProposeTransactionResult::RESPONSE_DATA);
        Writer->Flush(result->Record);

        // Allows sink to detect missing chunks and resume on failures
        result->Record.SetDataSeqNo(NextDataSeqNo++);
        if (lastKey) {
            result->Record.SetDataLastKey(TSerializedCellVec::Serialize(lastKey));
        }

        ctx.Send(Sink, result,
                 IEventHandle::FlagTrackDelivery | IEventHandle::FlagSubscribeOnSession);

        ++PendingAcks;
        --MessageQuota;

        LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                    "Send response data ShardId: " << ShardId
                    << ", TxId: " << TxId
                    << ", Size: " << Writer->GetMessageSize()
                    << ", Rows: " << Writer->GetMessageRows()
                    << ", PendingAcks: " << PendingAcks
                    << ", MessageQuota: " << MessageQuota);

        if (RowLimit) {
            RowLimit -= rows;
            if (!RowLimit)
                return EScan::Reset;
        }

        if (!last)
            CheckQuota(ctx);

        return MessageQuota ? EScan::Feed : EScan::Sleep;
    }

    EScan Feed(TArrayRef<const TCell> key, const TRow &row) noexcept override
    {
        Y_VERIFY_DEBUG(DebugCheckKeyInRange(key));

        Writer->PutRow(row);

        return MaybeSendResponseMessage(false, key);
    }

    bool DebugCheckKeyInRange(TArrayRef<const TCell> key) {
        auto cmp = CompareBorders<true, true>(
                key, ScanRange.To.GetCells(),
                true, ScanRange.ToInclusive,
                TableInfo->KeyColumnTypes);

        return cmp <= 0;
    }

    TAutoPtr<IDestructable> Finish(EAbort abort) noexcept override
    {
        auto ctx = TActivationContext::ActorContextFor(SelfId());

        if (!SchemaChanged) {
            if (abort != EAbort::None)
                Error = "Aborted by scan host env";

            TAutoPtr<TEvTxProcessing::TEvStreamQuotaRelease> request
                = new TEvTxProcessing::TEvStreamQuotaRelease;
            request->Record.SetTxId(TxId);
            request->Record.SetShardId(ShardId);

            ctx.Send(Sink, request.Release());
        }

        LOG_DEBUG_S(ctx, NKikimrServices::TX_DATASHARD,
                    "Finish scan ShardId: " << ShardId
                    << ", TxId: " << TxId
                    << ", MessageQuota: " << MessageQuota);

        Driver = nullptr;

        Die(ctx);

        return new TReadTableProd(Error, SchemaChanged);
    }

private:
    TVector<ui32> Tags;
    TActorId Sink;
    TActorId DataShard;
    ui64 TxId;
    ui64 ShardId;
    IDriver *Driver;
    ui64 MessageQuota;
    ui64 MessageSizeLimit;
    TString Error;
    THolder<TRowsToResult> Writer;
    TUserTable::TCPtr TableInfo;
    NKikimrTxDataShard::TReadTableTransaction Tx;
    TSerializedTableRange ScanRange;
    bool CheckUpper;
    ui64 RowLimit;
    ui64 PendingAcks;
    ui64 NextDataSeqNo = 1;
    bool Finished;
    bool SchemaChanged = false;
};

TAutoPtr<NTable::IScan> CreateReadTableScan(ui64 txId,
                                        ui64 shardId,
                                        TUserTable::TCPtr tableInfo,
                                        const NKikimrTxDataShard::TReadTableTransaction &tx,
                                        TActorId sink,
                                        TActorId dataShard)
{
    return new TReadTableScan(txId, shardId, tableInfo, tx, sink, dataShard);
}

} // namespace NDataShard
} // namespace NKikimr
