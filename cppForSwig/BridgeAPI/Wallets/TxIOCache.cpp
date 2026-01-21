////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2026, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "TxIOCache.h"

#include <Utils/DBUtils.h>
#include <BlockchainDatabase/txio.h>
#include <Ledgers/Context.h>
#include <TxClasses.h>
#include <AsyncClient.h>

using namespace Armory;
using namespace Armory::Bridge;

////////////////////////////////////////////////////////////////////////////////
// TxIOCache
TxIOCache::TxIOCache() :
   dbCache_{std::make_shared<Ledgers::DBCache>()}
{}

std::shared_ptr<const Ledgers::DBCache> TxIOCache::getDBCache() const
{
   return std::const_pointer_cast<const Ledgers::DBCache>(dbCache_);
}

std::map<BinaryData, TxIOPair> TxIOCache::resolve(
   const std::function<bool(const BinaryData&)>& filter,
   uint32_t fromHeight) const
{
   std::map<BinaryData, TxIOPair> result;
   for (const auto& txio : txioMap_) {
      const auto& txKey = txio.second.getTxRefOfOutput().getDBKey();
      const auto& tx = dbCache_->txMap.at(txKey);
      auto scrAddr = tx.getScrAddrForTxOut(txio.second.getIndexOfOutput());
      if (filter(scrAddr)) {
         result.emplace(txio);
      }
   }
   return result;
}

////////
uint32_t TxIOCache::update(
   std::shared_ptr<AsyncClient::BlockDataViewer> bdvPtr,
   uint32_t topHeight)
{
   uint32_t fromHeight = lastKnownBlock_ == UINT32_MAX ? 0 : lastKnownBlock_ + 1;

   //1. txios
   auto promTxios = std::make_shared<std::promise<std::vector<TxIOPair>>>();
   auto futTxios = promTxios->get_future();

   bdvPtr->getTxios(fromHeight, [prom = promTxios]
      (ReturnMessage<std::vector<TxIOPair>> result) {
         prom->set_value(result.get());
   });
   auto txios = std::move(futTxios.get());

   auto missingStuff = addTxios(txios, topHeight);
   auto missingTxKeys = std::move(missingStuff.first);
   auto missingHeights = std::move(missingStuff.second);

   //2. missing txs
   auto promTxs = std::make_shared<std::promise<std::vector<Tx>>>();
   auto futTxs = promTxs->get_future();
   bdvPtr->getTxsByKey(missingTxKeys, [prom = promTxs]
      (ReturnMessage<std::vector<Tx>> result) {
         prom->set_value(result.get());
   });

   //3. missing heights
   auto promHeights = std::make_shared<
      std::promise<std::map<uint32_t, uint32_t>>>();
   auto futHeights = promHeights->get_future();
   bdvPtr->getTimestampsForHeights(missingHeights, [prom = promHeights]
      (ReturnMessage<std::map<uint32_t, uint32_t>> result) {
         prom->set_value(result.get());
      }
   );

   //4. commit it all to the cache
   auto txs = std::move(futTxs.get());
   auto heights = std::move(futHeights.get());

   ReentrantLock lock(this);
   for (auto& tx : txs) {
      dbCache_->txMap.emplace(tx.getDBKey(), std::move(tx));
   }
   for (auto& ts : heights) {
      dbCache_->timestamps.emplace(ts);
   }
   return fromHeight;
}

std::pair<std::set<BinaryData>, std::set<uint32_t>> TxIOCache::addTxios(
   std::vector<TxIOPair>& txios, uint32_t fetchedHeight)
{
   ReentrantLock lock(this);

   std::set<BinaryData> missingTxKeys;
   std::set<uint32_t> missingHeights;
   for (auto& txio : txios) {
      const auto& txRef = txio.getTxRefOfOutput();
      auto height = DBUtils::hgtxToHeight(txRef.getDBKey().getSliceRef(0, 4));
      if (dbCache_->timestamps.find(height) == dbCache_->timestamps.end()) {
         missingHeights.emplace(height);
      }
      if (dbCache_->txMap.find(txRef.getDBKey()) == dbCache_->txMap.end()) {
         missingTxKeys.emplace(txRef.getDBKey());
      }

      auto keyOfOutput = txio.getDBKeyOfOutput();
      auto iter = txioMap_.find(keyOfOutput);
      if (iter != txioMap_.end()) {
         iter->second.merge(txio);
      } else {
         txioMap_.emplace(keyOfOutput, std::move(txio));
      }
   }

   lastKnownBlock_ = fetchedHeight;
   return {std::move(missingTxKeys), std::move(missingHeights)};
}
