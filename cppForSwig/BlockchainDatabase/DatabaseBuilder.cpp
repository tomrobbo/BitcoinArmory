////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2016-2026, goatpig.                                         //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "DatabaseBuilder.h"
#include <Utils/BtcUtils.h>
#include <Utils/varint.h>
#include <Utils/DBUtils.h>
#include <Utils/ArmoryConfig.h>
#include <Utils/BitcoinSettings.h>
#include <Utils/UniversalTimer.h>
#include <TxClasses.h>

#include "BlockUtils.h"
#include "lmdb_wrapper.h"
#include "Blockchain.h"
#include "BlockchainScanner.h"
#include "BlockchainScanner_Super.h"
#include "ScrAddrFilter.h"
#include "Transactions.h"
#include "TxHashFilters.h"
#include "txio.h"
#include "StoredBlockObj.h"

#define REWIND_COUNT 100

using namespace Armory;
using namespace Armory::Database;

namespace {
   void dumpBlock(
      LMDBBlockDatabase* db,
      std::shared_ptr<BlockHeader> bh)
   {
      //grab the block
      StoredHeader sbh;
      db->getStoredHeader(sbh, bh, true);

      std::cout << "###############################################" << std::endl;

      //header
      std::cout << "# hash: " << bh->getThisHash().toHexStr() << std::endl;
      std::cout << "# prev: " << bh->getPrevHash().toHexStr() << std::endl;
      std::cout << "# height: " << bh->getBlockHeight() << std::endl;
      std::cout << "# diffsum: " << bh->getDifficultySum() << std::endl;
      std::cout << "# size: " << bh->getBlockSize() << std::endl;
      std::cout << "########" << std::endl;
      std::cout << "# tx count: " << sbh.getNumTx() << std::endl;

      //txs
      for (unsigned i=0; i<sbh.getNumTx(); i++) {
         auto tx = sbh.getTxCopy(i);
         std::cout << "#  hash: " << tx.getThisHash().toHexStr() << std::endl;
      }
      std::cout << std::endl;
   }

   void dumpBlock(
      std::shared_ptr<Blockchain> bcPtr,
      LMDBBlockDatabase* db,
      unsigned blockId)
   {
      auto bh = bcPtr->getHeaderById(blockId);
      dumpBlock(db, bh);
   }
}

/////////////////////////////////////////////////////////////////////////////
// Builder
Builder::Builder(BlockDataManager& bdm,
   const ProgressCallback &progress, bool forceRescanSSH)
   : blockFiles_(bdm.blockFiles()), blockchain_(bdm.blockchain()),
   db_(bdm.getIFace()), scrAddrFilter_(bdm.getScrAddrFilter()),
   progress_(progress), forceRescanSSH_(forceRescanSSH)
{
   scannerCtx_ = std::make_unique<ScannerContext>();
}

////////
bool Builder::init()
{
   if (Config::DBSettings::checkChain()) {
      verifyChain();
      return true;
   }

   TIMER_START("initdb");

   //list all files in block data folder
   blockFiles_->detectAllBlockFiles();

   LOGINFO << "loading headers from db";
   loadBlockHeadersFromDB(progress_);

   LOGINFO << "organizing chain";
   blockchain_->organize(true, false);

   TIMER_START("updateblocksindb");

   LOGINFO << "parsing chain for new headers";
   auto startBO = parseForNewHeaders(
      Config::DBSettings::reportProgress() ? progress_ : nullptr);

   LOGINFO << "organizing chain";
   auto reorgState = blockchain_->organize(false, false);

   LOGINFO << "commiting new headers";
   blockchain_->putNewHeaders(db_);

   //is there something to repair?
   bool chainIsSain = true;
   /*
   auto flaggedFileNums = db_->getFlaggedFileNums();
   if (!flaggedFileNums.empty()) {
      LOGWARN << "the following block files are flagged for reparsing:";
      for (auto fileNum : flaggedFileNums) {
         LOGWARN << "   - " << fileNum;
      }

      //reparse these block files + 10 ahead
      std::set<uint32_t> filesToReparse;
      for (const auto& fileID : flaggedFileNums) {
         for (uint32_t i=fileID; i<fileID+10; i++) {
            filesToReparse.emplace(i);
         }
      }
      auto bdl = std::make_shared<BlockDataLoader>(
         blockFiles_, filesToReparse);
      auto reorgState = updateBlocksInDB(nullptr, bdl);

      if (Config::DBSettings::reportProgress()) {
         progress_(BDMPhase_OrganizingChain, 0, UINT32_MAX, 0);
      }
      blockchain_->updateBranchingMaps(db_, reorgState);

      //clear flagged filenums
      db_->clearFlaggedFileNums();

      //flag for rescan
      //NOTE: we cannot call resetHistory this early because it nukes
      //the SSH db, which scrAddrFilter reads to populate itself

      chainIsSain = false;
      LOGWARN << "done fixing chain";
   } else {
      if (Config::DBSettings::reportProgress()) {
         progress_(BDMPhase_OrganizingChain, 0, UINT32_MAX, 0);
      }
      auto initialReorgState = blockchain_->organize(true, false);
      blockchain_->updateBranchingMaps(db_, initialReorgState);
      LOGINFO << "done organizing chain";
   }

   try {
      //rewind the top block offset to catch on missed blocks for db init
      auto topBlock = blockchain_->top();
      auto rewindHeight = topBlock->getBlockHeight();
      if (rewindHeight > Config::DBSettings::rewindCount()) {
         rewindHeight -= Config::DBSettings::rewindCount();
      } else {
         rewindHeight = 1;
      }

      auto rewindBlock = blockchain_->getHeaderByHeight(rewindHeight, 0xFF);
      topBlockOffset_.fileID = rewindBlock->getBlockFileNum();
      topBlockOffset_.offset = rewindBlock->getOffset() + rewindBlock->getBlockSize();
      LOGINFO << "Rewinding " << topBlock->getBlockHeight() - rewindHeight << " blocks";
   } catch (const std::exception&) {}
   */

   LOGINFO << "parsing chain for new blocks";
   parseForNewBlocks(startBO,
      Config::DBSettings::reportProgress() ? progress_ : nullptr);
   TIMER_STOP("updateblocksindb");
   double updatetime = TIMER_READ_SEC("updateblocksindb");
   LOGINFO << "updated HEADERS db in " << updatetime << "s";

   if (Config::DBSettings::checkTxHints()) {
      checkTxHintsIntegrity();
   }
   cycleDatabases();

   /* blockchain object now has the longest chain, update address history */
   if (Config::DBSettings::getDbType() != ARMORY_DB_TYPE::Super) {
      //don't scan without any registered addresses
      if (scrAddrFilter_->empty()) {
         LOGINFO << "found no address to scan";
         TIMER_STOP("initdb");
         double timeSpent = TIMER_READ_SEC("initdb");
         LOGINFO << "init db in " << timeSpent << "s";
         return true;
      }
   }

   TIMER_START("scanning");
   while (true) {
      auto topScannedBlockHash = initTransactionHistory(reorgState);
      cycleDatabases();

      if (blockchain_->top()->getThisHash() == topScannedBlockHash) {
         break;
      }

      //if we got this far the scan failed, diagnose the DB and repair it
      if (!topScannedBlockHash.valid()) {
         //scan ran into a fatal error, notify clients and shutdown
         LOGERR << "ArmoryDB has failed to initialize, it will now terminate";
         LOGWARN << "you may restart it to enter the autorepair procedure";
         return false;
      }

      LOGWARN << "top scanned block does not match current top!";
      LOGWARN << "current top is height #" << blockchain_->top()->getBlockHeight();

      try {
         auto topscannedblock = blockchain_->getHeaderByHash(topScannedBlockHash);
         LOGWARN << "top scanned block is height #" << topscannedblock->getBlockHeight();
      } catch (...) {
         LOGWARN << "top scanned block is invalid";
         return false;
      }
   }

   TIMER_STOP("scanning");
   double scanning = TIMER_READ_SEC("scanning");
   LOGINFO << "scanned new blocks in " << scanning << "s";

   TIMER_STOP("initdb");
   double timeSpent = TIMER_READ_SEC("initdb");
   LOGINFO << "init db in " << timeSpent << "s";
   return true;
}

