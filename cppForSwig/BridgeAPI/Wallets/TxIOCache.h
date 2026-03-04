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
struct UTXO;

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
      struct NotifStruct;

      using AddressFilter = std::function<bool(const BinaryData&)>;
      struct CacheResolveResult
      {
         const uint32_t topBlock;
         std::map<BinaryData, TxIOPair> txioMap;
         std::map<BinaryData, std::vector<TxIOPair*>> addrTxioMap;

         void addTxio(const BinaryData&, const TxIOPair&, const BinaryData&);
      };

      class TxIOCache : public Lockable
      {
      private:
         std::map<BinaryData, TxIOPair> unspentTxios_;
         std::map<BinaryData, TxIOPair> spentTxios_;
         std::map<BinaryData, TxIOPair> zcTxios_;
         std::shared_ptr<Ledgers::DBCache> dbCache_;
         uint32_t lastKnownBlock_ = UINT32_MAX;

      private:
         void initAfterLock(void) override {}
         void cleanUpBeforeUnlock(void) override {}

         bool txKeyIsValid(const BinaryData&) const;
         std::pair<std::set<BinaryData>, std::set<uint32_t>> addTxios(
            std::vector<TxIOPair>&, uint32_t);
         void updateZC(std::shared_ptr<AsyncClient::BlockDataViewer>,
            const std::vector<TxIOPair>&);

      public:
         TxIOCache(void);

         uint32_t update(std::shared_ptr<AsyncClient::BlockDataViewer>,
            std::shared_ptr<NotifStruct>);
         std::shared_ptr<const Ledgers::DBCache> getDBCache(void) const;
         CacheResolveResult resolve(const AddressFilter&, uint32_t) const;
         CacheResolveResult resolveZC(const AddressFilter&) const;
         std::map<BinaryData, std::set<BinaryData>> getAddressBook(
            const AddressFilter&) const;
         std::vector<UTXO> getUTXOs(const AddressFilter&) const;
         std::map<BinaryData, TxIOPair> filterTxios(
            const std::vector<TxIOPair>&, const AddressFilter&) const;
      };
   } //namespace Bridge
} //namespace Armory
