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

namespace Armory
{
   namespace Bridge
   {
      class WalletManager;
      using BdvPtr = std::shared_ptr<AsyncClient::BlockDataViewer>;

      ////////
      bool spawnDb(void);
      BdvPtr setupClientConnection(
         const std::filesystem::path&,
         const std::function<void(BinaryData&)>&,
         std::shared_ptr<WalletManager>
      );
   }
}