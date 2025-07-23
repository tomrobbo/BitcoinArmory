////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2025, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include <cstdlib>

#include "BlockchainDbClient.h"
#include "Wallets/Manager.h"
#include "Wallets/Notifications.h"
#include "TerminalPassphrasePrompt.h"

#include "../Wallets/IOHeader.h"
#include "../AsyncClient.h"

#include "spawn.h"
#include <random>

using namespace Armory::Bridge;
using namespace std::string_view_literals;

////////////////////////////////////////////////////////////////////////////////
bool Armory::Bridge::spawnDb()
{
   //get full path to armorydb
   const std::filesystem::path armoryDbPath{};

   //setup ephemeral authPeers

   //generate random db port
   uint32_t port = (rand() % 10000) + 50000;
   auto portStr = std::to_string(port);

   //setup argv
   char* argv[] = {
      armoryDbPath.string().data(),
      (char*)"--ephemeral"sv.data(),
      std::string{"--dbPort=" + portStr}.data(),
      (char*)nullptr
   };

   throw std::runtime_error("[spawnDb] implement me");
}

////////////////////////////////////////////////////////////////////////////////
BdvPtr Armory::Bridge::setupClientConnection(
   const std::filesystem::path& path,
   const std::function<void(BinaryData&)>& writeFunc,
   std::shared_ptr<WalletManager> wltManager)
{
   //setup bdv obj
   auto cbPtr = wltManager->setupBdvCallback(writeFunc);
   BdvPtr result = AsyncClient::BlockDataViewer::getNewBDV(
      Config::NetworkSettings::dbIP(), Config::NetworkSettings::dbPort(),
      Wallets::IO::ReadOnlyFileParams {
         path / CLIENT_AUTH_PEER_FILENAME,
         TerminalPassphrasePrompt::getLambda("db identification key")
      },
      true, Config::NetworkSettings::oneWayAuth(), cbPtr
   );

   //TODO: set gui prompt to accept server pubkeys
   result->setCheckServerKeyPromptLambda(
      [](const BinaryData&, const std::string&)->bool
      { return true; }
   );

   //connect to db
   if (!result->connectToRemote()) {
      return nullptr;
   }
   result->registerWithDB(
      Config::BitcoinSettings::getMagicBytes().toHexStr());

   //notify setup is done
   cbPtr->notifySetupDone();
   wltManager->setBdvPtr(result);
   return result;
}
