/*
 * Copyright (C) 2008-2015 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2014 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
----- Opcodes Not Used yet -----

SMSG_CALENDAR_EVENT_INVITE_NOTES        [ packguid(Invitee), uint64(inviteId), string(Text), Boolean(Unk) ]
?CMSG_CALENDAR_EVENT_INVITE_NOTES       [ uint32(unk1), uint32(unk2), uint32(unk3), uint32(unk4), uint32(unk5) ]
SMSG_CALENDAR_EVENT_INVITE_NOTES_ALERT  [ uint64(inviteId), string(Text) ]
SMSG_CALENDAR_EVENT_INVITE_STATUS_ALERT [ uint64(eventId), uint32(eventTime), uint32(unkFlag), uint8(deletePending) ]
SMSG_CALENDAR_RAID_LOCKOUT_UPDATED      SendCalendarRaidLockoutUpdated(InstanceSave const* save)

@todo

Finish complains' handling - what to do with received complains and how to respond?
Find out what to do with all "not used yet" opcodes
Correct errors sending (event/invite not found, invites exceeded, event already passed, permissions etc.)
Fix locked events to be displayed properly and response time shouldn't be shown for people that haven't respond yet
Copied events should probably have a new owner

*/

#include "InstanceSaveMgr.h"
#include "Log.h"
#include "Opcodes.h"
#include "Player.h"
#include "SocialMgr.h"
#include "CalendarMgr.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "DatabaseEnv.h"
#include "GuildMgr.h"
#include "WorldSession.h"

