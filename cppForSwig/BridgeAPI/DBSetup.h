////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2025-2026, goatpig                                          //
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
      class PeerKey;
   }

   namespace Bridge
   {
      using BdvPtr = std::shared_ptr<AsyncClient::BlockDataViewer>;
   #ifdef _WIN32
      extern void* autoDbHandle;
   #else
      extern int autoDbPid;
   #endif

      /* db helpers */
      std::pair<std::shared_ptr<Wallets::AuthorizedPeers>, uint32_t> spawnDb(
         const std::filesystem::path&, const std::filesystem::path&);
      bool isDbRunning(void);

      BdvPtr setupClientConnection(
         std::shared_ptr<Wallets::AuthorizedPeers>,
         const std::string&, const std::string&, bool,
         const std::function<bool(const BinaryData&)>&,
         std::shared_ptr<RemoteCallback>
      );
      BdvPtr setupClientConnection(
         std::shared_ptr<Wallets::AuthorizedPeers>,
         const Wallets::PeerKey&,
         std::shared_ptr<RemoteCallback>
      );
   } //namespace Bridge
} //namespace Armory

namespace Node
{
   namespace Core
   {
      struct DatadirState
      {
         const std::filesystem::path datadir;
         const size_t fileSize;
         const bool isPruned;
      };

      struct BinaryState
      {
         const std::filesystem::path path;
         const std::string version;
      };

      std::filesystem::path findDatadir(void);
      std::filesystem::path findBinary(void);

      DatadirState validateDatadir(const std::filesystem::path&);
      BinaryState validateBinary(const std::filesystem::path&);
   }
}
