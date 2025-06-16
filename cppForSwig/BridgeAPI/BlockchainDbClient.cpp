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

using namespace Armory::Bridge;

////////////////////////////////////////////////////////////////////////////////
bool Armory::Bridge::spawnDb()
{
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
   try {
      result->connectToRemote();
      result->registerWithDB(
         Config::BitcoinSettings::getMagicBytes().toHexStr());

      //notify setup is done
      cbPtr->notifySetupDone();
   } catch (const std::exception& e) {
      LOGERR << "failed to connect to db with error: " << e.what();
   }

   wltManager->setBdvPtr(result);
   return result;
}
