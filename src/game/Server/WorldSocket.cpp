/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Server/WorldSocket.h"
#include "Common.h"

#include "Util/Util.h"
#include "World/World.h"
#include "Server/WorldPacket.h"
#include "Globals/SharedDefines.h"
#include "Util/ByteBuffer.h"
#include "Server/Opcodes.h"
#include "Database/DatabaseEnv.h"
#include "Auth/CryptoHash.h"
#include "Server/WorldSession.h"
#include "Util/CommonDefines.h"
#include "Log/Log.h"
#include "Server/DBCStores.h"
#ifdef BUILD_ELUNA
#include "LuaEngine/LuaEngine.h"
#endif

#include <chrono>
#include <functional>

#include <boost/asio.hpp>

#if defined( __GNUC__ )
#pragma pack(1)
#else
#pragma pack(push,1)
#endif
struct ServerPktHeader
{
    /**
    * size is the length of the payload _plus_ the length of the opcode
    */
    ServerPktHeader(uint32 size, uint16 cmd) : size(size)
    {
        uint8 headerIndex = 0;
        if (isLargePacket())
        {
            DEBUG_LOG("initializing large server to client packet. Size: %u, cmd: %u", size, cmd);
            header[headerIndex++] = 0x80 | (0xFF & (size >> 16));
        }
        header[headerIndex++] = 0xFF & (size >> 8);
        header[headerIndex++] = 0xFF & size;

        header[headerIndex++] = 0xFF & cmd;
        header[headerIndex++] = 0xFF & (cmd >> 8);
    }

    uint8 getHeaderLength()
    {
        // cmd = 2 bytes, size= 2||3bytes
        return 2 + (isLargePacket() ? 3 : 2);
    }

    bool isLargePacket()
    {
        return size > 0x7FFF;
    }

    const uint32 size;
    uint8 header[5];
};
#if defined( __GNUC__ )
#pragma pack()
#else
#pragma pack(pop)
#endif

WorldSocket::WorldSocket(boost::asio::io_context &context, std::function<void (Socket *)> closeHandler)
    : Socket(context, closeHandler), m_lastPingTime(std::chrono::system_clock::time_point::min()), m_overSpeedPings(0),
      m_useExistingHeader(false), m_session(nullptr),m_seed(urand())
{
    InitializeOpcodes();
}

void WorldSocket::SendPacket(const WorldPacket& pct, bool immediate)
{
    if (IsClosed())
        return;

    // Dump outgoing packet.
    sLog.outWorldPacketDump(GetRemoteEndpoint().c_str(), pct.GetOpcode(), pct.GetOpcodeName(), pct, false);

    ServerPktHeader header(pct.size() + 2, pct.GetOpcode());
    m_crypt.EncryptSend((uint8*)header.header, header.getHeaderLength());

    if (pct.size() > 0)
        Write(reinterpret_cast<const char *>(&header.header), header.getHeaderLength(), reinterpret_cast<const char *>(pct.contents()), pct.size());
    else
        Write(reinterpret_cast<const char *>(&header.header), header.getHeaderLength());

    if (immediate)
        ForceFlushOut();
}

bool WorldSocket::Open()
{
    if (!Socket::Open())
        return false;

    std::string ServerToClient = "RLD OF WARCRAFT CONNECTION - SERVER TO CLIENT";
    WorldPacket data(MSG_WOW_CONNECTION, 46);

    data << ServerToClient;

    SendPacket(data);

    return true;
}

bool WorldSocket::HandleWowConnection(WorldPacket& recvPacket)
{
    std::string ClientToServerMsg;
    recvPacket >> ClientToServerMsg;

    WorldPacket packet (SMSG_AUTH_CHALLENGE, 37);

    for (uint32 i = 0; i < 8; i++)
        packet << uint32(0);

    packet << m_seed;
    packet << uint8(1);

    SendPacket(packet);

    return true;
}

