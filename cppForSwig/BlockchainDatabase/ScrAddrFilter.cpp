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

#include <thread>
#include <cstring>

#include "ScrAddrFilter.h"
#include <Utils/BtcUtils.h>
#include <Utils/TxOutScrRef.h>
#include <Utils/DBUtils.h>
#include <Utils/ArmoryConfig.h>
#include <Utils/BitcoinSettings.h>

#include "lmdb_wrapper.h"
#include "Blockchain.h"
#include "BlockUtils.h"
#include "StoredBlockObj.h"
#include "txio.h"

using namespace Armory;
namespace {
   static constexpr uint16_t SIDESCAN_ID = 0x00FF;
}

///////////////////////////////////////////////////////////////////////////////
// ScrAddrFilter
ScrAddrFilter::ScrAddrFilter(LMDBBlockDatabase* lmdb, uint16_t sdbiKey)
   : sdbiKey_(0xFFFF - sdbiKey), lmdb_(lmdb)
{
   scanFilterAddrMap_ = std::make_shared<Threading::TransactionalMap<
      BinaryData, std::shared_ptr<AddrAndHash>>>();

   //seed SDBI if necessary
   try {
      getSDBI();
   } catch (const LmdbWrapperException&) {
      updateAddressMerkle();
   }
}

ScrAddrFilter::~ScrAddrFilter()
{
   shutdown();
}

bool ScrAddrFilter::empty() const
{
   return scanFilterAddrMap_->empty();
}

void ScrAddrFilter::start()
{
   LOGINFO << "loading known addresses";

   /* grab all scrAddr ids from db */
   {
      auto tx = lmdb_->beginTransaction(
         DB_SELECT::SCRADDR, LMDB::Mode::ReadOnly);
      auto dbIter = lmdb_->getIterator(DB_SELECT::SCRADDR);
      dbIter->seekToFirst();
      std::map<BinaryData, std::shared_ptr<AddrAndHash>> scrAddrMap;

      //iterate over scraddr DB
      topScrAddrID_ = 0;
      do {
         auto keyRef = dbIter->getKeyRef();
         if (keyRef.getSize() != sizeof(uint32_t)) {
            continue;
         }
         uint32_t scrAddrId;
         std::memcpy(&scrAddrId, keyRef.getPtr(), sizeof(uint32_t));
         if (scrAddrId >= 0xFFFF0000) {
            //sdbi entry, ignore
            continue;
         }
         auto scrAddrRef = dbIter->getValueRef();

         auto aah = std::make_shared<AddrAndHash>(scrAddrRef, scrAddrId);
         scrAddrMap.emplace(aah->scrAddr, aah);
         topScrAddrID_ = std::max(topScrAddrID_, scrAddrId);
      } while (dbIter->advanceAndRead());

      //update members
      scanFilterAddrMap_->update(scrAddrMap);
      ++topScrAddrID_;
   }
   merkleRoot_ = computeMerkleRoot();
   LOGINFO << "found " << scanFilterAddrMap_->size() << " known addresses";

   /* if bdm isn't ready, exhaust addr registration queue */
   if (bdmIsRunning() == false) {
      auto regQueue = registrationStack_.pop_all();
      for (auto batch : regQueue) {
         auto batchPtr = std::dynamic_pointer_cast<RegistrationBatch>(batch);
         if (batchPtr == nullptr) {
            //ignore unregistration batches at init time
            continue;
         }
         auto result = prepareRegistrationBatch(batchPtr);
         if (!result.empty()) {
            throw std::runtime_error(
               "no registration batch should lead to a scan at SCA start!");
         }
      }
   }

   /* start operation thread */
   thr_ = std::thread([this]{ run(); });
}

void ScrAddrFilter::shutdown()
{
   registrationStack_.terminate();
   if (thr_.joinable()) {
      thr_.join();
   }
}

