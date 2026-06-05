#include "stdafx.h"
#include "gc_client.h"
#include "graffiti.h"
#include "keyvalue.h"

#include <cstdlib>

// Forward-declared so isteamuser.h's GetAuthSessionTicket signature parses
// (steam_hook.cpp does the same before pulling in the Steam headers).
struct SteamNetworkingIdentity;
#include <steam/steam_api.h>

// Mint a Steam auth session ticket for the local user. The backend's steam-auth
// sidecar validates it via ISteamGameServer::BeginAuthSession, yielding the real
// steamID + ownership + VAC status (keyless). Returns empty on failure, in which
// case the gateway falls back to trusting the asserted steamID.
static std::vector<uint8_t> AcquireAuthTicket()
{
    std::vector<uint8_t> ticket;
    ISteamUser *user = SteamUser();
    if (!user)
    {
        Platform::Print("EdgeTransport: SteamUser() unavailable, sending no ticket\n");
        return ticket;
    }
    uint8_t buffer[2048];
    uint32 length = 0;
    HAuthTicket handle = user->GetAuthSessionTicket(buffer, sizeof(buffer), &length, nullptr);
    if (handle == k_HAuthTicketInvalid || length == 0)
    {
        Platform::Print("EdgeTransport: GetAuthSessionTicket failed\n");
        return ticket;
    }
    ticket.assign(buffer, buffer + length);
    return ticket;
}

ClientGC::ClientGC(uint64_t steamId)
    : m_steamId{ steamId }
    , m_inventory{ steamId }
{
    // also called from ServerGC's constructor
    Graffiti::Initialize();

    // Backend pushes (queue updates, reservations) are injected into the game.
    m_edge.SetPushHandler([this](uint32_t msgType, const std::string &body) {
        InjectFromBackend(msgType, body);
    });

    StartThread();

    Platform::Print("ClientGC spawned for user %llu\n", steamId);
}

ClientGC::~ClientGC()
{
    StopThread();
    Platform::Print("ClientGC destroyed\n");
}

void ClientGC::HandleEvent(GCEvent type, uint64_t id, const std::vector<uint8_t> &buffer)
{
    switch (type)
    {
    case GCEvent::Message:
        HandleMessage(static_cast<uint32_t>(id), buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    case GCEvent::NetMessage:
        HandleNetMessage(buffer.data(), static_cast<uint32_t>(buffer.size()));
        break;

    case GCEvent::SOCacheRequest:
        HandleSOCacheRequest();
        break;

    default:
        assert(false);
        break;
    }
}

void ClientGC::HandleMessage(uint32_t type, const void *data, uint32_t size)
{
    GCMessageRead messageRead{ type, data, size };
    if (!messageRead.IsValid())
    {
        assert(false);
        return;
    }

    if (messageRead.IsProtobuf())
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGCClientHello:
            OnClientHello(messageRead);
            break;

        // Matchmaking and rank messages are owned by the backend: forward them and
        // let it push updates back (GC2ClientUpdate, the populated rank update).
        case k_EMsgGCCStrike15_v2_MatchmakingStart:
        case k_EMsgGCCStrike15_v2_MatchmakingStop:
        case k_EMsgGCCStrike15_v2_ClientGCRankUpdate:
            ForwardToBackend(messageRead.TypeUnmasked(), messageRead);
            break;

        case k_EMsgGCAdjustItemEquippedState:
            AdjustItemEquippedState(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientPlayerDecalSign:
            ClientPlayerDecalSign(messageRead);
            break;

        case k_EMsgGCUseItemRequest:
            UseItemRequest(messageRead);
            break;

        case k_EMsgGCCStrike15_v2_ClientRequestJoinServerData:
            ClientRequestJoinServerData(messageRead);
            break;

        case k_EMsgGCSetItemPositions:
            SetItemPositions(messageRead);
            break;

        case k_EMsgGCApplySticker:
            ApplySticker(messageRead);
            break;

        case k_EMsgGCStoreGetUserData:
            StoreGetUserData(messageRead);
            break;

        case k_EMsgGCStorePurchaseInit:
            StorePurchaseInit(messageRead);
            break;

        case k_EMsgGCStorePurchaseFinalize:
            StorePurchaseFinalize(messageRead);
            break;

        default:
            Platform::Print("ClientGC::HandleMessage: unhandled protobuf message %s\n",
                MessageName(messageRead.TypeUnmasked()));
            break;
        }
    }
    else
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGCDelete:
            DeleteItem(messageRead);
            break;

        case k_EMsgGCUnlockCrate:
            UnlockCrate(messageRead);
            break;

        case k_EMsgGCNameItem:
            NameItem(messageRead);
            break;

        case k_EMsgGCNameBaseItem:
            NameBaseItem(messageRead);
            break;

        case k_EMsgGCRemoveItemName:
            RemoveItemName(messageRead);
            break;

        default:
            Platform::Print("ClientGC::HandleMessage: unhandled struct message %s\n",
                MessageName(messageRead.TypeUnmasked()));
            break;
        }
    }
}