////////
void Builder::loadBlockHeadersFromDB(
   const ProgressCallback& progress)
{
   //TODO: preload the headers db file to speed process up

   // every ten minutes we get a block, how many blocks exist?
   const time_t btcEpoch = 1230963300; // genesis block ts
   const time_t now = time(nullptr);
   const unsigned expectedTotalBlockCount = (now - btcEpoch) / 60 / 10;
   ProgressCalculator calc(expectedTotalBlockCount);

   const auto callback = [&calc, &progress](size_t count)
   {
      if (!Config::DBSettings::reportProgress()) {
         return;
      }

      calc.advance(count);
      progress(
         BDMPhase_DBHeaders,
         calc.fractionCompleted(),
         calc.remainingSeconds(),
         count
      );
   };
   blockchain_->loadHeadersFromDB(db_, callback);
   
}

////////
BlockOffset Builder::parseForNewHeaders(const ProgressCallback& progress)
{
   std::shared_ptr<BlockDataLoader> bdl;
   auto topBlockOffset = blockchain_->getTopBlockOffset();
   try {
      bdl = std::make_shared<BlockDataLoader>(blockFiles_, topBlockOffset);
      LOGINFO << "looking for new headers starting file " <<
         topBlockOffset.fileID() << ", offset " << topBlockOffset.offset();
   } catch (const BlockDataExhausted&) {
      return {UINT16_MAX, SIZE_MAX};
   }

   std::atomic<uint32_t> total, count;
   total.store(0, std::memory_order_relaxed);
   count.store(0, std::memory_order_relaxed);
   auto parseBlock = [this, bdl, &total, &count]()
   {
      std::vector<std::shared_ptr<BlockHeader>> headers;
      headers.reserve(200);

      auto deserHeader = [&headers](
         const uint8_t* ptr, size_t size, size_t offset)->bool
      {
         try {
            if (size < HEADER_SIZE) {
               throw BtcUtils::BlockDeserializingException(
                  "not enough data for a header");
            }
            BinaryData rawData{ptr, HEADER_SIZE};
            auto header = std::make_shared<BlockHeader>(rawData.getRef());
            header->setBlockSize(size);
            header->setBlockFileOffset(offset);
            header->setRawData(std::move(rawData));
            try {
               BinaryRefReader brr{ptr, size};
               brr.advance(HEADER_SIZE);
               header->setNumTx(brr.get_var_int());
            } catch (const std::exception&) {
               //data didn't carry numtx varint, nothing to do
            }

            headers.emplace_back(header);
            return true;
         } catch (const std::exception&) {
            return false;
         }
      };

      while (true) {
         auto fileCopy = bdl->getNextCopy();
         if (!fileCopy.isValid()) {
            break;
         }

         headers.clear();
         parseBlockFile(fileCopy, deserHeader);
         total.fetch_add(headers.size(), std::memory_order_relaxed);
         for (auto& header : headers) {
            header->setBlockFileNum(fileCopy.fileID);
            header->setBlockFileOffset(header->getOffset() + fileCopy.offset);
         }
         auto added = blockchain_->stageNewHeaders(headers);
         count.fetch_add(added, std::memory_order_relaxed);
      }
   };

   unsigned threadcount = std::min(
      (size_t)Config::DBSettings::threadCount(),
      bdl->size()
   );
   std::deque<std::thread> tIDs;
   for (unsigned i = 1; i < threadcount; i++) {
      tIDs.emplace_back(std::thread(parseBlock));
   }
   parseBlock();

   for (auto& tID : tIDs) {
      if (tID.joinable()) {
         tID.join();
      }
   }

   LOGINFO << "found " << total.load(std::memory_order_relaxed)
      << " new headers";
   LOGINFO << "staged " << count.load(std::memory_order_relaxed)
      << " of them";
   return topBlockOffset;
}