void WorldSession::HandleCalendarGetCalendar(WorldPacket& /*recvData*/)
{
    uint64 guid = _player->GetGUID();
    TC_LOG_DEBUG("network", "CMSG_CALENDAR_GET_CALENDAR [" UI64FMTD "]", guid);

    time_t currTime = time(NULL);

    WorldPacket data(SMSG_CALENDAR_SEND_CALENDAR, 1000); // Average size if no instance

    CalendarInviteStore invites = sCalendarMgr->GetPlayerInvites(guid);
    data << uint32(invites.size());
    for (CalendarInviteStore::const_iterator itr = invites.begin(); itr != invites.end(); ++itr)
    {
        data << uint64((*itr)->GetEventId());
        data << uint64((*itr)->GetInviteId());
        data << uint8((*itr)->GetStatus());
        data << uint8((*itr)->GetRank());

        if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent((*itr)->GetEventId()))
        {
            data << uint8(calendarEvent->IsGuildEvent());
            data.appendPackGUID(calendarEvent->GetCreatorGUID());
        }
        else
        {
            data << uint8(0);
            data.appendPackGUID((*itr)->GetSenderGUID());
        }
    }

    CalendarEventStore playerEvents = sCalendarMgr->GetPlayerEvents(guid);
    data << uint32(playerEvents.size());
    for (CalendarEventStore::const_iterator itr = playerEvents.begin(); itr != playerEvents.end(); ++itr)
    {
        CalendarEvent* calendarEvent = *itr;

        data << uint64(calendarEvent->GetEventId());
        data << calendarEvent->GetTitle();
        data << uint32(calendarEvent->GetType());
        data.AppendPackedTime(calendarEvent->GetEventTime());
        data << uint32(calendarEvent->GetFlags());
        data << int32(calendarEvent->GetDungeonId());

        Guild* guild = sGuildMgr->GetGuildById(calendarEvent->GetGuildId());
        data << uint64(guild ? guild->GetGUID() : 0);

        data.appendPackGUID(calendarEvent->GetCreatorGUID());
    }

    data << uint32(currTime);                              // server time
    data.AppendPackedTime(currTime);                       // zone time

    ByteBuffer dataBuffer;
    uint32 boundCounter = 0;
    for (uint8 i = 0; i < MAX_DIFFICULTY; ++i)
    {
        Player::BoundInstancesMap boundInstances = _player->GetBoundInstances(Difficulty(i));
        for (Player::BoundInstancesMap::const_iterator itr = boundInstances.begin(); itr != boundInstances.end(); ++itr)
        {
            if (itr->second.perm)
            {
                InstanceSave const* save = itr->second.save;
                dataBuffer << uint32(save->GetMapId());
                dataBuffer << uint32(save->GetDifficulty());
                dataBuffer << uint32(save->GetResetTime() - currTime);
                dataBuffer << uint64(save->GetInstanceId());     // instance save id as unique instance copy id
                ++boundCounter;
            }
        }
    }

    data << uint32(boundCounter);
    data.append(dataBuffer);

    data << uint32(1135753200);                            // Constant date, unk (28.12.2005 07:00)

    // Reuse variables
    boundCounter = 0;
    std::set<uint32> sentMaps;
    dataBuffer.clear();

    ResetTimeByMapDifficultyMap const& resets = sInstanceSaveMgr->GetResetTimeMap();
    for (ResetTimeByMapDifficultyMap::const_iterator itr = resets.begin(); itr != resets.end(); ++itr)
    {
        uint32 mapId = PAIR32_LOPART(itr->first);
        if (sentMaps.find(mapId) != sentMaps.end())
            continue;

        MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
        if (!mapEntry || !mapEntry->IsRaid())
            continue;

        sentMaps.insert(mapId);

        dataBuffer << int32(mapId);
        dataBuffer << int32(itr->second - currTime);
        dataBuffer << int32(0); // Never seen anything else in sniffs - still unknown
        ++boundCounter;
    }

    data << uint32(boundCounter);
    data.append(dataBuffer);

    /// @todo Fix this, how we do know how many and what holidays to send?
    uint32 holidayCount = 0;
    data << uint32(holidayCount);
    for (uint32 i = 0; i < holidayCount; ++i)
    {
        HolidaysEntry const* holiday = sHolidaysStore.LookupEntry(666);

        data << uint32(holiday->Id);                        // m_ID
        data << uint32(holiday->Region);                    // m_region, might be looping
        data << uint32(holiday->Looping);                   // m_looping, might be region
        data << uint32(holiday->Priority);                  // m_priority
        data << uint32(holiday->CalendarFilterType);        // m_calendarFilterType

        for (uint8 j = 0; j < MAX_HOLIDAY_DATES; ++j)
            data << uint32(holiday->Date[j]);               // 26 * m_date -- WritePackedTime ?

        for (uint8 j = 0; j < MAX_HOLIDAY_DURATIONS; ++j)
            data << uint32(holiday->Duration[j]);           // 10 * m_duration

        for (uint8 j = 0; j < MAX_HOLIDAY_FLAGS; ++j)
            data << uint32(holiday->CalendarFlags[j]);      // 10 * m_calendarFlags

        data << holiday->TextureFilename;                   // m_textureFilename (holiday name)
    }

    SendPacket(&data);
}

void WorldSession::HandleCalendarGetEvent(WorldPacket& recvData)
{
    uint64 eventId;
    recvData >> eventId;

    TC_LOG_DEBUG("network", "CMSG_CALENDAR_GET_EVENT. Player ["
        UI64FMTD "] Event [" UI64FMTD "]", _player->GetGUID(), eventId);

    if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
        sCalendarMgr->SendCalendarEvent(_player->GetGUID(), *calendarEvent, CALENDAR_SENDTYPE_GET);
    else
        sCalendarMgr->SendCalendarCommandResult(_player->GetGUID(), CALENDAR_ERROR_EVENT_INVALID);
}

void WorldSession::HandleCalendarGuildFilter(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "CMSG_CALENDAR_GUILD_FILTER [" UI64FMTD "]", _player->GetGUID());

    uint32 minLevel;
    uint32 maxLevel;
    uint32 minRank;

    recvData >> minLevel >> maxLevel >> minRank;

    if (Guild* guild = sGuildMgr->GetGuildById(_player->GetGuildId()))
        guild->MassInviteToEvent(this, minLevel, maxLevel, minRank);

    TC_LOG_DEBUG("network", "CMSG_CALENDAR_GUILD_FILTER: Min level [%d], Max level [%d], Min rank [%d]", minLevel, maxLevel, minRank);
}

