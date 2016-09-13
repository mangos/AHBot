/**
 * MaNGOS is a full featured server for World of Warcraft, supporting
 * the following clients: 1.12.x, 2.4.3, 3.3.5a, 4.3.4a and 5.4.8
 *
 * Copyright (C) 2005-2015  MaNGOS project <http://getmangos.eu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,r
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * World of Warcraft, and all World of Warcraft or Warcraft art, images,
 * and lore are copyrighted by Blizzard Entertainment, Inc.
 */

/// \addtogroup ahbotd AuctionHouseBot Daemon
/// @{
/// \file

#include "Common.h"
#include "Database/DatabaseEnv.h"

#include "Config/Config.h"
#include "Log.h"
#include "SystemConfig.h"
#include "revision.h"
#include "Util.h"

#include "AuctionHouseBot.h"

#include <ace/Get_Opt.h>

#ifdef WIN32
#include "Win/ServiceWin32.h"
char serviceName[] = "ahbotd";
char serviceLongName[] = "MaNGOS auction house bot service";
char serviceDescription[] = "Massive Network Game Object Server";
/*
 * -1 - not in service mode
 *  0 - stopped
 *  1 - running
 *  2 - paused
 */
int m_ServiceStatus = -1;
#else
#include "Linux/PosixDaemon.h"
#endif

/**
 * If we don't include this little gem it won't compile as we pull in
 * the whole server as a dependency and parts of that depend on this
 * symbol being available during compile and run time.
 */
uint32 realmID;

#include "Chat.h"
/**
 * Same goes for these guys..
 */
bool ChatHandler::HandleAccountDeleteCommand(char* args) {}
bool ChatHandler::GetDeletedCharacterInfoList(DeletedInfoList& foundList, std::string searchString) {}
std::string ChatHandler::GenerateDeletedCharacterGUIDsWhereStr(DeletedInfoList::const_iterator& itr, DeletedInfoList::const_iterator const& itr_end) {}
void ChatHandler::HandleCharacterDeletedListHelper(DeletedInfoList const& foundList) {}
bool ChatHandler::HandleCharacterDeletedListCommand(char* args) {}
void ChatHandler::HandleCharacterDeletedRestoreHelper(DeletedInfo const& delInfo) {}
bool ChatHandler::HandleCharacterDeletedRestoreCommand(char* args) {}
bool ChatHandler::HandleCharacterDeletedDeleteCommand(char* args) {}
bool ChatHandler::HandleCharacterDeletedOldCommand(char* args) {}
bool ChatHandler::HandleCharacterEraseCommand(char* args) {}
bool ChatHandler::HandleQuitCommand(char* /*args*/) {}
bool ChatHandler::HandleServerExitCommand(char* /*args*/) {}
bool ChatHandler::HandleAccountOnlineListCommand(char* args) {}
bool ChatHandler::HandleAccountCreateCommand(char* args) {}
bool ChatHandler::HandleServerLogFilterCommand(char* args) {}
bool ChatHandler::HandleServerLogLevelCommand(char* args) {}

bool StartDB();
void PingDatabases();
void HaltDelayThreads();
void AllowAsyncTransactions();
void UnhookSignals();
void HookSignals();
void MainLoop();
bool CreatePid();
void SetProcessPriority();
void CheckConfigFileVersion();

/// Setting it to true stops the server
bool stopEvent = false;

/**
 * These are needed by the auctionhouse bot and it's dependencies, 
 * mostly the sAuctionMgr
 */
DatabaseType WorldDatabase;      ///< Accessor to the world database
DatabaseType CharacterDatabase;  ///< Accessor to the character database
DatabaseType LoginDatabase;      ///< Accessor to the realm/login database

