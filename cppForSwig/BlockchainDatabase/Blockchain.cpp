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

#include <cstring>
#include <unordered_set>

#include "Blockchain.h"
#include <Utils/BtcUtils.h>
#include <Utils/UniversalTimer.h>
#include <Utils/DBUtils.h>

#include "StoredBlockObj.h"
#include "BlockDataMap.h"
#include "lmdb_wrapper.h"

using namespace Armory;

namespace {
   //TOPID in hex
   const BinaryData topIdKey = READHEX("544f504944");

   //helper containers used in Blockchain::traceDownChain
   using Hash32USet = std::unordered_set<Hash32, Hash32::Hasher, Hash32::Comparator>;
   std::map<Hash32, Hash32USet> orphans;
}

////////////////////////////////////////////////////////////////////////////////
// HeightAndDup
HeightAndDup::HeightAndDup(unsigned height, uint8_t dup, bool isMain) :
   height(height), dup(dup), isMain(isMain)
{}

////////////////////////////////////////////////////////////////////////////////
// Blockchain
Blockchain::Blockchain(const BinaryData &genesisHash)
   : genesisHash_(genesisHash)
{
   newlyParsedBlocks_.clear();
   headersByHeight_.clear();
   headersById_.clear();
   headerSet_.clear();

   Hash32 genHash{genesisHash_};
   Hash32 emptyPrev{};
   Hash32 emptyMerkle{};
   auto genesisBlock = std::shared_ptr<BlockHeader>(new BlockHeader(
      genHash, emptyPrev, emptyMerkle, 1, 0, 0));
   genesisBlock->setUniqueID(1);
   auto emplaceIter = headerSet_.emplace(genesisBlock);
   topBlockPtr_.store(*emplaceIter.first);

   //block ids start at 1
   idOfTopBlock_ = 1;
   highestBlockID_.store(2, std::memory_order_relaxed);
}

////////
HeaderPtr Blockchain::top() const
{
   auto ptr = std::atomic_load(&topBlockPtr_);
   return ptr;
}

HeaderPtr Blockchain::getGenesisBlock() const
{
   //NOTE: caller is responsible for holding the lock
   auto iter = headerSet_.find(genesisHash_);
   if (iter == headerSet_.end()) {
      throw std::runtime_error("missing genesis block header");
   }
   return *iter;
}

////////
const HeaderPtr Blockchain::getHeaderByHeight(
   unsigned index, uint8_t dupId) const
{
   /*
   Returns header for height.
   Passing a dupId above 0x7F will return the main chain header for this height.
   Passing a dupId for a forked block will throw.
   */

   std::unique_lock<std::mutex> lock(mu_);
   if (index >= headersByHeight_.size()) {
      throw std::range_error(
         "Cannot get block at height " + std::to_string(index)
      );
   }

   auto header = headersByHeight_[index];
   if (dupId > 0x7F || header->getDuplicateID() == dupId) {
      return header;
   } else {
      //if we get this far, we're looking for a block that isn't on the main chain
      throw std::length_error(
         "Cannot get block at height " + std::to_string(index) +
         " and dup " + std::to_string(dupId));
   }
}

HeaderPtr Blockchain::getHeaderByHash(const BinaryData& blkHash) const
{
   std::unique_lock<std::mutex> lock(mu_);
   auto iter = headerSet_.find(blkHash);
   if (iter == headerSet_.end()) {
      throw std::range_error(
         "Cannot find block with hash " + blkHash.toHexStr(true));
   }
   return *iter;
}

HeaderPtr Blockchain::getHeaderByHash(BinaryDataRef blkHash) const
{
   std::unique_lock<std::mutex> lock(mu_);
   auto iter = headerSet_.find(blkHash);
   if (iter == headerSet_.end()) {
      throw std::range_error(
         "Cannot find block with hash " + blkHash.toHexStr(true));
   }
   return *iter;
}

HeaderPtr Blockchain::getHeaderByHash(const Hash32& blkHash) const
{
   std::unique_lock<std::mutex> lock(mu_);
   auto iter = headerSet_.find(blkHash);
   if (iter == headerSet_.end()) {
      throw std::range_error(
         "Cannot find block with hash " + blkHash.toHexStr(true));
   }
   return *iter;
}