void WorldSession::HandleCalendarAddEvent(WorldPacket& recvData)
{
    uint64 guid = _player->GetGUID();

    std::string title;
    std::string description;
    uint8 type;
    int32 dungeonId;
    uint32 eventPackedTime;
    uint32 maxInvites;  // always 100, necesary? Not find the way how to change it
    uint32 flags;
    uint32 inviteeCount;
    uint16 descriptionLength, titleLength;

    recvData >> maxInvites >> flags >> dungeonId;
    recvData.ReadPackedTime(eventPackedTime);
    recvData >> type;
    inviteeCount = recvData.ReadBits(22);
    descriptionLength = recvData.ReadBits(11);

    std::list<CalendarInvitePacketInfo> calendarInviteList;
    for (uint32 i = 0; i < inviteeCount; i++)
    {
        CalendarInvitePacketInfo info;
        info.Guid[7] = recvData.ReadBit();
        info.Guid[2] = recvData.ReadBit();
        info.Guid[6] = recvData.ReadBit();
        info.Guid[3] = recvData.ReadBit();
        info.Guid[5] = recvData.ReadBit();
        info.Guid[1] = recvData.ReadBit();
        info.Guid[0] = recvData.ReadBit();
        info.Guid[4] = recvData.ReadBit();
        calendarInviteList.push_back(info);
    }

    titleLength = recvData.ReadBits(8);

    for (std::list<CalendarInvitePacketInfo>::iterator iter = calendarInviteList.begin(); iter != calendarInviteList.end(); ++iter)
    {
        recvData.ReadByteSeq(iter->Guid[4]);
        recvData.ReadByteSeq(iter->Guid[2]);
        recvData.ReadByteSeq(iter->Guid[3]);
        recvData.ReadByteSeq(iter->Guid[1]);
        recvData.ReadByteSeq(iter->Guid[0]);
        recvData.ReadByteSeq(iter->Guid[6]);
        recvData.ReadByteSeq(iter->Guid[7]);
        recvData >> iter->Status;
        recvData.ReadByteSeq(iter->Guid[5]);
        recvData >> iter->ModerationRank;
    }

    title = recvData.ReadString(titleLength);
    description = recvData.ReadString(descriptionLength);

    CalendarEvent* calendarEvent = new CalendarEvent(sCalendarMgr->GetFreeEventId(), guid, 0, CalendarEventType(type), dungeonId,
         time_t(eventPackedTime), flags, title, description);

    if (calendarEvent->IsGuildEvent() || calendarEvent->IsGuildAnnouncement())
        if (Player* creator = ObjectAccessor::FindPlayer(guid))
            calendarEvent->SetGuildId(creator->GetGuildId());

    if (calendarEvent->IsGuildAnnouncement())
    {
         CalendarInvite* invite = new CalendarInvite(0, calendarEvent->GetEventId(), 0, guid, 946684800, CALENDAR_STATUS_NOT_SIGNED_UP, CALENDAR_RANK_PLAYER, "");
         sCalendarMgr->AddInvite(calendarEvent, invite);
    }
    else
    {
        uint32 inviteCount;
        recvData >> inviteCount;

        for (uint32 i = 0; i < inviteCount; ++i)
        {
            uint64 invitee = 0;
            uint8 status = 0;
            uint8 rank = 0;
            recvData.readPackGUID(invitee);
            recvData >> status >> rank;

            CalendarInvite* invite = new CalendarInvite(sCalendarMgr->GetFreeInviteId(), calendarEvent->GetEventId(), invitee, guid, 946684800, CalendarInviteStatus(status), CalendarModerationRank(rank), "");
            sCalendarMgr->AddInvite(calendarEvent, invite);
        }
    }

    sCalendarMgr->AddEvent(calendarEvent, CALENDAR_SENDTYPE_ADD);
}

