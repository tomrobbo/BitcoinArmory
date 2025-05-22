////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2021-2025, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <functional>
#include <set>
#include <chrono>

#include "../SecureBinaryData.h"

////////////////////////////////////////////////////////////////////////////////
namespace Armory
{
   namespace Wallets
   {
      class EncryptionKeyId;

      namespace Encryption
      {
         struct ClearTextEncryptionKey;
      }
   }

   namespace Passphrase
   {
      /* to unlock an encrypted container */
      using UnlockFunc = std::function<SecureBinaryData(
         const std::set<Wallets::EncryptionKeyId>&)>;

      /* to get passphrase and kdf params at encrypted container creation */
      struct Params
      {
         std::chrono::milliseconds unlockMs;
         uint32_t memTargetMB=0;
         SecureBinaryData passphrase{};

         Params(void);
         Params(std::chrono::milliseconds, uint32_t, SecureBinaryData);
      };

      class SetNew
      {
         friend struct Armory::Wallets::Encryption::ClearTextEncryptionKey;

      private:
         using SetNewPassFunc = std::function<std::unique_ptr<Params>(void)>;

         mutable std::unique_ptr<Params> params_;
         const SetNewPassFunc setNewPassphrase_;

      private:
         std::unique_ptr<Params> moveParams(void);

      public:
         SetNew(void);
         SetNew(std::chrono::milliseconds, uint32_t, SecureBinaryData);
         SetNew(const SetNewPassFunc&);
         SetNew(const SetNew&);

         const Params& get(void) const;
         SetNew copy(void) const;
         UnlockFunc getUnlockFunc(void) const;
      };
   }
}
