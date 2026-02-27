////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2026, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "TxIOCache.h"

#include <Utils/DBUtils.h>
#include <Utils/BtcUtils.h>
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

CacheResolveResult TxIOCache::resolve(
   const AddressFilter& filter, uint32_t fromHeight) const
{
   //run through unspent txios first
   CacheResolveResult result{lastKnownBlock_};
   for (const auto& txio : unspentTxios_) {
      const auto& txKey = txio.second.getTxRefOfOutput().getDBKey();

      //skip txios that are not on the main chain
      if (!txKeyIsValid(txKey)) {
         continue;
      }

      //does this txout trigger our filter
      const auto& tx = dbCache_->txMap.at(txKey);
      auto scrAddr = tx.getScrAddrForTxOut(txio.second.getIndexOfOutput());
      if (filter(scrAddr)) {
         result.addTxio(txio.first, txio.second, scrAddr);
      }
   }

   //check spent txios now
   for (const auto& txio : spentTxios_) {
      //is output on mainchain?
      const auto& txKeyOutput = txio.second.getTxRefOfOutput().getDBKey();
      if (!txKeyIsValid(txKeyOutput)) {
         continue;
      }

      //is input on mainchain?
      const auto& txKeyInput = txio.second.getTxRefOfInput().getDBKey();
      if (!txKeyIsValid(txKeyInput)) {
         continue;
      }

      //do we have an unspent txio in the result map?
      auto txioKey = txio.second.getDBKeyOfOutput();
      auto iter = result.txioMap.find(txioKey);
      if (iter != result.txioMap.end()) {
         //we do, merge in the txin
         iter->second.merge(txio.second);
         continue;
      }

      //we don't, is the txout relevant then?
      const auto& tx = dbCache_->txMap.at(txKeyOutput);
      auto scrAddr = tx.getScrAddrForTxOut(txio.second.getIndexOfOutput());
      if (filter(scrAddr)) {
         result.addTxio(txioKey, txio.second, scrAddr);
      }
   }
   return result;
}

bool TxIOCache::txKeyIsValid(const BinaryData& txKey) const
{
   auto height = DBUtils::hgtxToHeight(txKey.getSliceRef(0, 4));
   auto dupId = DBUtils::hgtxToDupID(txKey.getSliceRef(0, 4));
   return dbCache_->isHeightValid(height, dupId);
}

////////
uint32_t TxIOCache::update(
   std::shared_ptr<AsyncClient::BlockDataViewer> bdvPtr,
   const NewBlockNotif& blockNotif)
{
   uint32_t fromHeight = 0;
   if (blockNotif.isReorg()) {
      fromHeight = blockNotif.getBranchHeight() + 1;
   } else if (blockNotif.isValid() && lastKnownBlock_ != UINT32_MAX) {
      fromHeight = lastKnownBlock_ + 1;
   }

   //1. txios
   auto promTxios = std::make_shared<std::promise<std::vector<TxIOPair>>>();
   auto futTxios = promTxios->get_future();

   bdvPtr->getTxios(fromHeight, [prom = promTxios]
      (ReturnMessage<std::vector<TxIOPair>> result) {
         prom->set_value(result.get());
   });
   auto txios = std::move(futTxios.get());

   auto fetchedHeight = blockNotif.isValid() ?
      blockNotif.getHeight() : UINT32_MAX;
   auto missingStuff = addTxios(txios, fetchedHeight);
   auto missingTxKeys = std::move(missingStuff.first);
   auto missingHeights = std::move(missingStuff.second);
   if (blockNotif.isReorg()) {
      missingHeights.clear();
      for (unsigned i = blockNotif.getBranchHeight() + 1;
         i <= blockNotif.getHeight(); i++) {
         missingHeights.emplace(i);
      }
   }

   //2. missing txs
   auto promTxs = std::make_shared<std::promise<std::vector<Tx>>>();
   auto futTxs = promTxs->get_future();
   bdvPtr->getTxsByKey(missingTxKeys, [prom = promTxs]
      (ReturnMessage<std::vector<Tx>> result) {
         prom->set_value(result.get());
   });

   //3. missing blocks
   auto promBlocks = std::make_shared<
      std::promise<std::vector<DBClientClasses::BlockHeader>>>();
   auto futBlocks = promBlocks->get_future();
   AsyncClient::Blockchain bc{*bdvPtr};
   bc.getHeadersByHeight(missingHeights, [prom = promBlocks]
      (ReturnMessage<std::vector<DBClientClasses::BlockHeader>> result) {
         prom->set_value(result.get());
      }
   );

   //4. commit it all to the cache
   auto txs = std::move(futTxs.get());
   auto blocks = std::move(futBlocks.get());

   ReentrantLock lock(this);
   for (auto& tx : txs) {
      dbCache_->txMap.emplace(tx.getDBKey(), std::move(tx));
   }
   dbCache_->addBlocks(blocks);
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
      if (dbCache_->blocks.find(height) == dbCache_->blocks.end()) {
         missingHeights.emplace(height);
      }
      if (dbCache_->txMap.find(txRef.getDBKey()) == dbCache_->txMap.end()) {
         missingTxKeys.emplace(txRef.getDBKey());
      }

      if (txio.hasTxIn()) {
         spentTxios_.emplace(txio.getDBKeyOfInput(), std::move(txio));
      } else {
         unspentTxios_.emplace(txio.getDBKeyOfOutput(), std::move(txio));
      }
   }

   if (fetchedHeight != UINT32_MAX) {
      lastKnownBlock_ = fetchedHeight;
   }
   return {std::move(missingTxKeys), std::move(missingHeights)};
}