////////
void Builder::parseForNewBlocks(const BlockOffset& startBO,
   const ProgressCallback& progress)
{
   //this is a helper to load block data files in memory
   if (!startBO.isValid()) {
      return;
   }

   std::shared_ptr<BlockDataLoader> bdl;
   try {
      bdl = std::make_shared<BlockDataLoader>(blockFiles_, startBO);
   } catch (const BlockDataExhausted&) {
      //no more fresh block data available
      return;
   }

   //do not run more threads than there are block files to read
   unsigned threadcount = std::min(
      (size_t)Config::DBSettings::threadCount(),
      bdl->size()
   );

   std::mutex progressMutex;
   unsigned lastParsedFileID = bdl->getFirstFileID();
   if (lastParsedFileID == UINT16_MAX) {
      lastParsedFileID = 0;
   }

   //init progress
   ProgressCalculator calc(blockFiles_->fileCount());
   if (progress) {
      calc.init(lastParsedFileID);
      progress(BDMPhase_BlockData,
         calc.fractionCompleted(),
         UINT32_MAX,
         lastParsedFileID
      );
   }

   //parser threads will start with block fileID + 1
   std::set<uint32_t> invalidatedBlockIDs;
   auto addBlocks = [&](void)->void
   {
      while (true) {
         auto fileCopy = bdl->getNextCopy();
         if (!fileCopy.isValid()) {
            break;
         }
         auto invalids = addBlocksToDB(fileCopy);

         std::unique_lock<std::mutex> lock(progressMutex, std::defer_lock);
         invalidatedBlockIDs.insert(invalids.begin(), invalids.end());

         //report to progress callback every ~100 block files
         if (progress && fileCopy.fileID >= lastParsedFileID + 100) {
            if (lock.try_lock()) {
               LOGINFO << "parsed block file #" << fileCopy.fileID;
               calc.advance(fileCopy.fileID);
               progress(BDMPhase_BlockData,
                  calc.fractionCompleted(), calc.remainingSeconds(),
                  fileCopy.fileID);

               //bump last seen file, this doesn't need to be accurate
               lastParsedFileID = fileCopy.fileID;
            }
         }
      }
   };

   std::vector<std::thread> tIDs;
   for (unsigned i = 1; i < threadcount; i++) {
      tIDs.push_back(std::thread(addBlocks));
   }

   //run one parser in current thread too
   addBlocks();

   //wait on parser threads to complete
   for (auto& tID : tIDs) {
      if (tID.joinable()) {
         tID.join();
      }
   }

   if (!invalidatedBlockIDs.empty()) {
      blockchain_->flagInvalidBlocks(db_, invalidatedBlockIDs);
   }
}

////////////////////////////////////////////////////////////////////////////////
std::set<uint32_t> Builder::addBlocksToDB(
   const BlockDataLoader::BlockDataCopy& bdc)
{
   /*
   Expect headers to already exist in blockchain objects.
   This checks blocks body, computes all tx hashes and merkle root and
   checks it vs root in header.
   Finally, it uses tx hashes to build + commits tx hints/filters to db.
   */

   std::vector<std::shared_ptr<BlockData>> blocksVec;
   std::set<uint32_t> invalidBlockIds;
   blocksVec.reserve(200);

   bool fullHints = Config::DBSettings::getDbType() == ARMORY_DB_TYPE::Super;
   auto tallyBlocks =
   [&blocksVec, &invalidBlockIds, fullHints, bc=blockchain_, fileID=bdc.fileID]
   (const uint8_t* data, size_t size, size_t)->bool
   {
      try {
         //deser raw header to get its hash
         auto header = std::make_shared<BlockHeader>(data, size);

         //look hash it in blockchain obj
         auto knownHeader = bc->getHeaderByHash(header->getThisHash());
         if (knownHeader->parsedBlockData()) {
            //we already parsed this block, skip it
            return size == knownHeader->getBlockSize();
         }

         //deser full block, it will compute all tx hashes and merkle root
         auto bd = BlockData::deserialize(
            data, size, knownHeader,
            fullHints ? BlockData::CheckHashes::FullHints :
               BlockData::CheckHashes::TxFilters
         );
         blocksVec.emplace_back(bd);
         if (!knownHeader->isMerkleValid()) {
            invalidBlockIds.emplace(knownHeader->getUniqueID());
         }
         return true;
      } catch (const BtcUtils::BlockDeserializingException &e) {
         LOGERR << "block deser except: " << e.what();
         LOGERR << "block fileID: " << fileID;
         return false;
      } catch (const std::range_error& e) {
         //header hash is unknown, this shouldn't happen
         LOGERR << "block parse error: " << e.what();
         return false;
      } catch (const std::exception &e) {
         LOGERR << "block deser exception: " << e.what();
         return false;
      } catch (...) {
         //deser failed, ignore this block
         LOGERR << "block deser unknown exception";
         return false;
      }
   };

   //parseBlockFile will crawl the whole file and callback tallyBlocks
   //for each new block; tallyBlocks will deser the blocks and append
   //them to blocksVec
   try {
      parseBlockFile(bdc, tallyBlocks);
   } catch (const std::exception& e) {
      LOGWARN << "halted block file parsing with error: " << e.what();
   }

   //blocksVec only carries unchecked blocks
   if (!fullHints) {
      //process filters
      if (Config::DBSettings::getDbType() == ARMORY_DB_TYPE::Super) {
         //pull existing file filter bucket from db (if any)
         auto pool = db_->getFilterPoolWriter(bdc.fileID);

         if (blocksVec.empty()) {
            if (pool.isValid()) {
               //this block has a filter pool and there is no data to append,
               //we can return
               return invalidBlockIds;
            }

            //if we got this far, this block file does not add any new blocks
            //to the chain, but it still needs an empty filter pool for the
            //resolver to fetch. we simply let it run on an empty block set
         }

         //tally all block filters
         std::map<uint32_t, std::shared_ptr<BlockHashVector>> allFilters;
         for (const auto& block : blocksVec) {
            allFilters.emplace(block->uniqueID(), block->getTxFilter());
         }

         //update bucket
         pool.update(allFilters);

         //update db entry
         db_->putFilterPoolForFileNum(bdc.fileID, pool);
      }
   } else {
      commitAllTxHints(blocksVec);
   }
   return invalidBlockIds;
}