void WorldSession::HandleCalendarUpdateEvent(WorldPacket& recvData)
{
    uint64 guid = _player->GetGUID();
    time_t oldEventTime;

    ObjectGuid eventId;
    ObjectGuid inviteId;
    std::string title;
    std::string description;
    uint8 type;
    uint32 maxInvites;
    int32 dungeonId;
    uint32 eventPackedTime;
    uint32 flags;
    uint16 descriptionLength;
    uint16 titleLength;

    recvData >> maxInvites >> dungeonId;
    recvData.ReadPackedTime(eventPackedTime);
    recvData >> flags >> type;
    eventId[4] = recvData.ReadBit();
    eventId[5] = recvData.ReadBit();
    eventId[2] = recvData.ReadBit();
    inviteId[4] = recvData.ReadBit();
    eventId[7] = recvData.ReadBit();
    eventId[0] = recvData.ReadBit();
    inviteId[5] = recvData.ReadBit();
    inviteId[3] = recvData.ReadBit();
    eventId[6] = recvData.ReadBit();
    eventId[1] = recvData.ReadBit();
    inviteId[6] = recvData.ReadBit();
    inviteId[2] = recvData.ReadBit();
    inviteId[7] = recvData.ReadBit();
    inviteId[1] = recvData.ReadBit();
    inviteId[0] = recvData.ReadBit();
    descriptionLength = recvData.ReadBits(11);
    titleLength = recvData.ReadBits(8);
    eventId[3] = recvData.ReadBit();

    recvData.ReadByteSeq(inviteId[6]);
    recvData.ReadByteSeq(eventId[0]);
    recvData.ReadByteSeq(inviteId[7]);
    recvData.ReadByteSeq(inviteId[3]);
    recvData.ReadByteSeq(eventId[6]);
    recvData.ReadByteSeq(inviteId[1]);
    recvData.ReadByteSeq(eventId[2]);
    title = recvData.ReadString(titleLength);
    recvData.ReadByteSeq(inviteId[5]);
    recvData.ReadByteSeq(inviteId[4]);
    recvData.ReadByteSeq(eventId[5]);
    recvData.ReadByteSeq(eventId[3]);
    recvData.ReadByteSeq(inviteId[0]);
    recvData.ReadByteSeq(eventId[4]);
    description = recvData.ReadString(descriptionLength);
    recvData.ReadByteSeq(eventId[1]);
    recvData.ReadByteSeq(inviteId[2]);
    recvData.ReadByteSeq(eventId[7]);

    TC_LOG_DEBUG("network", "CMSG_CALENDAR_UPDATE_EVENT [" UI64FMTD "] EventId [" UI64FMTD
        "], InviteId [" UI64FMTD "] Title %s, Description %s, type %u "
        "MaxInvites %u, Dungeon ID %d, Time %u, Flags %u",
        guid, (uint64)eventId, (uint64)inviteId, title.c_str(),
        description.c_str(), type, maxInvites, dungeonId,
        eventPackedTime, flags);

    if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
    {
        oldEventTime = calendarEvent->GetEventTime();

        calendarEvent->SetType(CalendarEventType(type));
        calendarEvent->SetFlags(flags);
        calendarEvent->SetEventTime(time_t(eventPackedTime));
        calendarEvent->SetDungeonId(dungeonId);
        calendarEvent->SetTitle(title);
        calendarEvent->SetDescription(description);

        sCalendarMgr->UpdateEvent(calendarEvent);
        sCalendarMgr->SendCalendarEventUpdateAlert(*calendarEvent, oldEventTime);
    }
    else
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_INVALID);
}

void WorldSession::HandleCalendarRemoveEvent(WorldPacket& recvData)
{
    uint64 guid = _player->GetGUID();
    recvData.read_skip<uint64>(); // Skip InviteId
    uint64 eventId;

    recvData >> eventId;
    recvData.rfinish(); // Skip flags

    sCalendarMgr->RemoveEvent(eventId, guid);
}