////////
std::map<BinaryData, std::set<BinaryData>> TxIOCache::getAddressBook(
   const AddressFilter& filter) const
{
   std::map<BinaryData, std::set<BinaryData>> result;
   for (const auto& txio : unspentTxios_) {
      const auto& txKey = txio.second.getTxRefOfOutput().getDBKey();

      //skip txios that are not on the main chain
      if (!txKeyIsValid(txKey)) {
         continue;
      }

      //track this <addr, hash>
      const auto& tx = dbCache_->txMap.at(txKey);
      auto scrAddr = tx.getScrAddrForTxOut(txio.second.getIndexOfOutput());
      if (filter(scrAddr)) {
         auto iter = result.find(scrAddr);
         if (iter == result.end()) {
            iter = result.emplace(scrAddr, std::set<BinaryData>{}).first;
         }
         iter->second.emplace(tx.getThisHash());
      }
   }

   for (const auto& txio : spentTxios_) {
      //is output on mainchain?
      const auto& txKeyOutput = txio.second.getTxRefOfOutput().getDBKey();
      if (!txKeyIsValid(txKeyOutput)) {
         continue;
      }

      //is input on mainchain?
      const auto& txKeyInput = txio.second.getTxRefOfInput().getDBKey();
      if (!txKeyIsValid(txKeyInput)) {
         continue;
      }

      //track this <addr, hash>
      const auto& tx = dbCache_->txMap.at(txKeyOutput);
      auto scrAddr = tx.getScrAddrForTxOut(txio.second.getIndexOfOutput());
      if (filter(scrAddr)) {
         auto iter = result.find(scrAddr);
         if (iter == result.end()) {
            iter = result.emplace(scrAddr, std::set<BinaryData>{}).first;
         }
         iter->second.emplace(tx.getThisHash());
      }
   }
   return result;
}

std::vector<UTXO> TxIOCache::getUTXOs(const AddressFilter& filter) const
{
   std::set<BinaryData> spentKeys;
   for (const auto& txio : spentTxios_) {
      auto outputKey = txio.second.getDBKeyOfOutput();
      if (!txKeyIsValid(outputKey)) {
         continue;
      }
      spentKeys.emplace(outputKey);
   }

   std::vector<UTXO> result;
   result.reserve(unspentTxios_.size());
   for (const auto& txio : unspentTxios_) {
      auto iter = spentKeys.find(txio.first);
      if (iter != spentKeys.end()) {
         continue;
      }

      const auto& txKey = txio.second.getTxRefOfOutput().getDBKey();
      if (!txKeyIsValid(txKey)) {
         continue;
      }
      const auto& tx = dbCache_->txMap.at(txKey);
      if (tx.getTxIndex() == 0 &&
         tx.getTxHeight() + COINBASE_MATURITY > lastKnownBlock_) {
         continue;
      }
      auto scrAddr = tx.getScrAddrForTxOut(txio.second.getIndexOfOutput());
      if (!filter(scrAddr)) {
         continue;
      }

      auto txOut = tx.getTxOutCopy(txio.second.getIndexOfOutput());
      result.emplace_back(UTXO{txio.second.getValue(),
         tx.getTxHeight(), tx.getTxIndex(), txio.second.getIndexOfOutput(),
         tx.getThisHash(), txOut.getScript()
      });
   }
   return result;
}

////////////////////////////////////////////////////////////////////////////////
// CacheResolveResult
void CacheResolveResult::addTxio(const BinaryData& key, const TxIOPair& txio,
   const BinaryData& addr)
{
   auto iter = txioMap.emplace(key, txio).first;
   auto addrIter = addrTxioMap.find(addr);
   if (addrIter == addrTxioMap.end()) {
      addrIter = addrTxioMap.emplace(addr, std::vector<TxIOPair*>{}).first;
   }
   addrIter->second.emplace_back(&iter->second);
}
