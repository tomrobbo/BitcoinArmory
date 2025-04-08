////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2025, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Progress.h"
#include "PassphraseLambda.h"
#include <set>
#include <filesystem>
#include <chrono>

namespace Armory
{
   namespace Wallets
   {
      namespace IO
      {
         using namespace std::chrono_literals;
         struct OpenFileParams
         {
            const std::filesystem::path filePath;
            const PassphraseLambda controlPassFunc=nullptr;
            const std::chrono::milliseconds controlUnlock=250ms;
            const bool fileExists=true;
         };

         struct CreationParams
         {
            const std::filesystem::path folder{"./"};

            //encrypts/unlocks private keys
            //2sec default unlock duration for private keys
            const SecureBinaryData privatePassphrase;
            const std::chrono::milliseconds privateUnlock=2000ms;
            //const uint32_t maxPrivateUnlockMemUsage = 128 * 1024 * 1024;

            //encrypts/unlocks all data in the wallet
            const SecureBinaryData controlPassphrase;
            const std::chrono::milliseconds controlUnlock=250ms;
            //const uint32_t maxControlUnlockMemUsage = 128 * 1024 * 1024;

            //to report on creation progress
            Progress::Func progressFunc=nullptr;

            //misc
            const size_t lookup{100};
            const std::string label{};
            const std::string description{};

            ////////
            OpenFileParams getOpenFileParams(const std::string& masterId,
               const std::string& suffix={"wallet"}) const
            {
               auto path = folder / std::filesystem::path{
                  "armory_" + masterId + "_" + suffix + ".lmdb"};
               auto passLbd = [pass=controlPassphrase]
                  (const std::set<EncryptionKeyId>&)->SecureBinaryData
               { return pass; };

               return OpenFileParams{path, passLbd, controlUnlock, false};
            }
         };
      };
   };
};