/// Print out the usage string for this program on the console.
void usage(const char* prog)
{
    sLog.outString(
        "Usage: \n %s [<options>]\n"
        "    -v, --version            print version and exit\n\r"
        "    -c config_file           use config_file as configuration file\n\r"
#ifdef WIN32
        "    Running as service functions:\n\r"
        "    -s run                   run as service\n\r"
        "    -s install               install service\n\r"
        "    -s uninstall             uninstall service\n\r"
#else
        "    Running as daemon functions:\n\r"
        "    -s run                   run as daemon\n\r"
        "    -s stop                  stop daemon\n\r"
#endif
        , prog);
}

/// Launch the realm server
int main(int argc, char** argv)
{
    ///- Command line parsing
    char const* cfg_file = AUCTIONHOUSEBOT_CONFIG_LOCATION;

    char const* options = ":c:s:";

    ACE_Get_Opt cmd_opts(argc, argv, options);
    cmd_opts.long_option("version", 'v');

    char serviceDaemonMode = '\0';

    //TODO: Move top ReadOptions or something
    int option;
    while ((option = cmd_opts()) != EOF)
    {
        switch (option)
        {
            case 'c':
                cfg_file = cmd_opts.opt_arg();
                break;
            case 'v':
                printf("%s\n", REVISION_NR);
                return 0;

            case 's':
            {
                const char* mode = cmd_opts.opt_arg();

                if (!strcmp(mode, "run"))
                    { serviceDaemonMode = 'r'; }
#ifdef WIN32
                else if (!strcmp(mode, "install"))
                    { serviceDaemonMode = 'i'; }
                else if (!strcmp(mode, "uninstall"))
                    { serviceDaemonMode = 'u'; }
#else
                else if (!strcmp(mode, "stop"))
                    { serviceDaemonMode = 's'; }
#endif
                else
                {
                    sLog.outError("Runtime-Error: -%c unsupported argument %s", cmd_opts.opt_opt(), mode);
                    usage(argv[0]);
                    Log::WaitBeforeContinueIfNeed();
                    return 1;
                }
                break;
            }
            case ':':
                sLog.outError("Runtime-Error: -%c option requires an input argument", cmd_opts.opt_opt());
                usage(argv[0]);
                Log::WaitBeforeContinueIfNeed();
                return 1;
            default:
                sLog.outError("Runtime-Error: bad format of commandline arguments");
                usage(argv[0]);
                Log::WaitBeforeContinueIfNeed();
                return 1;
        }
    }

#ifdef WIN32                                                // windows service command need execute before config read
    switch (serviceDaemonMode)
    {
        case 'i':
            if (WinServiceInstall())
                { sLog.outString("Installing service"); }
            return 1;
        case 'u':
            if (WinServiceUninstall())
                { sLog.outString("Uninstalling service"); }
            return 1;
        case 'r':
            WinServiceRun();
            break;
    }
#endif

    if (!sConfig.SetSource(cfg_file))
    {
        sLog.outError("Could not find configuration file %s.", cfg_file);
        Log::WaitBeforeContinueIfNeed();
        return 1;
    }

#ifndef WIN32                                               // posix daemon commands need apply after config read
    switch (serviceDaemonMode)
    {
        case 'r':
            startDaemon();
            break;
        case 's':
            stopDaemon();
            break;
    }
#endif

    sLog.Initialize();
    
    sLog.outString("%s [auctionhousebot-daemon]", REVISION_NR);
    sLog.outString("<Ctrl-C> to stop.\n");
    sLog.outString("Using configuration file %s.", cfg_file);

    CheckConfigFileVersion();

    DETAIL_LOG("Using ACE: %s", ACE_VERSION);

    if (!CreatePid())
    {
        return 1;
    }

    if (!StartDB())
    {
        return 1;
    }

    ///- Catch termination signals
    HookSignals();

    ///- Handle affinity for multiple processors and process priority on Windows
    SetProcessPriority();

    // server has started up successfully => enable async DB requests
    AllowAsyncTransactions();

#ifndef WIN32
    detachDaemon();
#endif

    MainLoop();

    ///- Wait for the delay thread to exit
    HaltDelayThreads();

    ///- Remove signal handling before leaving
    UnhookSignals();

    sLog.outString("Halting process...");
    return 0;
}