HeaderPtr Blockchain::getHeaderById(uint32_t id) const
{
   std::unique_lock<std::mutex> lock(mu_);
   if (id > highestBlockID_.load(std::memory_order_relaxed)) {
      LOGERR << "block id " << id << " is too big";
      throw std::range_error("block id overflow");
   }
   auto header = headersById_[id];
   if (header == nullptr) {
      LOGERR << "cannot find block for id: " << id;
      throw std::range_error("Cannot find block by id");
   }
   return header;
}

HeaderPtr Blockchain::getHeaderForTxKey(
   const BinaryData& txKey) const
{
   unsigned blockId;
   uint8_t dup;
   BinaryRefReader brrKey(txKey);
   DBUtils::readBlkDataKeyNoPrefix(brrKey, blockId, dup);
   if (dup == 0x7F) {
      return getHeaderById(blockId);
   } else {
      return getHeaderByHeight(blockId, dup);
   }
}

////////
ReorganizationState Blockchain::organize(bool force, bool verbose)
{
   ReorganizationState st;
   st.prevTop = top();
   st.reorgBranchPoint = organizeChain(force, verbose);
   st.prevTopStillValid = (st.reorgBranchPoint == nullptr);
   st.hasNewTop = (st.prevTop != top());
   st.newTop = top();
   return st;
}