/////////////////////////////////////////////////////////////////////////////
void Builder::parseBlockFile(
   const BlockDataLoader::BlockDataCopy& bdc,
   const std::function<bool(const uint8_t* data, size_t size, size_t offset)>& callback)
{
   //check magic bytes at start of data
   auto fileSize = bdc.data->size();
   auto dataPtr = bdc.data->ptr();
   const auto& magicBytes = Config::BitcoinSettings::getMagicBytes();
   auto magicBytesSize = magicBytes.getSize();

   //parse the file
   size_t progress = 0;
   while (progress + magicBytesSize < fileSize) {
      size_t localProgress = magicBytesSize;
      BinaryDataRef magic(dataPtr, magicBytesSize);

      if (magic != magicBytes) {
         //no magic byte trailing the last valid file offset, let's look for one
         BinaryDataRef theFile(dataPtr + localProgress,
            fileSize - progress - localProgress);
         int32_t foundOffset = theFile.find(magicBytes);
         if (foundOffset == -1) {
            return;
         }
         LOGINFO << "Found next block after skipping " << foundOffset - 4 << "bytes";

         localProgress += foundOffset;
         magic.setRef(dataPtr + localProgress, magicBytesSize);
         if (magic != magicBytes) {
            LOGWARN << "Could not find magicword in file " << bdc.fileID;
            throw std::runtime_error("parsing for magic byte failed");
         }
         localProgress += 4;
      }

      if (progress + localProgress + 4 >= fileSize) {
         return;
      }

      BinaryDataRef blockSize(dataPtr + localProgress, 4);
      localProgress += 4;
      size_t thisBlkSize = READ_UINT32_LE(blockSize.getPtr());
      if (progress + localProgress + thisBlkSize > fileSize) {
         return;
      }

      dataPtr += localProgress;
      progress += localProgress;
      if (callback(dataPtr, thisBlkSize, progress)) {
         //only advance for the whole blockSize if callback returned true
         dataPtr += thisBlkSize;
         progress += thisBlkSize;
      }
   }
}

////////
Hash32 Builder::initTransactionHistory(
   const ReorganizationState& reorgState)
{
   //Scan history
   scannerCtx_->init(db_);
   auto topScannedBlockHash = scanHistory(
      reorgState, Config::DBSettings::reportProgress(), true
   );

   //return the hash of the last scanned block
   return topScannedBlockHash;
}

Hash32 Builder::scanHistory(const ReorganizationState& reorgState,
   bool reportprogress, bool init)
{
   auto scanFrom = scrAddrFilter_->scanFrom();
   HeaderPtr startHeader = nullptr;
   if (scanFrom.valid()) {
      startHeader = blockchain_->getHeaderByHash(scanFrom);
      if (!reorgState.prevTopStillValid) {
         //reorg
         if (startHeader->isMainBranch()) {
            if (reorgState.reorgBranchPoint->getBlockHeight() <
               startHeader->getBlockHeight()) {
               startHeader = reorgState.reorgBranchPoint;
            }
         } else {
            startHeader = reorgState.reorgBranchPoint;
         }
      }
   }

   if (startHeader == nullptr) {
      startHeader = blockchain_->getGenesisHeader();
   } else {
      startHeader = blockchain_->getHeaderByHash(*startHeader->getNextHash());
   }

   if (Config::DBSettings::getDbType() != ARMORY_DB_TYPE::Super) {
      LOGINFO << "scanning new blocks from #" << startHeader->getBlockHeight() << " to #" <<
         blockchain_->top()->getBlockHeight();

      BlockchainScanner bcs(blockchain_,
         db_, scrAddrFilter_.get(),
         blockFiles_,
         Config::DBSettings::threadCount(), Config::DBSettings::ramUsage(),
         progress_, reportprogress);

      if (!bcs.scan(*scannerCtx_, startHeader->getBlockHeight())) {
         LOGERR << "scan failed!";
         return Hash32{};
      }

      unsigned count = 0;
      while (!bcs.resolveTxHashes()) {
         ++count;
         if (count > 5) {
            LOGERR << "failed to fix filters after 5 attempts";
            break;
         }
      }

      scrAddrFilter_->updateScannedHash(bcs.getTopScannedBlockHash());
      return bcs.getTopScannedBlockHash();
   } else {
      BlockchainScanner_Super bcs(
         blockchain_, db_,
         blockFiles_, init,
         Config::DBSettings::threadCount(), Config::DBSettings::ramUsage(),
         progress_, reportprogress);

      bcs.scan();
      bcs.scanSpentness();
      bcs.updateSSH(forceRescanSSH_ & init);
      return bcs.getTopScannedBlockHash();
   }
}

