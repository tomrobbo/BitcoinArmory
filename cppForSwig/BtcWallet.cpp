////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2011-2015, Armory Technologies, Inc.                        //
//  Distributed under the GNU Affero General Public License (AGPL v3)         //
//  See LICENSE-ATI or http://www.gnu.org/licenses/agpl.html                  //
//                                                                            //
//                                                                            //
//  Copyright (C) 2016-2026, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
#include <algorithm>

#include "BtcWallet.h"
#include <Utils/BtcUtils.h>
#include <Utils/DBUtils.h>
#include <Utils/TxOutScrRef.h>
#include <Utils/ArmoryConfig.h>
#include <BlockchainDatabase/lmdb_wrapper.h>
#include <BlockchainDatabase/BlockUtils.h>
#include <BlockchainDatabase/txio.h>
#include <Ledgers/LedgerEntry.h>
#include <Ledgers/Context.h>
#include <ZeroConf/Utils.h>
#include <ZeroConf/Parser.h>
#include "BlockDataViewer.h"

using namespace std;
using namespace Armory;

////////////////////////////////////////////////////////////////////////////////
// BtcWallet
BtcWallet::BtcWallet(BlockDataViewer* bdv, const std::string ID)
   : bdvPtr_(bdv), walletID_(ID), confTarget_{MIN_CONFIRMATIONS}
{}

BtcWallet::~BtcWallet()
{}

void BtcWallet::removeAddressBulk(vector<Types::ScrAddr> const & scrAddrBulk)
{
   scrAddrMap_.erase(scrAddrBulk);
   needsRefresh(true);
}

/////////////////////////////////////////////////////////////////////////////
bool BtcWallet::hasScrAddress(const Types::ScrAddr& scrAddr) const
{
   auto addrMap = scrAddrMap_.get();
   return (addrMap->find(scrAddr) != addrMap->end());
}

/////////////////////////////////////////////////////////////////////////////
std::set<BinaryDataRef> BtcWallet::getAddrSet() const
{
   auto addrMap = scrAddrMap_.get();
   std::set<BinaryDataRef> addrSet;

   for (auto& addrPair : *addrMap) {
      addrSet.emplace(addrPair.first);
   }
   return addrSet;
}

////////////////////////////////////////////////////////////////////////////////
void BtcWallet::clearBlkData()
{
}

////////////////////////////////////////////////////////////////////////////////
void BtcWallet::resetCounters()
{
}

////////////////////////////////////////////////////////////////////////////////
void BtcWallet::reset()
{
   clearBlkData();
}

////////////////////////////////////////////////////////////////////////////////
map<Types::TxIOKey, TxIOPairUint> BtcWallet::getTxioForRange(
   uint32_t start, uint32_t end) const
{
   auto db = bdvPtr_->getDB();
   map<Types::TxIOKey, TxIOPairUint> outMap;
   auto addrMap = scrAddrMap_.get();

   for (const auto& scrAddrPair : *addrMap) {
      auto saTxioMap = scrAddrPair.second->getTxios(db, start, end);
      outMap.insert(saTxioMap.begin(), saTxioMap.end());
   }
   return outMap;
}

////////////////////////////////////////////////////////////////////////////////
const ScrAddrObj* BtcWallet::getScrAddrObjByKey(const BinaryData& key) const
{
   auto addrMap = scrAddrMap_.get();
   auto saIter = addrMap->find(key);
   if (saIter == addrMap->end()) {
      LOGWARN << "unknown address in btcwallet";
      throw std::runtime_error("unknown address in btcwallet");
   }
   return saIter->second.get();
}

////////////////////////////////////////////////////////////////////////////////
void BtcWallet::needsRefresh(bool refresh)
{
   //TODO: fix the flagRefresh logic

   //notify BDV
   /*if (refresh && isRegistered_)
   {
      bdvPtr_->flagRefresh(
         BDV_refreshAndRescan, BinaryData::fromString(walletID_), nullptr);
   }*/

   //call custom callback
   doneRegisteringCallback_();
   doneRegisteringCallback_ = [](void){};
}

////////////////////////////////////////////////////////////////////////////////
void BtcWallet::setConfTarget(unsigned confTarget)
{
   if(confTarget != confTarget_) {
      confTarget_ = confTarget;
   }
   BinaryData wltId(walletID_.data(), walletID_.size());
}

////////////////////////////////////////////////////////////////////////////////
void BtcWallet::unregisterAddresses(const std::set<BinaryDataRef>& addrSet)
{
   vector<Types::ScrAddr> addrVec;
   addrVec.reserve(addrSet.size());
   for (const auto& addrRef : addrSet) {
      addrVec.emplace_back(addrRef);
   }
   scrAddrMap_.erase(addrVec);
}

std::shared_ptr<const std::map<Types::ScrAddr, std::shared_ptr<ScrAddrObj>>>
BtcWallet::getAddrMap() const
{
   return scrAddrMap_.get();
}