/**
 * Does all the main work, calls \ref AuctionHouseBot::Update
 */
void MainLoop()
{
    // maximum counter for next ping to the db's
    uint32 numLoops = (sConfig.GetIntDefault("MaxPingTime", 30) * (MINUTE * 1000000 / 100000));
    uint32 loopCounter = 0;
    
    ///- Wait for termination signal
    uint32 diff = WorldTimer::tick();
    //We want to sleep up until 5000 ms has passed since the last
    //iteration
    const uint32 sleepGoal = 5000;
    while (!stopEvent)
    {
        diff = WorldTimer::tick();
        if (diff < sleepGoal)
        {
            ACE_Based::Thread::Sleep(sleepGoal - diff);
        }
        //We tick to get the time after the sleep so that we sleep
        //properly the next time around
        WorldTimer::tick();

        sAuctionBot.Update();

        if ((++loopCounter) == numLoops)
        {
            loopCounter = 0;
            DETAIL_LOG("Ping MySQL to keep connection alive");
            PingDatabases();
        }

#ifdef WIN32
        if (m_ServiceStatus == 0) { stopEvent = true; }
        while (m_ServiceStatus == 2) { Sleep(1000); }
#endif
    }

}

/**
 * Creates a process id file if the config file contains a name for
 * that file. 
 */
bool CreatePid()
{
    std::string pidFile = sConfig.GetStringDefault("PidFile", "");
    if (!pidFile.empty())
    {
        uint32 pid = CreatePIDFile(pidFile);
        if (!pid)
        {
            sLog.outError("Can not create PID file %s.\n",
                          pidFile.c_str());
            Log::WaitBeforeContinueIfNeed();
            return false;
        }

        sLog.outString("Daemon PID: %u\n", pid);
    }
    return true;
}

/**
 * Checks the config file version and prints a small warning if it's
 * out of date. We start even if it is though.
 */
void CheckConfigFileVersion()
{
    ///- Check the version of the configuration file
    uint32 confVersion = sConfig.GetIntDefault("ConfVersion", 0);
    if (confVersion < AUCTIONHOUSEBOT_CONFIG_VERSION)
    {
        sLog.outError("*****************************************************************************");
        sLog.outError(" WARNING: Your ahbot.conf version indicates your conf file is out of date!");
        sLog.outError("          Please check for updates, as your current default values may cause");
        sLog.outError("          strange behavior.");
        sLog.outError("*****************************************************************************");
        Log::WaitBeforeContinueIfNeed();
    }
}

/**
 * Changes the process priority of the currently running process,
 * only implemented for windows.
 */
void SetProcessPriority()
{
#ifdef WIN32
    {
        HANDLE hProcess = GetCurrentProcess();

        uint32 Aff = sConfig.GetIntDefault("UseProcessors", 0);
        if (Aff > 0)
        {
            ULONG_PTR appAff;
            ULONG_PTR sysAff;

            if (GetProcessAffinityMask(hProcess, &appAff, &sysAff))
            {
                ULONG_PTR curAff = Aff & appAff;            // remove non accessible processors

                if (!curAff)
                {
                    sLog.outError("Processors marked in UseProcessors bitmask (hex) %x not accessible for realmd. Accessible processors bitmask (hex): %x", Aff, appAff);
                }
                else
                {
                    if (SetProcessAffinityMask(hProcess, curAff))
                    { sLog.outString("Using processors (bitmask, hex): %x", curAff); }
                    else
                    { sLog.outError("Can't set used processors (hex): %x", curAff); }
                }
            }
            sLog.outString();
        }

        bool Prio = sConfig.GetBoolDefault("ProcessPriority", false);

        if (Prio)
        {
            if (SetPriorityClass(hProcess, HIGH_PRIORITY_CLASS))
            { sLog.outString("ahbotd process priority class set to HIGH"); }
            else
            { sLog.outError("Can't set ahbotd process priority class."); }
            sLog.outString();
        }
    }
#endif
}

