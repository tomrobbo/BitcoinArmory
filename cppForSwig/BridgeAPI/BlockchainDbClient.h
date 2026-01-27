////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2025, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <functional>
#include <memory>
#include <filesystem>

namespace AsyncClient
{
   class BlockDataViewer;
}

class BinaryData;
class RemoteCallback;

namespace Armory
{
   namespace Wallets
   {
      class AuthorizedPeers;
   }

   namespace Bridge
   {
      using BdvPtr = std::shared_ptr<AsyncClient::BlockDataViewer>;
   #ifdef _WIN32
      extern void* autoDbHandle;
   #else
      extern int autoDbPid;
   #endif

      ////////
      std::pair<std::shared_ptr<Wallets::AuthorizedPeers>, uint32_t> spawnDb(void);
      bool isDbRunning(void);

      BdvPtr setupClientConnection(
         std::shared_ptr<Wallets::AuthorizedPeers>,
         const std::string&, const std::string&,
         bool, bool,
         std::shared_ptr<RemoteCallback>
      );
   }
}