bool WorldSocket::ProcessIncomingData()
{
    ClientPktHeader header;

    if (m_useExistingHeader)
    {
        m_useExistingHeader = false;
        header = m_existingHeader;

        ReadSkip(sizeof(ClientPktHeader));
    }
    else
    {
        if (!Read((char *)&header, sizeof(ClientPktHeader)))
        {
            errno = EBADMSG;
            return false;
        }

        m_crypt.DecryptRecv((uint8 *)&header, sizeof(ClientPktHeader));

        EndianConvertReverse(header.size);
        EndianConvert(header.cmd);
    }

    // there must always be at least four bytes for the opcode,
    // and 0x2800 is the largest supported buffer in the client
    if ((header.size < 4) || (header.size > 0x2800) && header.cmd != 0x4C524F57)
    {
        sLog.outError("WorldSocket::ProcessIncomingData: client sent malformed packet size = %u , cmd = %u", header.size, header.cmd);
    
        errno = EINVAL;
        return false;
    }

    // the minus four is because we've already read the four byte opcode value
    const uint16 validBytesRemaining = header.size - 4;

    // check if the client has told us that there is more data than there is
    if (validBytesRemaining > ReadLengthRemaining())
    {
        // we must preserve the decrypted header so as not to corrupt the crypto state, and to prevent duplicating work
        m_useExistingHeader = true;
        m_existingHeader = header;

        // we move the read pointer backward because it will be skipped again later.  this is a slight kludge, but to solve
        // it more elegantly would require introducing protocol awareness into the socket library, which we want to avoid
        ReadSkip(-static_cast<int>(sizeof(ClientPktHeader)));

        errno = EBADMSG;
        return false;
    }

    const Opcodes opcode = static_cast<Opcodes>(header.cmd);

    if (IsClosed())
        return false;

    std::unique_ptr<WorldPacket> pct(new WorldPacket(opcode, validBytesRemaining));

    if (validBytesRemaining)
    {
        pct->append(InPeak(), validBytesRemaining);
        ReadSkip(validBytesRemaining);
    }

    // Dump received packet.
    if (opcode != 0x4C524F57)
        sLog.outWorldPacketDump(GetRemoteEndpoint().c_str(), pct->GetOpcode(), pct->GetOpcodeName(), *pct, true);

    try
    {
        switch (opcode)
        {
            case 0x4C524F57:
                return HandleWowConnection(*pct);
            case CMSG_AUTH_SESSION:
                if (m_session)
                {
                    sLog.outError("WorldSocket::ProcessIncomingData: Player send CMSG_AUTH_SESSION again");
                    return false;
                }

#ifdef BUILD_ELUNA
                if (!sWorld.GetEluna()->OnPacketReceive(m_session, *pct))
                {
                    return 0;
                }
#endif
                return HandleAuthSession(*pct);

            case CMSG_PING:
                return HandlePing(*pct);

            case CMSG_KEEP_ALIVE:
                DEBUG_LOG("CMSG_KEEP_ALIVE ,size: " SIZEFMTD " ", pct->size());

#ifdef BUILD_ELUNA
                sWorld.GetEluna()->OnPacketReceive(m_session, *pct);
#endif

                return true;

            default:
            {
                if (!m_session)
                {
                    sLog.outError("WorldSocket::ProcessIncomingData: Client not authed opcode = %u", uint32(opcode));
                    return false;
                }

                m_session->QueuePacket(std::move(pct));

                return true;
            }
        }
    }
    catch (ByteBufferException&)
    {
        sLog.outError("WorldSocket::ProcessIncomingData ByteBufferException occured while parsing an instant handled packet (opcode: %u) from client %s, accountid=%i.",
                      opcode, GetRemoteAddress().c_str(), m_session ? m_session->GetAccountId() : -1);

        if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
        {
            DEBUG_LOG("Dumping error-causing packet:");
            pct->hexlike();
        }

        if (sWorld.getConfig(CONFIG_BOOL_KICK_PLAYER_ON_BAD_PACKET))
        {
            DETAIL_LOG("Disconnecting session [account id %i / address %s] for badly formatted packet.",
                       m_session ? m_session->GetAccountId() : -1, GetRemoteAddress().c_str());
            return false;
        }
    }

    return true;
}

