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

#include <stdint.h>
#include <string>

#include <Utils/Types.h>
#include <Ledgers/HistoryPager.h>
#include "BDV_Notification.h"
#include "bdmenums.h"

typedef enum
{
   order_ascending,
   order_descending
} HistoryOrdering;

enum class WalletRegType : int
{
   UNSET    = 0,
   WALLET   = 1,
   LOCKBOX  = 2
};

struct WalletRegistrationRequest
{
   std::string walletId;
   std::vector<Armory::Types::ScrAddr> addresses;
   bool isNew;
   WalletRegType type;

   WalletRegistrationRequest(const std::string& wId,
      std::vector<Armory::Types::ScrAddr>& addrs,
      bool isnew, WalletRegType wType) :
      walletId(wId), addresses(std::move(addrs)),
      isNew(isnew), type(wType)
   {}
};

class ScrAddrFilter;
class BtcWallet;
class BlockDataManager;
struct StoredHeader;
class Tx;
class TxOut;
class TxIn;
struct StoredTxOut;
struct ScanWalletStruct;
struct UTXO;
struct Output;

namespace Armory
{
   namespace ZeroConf
   {
      class ZeroConfContainer;
   }

   namespace Ledgers
   {
      class Delegate;
      class HistoryPager;
   }

   class BlockHeader;
   class Blockchain;
}

////////////////////////////////////////////////////////////////////////////////
class ReadWriteLock
{
   /***
   You have to make sure a read lock request from a thread already holding
   a read lock ignores write lock requests, otherwise you could end up in
   a deadlock where a write lock is requested before a child read lock is.

   Example:
   T1 creates a read lock. T2 requests a write lock. At this point no new
   read locks can be created, until the write lock is fulfilled. The
   write lock can only be fulfilled if all current read locks are released.

   Within T1's first read lock, a new read lock is requested. This new lock
   will never be acquired, waiting forever on T2's write lock to be
   fulfilled first.

   T2's write lock will never be fulfilled, as it is waiting on T1's
   currently held read lock to be released. T1's current lock won't be
   released as it will never exist its scope, waiting for T1's second
   lock to be acquired.

   Incidentally, in the scope of a same thread, requesting a write lock
   within a read lock will always deadlock. The condition should be tested
   and thrown. Requesting a read lock within a write lock can and should
   be accomodated.
   ***/

   std::mutex all_lock;
   unsigned num_readers = 0;
   bool has_writer=false;
   std::condition_variable no_readers, no_writers;
   std::map<std::thread::id, unsigned> thread_ids_;

public:
   void lockRead(void);
   void unlockRead(void);
   void lockWrite(void);
   void unlockWrite(void);

   class ReadLock
   {
      ReadWriteLock *const l;
      bool locked=true;

   public:
      ReadLock(ReadWriteLock&);
      ~ReadLock(void);

      void unlock(void);
   };

   class WriteLock
   {
      ReadWriteLock *const l;
      bool locked=true;

   public:
      WriteLock(ReadWriteLock&);
      ~WriteLock(void);

      void unlock(void);
   };
};

////////////////////////////////////////////////////////////////////////////////
class BlockDataViewer;
struct WalletGroup
{
   std::map<std::string, std::shared_ptr<BtcWallet>> wallets;
   mutable ReadWriteLock lock;

   //The globalLedger (used to render the main transaction ledger) is
   //different from wallet ledgers. While each wallet only has a single
   //entry per transactions (wallets merge all of their scrAddr txn into
   //a single one), the globalLedger does not merge wallet level txn. It
   //can thus have several entries under the same transaction. Thus, this
   //cannot be a map nor a set.
   Armory::Ledgers::HistoryPager hist;
   HistoryOrdering order = order_descending;

   BlockDataViewer* bdvPtr = nullptr;
   std::shared_ptr<ScrAddrFilter> saf;

public:
   WalletGroup(BlockDataViewer*, std::shared_ptr<ScrAddrFilter>);
   ~WalletGroup(void);

   std::shared_ptr<BtcWallet> getOrSetWallet(const std::string&);
   void registerAddresses(WalletRegistrationRequest&,
      const std::function<void(bool)>&);
   bool unregisterWallet(const std::string&);

   bool hasID(const std::string&) const;
   std::shared_ptr<BtcWallet> getWalletByID(const std::string&) const;

   void reset(void);
   std::map<Armory::Types::TxIOKey, TxIOPairUint> getTxioForRange(
      uint32_t, uint32_t) const;

   uint32_t getBlockInVicinity(uint32_t) const;
   uint32_t getPageIdForBlockHeight(uint32_t) const;
};

////////////////////////////////////////////////////////////////////////////////
class BlockDataViewer
{
public:
   BlockDataViewer(std::shared_ptr<BlockDataManager>);
   ~BlockDataViewer(void);
   void reset(void);

   /////////////////////////////////////////////////////////////////////////////
   // If you register you wallet with the BDM, it will automatically maintain
   // tx lists relevant to that wallet.  You can get away without registering
   // your wallet objects (using scanBlockchainForTx), but without the full
   // blockchain in RAM, each scan will take 30-120 seconds.  Registering makes
   // sure that the intial blockchain scan picks up wallet-relevant stuff as
   // it goes, and does a full [re-]scan of the blockchain only if necessary.
   void registerAWallet(WalletRegistrationRequest&,
      const std::function<void(bool)>&);
   void unregisterWallet(const std::string&);

   bool hasWallet(const std::string&) const;
   std::shared_ptr<BtcWallet> getWalletOrLockbox(const std::string&) const;

   bool scrAddressIsRegistered(const Armory::Types::ScrAddr&) const;
   bool hasScrAddress(const Armory::Types::ScrAddr&) const;
   std::set<Armory::Types::ScrAddr> getAddrSet(void) const;

   LMDBBlockDatabase* getDB(void) const;
   std::shared_ptr<BlockDataManager> bdm(void) const;
   Armory::ZeroConf::ZeroConfContainer* zcContainer(void) const;
   const Armory::Blockchain& blockchain(void) const;
   std::shared_ptr<ScrAddrFilter> getSAF(void) const;

   bool isBDMRunning(void) const;
   void blockUntilBDMisReady(void) const;

   bool isZcEnabled(void) const;
   void flagRescanZC(bool);
   bool getZCflag(void) const;

   //txios
   std::map<Armory::Types::TxIOKey, TxIOPairUint>
   getTxioForRange(uint32_t) const;
   std::map<Armory::Types::TxIOKey, std::shared_ptr<const TxIOPairUint>>
   getZcTxios(void) const;

protected:
   static void unregisterAddresses(
      std::set<BinaryData>, const std::function<void(void)>&);

protected:
   std::atomic<bool> rescanZC_;

   std::shared_ptr<BlockDataManager> bdm_;
   LMDBBlockDatabase* db_;
   std::shared_ptr<Armory::Blockchain> bc_;
   std::shared_ptr<ScrAddrFilter> saf_;

   uint32_t lastScanned_ = 0;
   const std::shared_ptr<Armory::ZeroConf::ZeroConfContainer> zeroConfCont_;
   int32_t updateID_ = 0;

   WalletGroup wallets_;
   WalletGroup lockboxes_;
};