// Returns nullptr if the new top block is a direct follower of
// the previous top. Returns the branch point if we had to reorg
// TODO: Figure out if there is an elegant way to deal with a forked
//   blockchain containing two equal-length chains
HeaderPtr Blockchain::organizeChain(
   bool forceRebuild, bool verbose)
{
   std::unique_lock<std::mutex> lock(mu_);
   if (forceRebuildFlag_) {
      forceRebuild = true;
      forceRebuildFlag_ = false;
      LOGINFO << "chain was flagged for a forced rebuild";
   }

   if (verbose) {
      TIMER_START("orgChain");
      LOGINFO << "Organizing chain " << (forceRebuild ? "w/ rebuild" : "");
   }

   // If rebuild, we zero out any original organization data and do a
   // rebuild of the chain from scratch.  This will need to be done in
   // the event that our first call to organizeChain returns false, which
   // means part of blockchain that was previously valid, has become
   // invalid.  Rather than get fancy, just rebuild all which takes less
   // than a second, anyway.

   if (forceRebuild) {
      for (auto& header : headerSet_) {
         header->difficultySum_  = -1;
         header->blockHeight_ = UINT32_MAX;
         header->isFinishedCalc_ = false;
         header->nextHash_ = nullptr;
         header->isMainBranch_ = false;
      }
      topBlockPtr_.store(nullptr);
   }

   // If this is the first run, the topBlock is the genesis block
   if (topBlockPtr_.load() == nullptr) {
      auto genBlock = getGenesisBlock();
      genBlock->blockHeight_ = 0;
      genBlock->difficultySum_ = 1.0;
      genBlock->isMainBranch_ = true;
      genBlock->isOrphan_ = false;
      genBlock->isFinishedCalc_ = true;
      topBlockPtr_.store(genBlock);
   }
   const auto prevTopBlock = top();
   auto newTopBlock = topBlockPtr_.load();
   double maxDiffSum = prevTopBlock->getDifficultySum();

   //prepare helper containers
   orphans.clear();

   // Iterate over all blocks, track the maximum difficulty-sum block
   for (auto& header : headerSet_) {
      // *** Walk down the chain following prevHash fields, until
      //     you find a "solved" block. Then walk back up and
      //     fill in the difficulty-sum values (do not set next-
      //     hash ptrs, as we don't know if this is the main branch)
      //     Method returns instantly if block is already "solved"
      if (header->difficultySum_ > 0.0) {
         continue;
      }

      double thisDiffSum = traceChainDown(header);
      if (header->isOrphan_) {
         // disregard this block
         continue;
      } else if (thisDiffSum > maxDiffSum) {
         // Determine if this is the top block.  If it's the same diffsum
         // as the prev top block, don't do anything
         maxDiffSum  = thisDiffSum;
         newTopBlock = header;
      }
   }

   //report long orphaned chains
   for (const auto& orphanChain : orphans) {
      if (orphanChain.second.size() >= 144) {
         auto headerIter = headerSet_.find(orphanChain.first);
         if (headerIter == headerSet_.end()) {
            LOGERR << "Could not find first orphan by hash! This is a fatal error!";
            throw std::runtime_error("could not find orphan");
         }

         auto headerPtr = *headerIter;
         LOGWARN << "Found a long orphan chain!";
         LOGWARN << "  file: " << headerPtr->getBlockFileNum();
         LOGWARN << "  first header hash  : " << headerPtr->getThisHash().toHexStr(true);
         LOGWARN << "  missing header hash: " << headerPtr->getPrevHash().toHexStr(true);
         LOGWARN << "  orphan chain length: " << orphanChain.second.size();
      }

      //reset finishedCalc flag on all orphans
      for (const auto& headerHash : orphanChain.second) {
         auto headerIter = headerSet_.find(orphanChain.first);
         if (headerIter == headerSet_.end()) {
            LOGERR << "Could not find an orphan by hash! This is a fatal error!";
            throw std::runtime_error("could not find orphan");
         }
         (*headerIter)->isFinishedCalc_ = false;
      }
   }

   // Walk down the list one more time, set nextHash fields
   // Also set headersByHeight_;
   bool prevChainStillValid = (newTopBlock == prevTopBlock);
   newTopBlock->nextHash_ = nullptr;
   auto thisHeaderPtr = newTopBlock;
   if (headersByHeight_.size() <= newTopBlock->getBlockHeight()) {
      if (headersByHeight_.capacity() <= newTopBlock->getBlockHeight()) {
         headersByHeight_.reserve(newTopBlock->getBlockHeight() + 100);
      }
      headersByHeight_.resize(newTopBlock->getBlockHeight() + 1);
   }

   while (!thisHeaderPtr->isFinishedCalc_) {
      thisHeaderPtr->isFinishedCalc_ = true;
      thisHeaderPtr->isMainBranch_   = true;
      thisHeaderPtr->isOrphan_       = false;
      headersByHeight_[thisHeaderPtr->getBlockHeight()] = thisHeaderPtr;

      auto childIter = headerSet_.find(thisHeaderPtr->getPrevHash());
      if (childIter == headerSet_.end()) {
         LOGERR << "failed to get prev header by hash";
         throw std::runtime_error("failed to get prev header by hash");
      }

      (*childIter)->nextHash_ = &thisHeaderPtr->getThisHash();
      thisHeaderPtr = *childIter;
      if (thisHeaderPtr == prevTopBlock) {
         prevChainStillValid = true;
      }
   }

   // Last header in the loop didn't get added (the genesis block on first run)
   thisHeaderPtr->isMainBranch_ = true;
   headersByHeight_[thisHeaderPtr->getBlockHeight()] = thisHeaderPtr;
   idOfTopBlock_ = newTopBlock->getUniqueID();
   std::atomic_store(&topBlockPtr_, newTopBlock);

   //cleanup helper containers
   orphans.clear();

   // Force a full rebuild to make sure everything is marked properly
   // On a full rebuild, prevChainStillValid should ALWAYS be true
   if (!prevChainStillValid) {
      // force-rebuild blockchain (takes less than 1s)
      LOGWARN << "Reorg detected! Forcing a rebuild of the header chain";

      //reset calculation flag on lesser chain
      auto prevHeadPtr = prevTopBlock;
      while (prevHeadPtr->thisHash_ != thisHeaderPtr->thisHash_) {
         prevHeadPtr->isFinishedCalc_ = false;
         prevHeadPtr = *headerSet_.find(prevHeadPtr->prevHash_);
      }
      return thisHeaderPtr;
   }

   if (verbose) {
      TIMER_STOP("orgChain");
      auto duration = TIMER_READ_SEC("orgChain");
      LOGINFO << "Organized chain in " << duration << "s";
   }
   return nullptr;
}

