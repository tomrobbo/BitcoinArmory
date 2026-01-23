////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2026, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <memory>
#include <vector>
#include <set>
#include <map>
#include <functional>

#include <Utils/BinaryData.h>
#include <Utils/ReentrantLock.h>

class TxIOPair;
class Tx;
class NewBlockNotif;

namespace AsyncClient
{
   class BlockDataViewer;
}

namespace Armory
{
   namespace Ledgers
   {
      struct DBCache;
   }

   namespace Bridge
   {
      class TxIOCache : public Lockable
      {
      private:
         std::map<BinaryData, TxIOPair> unspentTxios_;
         std::map<BinaryData, TxIOPair> spentTxios_;
         std::shared_ptr<Ledgers::DBCache> dbCache_;
         uint32_t lastKnownBlock_ = UINT32_MAX;

      private:
         void initAfterLock(void) override {}
         void cleanUpBeforeUnlock(void) override {}

         bool txKeyIsValid(const BinaryData&) const;
         std::pair<std::set<BinaryData>, std::set<uint32_t>> addTxios(
            std::vector<TxIOPair>&, uint32_t);

      public:
         TxIOCache(void);

         uint32_t update(std::shared_ptr<AsyncClient::BlockDataViewer>,
            const NewBlockNotif&);
         std::shared_ptr<const Ledgers::DBCache> getDBCache(void) const;
         std::map<BinaryData, TxIOPair> resolve(
            const std::function<bool(const BinaryData&)>&,
            uint32_t) const;
      };
   } //namespace Bridge
} //namespace Armory
