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

      using TxIOKey = BinaryData;
      using ScrAddr = BinaryData;
      using AddressFilter = std::function<bool(const ScrAddr&)>;

      struct CacheResolveResult
      {
         const uint32_t topBlock;
         std::map<TxIOKey, TxIOPair> txioMap;
         std::map<ScrAddr, std::vector<TxIOPair*>> addrTxioMap;

         void addTxio(const TxIOKey&, const TxIOPair&, const ScrAddr&);
      };

      struct ChainData
      {
         const std::map<TxIOKey, TxIOPair> txioMap;
         std::map<ScrAddr, std::vector<int64_t>> balanceMap;
         std::map<ScrAddr, uint64_t> countMap;

         int64_t totalBalance       = 0;
         int64_t spendableBalance   = 0;
         int64_t unconfirmedBalance = 0;
         int64_t txioCount          = 0;

         ChainData(CacheResolveResult&);
      };

      class TxIOCache : public Lockable
      {
      private:
         std::map<TxIOKey, TxIOPair> unspentTxios_;
         std::map<TxIOKey, TxIOPair> spentTxios_;
         std::map<TxIOKey, TxIOPair> zcTxios_;
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