////////
ScrAddrFilter::AddrMap ScrAddrFilter::prepareRegistrationBatch(
   std::shared_ptr<RegistrationBatch> batch)
{
   if (Config::DBSettings::getDbType() == ARMORY_DB_TYPE::Super) {
      //this class currently serves no purpose in supernode
      batch->callback({}, true);
      return {};
   }

   //filter out collisions, assign IDs for fresh addresses
   auto newScrAddrMap = assignScrAddrKeys(batch->scrAddrSet);
   if (newScrAddrMap.empty()) {
      //all addresses are already registered
      batch->callback({}, true);
      return {};
   }

   if (batch->isNew) {
      //batch is flagged as new, all addresses within it are assumed
      //clean of history. Update the map and continue
      auto scaSet = mergeAddresses(std::move(newScrAddrMap), true);
      batch->callback(scaSet, true);
      return {};
   } else if (!bdmIsRunning()) {
      //BDM isn't running, merge address set but do not update addr
      //merkle root. This will trigger a rescan of all addresses once
      //BDM is up.
      auto scaSet = mergeAddresses(std::move(newScrAddrMap), false);
      batch->callback(scaSet, true);
      return {};
   }
   return newScrAddrMap;
}

////////
std::shared_ptr<const ScrAddrFilter::AddrMap>
ScrAddrFilter::getScanFilterAddrMap() const
{
   return scanFilterAddrMap_->get();
}

////////
ScrAddrFilter::AddrMap ScrAddrFilter::assignScrAddrKeys(
   const std::set<BinaryData>& addrSet)
{
   AddrMap result;
   auto scraddrmap = scanFilterAddrMap_->get();
   for (const auto& sa : addrSet) {
      auto iter = scraddrmap->find(sa);
      if (iter != scraddrmap->end()) {
         continue;
      }
      result.emplace(sa, std::make_shared<AddrAndHash>(sa, topScrAddrID_++));
   }
   if (result.empty()) {
      return {};
   }

   //commit fresh <db, addr> pairs to db
   auto tx = lmdb_->beginTransaction(DB_SELECT::SCRADDR, LMDB::Mode::ReadWrite);
   for (const auto& aaPair : result) {
      BinaryDataRef keyRef{(const uint8_t*)&aaPair.second->id, sizeof(uint32_t)};
      lmdb_->putValue(DB_SELECT::SCRADDR,
         keyRef,
         aaPair.first.getRef()
      );
   }
   return result;
}

std::set<BinaryDataRef> ScrAddrFilter::mergeAddresses(ScrAddrFilter::AddrMap addrMap,
   bool updateMerkleRoot)
{
   std::set<BinaryDataRef> result;
   for (const auto& aaPair : addrMap) {
      result.emplace(aaPair.second->scrAddr.getRef());
   }

   scanFilterAddrMap_->update(std::move(addrMap));
   if (!updateMerkleRoot) {
      /* edge case:
         if any of the merged batches skipped the merkle root update, addrMap
         has to be treated as fresh
      */
      if (merkleRoot_.valid()) {
         merkleRoot_  = Hash32{};
      }
   } else if (merkleRoot_.valid()) {
      merkleRoot_ = computeMerkleRoot();
      updateAddressMerkle();
   }
   return result;
}

Hash32 ScrAddrFilter::scanFrom() const
{
   auto sdbi = getSDBI();
   if (merkleRoot_ == sdbi.metaHash) {
      return sdbi.topScannedBlkHash;
   } else {
      return Hash32{};
   }
}

////////
Hash32 ScrAddrFilter::computeMerkleRoot() const
{
   std::vector<BinaryData> addrVec;
   addrVec.reserve(scanFilterAddrMap_->size());

   auto scraddrmap = scanFilterAddrMap_->get();
   for (const auto& addr : *scraddrmap) {
      addrVec.emplace_back(addr.second->getHash());
   }

   if (!addrVec.empty()) {
      Hash32 result;
      auto merkle = BtcUtils::calculateMerkleRoot(addrVec);
      std::memcpy(result.data, merkle.getPtr(), 32);
      return result;
   }
   return Hash32{};
}