/// Handle termination signals
/** Put the global variable stopEvent to 'true' if a termination signal is caught **/
void OnSignal(int s)
{
    switch (s)
    {
        case SIGINT:
        case SIGTERM:
            stopEvent = true;
            break;
#ifdef _WIN32
        case SIGBREAK:
            stopEvent = true;
            break;
#endif
    }

    signal(s, OnSignal);
}

struct DatabaseInfo
{
    DatabaseType& db; /** The actual database */
    std::string infoCfgKey; /** The config key that holds the db config values */
    std::string connsCfgKey; /** The config key that holds the number of db connections to use */
    std::string name; /** Descriptive name of this database */
     /** The expected version of the database
      * \see REVISION_DB_REALMD
      */
    std::string version;
};

DatabaseInfo databases[] = {
    {
        LoginDatabase,
        "LoginDatabaseInfo",
        "LoginDatabaseConnections",
        "login database",
        REVISION_DB_REALMD
    },
    {
        WorldDatabase,
        "WorldDatabaseInfo",
        "WorldDatabaseConnections",
        "world database",
        REVISION_DB_MANGOS
    },
    {
        CharacterDatabase,
        "CharacterDatabaseInfo",
        "CharacterDatabaseConnections",
        "character database",
        REVISION_DB_CHARACTERS
    },
};
static const int DATABASE_INFO_NUM = 3;

void HaltDelayThreads()
{
    for (int i = 0; i < DATABASE_INFO_NUM; ++i)
    {
        databases[i].db.HaltDelayThread();
    }
}

void AllowAsyncTransactions()
{
    for (int i = 0; i < DATABASE_INFO_NUM; ++i)
    {
        databases[i].db.AllowAsyncTransactions();
    }
}

void PingDatabases()
{
    for (int i = 0; i < DATABASE_INFO_NUM; ++i)
    {
        databases[i].db.Ping();
    }
}

/// Initialize connection to the database
bool StartDB()
{
    for (int i = 0; i < DATABASE_INFO_NUM; ++i)
    {
        DatabaseInfo& dbInfo = databases[i];
        DatabaseType& db = dbInfo.db;
        std::string dbConfig = sConfig.GetStringDefault(dbInfo.infoCfgKey.c_str(), "");
        int nConnections = sConfig.GetIntDefault(dbInfo.connsCfgKey.c_str(), 1);
        if (dbConfig.empty())
        {
            sLog.outError("Database not specified in configuration file");
            Log::WaitBeforeContinueIfNeed();
            return false;
        }
        sLog.outString("%s total connections: %i",
                       dbInfo.name.c_str(),
                       nConnections + 1);

        if (!db.Initialize(dbConfig.c_str(), nConnections))
        {
            sLog.outError("Can not connect to %s %s",
                          dbInfo.name.c_str(),
                          dbConfig.c_str());
            Log::WaitBeforeContinueIfNeed();
            return false;
        }

        sLog.outError("TODO: Actually check the db version..");
        //TODO: Different names of these fields in the different
        //dbs..
        // if (!db.CheckRequiredField("db_version", dbInfo.version.c_str()))
        // {
        //     ///- Wait for already started DB delay threads to end
        //     WorldDatabase.HaltDelayThread();
        //     return false;
        // }
    }
    return true;
}

/// Define hook 'OnSignal' for all termination signals
void HookSignals()
{
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);
#ifdef _WIN32
    signal(SIGBREAK, OnSignal);
#endif
}

/// Unhook the signals before leaving
void UnhookSignals()
{
    signal(SIGINT, 0);
    signal(SIGTERM, 0);
#ifdef _WIN32
    signal(SIGBREAK, 0);
#endif
}

/// @}