/////////////////////////////////////////////////////////////////////////////
// Start from a node, trace down to the highest solved block, accumulate
// difficulties and difficultySum values. Return the difficultySum of
// this block.
double Blockchain::traceChainDown(std::shared_ptr<BlockHeader> bhpStart)
{
   /*
   NOTE: caller is responsible for locking the mutex
   TODO: check difficulty target matches for each block
   */

   // Walk down the chain of prevHash_ values, until we find a block
   // that has a definitive difficultySum value (i.e. >0).
   auto thisPtr = bhpStart;
   while (thisPtr->difficultySum_ < 0.0) {
      auto iter = headerSet_.find(thisPtr->getPrevHash());
      if (iter != headerSet_.end()) {
         auto hPtr = *iter;
         hPtr->nextPtr_ = thisPtr;
         thisPtr = hPtr;
      } else {
         // this block is an orphan, possibly caused by a HeadersFirst
         // blockchain. Nothing to do about that
         break;
      }
   }

   // Now we have a stack of difficulties and pointers. Walk back up
   // (by pointer) and accumulate the difficulty values
   if (!thisPtr->isOrphan_) {
      while (thisPtr->nextPtr_ != nullptr) {
         auto hPtr = thisPtr->nextPtr_;
         hPtr->blockHeight_ = thisPtr->blockHeight_ + 1;
         hPtr->difficultySum_ = thisPtr->difficultySum_ + hPtr->difficultyDbl_;
         hPtr->isOrphan_ = false;
         thisPtr = hPtr;
      }
   } else {
      //look for an orphan chain this new chain connects to
      auto orphanIter = orphans.begin();
      while (orphanIter != orphans.end()) {
         auto parent = orphanIter->second.find(thisPtr->getPrevHash());
         if (parent != orphanIter->second.end()) {
            break;
         }
         ++orphanIter;
      }

      //there was no chain for this orphan, start a new one
      if (orphanIter == orphans.end()) {
         orphanIter = orphans.emplace(
            thisPtr->getThisHash(), Hash32USet{}).first;
      }

      //mark all blocks in that chain as orphans and track them
      while (thisPtr->nextPtr_ != nullptr) {
         auto hPtr = thisPtr->nextPtr_;
         hPtr->isOrphan_ = true;
         hPtr->isFinishedCalc_ = true;
         orphanIter->second.emplace(hPtr->getThisHash());
         thisPtr = hPtr;
      }
      return 0.0;
   }

   // Finally, we have all the difficulty sums calculated, return this one
   return bhpStart->difficultySum_;
}

////////
void Blockchain::updateBranchingMaps(
   LMDBBlockDatabase* db, ReorganizationState& reorgState)
{
   std::map<unsigned, uint8_t> dupIDs;
   std::map<unsigned, bool> blockIDs;
   std::unique_lock<std::mutex> lock(mu_);

   try {
      HeaderPtr headerPtr;
      if (reorgState.prevTopStillValid) {
         headerPtr = reorgState.prevTop;
      } else {
         headerPtr = reorgState.reorgBranchPoint;
      }
      if (headerPtr->getBlockHeight() == UINT32_MAX) {
         headerPtr = getGenesisBlock();
      }

      while (true) {
         dupIDs.emplace(
            headerPtr->getBlockHeight(), headerPtr->getDuplicateID());
         blockIDs.emplace(
            headerPtr->getUniqueID(), headerPtr->isMainBranch());

         auto hashPtr = headerPtr->getNextHash();
         if (hashPtr == nullptr) {
            break;
         }

         auto iter = headerSet_.find(*hashPtr);
         if (iter == headerSet_.end()) {
            throw std::runtime_error({});
         }
         headerPtr = *iter;
      }
   } catch (const std::exception&) {
      LOGERR << "could not trace chain form prev top to new top";
   }

   if (!reorgState.prevTopStillValid) {
      try {
         auto headerPtr = reorgState.prevTop;
         while (headerPtr != reorgState.reorgBranchPoint) {
            blockIDs.emplace(
               headerPtr->getUniqueID(), headerPtr->isMainBranch());
            auto iter = headerSet_.find(headerPtr->getPrevHash());
            if (iter == headerSet_.end()) {
               throw std::runtime_error({});
            }
            headerPtr = *iter;
         }
      } catch (const std::exception&) {
         LOGERR << "could not trace chain form prev top to branch point";
      }
   }

   db->setValidDupIDForHeight(dupIDs);
   db->setBlockIDBranch(blockIDs);
   initHighestBlockID(db);
}