/////////////////////////////////////////////////////////////////////////////
ReorganizationState Builder::update()
{
   std::unique_lock<std::mutex> lock(scrAddrFilter_->mergeLock_);

   //list new files in block data folder
   blockFiles_->detectNewBlockFiles();

   //update db blocks
   auto bo = parseForNewHeaders(nullptr);
   parseForNewBlocks(bo, nullptr);
   auto reorgState = blockchain_->organize(false, false);
   blockchain_->putNewHeaders(db_);
   if (!reorgState.hasNewTop) {
      return reorgState;
   }

   //scan new blocks
   auto topScannedHash = scanHistory(reorgState, false, false);
   if (topScannedHash != blockchain_->top()->getThisHash()) {
      LOGERR << "scan failure during DatabaseBuilder::update";
      throw std::runtime_error("scan failure during DatabaseBuilder::update");
   }

   //TODO: gracefully shutdown on failed scan
   return reorgState;
}

/////////////////////////////////////////////////////////////////////////////
#if 0
void Builder::undoHistory(ReorganizationState& reorgState)
{
   if (Config::DBSettings::getDbType() != ARMORY_DB_TYPE::Super) {
      BlockchainScanner bcs(blockchain_, db_, scrAddrFilter_.get(),
         blockFiles_,
         Config::DBSettings::threadCount(), Config::DBSettings::ramUsage(),
         progress_, false);
      bcs.undo(reorgState);
   } else {
      BlockchainScanner_Super bcs(blockchain_, db_,
         blockFiles_, false,
         Config::DBSettings::threadCount(), Config::DBSettings::ramUsage(),
         progress_, false);
      bcs.undo(reorgState);
   }
}
#endif

void Builder::resetHistory()
{
   //nuke SSH, SUBSSH, TXHINT and STXO DBs
   LOGINFO << "reseting history in DB";
   db_->resetHistoryDatabases();
}

/////////////////////////////////////////////////////////////////////////////
void Builder::verifyChain()
{
   /*
   builds db (no scanning) with full txhints, then verifies all tx
   (consensus and sigs).
   */

   //list all files in block data folder
   blockFiles_->detectAllBlockFiles();

   //read all blocks already in DB and populate blockchain
   loadBlockHeadersFromDB(progress_);
   auto bo = parseForNewHeaders(
      Config::DBSettings::reportProgress() ? progress_ : nullptr);
   blockchain_->putNewHeaders(db_);

   if (Config::DBSettings::reportProgress()) {
      progress_(BDMPhase_OrganizingChain, 0, UINT32_MAX, 0);
   }
   auto initialReorgState = blockchain_->organize(true, false);

   //update db
   LOGINFO << "updating HEADERS db";
   parseForNewBlocks(bo,
      Config::DBSettings::reportProgress() ? progress_ : nullptr);
   auto reorgState = blockchain_->organize(false, false);
   LOGINFO << "updated HEADERS db";

   //verify transactions
   LOGWARN << "fix me =)";
   //verifyTransactions();
}

/////////////////////////////////////////////////////////////////////////////
void Builder::commitAllTxHints(
   const std::vector<std::shared_ptr<BlockData>>& blocks)
{
   auto addHint = [](StoredTxHints& stxh, const BinaryData& txkey)->void
   {
      //make sure key isn't already in there
      for (const auto& key : stxh.dbKeyList) {
         if (key == txkey) {
            return;
         }
      }
      stxh.dbKeyList.emplace_back(txkey);
   };

   //The readwrite db transactions makes sure only one thread is batching
   //txhints at a time. This is relevant, as hints are first pulled from
   //disk then updated. In case 2 different blocks commit to the same
   //hint, one will likely overwrite the other.
   auto hintdbtx = db_->beginTransaction(DB_SELECT::TXHINTS, LMDB::Mode::ReadWrite);
   std::map<BinaryData, StoredTxHints> txHints;
   {
      auto addTxHintMap = [&txHints, addHint, db=db_]
      (const std::shared_ptr<BCTX>& txn, const BinaryData& txkey)
      {
         auto txHashPrefix = txn->getHash().getSliceCopy(0, 4);
         auto& stxh = txHints[txHashPrefix];

         //pull txHint from memory first, don't want to overwrite
         //existing hints
         if (!stxh.isInitialized()) {
            db->getStoredTxHints(stxh, txHashPrefix);
         }
         addHint(stxh, txkey);
         stxh.preferredDBKey = stxh.dbKeyList.front();
      };

      for (const auto& block : blocks) {
         auto blockId = block->uniqueID();
         const auto& txns = block->getTxns();
         for (unsigned i=0; i < txns.size(); i++) {
            const auto& txn = txns[i];
            auto txkey = DBUtils::getBlkDataKeyNoPrefix(blockId, 0xFF, i);
            addTxHintMap(txn, txkey);
         }
      }
   }

   std::map<BinaryData, BinaryWriter> serializedHints;

   //serialize
   for (const auto& txhint : txHints) {
      auto& bw = serializedHints[txhint.second.getDBKey()];
      txhint.second.serializeDBValue(bw);
   }

   //write
   for (const auto& txhint : serializedHints) {
      db_->putValue(DB_SELECT::TXHINTS,
         txhint.first.getRef(),
         txhint.second.getDataRef());
   }
}