void WorldSession::HandleCalendarCopyEvent(WorldPacket& recvData)
{
	uint64 guid = _player->GetGUID();
	uint64 eventId;
	uint64 inviteId;
	uint32 time;

	recvData >> eventId >> inviteId;
	recvData.ReadPackedTime(time);
	TC_LOG_DEBUG("network", "CMSG_CALENDAR_COPY_EVENT [" UI64FMTD "], EventId [" UI64FMTD
		"] inviteId [" UI64FMTD "] Time: %u", guid, eventId, inviteId, time);

	if (CalendarEvent* oldEvent = sCalendarMgr->GetEvent(eventId))
	{
		CalendarEvent* newEvent = new CalendarEvent(*oldEvent, sCalendarMgr->GetFreeEventId()), guid; // FIX ME
		newEvent->SetEventTime(time_t(time));
		CalendarInviteStore invites = sCalendarMgr->GetEventInvites(eventId);

		for (CalendarInviteStore::const_iterator itr = invites.begin(); itr != invites.end(); ++itr)
			sCalendarMgr->AddInvite(newEvent, new CalendarInvite(**itr, sCalendarMgr->GetFreeInviteId(), newEvent->GetEventId())); // FIX ME

		sCalendarMgr->AddEvent(newEvent, CALENDAR_SENDTYPE_COPY);
	}
	else
		sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_INVALID);
}

void WorldSession::HandleCalendarEventInvite(WorldPacket& recvData)
{
    TC_LOG_DEBUG("network", "CMSG_CALENDAR_EVENT_INVITE");

    uint64 playerGuid = _player->GetGUID();

    uint64 eventId;
    uint64 inviteId;
    std::string name;
    uint16 length;
    bool isPreInvite;
    bool isGuildEvent;

    uint64 inviteeGuid = 0;
    uint32 inviteeTeam = 0;
    uint32 inviteeGuildId = 0;

    recvData >> eventId >> inviteId;
    isPreInvite = recvData.ReadBit();
    length = recvData.ReadBits(9);
    isGuildEvent = recvData.ReadBit();
    name = recvData.ReadString(length);

    if (Player* player = sObjectAccessor->FindPlayerByName(name.c_str()))
    {
        // Invitee is online
        inviteeGuid = player->GetGUID();
        inviteeTeam = player->GetTeam();
        inviteeGuildId = player->GetGuildId();
    }
    else
    {
        // Invitee offline, get data from database
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_GUID_RACE_ACC_BY_NAME);
        stmt->setString(0, name);
        if (PreparedQueryResult result = CharacterDatabase.Query(stmt))
        {
            Field* fields = result->Fetch();
            inviteeGuid = MAKE_NEW_GUID(fields[0].GetUInt32(), 0, HIGHGUID_PLAYER);
            inviteeTeam = Player::TeamForRace(fields[1].GetUInt8());
            inviteeGuildId = Player::GetGuildIdFromDB(inviteeGuid);
        }
    }

    if (!inviteeGuid)
    {
        sCalendarMgr->SendCalendarCommandResult(playerGuid, CALENDAR_ERROR_PLAYER_NOT_FOUND);
        return;
    }

    if (_player->GetTeam() != inviteeTeam && !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CALENDAR))
    {
        sCalendarMgr->SendCalendarCommandResult(playerGuid, CALENDAR_ERROR_NOT_ALLIED);
        return;
    }

    if (QueryResult result = CharacterDatabase.PQuery("SELECT flags FROM character_social WHERE guid = " UI64FMTD " AND friend = " UI64FMTD, inviteeGuid, playerGuid))
    {
        Field* fields = result->Fetch();
        if (fields[0].GetUInt8() & SOCIAL_FLAG_IGNORED)
        {
            sCalendarMgr->SendCalendarCommandResult(playerGuid, CALENDAR_ERROR_IGNORING_YOU_S, name.c_str());
            return;
        }
    }

    if (!isPreInvite)
    {
        if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
        {
            if (calendarEvent->IsGuildEvent() && calendarEvent->GetGuildId() == inviteeGuildId)
            {
                // we can't invite guild members to guild events
                sCalendarMgr->SendCalendarCommandResult(playerGuid, CALENDAR_ERROR_NO_GUILD_INVITES);
                return;
            }

            // 946684800 is 01/01/2000 00:00:00 - default response time
            CalendarInvite* invite = new CalendarInvite(sCalendarMgr->GetFreeInviteId(), eventId, inviteeGuid, playerGuid, 946684800, CALENDAR_STATUS_INVITED, CALENDAR_RANK_PLAYER, "");
            sCalendarMgr->AddInvite(calendarEvent, invite);
        }
        else
            sCalendarMgr->SendCalendarCommandResult(playerGuid, CALENDAR_ERROR_EVENT_INVALID);
    }
    else
    {
        if (isGuildEvent && inviteeGuildId == _player->GetGuildId())
        {
            sCalendarMgr->SendCalendarCommandResult(playerGuid, CALENDAR_ERROR_NO_GUILD_INVITES);
            return;
        }

        // 946684800 is 01/01/2000 00:00:00 - default response time
        CalendarInvite invite(inviteId, 0, inviteeGuid, playerGuid, 946684800, CALENDAR_STATUS_INVITED, CALENDAR_RANK_PLAYER, "");
        sCalendarMgr->SendCalendarEventInvite(invite);
    }
}