void Blockchain::putNewBareHeaders(LMDBBlockDatabase *db)
{
   std::unique_lock<std::mutex> lock(mu_);
   if (newlyParsedBlocks_.empty()) {
      return;
   }

   std::map<unsigned, uint8_t> dupIdMap;
   std::map<unsigned, bool> blockIdMap;
   std::vector<std::shared_ptr<BlockHeader>> unputHeaders;

   //create transaction here to batch the write
   auto tx = db->beginTransaction(DB_SELECT::HEADERS, LMDB::Mode::ReadWrite);
   for (const auto& block : newlyParsedBlocks_) {
      if (block->blockHeight_ != UINT32_MAX) {
         StoredHeader sbh;
         sbh.createFromBlockHeader(*block);
         uint8_t dup = db->putBareHeader(sbh, true, false);
         block->setDuplicateID(dup); // make sure headerSet_ and DB agree

         if (block->isMainBranch()) {
            dupIdMap.emplace(block->blockHeight_, dup);
         }
         blockIdMap.emplace(block->getUniqueID(), block->isMainBranch());
      } else {
         unputHeaders.emplace_back(block);
      }
   }

   //update SDBI, keep within the batch transaction
   auto sdbiH = db->getStoredDBInfo(DB_SELECT::HEADERS, 0);
   auto topBlock = topBlockPtr_.load();
   if (topBlock == nullptr) {
      LOGINFO << "No known top block, didn't update SDBI";
      return;
   }

   if (topBlock->blockHeight_ >= sdbiH.topBlkHgt) {
      sdbiH.topBlkHgt = topBlock->blockHeight_;
      sdbiH.topScannedBlkHash = topBlock->thisHash_.toBinaryData();
      db->putStoredDBInfo(DB_SELECT::HEADERS, sdbiH, 0);
   }

   //once commited to the DB, they aren't considered new anymore,
   //so clean up the container
   newlyParsedBlocks_ = std::move(unputHeaders);

   /*
   We need to keep track of the highest assigned
   topID across runs so we manually update it instead of relying on
   headers in the db.
   */
   updateHighestBlockIDInDb(db);

   db->setValidDupIDForHeight(dupIdMap);
   db->setBlockIDBranch(blockIdMap);
}

////////
uint32_t Blockchain::getHighestBlockIDFromDb(LMDBBlockDatabase *db) const
{
   auto tx = db->beginTransaction(
      DB_SELECT::HEADERS, LMDB::Mode::ReadOnly);

   auto value = db->getValueNoCopy(DB_SELECT::HEADERS, topIdKey);
   if (value.getSize() != 4)
      return 0;

   uint32_t topId;
   memcpy(&topId, value.getPtr(), sizeof(uint32_t));
   return topId;
}

void Blockchain::initHighestBlockID(LMDBBlockDatabase* db)
{
   auto grabLastStxoKey = [db](void)->uint32_t
   {
      //only works for supernode
      if (db->getDbType() != ARMORY_DB_TYPE::Super) {
         return 0;
      }
      auto tx = db->beginTransaction(DB_SELECT::STXO, LMDB::Mode::ReadOnly);
      auto stxoIter = db->getIterator(DB_SELECT::STXO);

      if (!stxoIter->seekToLast()) {
         return 0;
      }
      auto lastKey = stxoIter->getKey();
      if (lastKey.getSize() < 4) {
         return 0;
      }

      BinaryRefReader keyReader(lastKey.getRef());
      auto intKey = keyReader.get_uint32_t(BE);
      if ((intKey & 0x000000FF) != 0xFF) {
         return 0;
      }
      return intKey >> 8;
   };

   //grab top id from block headers sdbi
   auto highestBlockID = getHighestBlockIDFromDb(db);
   
   //also check the top block id used to record stxos
   auto stxoTopId = grabLastStxoKey();

   if (stxoTopId != 0 && stxoTopId >= highestBlockID) {
      LOGWARN << "top ID in stxo DB isn't less than top ID in headers DB";
      highestBlockID = stxoTopId + 1;
   }

   if (highestBlockID > highestBlockID_.load(std::memory_order_relaxed)) {
      highestBlockID_.store(highestBlockID, std::memory_order_relaxed);
   }
}

void Blockchain::updateHighestBlockIDInDb(LMDBBlockDatabase *db)
{
   auto inDbTopId = getHighestBlockIDFromDb(db);
   auto currentTopId = highestBlockID_.load(std::memory_order_relaxed);
   if (inDbTopId >= currentTopId) {
      return;
   }
   BinaryDataRef valRef((const uint8_t*)&currentTopId, 4);

   auto tx = db->beginTransaction(DB_SELECT::HEADERS, LMDB::Mode::ReadWrite);
   db->putValue(DB_SELECT::HEADERS, topIdKey.getRef(), valRef);
}

uint32_t Blockchain::getNewUniqueID()
{
   return highestBlockID_.fetch_add(1, std::memory_order_relaxed);
}