void ClientGC::HandleNetMessage(const void *data, uint32_t size)
{
    // pass 0 as type so it gets parsed from the message
    GCMessageRead messageRead{ 0, data, size };
    if (!messageRead.IsValid())
    {
        assert(false);
        return;
    }

    if (messageRead.IsProtobuf())
    {
        switch (messageRead.TypeUnmasked())
        {
        case k_EMsgGC_IncrementKillCountAttribute:
            IncrementKillCountAttribute(messageRead);
            return;
        }
    }

    Platform::Print("ClientGC::HandleNetMessage: unhandled protobuf message %s\n",
        MessageName(messageRead.TypeUnmasked()));
}

void ClientGC::HandleSOCacheRequest()
{
    CMsgSOCacheSubscribed message;
    m_inventory.BuildCacheSubscription(message, GetConfig().Level(), true);

    GCMessageWrite messageWrite{ k_ESOMsg_CacheSubscribed, message };
    PostToHost(HostEvent::NetMessage, 0, messageWrite.Data(), messageWrite.Size());
}

void ClientGC::SendMessageToGame(bool sendToGameServer, uint32_t type,
    const google::protobuf::MessageLite &message, uint64_t jobId)
{
    GCMessageWrite messageWrite{ type, message, jobId };

    if (sendToGameServer)
    {
        PostToHost(HostEvent::NetMessage, 0, messageWrite.Data(), messageWrite.Size());
    }

    PostToHost(HostEvent::Message, messageWrite.TypeMasked(), messageWrite.Data(), messageWrite.Size());
}

void ClientGC::ForwardToBackend(uint32_t msgType, GCMessageRead &messageRead)
{
    if (!m_edge.Connected())
    {
        Platform::Print("EdgeTransport: not connected, dropping msg %u\n", msgType);
        return;
    }
    std::string_view body = messageRead.Remaining();
    m_edge.Send(msgType, m_steamId, std::string(body));
}

void ClientGC::InjectFromBackend(uint32_t msgType, const std::string &body)
{
    // Runs on the EdgeTransport receive thread; PostToHost is thread-safe.
    GCMessageWrite messageWrite{ msgType, body.data(), static_cast<uint32_t>(body.size()), JobIdInvalid };
    PostToHost(HostEvent::Message, messageWrite.TypeMasked(), messageWrite.Data(), messageWrite.Size());
}

constexpr uint32_t MakeAddress(uint32_t v1, uint32_t v2, uint32_t v3, uint32_t v4)
{
    return v4 | (v3 << 8) | (v2 << 16) | (v1 << 24);
}

static void BuildCSWelcome(CMsgCStrike15Welcome &message)
{
    // mikkotodo cleanup dox
    message.set_store_item_hash(136617352);
    message.set_timeplayedconsecutively(0);
    message.set_time_first_played(1329845773);
    message.set_last_time_played(1680260376);
    message.set_last_ip_address(MakeAddress(127, 0, 0, 1));
}