void WorldSession::HandleCalendarEventSignup(WorldPacket& recvData)
{
    uint64 guid = _player->GetGUID();
    uint64 eventId;
    bool tentative;

    recvData >> eventId >> tentative;
    TC_LOG_DEBUG("network", "CMSG_CALENDAR_EVENT_SIGNUP [" UI64FMTD "] EventId [" UI64FMTD "] Tentative %u", guid, eventId, tentative);

    if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
    {
        if (calendarEvent->IsGuildEvent() && calendarEvent->GetGuildId() != _player->GetGuildId())
        {
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_GUILD_PLAYER_NOT_IN_GUILD);
            return;
        }

        CalendarInviteStatus status = tentative ? CALENDAR_STATUS_TENTATIVE : CALENDAR_STATUS_SIGNED_UP;
        CalendarInvite* invite = new CalendarInvite(sCalendarMgr->GetFreeInviteId(), eventId, guid, guid, time(NULL), status, CALENDAR_RANK_PLAYER, "");
        sCalendarMgr->AddInvite(calendarEvent, invite);
        sCalendarMgr->SendCalendarClearPendingAction(guid);
    }
    else
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_INVALID);
}

void WorldSession::HandleCalendarEventRsvp(WorldPacket& recvData)
{
    uint64 guid = _player->GetGUID();
    uint64 eventId;
    uint64 inviteId;
    uint32 status;

    recvData >> eventId >> inviteId >> status;
    TC_LOG_DEBUG("network", "CMSG_CALENDAR_EVENT_RSVP [" UI64FMTD "] EventId ["
        UI64FMTD "], InviteId [" UI64FMTD "], status %u", guid, eventId,
        inviteId, status);

    if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
    {
        // i think we still should be able to remove self from locked events
        if (status != CALENDAR_STATUS_REMOVED && calendarEvent->GetFlags() & CALENDAR_FLAG_INVITES_LOCKED)
        {
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_LOCKED);
            return;
        }

        if (CalendarInvite* invite = sCalendarMgr->GetInvite(inviteId))
        {
            invite->SetStatus(CalendarInviteStatus(status));
            invite->SetStatusTime(time(NULL));

            sCalendarMgr->UpdateInvite(invite);
            sCalendarMgr->SendCalendarEventStatus(*calendarEvent, *invite);
            sCalendarMgr->SendCalendarClearPendingAction(guid);
        }
        else
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_NO_INVITE); // correct?
    }
    else
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_INVALID);
}