////////
std::set<uint32_t> Blockchain::checkForNewBlocks(
   const std::vector<std::shared_ptr<BlockData>>& blocks)
{
   std::unique_lock<std::mutex> lock(mu_);
   std::set<uint32_t> result;
   for (const auto block : blocks) {
      auto iter = headerSet_.find(block->getHash());
      if (iter != headerSet_.end()) {
         continue;
      }

      if (block->uniqueID() == UINT32_MAX) {
         block->setUniqueID(getNewUniqueID());
      }
      result.emplace(block->uniqueID());
   }
   return result;
}

void Blockchain::addBlocksInBulk(
   const std::deque<std::deque<HeaderPtr>>& headerLists, bool areNew)
{
   if (headerLists.empty()) {
      return;
   }

   std::unique_lock<std::mutex> lock(mu_);
   uint32_t count = 0;
   uint32_t highestBlockID = highestBlockID_.load(
      std::memory_order_relaxed);
   for (const auto& headers : headerLists) {
      count += headers.size();
      for (const auto& header : headers) {
         auto headerId = header->getUniqueID();
         if (headerId != UINT32_MAX) {
            highestBlockID = std::max(highestBlockID, headerId);
         }
      }
   }
   if (headerSet_.bucket_count() < headerSet_.size() + count) {
      headerSet_.reserve(headerSet_.size() + count);
   }
   highestBlockID = std::max(highestBlockID, count);
   if (headersById_.size() < highestBlockID + 1) {
      headersById_.resize(highestBlockID + 100);
   }

   for (auto& headers : headerLists) {
      for (auto& newHeader : headers) {
         bool commitHeader = false;
         auto iter = headerSet_.find(newHeader);
         if (iter != headerSet_.end()) {
            auto thisHeader = *iter;
            if (thisHeader->getBlockFileNum() == newHeader->getBlockFileNum() &&
               thisHeader->getOffset() == newHeader->getOffset()) {
               continue;
            }

            //header will be replaced, carry the uniqueID over
            newHeader->setUniqueID(thisHeader->getUniqueID());
            headerSet_.erase(iter);
            commitHeader = true;
            forceRebuildFlag_ = true;
         } else {
            commitHeader = true;
         }

         //assign uniqueID if necessary
         if (newHeader->getUniqueID() == UINT32_MAX) {
            newHeader->setUniqueID(getNewUniqueID());
            commitHeader = true;
         }

         headerSet_.emplace(newHeader);
         headersById_[newHeader->getUniqueID()] = newHeader;
         if (areNew && commitHeader) {
            newlyParsedBlocks_.emplace_back(newHeader);
         }
      }
   }

   if (!areNew) {
      /*
      Only set the top id when blocks are originally loaded,
      do not allow the process to backtrack the top id to a
      lower value (i.e. if the block insertion was rejected).

      It is crucial block IDs are not reused.
      */
      highestBlockID = highestBlockID_.load(std::memory_order_relaxed);
      for (const auto& headers : headerLists) {
         for (const auto& header : headers) {
            if (highestBlockID < header->getUniqueID()) {
               highestBlockID = header->getUniqueID();
            }
         }
      }
      highestBlockID_.store(highestBlockID, std::memory_order_relaxed);
   }
}

////////
std::map<unsigned, std::set<unsigned>> Blockchain::mapIDsPerBlockFile() const
{
   std::unique_lock<std::mutex> lock(mu_);
   std::map<unsigned, std::set<unsigned>> resultMap;
   for (const auto& header : headersById_) {
      if (header == nullptr) {
         continue;
      }
      auto& result_set = resultMap[header->blkFileNum_];
      result_set.emplace(header->uniqueID_);
   }
   return resultMap;
}

////////
std::map<uint32_t, HeightAndDup> Blockchain::getHeightAndDupMap() const
{
   std::unique_lock<std::mutex> lock(mu_);
   std::map<uint32_t, HeightAndDup> result;
   for (const auto& header : headersById_) {
      if (header == nullptr) {
         continue;
      }
      result.emplace(header->getUniqueID(),
         HeightAndDup{
            header->getBlockHeight(),
            header->getDuplicateID(),
            header->isMainBranch()}
      );
   }
   return result;
}

////////
void Blockchain::flagBlockHeader(std::shared_ptr<BlockHeader> header,
   LMDBBlockDatabase *db)
{
   if (db->getOrSetFlaggedBlockFile(header->getBlockFileNum())) {
      LOGINFO << "flagging block file " << header->getBlockFileNum() <<
         " for reparsing";
   }
}