bool ClientGC::FetchMatchmakingHelloFromBackend(CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &message)
{
    if (!m_edge.Connected())
    {
        const char *host = std::getenv("CSGOGC_BACKEND_HOST");
        const char *portStr = std::getenv("CSGOGC_BACKEND_PORT");
        uint16_t port = portStr ? static_cast<uint16_t>(std::atoi(portStr)) : 9510;
        std::vector<uint8_t> ticket = AcquireAuthTicket();
        if (!m_edge.Connect(host ? host : "127.0.0.1", port,
                1 /* EDGE_ROLE_CLIENT */, m_steamId, ticket))
        {
            return false;
        }
    }

    // Phase 1: an empty Client2GCHello is enough; the backend authors the reply.
    CMsgGCCStrike15_v2_MatchmakingClient2GCHello request;

    uint32_t replyType = 0;
    std::string replyPayload;
    if (!m_edge.Request(k_EMsgGCCStrike15_v2_MatchmakingClient2GCHello, m_steamId,
            request.SerializeAsString(), replyType, replyPayload))
    {
        return false;
    }
    if (replyType != k_EMsgGCCStrike15_v2_MatchmakingGC2ClientHello)
    {
        Platform::Print("EdgeTransport: unexpected hello reply type %u\n", replyType);
        return false;
    }
    return message.ParseFromString(replyPayload);
}

void ClientGC::BuildClientWelcome(CMsgClientWelcome &message, const CMsgCStrike15Welcome &csWelcome,
    const CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &matchmakingHello)
{
    // mikkotodo remove dox
    message.set_version(0); // this is accurate
    message.set_game_data(csWelcome.SerializeAsString());
    m_inventory.BuildCacheSubscription(*message.add_outofdate_subscribed_caches(), GetConfig().Level(), false);
    message.mutable_location()->set_latitude(65.0133006f);
    message.mutable_location()->set_longitude(25.4646212f);
    message.mutable_location()->set_country("FI"); // finland
    message.set_game_data2(matchmakingHello.SerializeAsString());
    message.set_rtime32_gc_welcome_timestamp(static_cast<uint32_t>(time(nullptr)));
    message.set_currency(2); // euros
    message.set_txn_country_code("FI"); // finland
}

void ClientGC::SendRankUpdate(const CMsgGCCStrike15_v2_MatchmakingGC2ClientHello &mmHello)
{
    // Ranks are authored by the backend (the hello), not config.txt. Forward
    // whatever the backend provided: the per-mode rankings array if present,
    // otherwise the single competitive ranking.
    CMsgGCCStrike15_v2_ClientGCRankUpdate message;
    if (mmHello.rankings_size() > 0)
    {
        for (const PlayerRankingInfo &rank : mmHello.rankings())
        {
            *message.add_rankings() = rank;
        }
    }
    else if (mmHello.has_ranking())
    {
        *message.add_rankings() = mmHello.ranking();
    }

    if (message.rankings_size() > 0)
    {
        SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientGCRankUpdate, message);
    }
}

void ClientGC::OnClientHello(GCMessageRead &messageRead)
{
    CMsgClientHello hello;
    if (!messageRead.ReadProtobuf(hello))
    {
        Platform::Print("Parsing CMsgClientHello failed, ignoring\n");
        return;
    }

    // we don't care about anything in this message, just reply
    CMsgCStrike15Welcome csWelcome;
    BuildCSWelcome(csWelcome);

    // The matchmaking hello (rank, profile) is authored by the backend. The shim
    // does not synthesize it locally; if the backend is unreachable the hello
    // cannot be completed.
    CMsgGCCStrike15_v2_MatchmakingGC2ClientHello mmHello;
    if (!FetchMatchmakingHelloFromBackend(mmHello))
    {
        Platform::Print("EdgeTransport: backend unavailable, cannot complete client hello\n");
        return;
    }

    CMsgClientWelcome clientWelcome;
    BuildClientWelcome(clientWelcome, csWelcome, mmHello);

    SendMessageToGame(false, k_EMsgGCClientWelcome, clientWelcome);

    // the real gc sends this a bit later when it has more info to put on it
    // however we have everything at our fingertips so send it right away
    // mikkotodo is this even needed? k_EMsgGCClientWelcome should have it all already
    SendMessageToGame(false, k_EMsgGCCStrike15_v2_MatchmakingGC2ClientHello, mmHello);

    // send all ranks here as well, it's a bit back and forth with real gc
    SendRankUpdate(mmHello);
}