#if 0
/////////////////////////////////////////////////////////////////////////////
void Builder::verifyTransactions()
{
   struct ParserState
   {
      atomic<unsigned> blockHeight_;
      atomic<unsigned> unknownErrors_;
      atomic<unsigned> unsupportedSigHash_;
      atomic<unsigned> unresolvedHashes_;
      atomic<unsigned> parsedCount_;
      mutex mu_;

      ParserState()
      {
         blockHeight_.store(0);
         unknownErrors_.store(0);
         unsupportedSigHash_.store(0);
         unresolvedHashes_.store(0);
         parsedCount_.store(0);
      }
   };

   TIMER_START("10blocks");

   //dont preload, prefetch
   BlockDataLoader bdl(blockFiles_, BlockOffset());

   auto stateStruct = make_shared<ParserState>();

   auto verifyBlockTx = [&bdl, this, stateStruct](void)->void
   {
      auto getUtxoMap = [&bdl, stateStruct, this]
         (shared_ptr<BCTX> txn)->Armory::Signing::TransactionVerifier::utxoMap
      {
         Armory::Signing::TransactionVerifier::utxoMap utxomap;
         for (auto& txin : txn->txins_)
         {
            //get output hash
            BinaryDataRef hashref(txn->data_ + txin.first, 32);
            auto outputID = (uint32_t*)(txn->data_ + txin.first + 32);

            //resolve hash
            StoredTxHints sths;
            if (!db_->getStoredTxHints(sths, hashref.getSliceRef(0, 4)))
            {
               stateStruct->unresolvedHashes_.fetch_add(1, memory_order_relaxed);
               throw UnresolvedHashException();
            }

            bool foundtx = false;
            for (auto& outpointkey : sths.dbKeyList_)
            {
               if (outpointkey.getSize() == 0)
                  continue;

               //parse key
               auto blockkey = outpointkey.getSliceRef(0, 4);
               auto opDup = (uint8_t*)(outpointkey.getPtr() + 3);
               if (*opDup != 0xFF)
                  continue;

               auto blockID = DBUtils::hgtxToHeight(blockkey);
               shared_ptr<BlockHeader> bhPtr;
               try
               {
                  bhPtr = blockchain_->getHeaderById(blockID);
               }
               catch (exception&)
               {
                  continue;
               }

               //get tx index
               BinaryRefReader brr(outpointkey);
               brr.advance(4);
               auto txid = brr.get_uint16_t(BE);

               //get block data
               auto blockFileNum = bhPtr->getBlockFileNum();
               auto fileCopy = bdl.getNextCopy();

               auto getID = [bhPtr](const BinaryData&)->unsigned int
               {
                  return bhPtr->getThisID();
               };

               auto bdata = BlockData::deserialize(
                  fileMap->data() + bhPtr->getOffset(),
                  bhPtr->getBlockSize(),
                  bhPtr, getID, BlockData::CheckHashes::NoChecks);

               const auto& txns = bdata->getTxns();
               if (txid > txns.size())
                  continue;

               //check hash
               const auto& _txn = txns[txid];
               const auto& txhash = _txn->getHash();
               if (hashref != txhash.getRef())
                  continue;

               //grab output
               auto txoutcount = _txn->txouts_.size();
               if (*outputID > txoutcount)
                  break;

               BinaryDataRef output(_txn->data_ + _txn->txouts_[*outputID].first,
                  _txn->txouts_[*outputID].second);

               UTXO utxo;
               utxo.unserializeRaw(output);
               auto& idmap = utxomap[hashref];
               idmap[*outputID] = move(utxo);

               foundtx = true;
               break;
            }

            if (!foundtx)
               throw UnresolvedHashException();
         }

         return utxomap;
      };

      unsigned thisHeight = 0;
      unsigned failedVerifications = 0;

      auto&& hintdbtx = db_->beginTransaction(TXHINTS, LMDB::Mode::ReadOnly);

      while (thisHeight < blockchain_->top()->getBlockHeight())
      {
         //grab blockheight
         thisHeight = stateStruct->blockHeight_.fetch_add(1, memory_order_relaxed);
         auto blockheader = blockchain_->getHeaderByHeight(thisHeight, 0xFF);
         auto fileMap = getFileMap(blockheader->getBlockFileNum());

         auto getID = [blockheader](const BinaryData&)->unsigned int
         {
            return blockheader->getThisID();
         };

         auto bdata = BlockData::deserialize(
            fileMap->data() + blockheader->getOffset(),
            blockheader->getBlockSize(),
            blockheader, getID, BlockData::CheckHashes::NoChecks);

         const auto& txns = bdata->getTxns();
         for (unsigned i = 1; i < txns.size(); i++)
         {
            const auto& txn = txns[i];

            try
            {
               //gather utxos
               auto utxomap = getUtxoMap(txn);

               //verify tx
               Armory::Signing::TransactionVerifier txV(*txn, utxomap);
               auto flags = txV.getFlags();

               if (blockheader->getTimestamp() > P2SH_TIMESTAMP)
                  flags |= SCRIPT_VERIFY_P2SH;

               if (txn->usesWitness_)
                  flags |= SCRIPT_VERIFY_SEGWIT;

               txV.setFlags(flags);

               if (txV.verify())
                  stateStruct->parsedCount_.fetch_add(1, memory_order_relaxed);
               else
                  ++failedVerifications;
            }
            catch (Armory::Signing::UnsupportedSigHashTypeException&)
            {
               stateStruct->unsupportedSigHash_.fetch_add(1, memory_order_relaxed);
            }
            catch (UnresolvedHashException&)
            {
               stateStruct->unresolvedHashes_.fetch_add(1, memory_order_relaxed);
            }
            catch (const exception& e)
            {
               unique_lock<mutex> lock(stateStruct->mu_);
               LOGERR << "+++ error at #" << thisHeight << ":" << i;
               LOGERR << "+++ strerr: " << e.what();
               stateStruct->unknownErrors_.fetch_add(1, memory_order_relaxed);
            }
         }

         if (thisHeight % 1000 == 0)
         {
            unique_lock<mutex> lock(stateStruct->mu_);
            auto tE = TIMER_READ_SEC("10blocks");
            TIMER_RESTART("10blocks");

            LOGINFO << "=== time elapsed: " << tE << " ===";

            LOGINFO << "current block: " << thisHeight;
            LOGINFO << "--- verified " << 
               stateStruct->parsedCount_.load(memory_order_relaxed) << " transactions";

            LOGINFO << "--- *encountered " <<
               stateStruct->unsupportedSigHash_.load(memory_order_relaxed) <<
               " unknown sighashes";

            LOGINFO << "--- *encountered " <<
               stateStruct->unresolvedHashes_.load(memory_order_relaxed) <<
               " unresolved hashes";

            LOGINFO << "--- ***encountered " <<
               stateStruct->unknownErrors_.load(memory_order_relaxed) <<
               " unknown errors";
         }
      }
   };

   vector<thread> parserThrVec;
   for (unsigned i = 1; i < DBSettings::threadCount(); i++)
      parserThrVec.push_back(thread(verifyBlockTx));

   verifyBlockTx();

   checkedTransactions_ = stateStruct->parsedCount_.load(memory_order_relaxed);

   for (auto& thr : parserThrVec)
      if (thr.joinable())
         thr.join();

   if (stateStruct->unresolvedHashes_.load(memory_order_relaxed) > 0)
      throw runtime_error("checkChain failed with unresolved hash errors");

   if (stateStruct->unsupportedSigHash_.load(memory_order_relaxed) > 0)
      throw runtime_error("checkChain failed with unsupported sig hash errors");

   if (stateStruct->unknownErrors_.load(memory_order_relaxed) > 0)
      throw runtime_error("checkChain failed with unknown errors");

   LOGINFO << "Done checking chain";
}

