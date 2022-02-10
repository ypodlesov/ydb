#include "hive_impl.h"
#include "hive_log.h"

namespace NKikimr {
namespace NHive {

class TTxLockTabletExecution : public TTransactionBase<THive> {
private:
    const ui64 TabletId;
    const TActorId OwnerActor;
    const TDuration ReconnectTimeout;
    const bool IsReconnect;

    const TActorId Sender;
    const ui64 Cookie;

    NKikimrProto::EReplyStatus Status;
    TString StatusMessage;
    TActorId PreviousOwner;

public:
    TTxLockTabletExecution(const NKikimrHive::TEvLockTabletExecution& rec, const TActorId& sender, const ui64 cookie, THive* hive)
        : TBase(hive)
        , TabletId(rec.GetTabletID())
        , OwnerActor(GetOwnerActor(rec, sender))
        , ReconnectTimeout(TDuration::MilliSeconds(rec.GetMaxReconnectTimeout()))
        , IsReconnect(rec.GetReconnect())
        , Sender(sender)
        , Cookie(cookie)
    {
        Y_VERIFY(!!Sender);
    }

    TTxType GetTxType() const override { return NHive::TXTYPE_LOCK_TABLET_EXECUTION; }

    bool Execute(TTransactionContext& txc, const TActorContext&) override {
        BLOG_D("THive::TTxLockTabletExecution::Execute");

        if (!OwnerActor) {
            Status = NKikimrProto::ERROR;
            StatusMessage = TStringBuilder() << "Trying to lock tablet " << TabletId
                << " to an invalid owner actor";
            return true;
        }

        TLeaderTabletInfo* tablet = Self->FindTablet(TabletId);
        if (tablet == nullptr) {
            Status = NKikimrProto::ERROR;
            StatusMessage = TStringBuilder() << "Trying to lock tablet " << TabletId
                << ", which doesn't exist";
            return true;
        }

        if (OwnerActor.NodeId() != Sender.NodeId()) {
            Status = NKikimrProto::ERROR;
            StatusMessage = TStringBuilder() << "Trying to lock tablet " << TabletId
                << " to " << OwnerActor << ", which is on a different node";
            return true;
        }

        if (IsReconnect && tablet->LockedToActor != OwnerActor) {
            Status = NKikimrProto::ERROR;
            StatusMessage = TStringBuilder() << "Trying to restore lock to tablet " << TabletId
                << ", which has expired";
            return true;
        }

        // Mark tablet locked to the new owner
        PreviousOwner = tablet->SetLockedToActor(OwnerActor, ReconnectTimeout);

        // Persist to database
        NIceDb::TNiceDb db(txc.DB);
        db.Table<Schema::Tablet>().Key(TabletId).Update(
            NIceDb::TUpdate<Schema::Tablet::LockedToActor>(tablet->LockedToActor),
            NIceDb::TUpdate<Schema::Tablet::LockedReconnectTimeout>(tablet->LockedReconnectTimeout.MilliSeconds()));

        Status = NKikimrProto::OK;
        return true;
    }

    void Complete(const TActorContext& ctx) override {
        BLOG_D("THive::TTxLockTabletExecution::Complete TabletId: " << TabletId
                << " Owner: " << OwnerActor << " Status: " << Status << " " << StatusMessage);

        ui32 flags = 0;
        if (Status == NKikimrProto::OK) {
            if (PreviousOwner && PreviousOwner != OwnerActor) {
                // Notify previous owner that its lock ownership has been lost
                ctx.Send(PreviousOwner, new TEvHive::TEvLockTabletExecutionLost(TabletId));
            }

            if (TLeaderTabletInfo* tablet = Self->FindTablet(TabletId)) {
                // Tablet still exists by the time transaction finished
                if (tablet->IsLockedToActor()) {
                    // Make sure running tablets will be stopped
                    for (auto& follower : tablet->Followers) {
                        follower.InitiateStop();
                    }
                    tablet->InitiateStop();
                }
                if (tablet->LockedToActor == OwnerActor && tablet->PendingUnlockSeqNo == 0) {
                    // Lock is still valid, watch for node disconnections
                    flags |= IEventHandle::FlagSubscribeOnSession;
                }
            }
        }

        ctx.Send(Sender, new TEvHive::TEvLockTabletExecutionResult(TabletId, Status, StatusMessage), flags, Cookie);
    }

private:
    static TActorId GetOwnerActor(const NKikimrHive::TEvLockTabletExecution& rec, const TActorId& sender) {
        TActorId owner = sender;
        if (rec.HasOwnerActor()) {
            owner = ActorIdFromProto(rec.GetOwnerActor());
        }
        return owner;
    }
};

ITransaction* THive::CreateLockTabletExecution(const NKikimrHive::TEvLockTabletExecution& rec, const TActorId& sender, const ui64 cookie) {
    return new TTxLockTabletExecution(rec, sender, cookie, this);
}

void THive::Handle(TEvHive::TEvLockTabletExecution::TPtr& ev) {
    Execute(CreateLockTabletExecution(ev->Get()->Record, ev->Sender, ev->Cookie));
}

} // NHive
} // NKikimr
