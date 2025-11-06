////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2016-2025, goatpig.                                         //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include <string>
#include <iostream>
#include <sstream>
#include <btc/ecc.h>
#include <chrono>

#include "ArmoryConfig.h"
#include "BDM_mainthread.h"
#include "BDM_Server.h"
#include "TerminalPassphrasePrompt.h"
#include "Wallets/IOHeader.h"
#include "Wallets/AuthorizedPeers.h"

using namespace Armory::Config;
using namespace std::chrono_literals;

#define LOG_FILE_NAME "dbLog"

int main(int argc, char* argv[])
{
   Cryptography::ECDSA::setupContext();
   startupBIP151CTX();
   startupBIP150CTX(4);

#ifdef _WIN32
   WSADATA wsaData;
   WORD wVersion = MAKEWORD(2, 0);
   WSAStartup(wVersion, &wsaData);
#endif

   try {
      Armory::Config::parseArgs(argc, argv, Armory::Config::ProcessType::DB);
   } catch (const DbErrorMsg& e) {
      std::cout << "Failed to setup with error:" << std::endl;
      std::cout << "   " << e.what() << std::endl;
      std::cout << "Aborting!" << std::endl;

      return -1;
   }

   auto logFilePath = Pathing::logFilePath(LOG_FILE_NAME).string();
   std::cout << "logging in " << logFilePath << std::endl;
   STARTLOGGING(logFilePath, LogLvlDebug);
   if (!NetworkSettings::ephemeralPeers()) {
      LOGENABLESTDOUT();
   } else {
      LOGDISABLESTDOUT();
   }

   LOGINFO << "Running on " << DBSettings::threadCount() << " threads";
   LOGINFO << "Ram usage level: " << DBSettings::ramUsage();

   //init state
   DBSettings::setServiceType(SERVICE_WEBSOCKET);
   BlockDataManagerThread bdmThread;

   if (!DBSettings::checkChain()) {
      //check we can listen on this ip:port
      if (SimpleSocket::checkSocket("127.0.0.1", NetworkSettings::dbPort())) {
         LOGERR << "There is already a process listening on port " << 
            NetworkSettings::dbPort();
         LOGERR << "ArmoryDB cannot start under these conditions. Shutting down!";
         LOGERR << "Make sure to shutdown the conflicting process" <<
            "before trying again (most likely another ArmoryDB instance)";
         exit(-2);
      }
   }
   LOGINFO << "datadir: " << Armory::Config::getDataDir().string();

   if (NetworkSettings::ephemeralPeers()) {
      if (NetworkSettings::oneWayAuth()) {
         LOGERR << "--ephemeral and --oneWayAuth are mutually exclusive for db";
         exit(-3);
      }
      //initAuthPeers will setup the ephemeral keys
      WebSocketServer::initAuthPeers(
         Armory::Wallets::IO::ReadOnlyFileParams{{}, nullptr});
   } else {
      //setup remote peers db, this will block the init process until
      //peers db is unlocked
      auto serverPeersFile = Armory::Config::getDataDir() / SERVER_AUTH_PEER_FILENAME;
      if (!FileUtils::fileExists(serverPeersFile, 0) &&
         !Armory::Config::NetworkSettings::ephemeralPeers()) {
         LOGINFO << "no server peers store found, creating one...";
         auto passWrapper = []()->std::unique_ptr<Armory::Passphrase::Params>
         {
            auto passLbd = TerminalPassphrasePrompt::getLambda(
               "new server peers store");
            auto result = passLbd({});
            if (!result.success) {
               throw std::runtime_error("peers store init was rejected");
            }
            return std::make_unique<Armory::Passphrase::Params>(
               250ms, 0, std::move(result.passphrase));
         };
         auto peers = Armory::Wallets::AuthorizedPeers::createWallet(
            {serverPeersFile, {passWrapper}});
         WebSocketServer::initAuthPeers(peers);
      } else {
         auto passLbd = TerminalPassphrasePrompt::getLambda(
            "server peers store");
         WebSocketServer::initAuthPeers({serverPeersFile, passLbd});
      }
   }

   //start blockchain service
   bdmThread.start(DBSettings::initMode());
   if (!DBSettings::checkChain()) {
      WebSocketServer::start(bdmThread.bdm(), false);
      LOGINFO << "WS server has shut down" << std::endl;
   } else {
      bdmThread.join();
   }

   //shutdown BDM and cleanup crypto contexts
   bdmThread.shutdown();
   shutdownBIP151CTX();
   Cryptography::ECDSA::shutdown();

   return 0;
}