/////////////////////////////////////////////////////////////////////////////
void Builder::verifyTxFilters()
{
   if (DBSettings::getDbType() != ARMORY_DB_FULL)
      return;

   LOGINFO << "verifying txfilters integrity";

   atomic<unsigned> fileCounter;
   fileCounter.store(0, memory_order_relaxed);

   mutex resultMutex;
   set<unsigned> damagedFilters;

   auto file_id_map = blockchain_->mapIDsPerBlockFile();
   auto checkThr = [&](void)->void
   {
      auto&& tx = db_->beginTransaction(TXFILTERS, LMDB::Mode::ReadOnly);

      set<unsigned> mismatchedFilters;

      while (1)
      {
         unsigned mismatchCount = 0;
         unsigned fileNum = fileCounter.fetch_add(1, memory_order_relaxed);
         auto file_id_iter = file_id_map.find(fileNum);
         if (file_id_iter == file_id_map.end())
         {
            if (fileNum < blockFiles_.fileCount())
            {
               LOGINFO << "no recorded block headers in file #" << fileNum;
               LOGINFO << "skipping";
               continue;
            }

            if (mismatchedFilters.size() > 0)
            {
               auto lock = unique_lock<mutex>(resultMutex);
               damagedFilters.insert(
                  mismatchedFilters.begin(), mismatchedFilters.end());
            }

            return;
         }

         auto& idset = file_id_iter->second;

         try
         {
            auto pool = db_->getFilterPoolRefForFileNum(fileNum);
            auto filters = pool.getFilterPoolPtr();

            auto match_count = 0;
            for (const auto& filter : filters)
            {
               //check filter blockid is for this block file
               auto id_iter = idset.find(filter.getBlockKey());
               if (id_iter != idset.end())
                  ++match_count;
            }

            mismatchCount = idset.size() - match_count;

            if (mismatchCount > 0)
            {
               mismatchedFilters.insert(fileNum);
               LOGWARN << mismatchCount << " mismatches in txfilter for file #" << fileNum;
            }
         }
         catch (const runtime_error&)
         {
            mismatchedFilters.insert(fileNum);
            LOGWARN << "couldnt get filter pool for file: " << fileNum;
         }
      }
   };

   vector<thread> thrs;
   for (unsigned i = 1; i < DBSettings::threadCount(); i++)
      thrs.push_back(thread(checkThr));
   checkThr();

   for (auto& thr : thrs)
      if (thr.joinable())
         thr.join();
   
   if (damagedFilters.size() == 0)
   {
      LOGINFO << "done checking txfilters";
      return;
   }

   LOGWARN << damagedFilters.size() << " damaged filters, repairing";
   repairTxFilters(damagedFilters);
}

/////////////////////////////////////////////////////////////////////////////
void Builder::repairTxFilters(const set<unsigned>& badFilters)
{
   {
      LOGINFO << "clearing damaged filters";

      auto&& tx = db_->beginTransaction(TXFILTERS, LMDB::Mode::ReadWrite);

      for (auto& filter : badFilters)
      {
         auto&& dbkey = DBUtils::getFilterPoolKey(filter);
         db_->deleteValue(TXFILTERS, dbkey);
      }
   }

   //no preload nor prefetch
   BlockDataLoader bdl(blockFiles_.folderPath());

   vector<unsigned> idVec;
   for (auto& id : badFilters)
      idVec.push_back(id);

   atomic<unsigned> counter;
   counter.store(0, memory_order_relaxed);

   auto fixFilterThr = [&](void)->void
   {
      while (counter.load(memory_order_relaxed) < badFilters.size())
      {
         auto counterID = counter.fetch_add(1, memory_order_relaxed);
         auto fileID = *(idVec.cbegin() + counterID);
         auto&& blockfilemapptr = bdl.get(fileID);

         this->reprocessTxFilter(blockfilemapptr, fileID);
      }
   };

   vector<thread> thrs;
   for (unsigned i = 1; i < DBSettings::threadCount(); i++)
      thrs.push_back(thread(fixFilterThr));
   fixFilterThr();

   for (auto& thr : thrs)
      if (thr.joinable())
         thr.join();
}