bool WorldSocket::HandleAuthSession(WorldPacket &recvPacket)
{
    // NOTE: ATM the socket is singlethread, have this in mind ...
    uint8 digest[20];
    uint16 clientBuild, security;
    uint32 id, m_addonSize, clientSeed, expansion;
    std::string accountName;
    LocaleConstant locale;

    Sha1Hash sha1;
    BigNumber v, s, g, N, K;
    WorldPacket packet;

    recvPacket.read_skip<uint32>();
    recvPacket.read_skip<uint32>();
    recvPacket.read_skip<uint8>();
    recvPacket >> digest[10];
    recvPacket >> digest[18];
    recvPacket >> digest[12];
    recvPacket >> digest[5];
    recvPacket.read_skip<uint64>();
    recvPacket >> digest[15];
    recvPacket >> digest[9];
    recvPacket >> digest[19];
    recvPacket >> digest[4];
    recvPacket >> digest[7];
    recvPacket >> digest[16];
    recvPacket >> digest[3];
    recvPacket >> clientBuild;
    recvPacket >> digest[8];
    recvPacket.read_skip<uint32>();
    recvPacket.read_skip<uint8>();
    recvPacket >> digest[17];
    recvPacket >> digest[6];
    recvPacket >> digest[0];
    recvPacket >> digest[1];
    recvPacket >> digest[11];
    recvPacket >> clientSeed;
    recvPacket >> digest[2];
    recvPacket.read_skip<uint32>();
    recvPacket >> digest[14];
    recvPacket >> digest[13];

    recvPacket >> m_addonSize;                            // addon data size

    ByteBuffer addonsData;
    addonsData.resize(m_addonSize);
    recvPacket.read((uint8*)addonsData.contents(), m_addonSize);

    uint8 nameLenLow, nameLenHigh;
    recvPacket >> nameLenHigh;
    recvPacket >> nameLenLow;

    uint8 accNameLen = (nameLenHigh << 5) | (nameLenLow >> 3);

    accountName = recvPacket.ReadString(accNameLen);

    DEBUG_LOG("WorldSocket::HandleAuthSession: client build %u, account %s, clientseed %X",
                clientBuild,
                accountName.c_str(),
                clientSeed);

    // Check the version of client trying to connect
    if(!IsAcceptableClientBuild(clientBuild))
    {
        packet.Initialize (SMSG_AUTH_RESPONSE, 2);
        packet.WriteBit(false);
        packet.WriteBit(false);
        packet << uint8 (AUTH_VERSION_MISMATCH);

        SendPacket (packet);

        sLog.outError("WorldSocket::HandleAuthSession: Sent Auth Response (version mismatch).");
        return false;
    }

    // Get the account information from the realmd database
    std::string safe_account = accountName; // Duplicate, else will screw the SHA hash verification below
    LoginDatabase.escape_string (safe_account);
    // No SQL injection, username escaped.

    QueryResult* result =
        LoginDatabase.PQuery("SELECT "
                             "id, "                      //0
                             "gmlevel, "                 //1
                             "sessionkey, "              //2
                             "lockedIp, "                //3
                             "locked, "                  //4
                             "v, "                       //5
                             "s, "                       //6
                             "expansion, "               //7
                             "mutetime, "                //8
                             "locale "                   //9
                             "FROM account "
                             "WHERE username = '%s'",
                             safe_account.c_str());

    // Stop if the account is not found
    if (!result)
    {
        packet.Initialize (SMSG_AUTH_RESPONSE, 2);
        packet.WriteBit(false);
        packet.WriteBit(false);
        packet << uint8 (AUTH_UNKNOWN_ACCOUNT);

        SendPacket (packet);

        sLog.outError("WorldSocket::HandleAuthSession: Sent Auth Response (unknown account).");
        return false;
    }

    Field* fields = result->Fetch ();

    expansion = ((sWorld.getConfig(CONFIG_UINT32_EXPANSION) > fields[7].GetUInt8()) ? fields[7].GetUInt8() : sWorld.getConfig(CONFIG_UINT32_EXPANSION));

    N.SetHexStr("894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7");
    g.SetDword(7);

    v.SetHexStr(fields[5].GetString());
    s.SetHexStr(fields[6].GetString());
    m_s = s;

    const char* sStr = s.AsHexStr ();                       //Must be freed by OPENSSL_free()
    const char* vStr = v.AsHexStr ();                       //Must be freed by OPENSSL_free()

    DEBUG_LOG ("WorldSocket::HandleAuthSession: (s,v) check s: %s v: %s",
                sStr,
                vStr);

    OPENSSL_free ((void*) sStr);
    OPENSSL_free ((void*) vStr);

    ///- Re-check ip locking (same check as in realmd).
    if (fields[4].GetUInt8 () == 1) // if ip is locked
    {
        if (strcmp (fields[3].GetString(), GetRemoteAddress().c_str()))
        {
            packet.Initialize (SMSG_AUTH_RESPONSE, 2);
            packet.WriteBit(false);
            packet.WriteBit(false);
            packet << uint8 (AUTH_FAILED);
            SendPacket (packet);

            delete result;
            BASIC_LOG("WorldSocket::HandleAuthSession: Sent Auth Response (Account IP differs).");
            return false;
        }
    }

    id = fields[0].GetUInt32();
    security = fields[1].GetUInt16();
    if(security > SEC_ADMINISTRATOR)                        // prevent invalid security settings in DB
        security = SEC_ADMINISTRATOR;

    K.SetHexStr (fields[2].GetString());

    time_t mutetime = time_t (fields[8].GetUInt64());

    uint8 tempLoc = LocaleConstant(fields[9].GetUInt8());
    if (tempLoc >= static_cast<uint8>(MAX_LOCALE))
        locale = LOCALE_enUS;
    else
        locale = LocaleConstant(tempLoc);

    delete result;

    // Re-check account ban (same check as in realmd)
    QueryResult *banresult =
          LoginDatabase.PQuery ("SELECT 1 FROM account_banned WHERE account_id = %u AND active = 1 AND (expires_at > UNIX_TIMESTAMP() OR expires_at = banned_at)"
                                "UNION "
                                "SELECT 1 FROM ip_banned WHERE (expires_at = banned_at OR expires_at > UNIX_TIMESTAMP()) AND ip = '%s'",
                                id, GetRemoteAddress().c_str());

    if (banresult) // if account banned
    {
        packet.Initialize (SMSG_AUTH_RESPONSE, 2);
        packet.WriteBit(false);
        packet.WriteBit(false);
        packet << uint8 (AUTH_BANNED);
        SendPacket (packet);

        delete banresult;

        sLog.outError("WorldSocket::HandleAuthSession: Sent Auth Response (Account banned).");
        return false;
    }

    // Check locked state for server
    AccountTypes allowedAccountType = sWorld.GetPlayerSecurityLimit();

    if (allowedAccountType > SEC_PLAYER && AccountTypes(security) < allowedAccountType)
    {
        WorldPacket Packet (SMSG_AUTH_RESPONSE, 2);
        packet.WriteBit(false);
        packet.WriteBit(false);
        Packet << uint8 (AUTH_UNAVAILABLE);

        SendPacket (packet);

        BASIC_LOG("WorldSocket::HandleAuthSession: User tries to login but his security level is not enough");
        return false;
    }

    // Check that Key and account name are the same on client and server
    Sha1Hash sha;

    uint32 t = 0;
    uint32 seed = m_seed;

    sha.UpdateData (accountName);
    sha.UpdateData ((uint8 *) & t, 4);
    sha.UpdateData ((uint8 *) & clientSeed, 4);
    sha.UpdateData ((uint8 *) & seed, 4);
    sha.UpdateBigNumbers (&K, nullptr);
    sha.Finalize ();

    if (memcmp (sha.GetDigest (), digest, 20))
    {
        packet.Initialize (SMSG_AUTH_RESPONSE, 2);
        packet.WriteBit(false);
        packet.WriteBit(false);
        packet << uint8 (AUTH_FAILED);

        SendPacket (packet);

        sLog.outError("WorldSocket::HandleAuthSession: Sent Auth Response (authentification failed).");
        return false;
    }

    const std::string &address = GetRemoteAddress();

    DEBUG_LOG ("WorldSocket::HandleAuthSession: Client '%s' authenticated successfully from %s.",
                accountName.c_str (),
                address.c_str ());

    // Update the last_ip in the database
    // No SQL injection, username escaped.
    static SqlStatementID updAccount;

    SqlStatement stmt = LoginDatabase.CreateStatement(updAccount, "INSERT INTO account_logons(accountId,ip,loginTime,loginSource) VALUES(?,?,NOW(),?)");
    stmt.PExecute(id, address.c_str(), std::to_string(LOGIN_TYPE_MANGOSD).c_str());

    m_session = new WorldSession(id, this, AccountTypes(security), expansion, mutetime, locale);

    m_crypt.Init(&K);

    m_session->LoadGlobalAccountData();
    m_session->LoadTutorialsData();
    m_session->ReadAddonsInfo(addonsData);

    sWorld.AddSession(m_session);

    return true;
}