void ClientGC::AdjustItemEquippedState(GCMessageRead &messageRead)
{
    CMsgAdjustItemEquippedState message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgAdjustItemEquippedState failed, ignoring\n");
        return;
    }

    CMsgSOMultipleObjects update;
    if (!m_inventory.EquipItem(message.item_id(), message.new_class(), message.new_slot(), update))
    {
        // no change
        assert(false);
        return;
    }

    // let the gameserver know, too
    SendMessageToGame(true, k_ESOMsg_UpdateMultiple, update);
}

void ClientGC::ClientPlayerDecalSign(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientPlayerDecalSign message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCCStrike15_v2_ClientPlayerDecalSign failed, ignoring\n");
        return;
    }

    if (!Graffiti::SignMessage(*message.mutable_data()))
    {
        Platform::Print("Could not sign graffiti! it won't appear\n");
        return;
    }

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientPlayerDecalSign, message);
}

void ClientGC::UseItemRequest(GCMessageRead &messageRead)
{
    CMsgUseItem message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgUseItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject destroy;
    CMsgSOMultipleObjects updateMultiple;
    CMsgGCItemCustomizationNotification notification;

    if (m_inventory.UseItem(message.item_id(), destroy, updateMultiple, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        SendMessageToGame(true, k_ESOMsg_UpdateMultiple, updateMultiple);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
}

static void AddressString(uint32_t ip, uint32_t port, char *buffer, size_t bufferSize)
{
    snprintf(buffer, bufferSize,
        "%u.%u.%u.%u:%u\n",
        (ip >> 24) & 0xff,
        (ip >> 16) & 0xff,
        (ip >> 8) & 0xff,
        ip & 0xff,
        port);
}

void ClientGC::ClientRequestJoinServerData(GCMessageRead &messageRead)
{
    CMsgGCCStrike15_v2_ClientRequestJoinServerData request;
    if (!messageRead.ReadProtobuf(request))
    {
        Platform::Print("Parsing CMsgGCCStrike15_v2_ClientRequestJoinServerData failed, ignoring\n");
        return;
    }

    CMsgGCCStrike15_v2_ClientRequestJoinServerData response = request;
    response.mutable_res()->set_serverid(request.version());
    response.mutable_res()->set_direct_udp_ip(request.server_ip());
    response.mutable_res()->set_direct_udp_port(request.server_port());
    response.mutable_res()->set_reservationid(GameServerCookieId);

    char addressString[32];
    AddressString(request.server_ip(), request.server_port(), addressString, sizeof(addressString));
    response.mutable_res()->set_server_address(addressString);

    SendMessageToGame(false, k_EMsgGCCStrike15_v2_ClientRequestJoinServerData, response);
}

void ClientGC::SetItemPositions(GCMessageRead &messageRead)
{
    CMsgSetItemPositions message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgSetItemPositions failed, ignoring\n");
        return;
    }

    std::vector<CMsgItemAcknowledged> acknowledgements;
    acknowledgements.reserve(message.item_positions_size());

    CMsgSOMultipleObjects update;
    if (m_inventory.SetItemPositions(message, acknowledgements, update))
    {
        for (const CMsgItemAcknowledged &acknowledgement : acknowledgements)
        {
            // send these to the server only
            GCMessageWrite messageWrite{ k_EMsgGCItemAcknowledged, acknowledgement };
            PostToHost(HostEvent::NetMessage, 0, messageWrite.Data(), messageWrite.Size());
        }

        SendMessageToGame(true, k_ESOMsg_UpdateMultiple, update);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::IncrementKillCountAttribute(GCMessageRead &messageRead)
{
    CMsgIncrementKillCountAttribute message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgIncrementKillCountAttribute failed, ignoring\n");
        return;
    }

    assert(message.event_type() == 0);

    CMsgSOSingleObject update;
    if (m_inventory.IncrementKillCountAttribute(message.item_id(), message.amount(), update))
    {
        SendMessageToGame(true, k_ESOMsg_Update, update);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::ApplySticker(GCMessageRead &messageRead)
{
    CMsgApplySticker message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgApplySticker failed, ignoring\n");
        return;
    }

    assert(!message.item_item_id() != !message.baseitem_defidx());

    CMsgSOSingleObject update, destroy;
    CMsgGCItemCustomizationNotification notification;

    if (!message.sticker_item_id())
    {
        // scrape
        if (m_inventory.ScrapeSticker(message, update, destroy, notification))
        {
            if (destroy.has_type_id())
            {
                // destroying a default item
                SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
            }

            if (update.has_type_id())
            {
                // if the item got removed (handled above), nothing gets updated
                SendMessageToGame(true, k_ESOMsg_Update, update);
            }

            if (notification.has_request())
            {
                // might get a k_EGCItemCustomizationNotification_RemoveSticker
                SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
            }
        }
        else
        {
            assert(false);
        }
    }
    else if (m_inventory.ApplySticker(message, update, destroy, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        SendMessageToGame(true, k_ESOMsg_Update, update);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::StoreGetUserData(GCMessageRead &messageRead)
{
    CMsgStoreGetUserData message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgStoreGetUserData failed, ignoring\n");
        return;
    }

    KeyValue priceSheet{ "price_sheet" };
    if (!priceSheet.ParseFromFile("csgo_gc/price_sheet.txt"))
    {
        return;
    }

    std::string binaryString;
    binaryString.reserve(1 << 17);
    priceSheet.BinaryWriteToString(binaryString);

    // fuck you idiot
    CMsgStoreGetUserDataResponse response;
    response.set_result(1);
    response.set_price_sheet_version(1729); // what
    *response.mutable_price_sheet() = std::move(binaryString);

    SendMessageToGame(false, k_EMsgGCStoreGetUserDataResponse, response);
}

void ClientGC::StorePurchaseInit(GCMessageRead &messageRead)
{
    CMsgGCStorePurchaseInit message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCStorePurchaseInit failed, ignoring\n");
        return;
    }

    // value doesn't matter
    uint64_t transactionId = Random{}.Integer<uint64_t>();

    assert(!m_transactionId);
    m_transactionId = transactionId;
    m_transactionItemIds.reserve(message.line_items_size()); // rough approx

    // inventory update response
    std::vector<CMsgSOSingleObject> inventoryUpdate;

    for (const auto &item : message.line_items())
    {
        for (uint32_t i = 0; i < item.quantity(); i++)
        {
            uint64_t itemId = m_inventory.PurchaseItem(item.item_def_id(), inventoryUpdate);
            if (!itemId)
            {
                assert(false);
            }
            else
            {
                m_transactionItemIds.push_back(itemId);
            }
        }
    }

    char url[128]; // url doesn't matter, but it needs to be set
    snprintf(url, sizeof(url), "https://checkout.steampowered.com/checkout/approvetxn/%llu/?returnurl=steam", transactionId);

    CMsgGCStorePurchaseInitResponse response;
    response.set_result(1); // success
    response.set_txn_id(transactionId);
    response.set_url(url);
    response.mutable_item_ids()->Assign(m_transactionItemIds.begin(), m_transactionItemIds.end());

    SendMessageToGame(false, k_EMsgGCStorePurchaseInitResponse, response, messageRead.JobId());

    // FIXME: why would the server care???
    for (auto &newItem : inventoryUpdate)
    {
        SendMessageToGame(true, k_ESOMsg_Create, newItem);
    }

    // this will run the steam callback
    PostToHost(HostEvent::MicroTransactionResponse, 0, nullptr, 0);
}

void ClientGC::StorePurchaseFinalize(GCMessageRead &messageRead)
{
    CMsgGCStorePurchaseFinalize message;
    if (!messageRead.ReadProtobuf(message))
    {
        Platform::Print("Parsing CMsgGCStorePurchaseFinalize failed, ignoring\n");
        return;
    }

    assert(m_transactionId);

    CMsgGCStorePurchaseFinalizeResponse response;
    response.set_result(1); // success
    response.mutable_item_ids()->Assign(m_transactionItemIds.begin(), m_transactionItemIds.end());
    SendMessageToGame(false, k_EMsgGCStorePurchaseFinalizeResponse, response, messageRead.JobId());

    // done with this one
    m_transactionId = 0;
}

void ClientGC::DeleteItem(GCMessageRead &messageRead)
{
    // there is data after this, but i don't know what it is
    uint64_t itemId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCDelete failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject destroyed;
    if (m_inventory.RemoveItem(itemId, destroyed))
    {
        // mikkotodo what does the server want to know
        SendMessageToGame(true, k_ESOMsg_Destroy, destroyed);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::UnlockCrate(GCMessageRead &messageRead)
{
    uint64_t keyId = messageRead.ReadUint64();
    uint64_t crateId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCUnlockCrate failed, ignoring\n");
        return;
    }

    Platform::Print("CASE OPENING %llu with %llu\n", crateId, keyId);

    CMsgSOSingleObject destroyCrate, destroyKey, newItem;
    CMsgGCItemCustomizationNotification notification;

    if (m_inventory.UnlockCrate(
            crateId,
            keyId,
            destroyCrate,
            destroyKey,
            newItem,
            notification))
    {
        // mikkotodo what does the server want to know
        SendMessageToGame(true, k_ESOMsg_Destroy, destroyCrate);
        SendMessageToGame(true, k_ESOMsg_Destroy, destroyKey);
        SendMessageToGame(true, k_ESOMsg_Create, newItem);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::NameItem(GCMessageRead &messageRead)
{
    uint64_t nameTagId = messageRead.ReadUint64();
    uint64_t itemId = messageRead.ReadUint64();
    messageRead.ReadData(1); // skip the sentinel
    std::string_view name = messageRead.ReadString();

    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCNameItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject update, destroy;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.NameItem(nameTagId, itemId, name, update, destroy, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Update, update);
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::NameBaseItem(GCMessageRead &messageRead)
{
    uint64_t nameTagId = messageRead.ReadUint64();
    uint32_t defIndex = messageRead.ReadUint32();
    messageRead.ReadData(1); // skip the sentinel
    std::string_view name = messageRead.ReadString();

    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCNameBaseItem failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject create, destroy;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.NameBaseItem(nameTagId, defIndex, name, create, destroy, notification))
    {
        SendMessageToGame(true, k_ESOMsg_Create, create);
        SendMessageToGame(true, k_ESOMsg_Destroy, destroy);

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}

void ClientGC::RemoveItemName(GCMessageRead &messageRead)
{
    uint64_t itemId = messageRead.ReadUint64();
    if (!messageRead.IsValid())
    {
        Platform::Print("Parsing CMsgGCRemoveItemName failed, ignoring\n");
        return;
    }

    CMsgSOSingleObject update, destroy;
    CMsgGCItemCustomizationNotification notification;
    if (m_inventory.RemoveItemName(itemId, update, destroy, notification))
    {
        if (update.has_type_id())
        {
            SendMessageToGame(true, k_ESOMsg_Update, update);
        }

        if (destroy.has_type_id())
        {
            SendMessageToGame(true, k_ESOMsg_Destroy, destroy);
        }

        SendMessageToGame(false, k_EMsgGCItemCustomizationNotification, notification);
    }
    else
    {
        assert(false);
    }
}