/////////////////////////////////////////////////////////////////////////////
void Builder::reprocessTxFilter(
   shared_ptr<BlockDataFileMap> blockfilemappointer, unsigned fileID)
{
   auto ptr = blockfilemappointer->getPtr();

   //ptr is null if we're out of block files
   if (ptr == nullptr)
      return;

   map<uint32_t, shared_ptr<BlockData>> bdMap;

   auto getID = [&](const BinaryData& heahder_hash)->uint32_t
   {
      try {
         auto header = blockchain_->getHeaderByHash(heahder_hash);
         return header->getThisID();
      } catch (...) {
         LOGERR << "no header in db matches this hash!";
         return UINT32_MAX;
      }
   };

   auto tallyBlocks =
      [&](const uint8_t* data, size_t size, size_t offset)->bool
   {
      //deser full block, check merkle
      shared_ptr<BlockData> bd;
      BinaryRefReader brr(data, size);

      try {
         bd = BlockData::deserialize(data, size, nullptr,
            getID, true, false);
      } catch (const BtcUtils::BlockDeserializingException &e) {
         LOGERR << "block deser except: " << e.what();
         LOGERR << "block fileID: " << fileID;
         return false;
      } catch (const exception &e) {
         LOGERR << "exception: " << e.what();
         return false;
      } catch (...) {
         //deser failed, ignore this block
         LOGERR << "unknown block deser exception";
         return false;
      }

      //block is valid, add to container
      bd->setFileID(fileID);
      bd->setOffset(offset);
      bdMap.emplace(bd->uniqueID(), move(bd));
      return true;
   };
   parseBlockFile(bdl, 0, tallyBlocks);

   {
      //delete existing txfilter
      auto tx = db_->beginTransaction(TXFILTERS, LMDB::Mode::ReadWrite);
      auto dbkey = DBUtils::getFilterPoolKey(fileID);
      db_->deleteValue(TXFILTERS, dbkey);

      //tally all block filters
      std::map<uint32_t, BlockHashVector> allFilters;
      for (const auto& bd_pair : bdMap) {
         allFilters.emplace(
            bd_pair.first, bd_pair.second->getTxFilter());
      }

      TxFilterPool pool(allFilters);

      //commit to db
      db_->putFilterPoolForFileNum(fileID, pool);
   }

   LOGINFO << "fixed txfilter for file #" << fileID;
}
#endif

/////////////////////////////////////////////////////////////////////////////
void Builder::cycleDatabases()
{
   db_->closeDatabases();
   db_->openDatabases(Config::Pathing::dbDir());
}

/////////////////////////////////////////////////////////////////////////////
void Builder::checkTxHintsIntegrity()
{
   BlockDataLoader bdl(blockFiles_, BlockOffset{0, 0});
   unsigned threadcount = std::min(
      size_t(Config::DBSettings::threadCount()),
      bdl.size()
   );

   auto parserThread = [this, &bdl]()
   {
      while (true) {
         auto fileCopy = bdl.getNextCopy();
         if (!fileCopy.isValid()) {
            return;
         }

         if (fileCopy.fileID % 100 == 0) {
            LOGINFO << "checking txhints for file " << fileCopy.fileID;
         }

         //tally all blocks in file
         std::list<std::shared_ptr<BlockData>> bdList;
         parseBlockFile(fileCopy,
            [&bdList](const uint8_t* data, size_t size, size_t)->bool
            {
               try {
                  auto bd = BlockData::deserialize(
                     data, size, nullptr,
                     BlockData::CheckHashes::FullHints);
                  bdList.emplace_back(bd);
                  return true;
               } catch (...) {
                  return false;
               }
            }
         );

         //check hashes can be resolved via tx hints db
         int missedCount = 0;
         auto dbtx = db_->beginTransaction(DB_SELECT::TXHINTS, LMDB::Mode::ReadOnly);
         for (const auto& blockData : bdList) {
            //skip blocks not in the main chain
            auto headerPtr = blockchain_->getHeaderByHash(blockData->getHash());
            if (headerPtr == nullptr || !headerPtr->isMainBranch()) {
               continue;
            }

            const auto& txns = blockData->getTxns();
            for (const auto& txn : txns) {
               auto hash4 = txn->getHash().getSliceRef(0, 4);
               BinaryRefReader brrHints = db_->getValueRef(
                  DB_SELECT::TXHINTS, DbPrefix::TXHINTS, hash4);

               uint32_t valSize = brrHints.getSize();
               if (valSize < 6) {
                  ++missedCount;
                  return;
               }

               bool hit = false;
               uint32_t numHints = (uint32_t)brrHints.get_var_int();
               for (uint32_t i = 0; i < numHints; i++) {
                  BinaryDataRef hint = brrHints.get_BinaryDataRef(6);

                  //check this key is on the main branch
                  auto hintRef = hint.getSliceRef(0, 4);
                  auto blockId = DBUtils::hgtxToHeight(hintRef);
                  if (blockId == headerPtr->getUniqueID()) {
                     hit = true;
                     break;
                  }
               }

               if (!hit) {
                  ++missedCount;
               }
            }
         }

         if (missedCount != 0) {
            LOGERR << "missed " << missedCount << " hashes" <<
               " in file " << fileCopy.fileID;
         }
      }
   };

   std::vector<std::thread> threads;
   LOGINFO << "checking txhints for " << bdl.size() <<
      " files on " << threadcount << " threads";
   threads.reserve(threadcount);
   for (unsigned i=1; i<threadcount; i++) {
      threads.emplace_back(std::thread(parserThread));
   }
   parserThread();

   for (auto& thr : threads) {
      if (thr.joinable()) {
         thr.join();
      }
   }
   LOGINFO << "done checking txhints";
}

unsigned Builder::getCheckedTxCount() const
{
   return checkedTransactions_;
}

////////
void Builder::mergeContext(ScannerContext& incoming)
{
   scannerCtx_->merge(incoming);
}