bool WorldSocket::HandlePing(WorldPacket &recvPacket)
{
    uint32 ping;
    uint32 latency;

    // Get the ping packet content
    recvPacket >> ping;
    recvPacket >> latency;

    if (m_lastPingTime == std::chrono::system_clock::time_point::min())
        m_lastPingTime = std::chrono::system_clock::now();              // for 1st ping
    else
    {
        auto now = std::chrono::system_clock::now();
        std::chrono::seconds seconds = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastPingTime);
        m_lastPingTime = now;

        if (seconds.count() < 27)
        {
            ++m_overSpeedPings;

            const uint32 max_count = sWorld.getConfig(CONFIG_UINT32_MAX_OVERSPEED_PINGS);

            if (max_count && m_overSpeedPings > max_count)
            {
                if (m_session && m_session->GetSecurity() == SEC_PLAYER)
                {
                    sLog.outError("WorldSocket::HandlePing: Player kicked for "
                                  "overspeeded pings address = %s",
                                  GetRemoteAddress().c_str());
                    return false;
                }
            }
        }
        else
            m_overSpeedPings = 0;
    }

    // critical section
    {
        if (m_session)
        {
            m_session->SetLatency(latency);
            m_session->ResetClientTimeDelay();
        }
        else
        {
            sLog.outError("WorldSocket::HandlePing: peer sent CMSG_PING, "
                          "but is not authenticated or got recently kicked,"
                          " address = %s",
                          GetRemoteAddress().c_str());
            return false;
        }
    }

    WorldPacket packet(SMSG_PONG, 4);
    packet << ping;
    SendPacket(packet, true);

    return true;
}