void ScrAddrFilter::updateAddressMerkle()
{
   auto tx = lmdb_->beginTransaction(DB_SELECT::SCRADDR, LMDB::Mode::ReadWrite);
   StoredDBInfo sdbi;
   try {
      sdbi = std::move(lmdb_->getStoredDBInfo(DB_SELECT::SCRADDR, sdbiKey_));
   } catch (const LmdbWrapperException&) {
      sdbi.magicBytes = Config::BitcoinSettings::getMagicBytes();
      sdbi.armoryType = Config::DBSettings::getDbType();
   }
   sdbi.metaHash = merkleRoot_;
   lmdb_->putStoredDBInfo(DB_SELECT::SCRADDR, sdbi, sdbiKey_);
}

void ScrAddrFilter::updateScannedHash(const Hash32& hash)
{
   auto tx = lmdb_->beginTransaction(DB_SELECT::SCRADDR, LMDB::Mode::ReadWrite);
   auto sdbi = getSDBI();
   sdbi.topScannedBlkHash = hash;
   sdbi.metaHash = merkleRoot_;
   lmdb_->putStoredDBInfo(DB_SELECT::SCRADDR, sdbi, sdbiKey_);
}

////////
StoredDBInfo ScrAddrFilter::getSDBI() const
{
   auto tx = lmdb_->beginTransaction(DB_SELECT::SCRADDR, LMDB::Mode::ReadOnly);
   return lmdb_->getStoredDBInfo(DB_SELECT::SCRADDR, sdbiKey_);
}

////////
std::set<BinaryData> ScrAddrFilter::getMissingHashes() const
{
   return lmdb_->getMissingHashes(sdbiKey_);
}

void ScrAddrFilter::putMissingHashes(const std::set<BinaryData>& hashSet)
{
   auto tx = lmdb_->beginTransaction(DB_SELECT::TXFILTERS, LMDB::Mode::ReadWrite);
   lmdb_->putMissingHashes(hashSet, sdbiKey_);
}

////////
void ScrAddrFilter::pushAddressBatch(std::shared_ptr<AddressBatch> batch)
{
   registrationStack_.push_back(std::move(batch));
}

void ScrAddrFilter::run()
{
   while (true) {
      std::shared_ptr<AddressBatch> batch;
      try {
         batch = std::move(registrationStack_.pop_front());
      } catch (const Threading::StopBlockingLoop&) {
         //end loop condition
         break;
      }

      switch (batch->type)
      {
         case AddressBatchType::Register:
         {
            auto batchPtr = std::dynamic_pointer_cast<RegistrationBatch>(batch);
            if (batchPtr == nullptr) {
               throw std::runtime_error("unexpected batch ptr type");
            }

            //prepare the batch, it will also wrap up registration for edge case.
            //if the returned addr map is empty, there's nothing else to do.
            auto newScrAddrMap = prepareRegistrationBatch(batchPtr);
            if (newScrAddrMap.empty()) {
               return;
            }

            /* BDM is initialized and maintenance thread is running, scan the batch */
            LOGINFO << "Starting address registration process";

            //scan the batch
            std::vector<std::string> walletIDs;
            if (!batchPtr->walletID.empty()) {
               walletIDs.emplace_back(batchPtr->walletID);
            }
            auto saf = getNew(SIDESCAN_ID);
            saf->mergeAddresses(newScrAddrMap, false);
            auto topHeader = blockchain()->top();
            auto topBlockHeight = topHeader->getBlockHeight();
            auto scanResult = saf->applyBlockRangeToDB(0, walletIDs, true);

            //merge with main address filter
            auto scaSet = mergeAddresses(std::move(newScrAddrMap), true);

            //cleanup side scan context
            saf->cleanUpSdbis();

            //was the scan successful?
            if (scanResult == false) {
               //no, fire callback and exit thread
               batchPtr->callback({}, false);
               return;
            }

            //final scan to sync all addresses to same height
            applyBlockRangeToDB(topBlockHeight + 1, walletIDs, false);

            //notify
            for (const auto& wID : walletIDs) {
               LOGINFO << "Completed scan of wallet " << wID;
            }
            batchPtr->callback(scaSet, true);
            break;
         }

         case AddressBatchType::Unregister:
         {
            auto batchPtr = std::dynamic_pointer_cast<UnregistrationBatch>(batch);
            if (batchPtr == nullptr) {
               throw std::runtime_error("unexpected batch ptr type");
            }

            /*
            NOTE: is there actually anything to do here?

            std::set<BinaryData> scrAddrSet;
            scrAddrSet.insert(
               batchPtr->scrAddrSet_.begin(), batchPtr->scrAddrSet_.end());
            updateAddrMap(scrAddrSet, 0, true);
            */
            if (batchPtr->callback) {
               batchPtr->callback();
            }
            break;
         }
      }
   }
}