void WorldSession::HandleCalendarEventRemoveInvite(WorldPacket& recvData)
{
    uint64 guid = _player->GetGUID();
    uint64 invitee;
    uint64 eventId;
    uint64 ownerInviteId; // isn't it sender's inviteId?
    uint64 inviteId;

    recvData.readPackGUID(invitee);
    recvData >> inviteId >> ownerInviteId >> eventId;

    TC_LOG_DEBUG("network", "CMSG_CALENDAR_EVENT_REMOVE_INVITE ["
        UI64FMTD "] EventId [" UI64FMTD "], ownerInviteId ["
        UI64FMTD "], Invitee ([" UI64FMTD "] id: [" UI64FMTD "])",
        guid, eventId, ownerInviteId, invitee, inviteId);

    if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
    {
        if (calendarEvent->GetCreatorGUID() == invitee)
        {
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_DELETE_CREATOR_FAILED);
            return;
        }

        sCalendarMgr->RemoveInvite(inviteId, eventId, guid);
    }
    else
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_NO_INVITE);
}

void WorldSession::HandleCalendarEventStatus(WorldPacket& recvData)
{
    uint64 guid = _player->GetGUID();
    uint64 invitee;
    uint64 eventId;
    uint64 inviteId;
    uint64 ownerInviteId; // isn't it sender's inviteId?
    uint8 status;

    recvData.readPackGUID(invitee);
    recvData >> eventId >> inviteId >> ownerInviteId >> status;
    TC_LOG_DEBUG("network", "CMSG_CALENDAR_EVENT_STATUS [" UI64FMTD"] EventId ["
        UI64FMTD "] ownerInviteId [" UI64FMTD "], Invitee ([" UI64FMTD "] id: ["
        UI64FMTD "], status %u", guid, eventId, ownerInviteId, invitee, inviteId, status);

    if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
    {
        if (CalendarInvite* invite = sCalendarMgr->GetInvite(inviteId))
        {
            invite->SetStatus((CalendarInviteStatus)status);
            // not sure if we should set response time when moderator changes invite status
            //invite->SetStatusTime(time(NULL));

            sCalendarMgr->UpdateInvite(invite);
            sCalendarMgr->SendCalendarEventStatus(*calendarEvent, *invite);
            sCalendarMgr->SendCalendarClearPendingAction(invitee);
        }
        else
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_NO_INVITE); // correct?
    }
    else
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_INVALID);
}

void WorldSession::HandleCalendarEventModeratorStatus(WorldPacket& recvData)
{
    uint64 guid = _player->GetGUID();
    uint64 invitee;
    uint64 eventId;
    uint64 inviteId;
    uint64 ownerInviteId; // isn't it sender's inviteId?
    uint8 rank;

    recvData.readPackGUID(invitee);
    recvData >> eventId >>  inviteId >> ownerInviteId >> rank;
    TC_LOG_DEBUG("network", "CMSG_CALENDAR_EVENT_MODERATOR_STATUS [" UI64FMTD "] EventId ["
        UI64FMTD "] ownerInviteId [" UI64FMTD "], Invitee ([" UI64FMTD "] id: ["
        UI64FMTD "], rank %u", guid, eventId, ownerInviteId, invitee, inviteId, rank);

    if (CalendarEvent* calendarEvent = sCalendarMgr->GetEvent(eventId))
    {
        if (CalendarInvite* invite = sCalendarMgr->GetInvite(inviteId))
        {
            invite->SetRank(CalendarModerationRank(rank));
            sCalendarMgr->UpdateInvite(invite);
            sCalendarMgr->SendCalendarEventModeratorStatusAlert(*calendarEvent, *invite);
        }
        else
            sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_NO_INVITE); // correct?
    }
    else
        sCalendarMgr->SendCalendarCommandResult(guid, CALENDAR_ERROR_EVENT_INVALID);
}

