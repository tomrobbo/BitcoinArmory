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

#pragma once

#include <Utils/BinaryData.h>
#include <Utils/ThreadSafeClasses.h>
#include <BlockchainDatabase/BlockObj.h>
#include <BlockchainDatabase/StoredBlockObj.h>
#include <Ledgers/LedgerEntry.h>
#include <Ledgers/HistoryPager.h>
#include "ScrAddrObj.h"
#include "bdmenums.h"
#include "TxClasses.h"

class BlockDataViewer;

struct ScanWalletStruct
{
   BDV_Action action_;

   unsigned prevTopBlockHeight_;
   unsigned startBlock_;
   unsigned endBlock_ = UINT32_MAX;
   bool reorg_ = false;
};

////////////////////////////////////////////////////////////////////////////////
//
// BtcWallet
//
////////////////////////////////////////////////////////////////////////////////

class BtcWallet
{
   friend class BlockDataViewer;

private:
   BtcWallet(const BtcWallet&) = delete;

public:
   BtcWallet(BlockDataViewer*, const std::string);
   ~BtcWallet(void);

   /////////////////////////////////////////////////////////////////////////////
   // addScrAddr when blockchain rescan req'd, addNewScrAddr for just-created
   void removeAddressBulk(const std::vector<Armory::Types::ScrAddr>&);
   bool hasScrAddress(const Armory::Types::ScrAddr&) const;
   std::set<BinaryDataRef> getAddrSet(void) const;

   void clearBlkData(void);
   void reset(void);
   const ScrAddrObj* getScrAddrObjByKey(const BinaryData& key) const;
   const std::string& walletID() const { return walletID_; }

   void needsRefresh(bool refresh);
   void setConfTarget(unsigned);

   std::shared_ptr<const std::map<
   Armory::Types::ScrAddr, std::shared_ptr<ScrAddrObj>>>
      getAddrMap(void) const;
   void unregisterAddresses(const std::set<BinaryDataRef>&);

private:
   void setRegistered(bool isTrue = true) { isRegistered_ = isTrue; }

   std::map<Armory::Types::TxIOKey, TxIOPairUint> getTxioForRange(
      uint32_t, uint32_t) const;
   void unregister(void) { isRegistered_ = false; }
   void resetCounters(void);

private:
   const std::string walletID_;

   BlockDataViewer* const bdvPtr_;
   Armory::Threading::TransactionalMap<
      Armory::Types::ScrAddr, std::shared_ptr<ScrAddrObj>> scrAddrMap_;

   bool isRegistered_ = false;


   //call this lambda once a wallet is done registering and scanning
   //for the first time
   std::function<void(void)> doneRegisteringCallback_{};
   unsigned confTarget_;
};