////////
ScrAddrIdMap ScrAddrFilter::getScrAddrIds() const
{
   ScrAddrIdMap result;
   auto scrAddrMap = scanFilterAddrMap_->get();
   for (auto& scrAddr : *scrAddrMap) {
      if (scrAddr.first.empty()) {
         continue;
      }
      result.emplace(scrAddr.first, scrAddr.second->id);
   }
   return result;
}

////////
void ScrAddrFilter::cleanUpSdbis()
{
   //SSH
   {
      auto tx = lmdb_->beginTransaction(DB_SELECT::SSH, LMDB::Mode::ReadWrite);
      lmdb_->deleteValue(DB_SELECT::SSH, StoredDBInfo::getDBKey(sdbiKey_));
   }

   //SUBSSH
   {
      auto tx = lmdb_->beginTransaction(DB_SELECT::SUBSSH, LMDB::Mode::ReadWrite);
      lmdb_->deleteValue(DB_SELECT::SUBSSH, StoredDBInfo::getDBKey(sdbiKey_));
   }

   //TXFILTERS
   {
      auto tx = lmdb_->beginTransaction(
         DB_SELECT::TXFILTERS, LMDB::Mode::ReadWrite);
      lmdb_->deleteValue(
         DB_SELECT::TXFILTERS, DBUtils::getMissingHashesKey(sdbiKey_));
   }
}

////////
void ScrAddrFilter::unregisterAddresses(
   const std::set<BinaryDataRef>& scrAddrSet,
   const std::function<void(void)>& callback)
{
   /*
   Remove addresses from the ScrAddrFilter zcFilter map
   */

   auto batch = std::make_shared<UnregistrationBatch>();
   batch->scrAddrSet.insert(scrAddrSet.begin(), scrAddrSet.end());
   batch->callback = callback;
   pushAddressBatch(std::move(batch));
}

std::shared_ptr<Threading::TransactionalMap<
   BinaryData, std::shared_ptr<AddrAndHash>>>
ScrAddrFilter::getZcFilterMapPtr() const
{
   return scanFilterAddrMap_;
}

///////////////////////////////////////////////////////////////////////////////
// AddressBatch
AddressBatch::AddressBatch(AddressBatchType t) :
   type(t)
{}

AddressBatch::~AddressBatch()
{}

////////
RegistrationBatch::RegistrationBatch(std::set<BinaryData> addrSet,
   bool isnew, const RegistrationBatch::Callback& cb) :
   AddressBatch(AddressBatchType::Register),
   scrAddrSet{std::move(addrSet)}, isNew(isnew), callback(cb)
{}

UnregistrationBatch::UnregistrationBatch() :
   AddressBatch(AddressBatchType::Unregister)
{}

///////////////////////////////////////////////////////////////////////////////
// AddrAndHash
AddrAndHash::AddrAndHash(BinaryDataRef addrRef, uint32_t scrAddrId) :
   scrAddr(addrRef), id(scrAddrId)
{}

const BinaryData& AddrAndHash::getHash() const
{
   if (addrHash_.empty()) {
      addrHash_ = std::move(BtcUtils::getHash256(scrAddr));
   }
   return addrHash_;
}

bool AddrAndHash::operator<(const AddrAndHash& rhs) const
{
   return this->scrAddr < rhs.scrAddr;
}

bool AddrAndHash::operator<(const BinaryDataRef& rhs) const
{
   return this->scrAddr.getRef() < rhs;
}