void WorldSession::HandleCalendarComplain(WorldPacket& recvData)
{
    uint64 guid = _player->GetGUID();
    uint64 eventId;
    ObjectGuid complainGUID;
    uint64 inviteId;

    recvData >> inviteId >> eventId;
    complainGUID[4] = recvData.ReadBit();
    complainGUID[6] = recvData.ReadBit();
    complainGUID[2] = recvData.ReadBit();
    complainGUID[7] = recvData.ReadBit();
    complainGUID[1] = recvData.ReadBit();
    complainGUID[5] = recvData.ReadBit();
    complainGUID[3] = recvData.ReadBit();
    complainGUID[0] = recvData.ReadBit();

    recvData.ReadByteSeq(complainGUID[6]);
    recvData.ReadByteSeq(complainGUID[7]);
    recvData.ReadByteSeq(complainGUID[1]);
    recvData.ReadByteSeq(complainGUID[0]);
    recvData.ReadByteSeq(complainGUID[4]);
    recvData.ReadByteSeq(complainGUID[2]);
    recvData.ReadByteSeq(complainGUID[3]);
    recvData.ReadByteSeq(complainGUID[5]);

    TC_LOG_DEBUG("network", "CMSG_CALENDAR_COMPLAIN [" UI64FMTD "] EventId ["
        UI64FMTD "] guid [" UI64FMTD "] InviteId [" UI64FMTD "]", guid, eventId, (uint64)complainGUID, inviteId);

    // what to do with complains?
}

void WorldSession::HandleCalendarGetNumPending(WorldPacket& /*recvData*/)
{
    uint64 guid = _player->GetGUID();
    uint32 pending = sCalendarMgr->GetPlayerNumPending(guid);

    TC_LOG_DEBUG("network", "CMSG_CALENDAR_GET_NUM_PENDING: [" UI64FMTD
        "] Pending: %u", guid, pending);

    WorldPacket data(SMSG_CALENDAR_SEND_NUM_PENDING, 4);
    data << uint32(pending);
    SendPacket(&data);
}

void WorldSession::HandleSetSavedInstanceExtend(WorldPacket& recvData)
{
    uint32 mapId, difficulty;
    uint8 toggleExtend;
    recvData >> mapId >> difficulty>> toggleExtend;
    TC_LOG_DEBUG("network", "CMSG_SET_SAVED_INSTANCE_EXTEND - MapId: %u, Difficulty: %u, ToggleExtend: %s", mapId, difficulty, toggleExtend ? "On" : "Off");

    /*
    InstancePlayerBind* instanceBind = _player->GetBoundInstance(mapId, Difficulty(difficulty));
    if (!instanceBind || !instanceBind->save)
        return;

    InstanceSave* save = instanceBind->save;
    // http://www.wowwiki.com/Instance_Lock_Extension
    // SendCalendarRaidLockoutUpdated(save);
    */
}

// ----------------------------------- SEND ------------------------------------

void WorldSession::SendCalendarRaidLockout(InstanceSave const* save, bool add)
{
    TC_LOG_DEBUG("network", "%s", add ? "SMSG_CALENDAR_RAID_LOCKOUT_ADDED" : "SMSG_CALENDAR_RAID_LOCKOUT_REMOVED");
    time_t currTime = time(NULL);

    WorldPacket data(SMSG_CALENDAR_RAID_LOCKOUT_REMOVED, (add ? 4 : 0) + 4 + 4 + 4 + 8);
    if (add)
    {
        data.SetOpcode(SMSG_CALENDAR_RAID_LOCKOUT_ADDED);
        data.AppendPackedTime(currTime);
    }

    data << uint32(save->GetMapId());
    data << uint32(save->GetDifficulty());
    data << uint32(save->GetResetTime() - currTime);
    data << uint64(save->GetInstanceId());
    SendPacket(&data);
}

void WorldSession::SendCalendarRaidLockoutUpdated(InstanceSave const* save)
{
    if (!save)
        return;

    uint64 guid = _player->GetGUID();
    TC_LOG_DEBUG("network", "SMSG_CALENDAR_RAID_LOCKOUT_UPDATED [" UI64FMTD
        "] Map: %u, Difficulty %u", guid, save->GetMapId(), save->GetDifficulty());

    time_t currTime = time(NULL);

    WorldPacket data(SMSG_CALENDAR_RAID_LOCKOUT_UPDATED, 4 + 4 + 4 + 4 + 8);
    data.AppendPackedTime(currTime);
    data << uint32(save->GetMapId());
    data << uint32(save->GetDifficulty());
    data << uint32(0); // Amount of seconds that has changed to the reset time
    data << uint32(save->GetResetTime() - currTime);
    SendPacket(&data);
}
