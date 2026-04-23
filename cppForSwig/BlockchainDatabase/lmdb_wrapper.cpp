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

#include <map>
#include <vector>
#include <set>
#include <cmath>
#include <cstring>
#include <arpa/inet.h>

#include "lmdb_wrapper.h"
#include <Utils/BtcUtils.h>
#include <Utils/varint.h>
#include <Utils/DBUtils.h>
#include <Utils/ArmoryConfig.h>
#include <Utils/BitcoinSettings.h>
#include <Utils/ArmoryErrors.h>
#include <Utils/BCTX.h>
#include <Utils/UniversalTimer.h>
#include <TxClasses.h>

#include "BlockObj.h"
#include "StoredBlockObj.h"
#include "txio.h"
#include "BlockDataMap.h"
#include "Blockchain.h"
#include "TxHashFilters.h"

using namespace Armory;

extern const std::vector<DB_SELECT> SUPERNODEDBS{
   DB_SELECT::HEADERS, DB_SELECT::SCRADDR,
   DB_SELECT::ZERO_CONF
};
extern const std::vector<DB_SELECT> SUPERNODEHASHTABLES{
   DB_SELECT::TXOUTS, DB_SELECT::TXINS,
   DB_SELECT::TXHINTS
};

extern const std::vector<DB_SELECT> FULLNODEDBS{
   DB_SELECT::HEADERS, DB_SELECT::SCRADDR,
   DB_SELECT::TXOUTS, DB_SELECT::TXINS,
   DB_SELECT::KNOWNHASHES, DB_SELECT::ZERO_CONF
};
extern const std::vector<DB_SELECT> FULLNODEHASHTABLES{
   DB_SELECT::TXHINTS
};

extern const std::vector<DB_SELECT> BARENODEDBS{
   DB_SELECT::HEADERS, DB_SELECT::SCRADDR,
   DB_SELECT::TXOUTS, DB_SELECT::TXINS,
   DB_SELECT::KNOWNHASHES, DB_SELECT::TXHINTS,
   DB_SELECT::ZERO_CONF
};

extern const std::map<DB_SELECT, size_t> MAPSIZES{
   { DB_SELECT::HEADERS,         10 * 1024 * 1024 * 1024ULL },
   { DB_SELECT::TXHINTS,          5 * 1024 * 1024 * 1024ULL },
   { DB_SELECT::SSH,           2000 * 1024 * 1024 * 1024ULL },
   { DB_SELECT::SUBSSH,        2000 * 1024 * 1024 * 1024ULL },
   { DB_SELECT::SUBSSH_META,      1 * 1024 * 1024 * 1024ULL },
   { DB_SELECT::ZERO_CONF,       10 * 1024 * 1024 * 1024ULL },
   { DB_SELECT::TXFILTERS,      100 * 1024 * 1024 * 1024ULL },
   { DB_SELECT::SPENTNESS,     2000 * 1024 * 1024 * 1024ULL },
   { DB_SELECT::SCRADDR,        500 * 1024 * 1024 * 1024ULL },
   { DB_SELECT::TXOUTS,        2000 * 1024 * 1024 * 1024ULL },
   { DB_SELECT::TXINS,         2000 * 1024 * 1024 * 1024ULL },
   { DB_SELECT::KNOWNHASHES,      1 * 1024 * 1024 * 1024ULL }
};

////////////////////////////////////////////////////////////////////////////////
// exceptions
FilterException::FilterException(const std::string& err) :
   std::runtime_error(err)
{}

LmdbWrapperException::LmdbWrapperException(const std::string& err) :
   std::runtime_error(err)
{}

////////////////////////////////////////////////////////////////////////////////
// DBTransaction
DBTransaction::DBTransaction(LMDB::Transaction&& dbtx) :
   dbtx_(std::move(dbtx))
{}

DBTransaction::~DBTransaction()
{}

void DBTransaction::insert(const LMDB::DataRef& key, const LMDB::DataRef& val)
{
   dbtx_.insert(key, val);
}

void DBTransaction::erase(const LMDB::DataRef& key)
{
   dbtx_.erase(key);
}

LMDB::DataRef DBTransaction::get(const LMDB::DataRef& key) const
{
   return dbtx_.get(key);
}

DBIterator DBTransaction::getIterator() const
{
   return DBIterator{LMDB::Iterator{&dbtx_}};
}

////////////////////////////////////////////////////////////////////////////////
// DBIterator
DBIterator::DBIterator(LMDB::Iterator&& iter) :
   iter_(std::move(iter)), isDirty_(true)
{}

DBIterator::~DBIterator()
{}

void DBIterator::resetReaders()
{
   currKeyReader_.resetPosition();
   currValueReader_.resetPosition();
}

////////
bool DBIterator::isNull() const
{
   return !iter_.isValid();
}

bool DBIterator::isValid() const
{
   return iter_.isValid();
}

bool DBIterator::isValid(DbPrefix dbpref)
{
   if (!isValid()) {
      return false;
   }
   readIterData();
   if (currKey_.empty()) {
      return false;
   }
   return currKey_.getPtr()[0] == (uint8_t)dbpref;
}

////////
bool DBIterator::advance(DbPrefix prefix)
{
   advance();
   return isValid(prefix);
}

bool DBIterator::advanceAndRead()
{
   if (!advance()) {
      return false;
   }
   return readIterData();
}

bool DBIterator::advanceAndRead(DbPrefix prefix)
{
   if (!advance(prefix)) {
      return false;
   }
   return readIterData();
}

////////
BinaryData DBIterator::getKey() const
{
   if(isDirty_) {
      LOGERR << "Returning dirty key ref";
      return {};
   }
   return currKey_;
}

BinaryData DBIterator::getValue() const
{
   if(isDirty_) {
      LOGERR << "Returning dirty value ref";
      return {};
   }
   return currValue_;
}

BinaryDataRef DBIterator::getKeyRef() const
{
   if(isDirty_) {
      LOGERR << "Returning dirty key ref";
      return {};
   }
   return currKeyReader_.getRawRef();
}

BinaryDataRef DBIterator::getValueRef() const
{
   if(isDirty_) {
      LOGERR << "Returning dirty value ref";
      return {};
   }
   return currValueReader_.getRawRef();
}

BinaryRefReader& DBIterator::getKeyReader() const
{
   if(isDirty_) {
      LOGERR << "Returning dirty key reader";
   }
   return currKeyReader_;
}

BinaryRefReader& DBIterator::getValueReader() const
{
   if(isDirty_)
      LOGERR << "Returning dirty value reader";
   return currValueReader_;
}

////////
bool DBIterator::seekTo(DbPrefix pref, BinaryDataRef key)
{
   BinaryWriter bw(key.getSize() + 1);
   bw.put_uint8_t((uint8_t)pref);
   bw.put_BinaryData(key);
   return seekTo(bw.getDataRef());
}

bool DBIterator::seekToExact(DbPrefix pref, BinaryDataRef key)
{
   BinaryWriter bw(key.getSize() + 1);
   bw.put_uint8_t((uint8_t)pref);
   bw.put_BinaryData(key);
   return seekToExact(bw.getDataRef());
}

bool DBIterator::seekToStartsWith(BinaryDataRef key)
{
   if (!seekTo(key)) {
      return false;
   }
   return checkKeyStartsWith(key);
}

bool DBIterator::seekToStartsWith(DbPrefix prefix)
{
   BinaryWriter bw(1);
   bw.put_uint8_t((uint8_t)prefix);
   if (!seekTo(bw.getDataRef())) {
      return false;
   }
   return checkKeyStartsWith(bw.getDataRef());
}

bool DBIterator::seekToStartsWith(DbPrefix pref, BinaryDataRef key)
{
   if (!seekTo(pref, key)) {
      return false;
   }
   return checkKeyStartsWith(pref, key);
}

bool DBIterator::seekToBefore(DbPrefix prefix)
{
   BinaryWriter bw(1);
   bw.put_uint8_t((uint8_t)prefix);
   return seekToBefore(bw.getDataRef());
}

bool DBIterator::seekToBefore(DbPrefix pref, BinaryDataRef key)
{
   BinaryWriter bw(key.getSize() + 1);
   bw.put_uint8_t((uint8_t)pref);
   bw.put_BinaryData(key);
   return seekToBefore(bw.getDataRef());
}

////////
bool DBIterator::checkKeyExact(BinaryDataRef key)
{
   if (isDirty_ && !readIterData()) {
      return false;
   }
   return (key==currKeyReader_.getRawRef());
}

bool DBIterator::checkKeyExact(DbPrefix prefix, BinaryDataRef key)
{
   BinaryWriter bw(key.getSize() + 1);
   bw.put_uint8_t((uint8_t)prefix);
   bw.put_BinaryData(key);
   if (isDirty_ && !readIterData()) {
      return false;
   }
   return bw.getDataRef() == currKeyReader_.getRawRef();
}

bool DBIterator::checkKeyStartsWith(BinaryDataRef key)
{
   if (isDirty_ && !readIterData()) {
      return false;
   }
   return currKeyReader_.getRawRef().startsWith(key);
}

bool DBIterator::verifyPrefix(DbPrefix prefix, bool advanceReader)
{
   if (isDirty_ && !readIterData()) {
      return false;
   }
   if (currKeyReader_.getSizeRemaining() < 1) {
      return false;
   }
   if (advanceReader) {
      return currKeyReader_.get_uint8_t() == (uint8_t)prefix;
   } else {
      return currKeyReader_.getRawRef()[0] == (uint8_t)prefix;
   }
}

bool DBIterator::checkKeyStartsWith(DbPrefix prefix, BinaryDataRef key)
{
   BinaryWriter bw(key.getSize() + 1);
   bw.put_uint8_t((uint8_t)prefix);
   bw.put_BinaryData(key);
   return checkKeyStartsWith(bw.getDataRef());
}

////////
bool DBIterator::seekTo(BinaryDataRef key)
{
   iter_.seek(
      LMDB::DataRef{key.getSize(), key.getPtr()},
      LMDB::Iterator::SeekBy::GE);
   return readIterData();
}

bool DBIterator::seekToExact(BinaryDataRef key)
{
   if (!seekTo(key)) {
      return false;
   }
   return checkKeyExact(key);
}

bool DBIterator::seekToBefore(BinaryDataRef key)
{
   iter_.seek(
      LMDB::DataRef{key.getSize(), key.getPtr()},
      LMDB::Iterator::SeekBy::LE);
   return readIterData();
}

////////
bool DBIterator::advance()
{
   ++iter_;
   isDirty_ = true;
   return isValid();
}

bool DBIterator::retreat()
{
   --iter_;
   isDirty_ = true;
   return isValid();
}

////////
bool DBIterator::readIterData()
{
   if (!isValid()){
      isDirty_ = true;
      return false;
   }

   currKey_ = BinaryDataRef{
      (uint8_t*)iter_.key().mv_data,
      iter_.key().mv_size};
   currValue_ = BinaryDataRef{
      (uint8_t*)iter_.value().mv_data,
      iter_.value().mv_size};

   currKeyReader_.setNewData(currKey_);
   currValueReader_.setNewData(currValue_);
   isDirty_ = false;
   return true;
}

////////
bool DBIterator::seekToFirst()
{
   iter_.toFirst();
   return readIterData();
}

bool DBIterator::seekToLast()
{
   iter_.toLast();
   return readIterData();
}

////////////////////////////////////////////////////////////////////////////////
// LMDBBlockDatabase
LMDBBlockDatabase::LMDBBlockDatabase(const std::filesystem::path& dbdir) :
   dbDir_{dbdir}
{}

LMDBBlockDatabase::~LMDBBlockDatabase()
{
   closeDatabases();
}

////////
std::shared_ptr<DatabaseContainer> LMDBBlockDatabase::getDbPtr(
   DB_SELECT db) const
{
   auto iter = dbMap_.find(db);
   if (iter == dbMap_.end()) {
      throw LmdbWrapperException("unexpected db");
   }
   return iter->second;
}

std::shared_ptr<DatabaseContainer> LMDBBlockDatabase::getHashTablePtr(
   DB_SELECT db, uint8_t id) const
{
   auto iter = dbHashTables_.find(db);
   if (iter == dbHashTables_.end()) {
      throw LmdbWrapperException("unexpected hash table");
   }
   return iter->second[id];
}

////////
std::unique_ptr<DBTransaction> LMDBBlockDatabase::beginTransaction(
   DB_SELECT db, LMDB::Mode mode) const
{
   auto dbObj = getDbPtr(db);
   return dbObj->beginTransaction(mode);
}

std::unique_ptr<DBTransaction> LMDBBlockDatabase::beginHashTableTx(
   DB_SELECT db, uint8_t id, LMDB::Mode mode) const
{
   auto dbObj = getHashTablePtr(db, id);
   return dbObj->beginTransaction(mode);
}

/////////////////////////////////////////////////////////////////////////////
// The dbType and pruneType inputs are left blank if you are just going to
// take whatever is the current state of database.  You can choose to
// manually specify them, if you want to throw an error if it's not what you
// were expecting
void LMDBBlockDatabase::openDatabases()
{
   LOGINFO << "Opening databases...";
   LOGINFO << "dbmode: " << Config::DBSettings::getDbModeStr();

   if (!Config::BitcoinSettings::isInitialized()) {
      LOGERR << " must set magic bytes and genesis block";
      LOGERR << "           before opening databases.";
      throw LmdbWrapperException("magic bytes not set");
   }

   // Just in case this isn't the first time we tried to open it.
   closeDatabases();

   dbMap_.clear();
   std::vector<DB_SELECT> dbSet;
   std::vector<DB_SELECT> hashTables;
   switch (Config::DBSettings::getDbType())
   {
      case ARMORY_DB_TYPE::Bare:
         dbSet = BARENODEDBS;
         break;

      case ARMORY_DB_TYPE::Full:
         dbSet = FULLNODEDBS;
         hashTables = FULLNODEHASHTABLES;
         break;

      case ARMORY_DB_TYPE::Super:
         dbSet = SUPERNODEDBS;
         hashTables = SUPERNODEHASHTABLES;
         break;
   }

   //regular dbs
   for (const auto& currDb : dbSet) {
      const auto& dbName = DatabaseContainer::getDbName(currDb);
      dbMap_.emplace(currDb,
         std::make_shared<DatabaseContainer>(
            dbDir_, dbName, MAPSIZES.at(currDb)));
      openDB(currDb);
   }

   //hashtables
   for (const auto& currDb : hashTables) {
      auto size = MAPSIZES.at(currDb);
      const auto& tableName = DatabaseContainer::getDbName(currDb);
      std::filesystem::path hashDir = dbDir_ / tableName;
      if (!FileUtils::pathExists(hashDir, 0)) {
         std::filesystem::create_directory(hashDir);
      }

      auto emplaceResult = dbHashTables_.emplace(currDb,
         std::vector<std::shared_ptr<DatabaseContainer>>{256});
      for (uint32_t index = 0; index < 256; index++) {
         auto name = std::format("{}_{:x}", tableName, index);
         auto db = std::make_shared<DatabaseContainer>(
            hashDir, name, size);
         db->open();
         emplaceResult.first->second[index] = db;
      }
   }

   //check/seed headers SDBI
   try {
      auto sdbi = getStoredDBInfo(DB_SELECT::HEADERS, 0xFFFF);
      if (Config::BitcoinSettings::getMagicBytes() != sdbi.magicBytes) {
         LOGERR << "magic bytes mismatch, aborting";
         exit(-2);
      } else if (Config::DBSettings::getDbType() != sdbi.armoryType) {
         LOGERR << "db type mismatch, aborting";
         exit(-3);
      }
   } catch (const LmdbWrapperException&) {
      //fresh db, seed headers sdbi
      StoredDBInfo sdbi;
      sdbi.armoryType = Config::DBSettings::getDbType();
      sdbi.magicBytes = Config::BitcoinSettings::getMagicBytes();
      auto tx = beginTransaction(DB_SELECT::HEADERS, LMDB::Mode::ReadWrite);
      putStoredDBInfo(DB_SELECT::HEADERS, sdbi, 0xFFFF);
   }

   if (Config::DBSettings::getDbType() == ARMORY_DB_TYPE::Super) {
      loadHeightToIdMap();
   }
   dbIsOpen_ = true;
}

bool LMDBBlockDatabase::databasesAreOpen() const
{
   return dbIsOpen_;
}

////////
void LMDBBlockDatabase::closeDatabases()
{
   for (auto& dbPair : dbMap_) {
      dbPair.second->close();
   }
   dbMap_.clear();
   for (auto& table : dbHashTables_) {
      for (auto& db : table.second) {
         db->close();
      }
   }
   dbHashTables_.clear();
   dbIsOpen_ = false;
}

void LMDBBlockDatabase::cycleDatabase(DB_SELECT db)
{
   auto dbPtr = getDbPtr(db);
   dbPtr->close();
   dbPtr->open();
}

void LMDBBlockDatabase::resetHistoryDatabases()
{
   if (Config::DBSettings::getDbType() != ARMORY_DB_TYPE::Super) {
      auto dbTxouts = getDbPtr(DB_SELECT::TXOUTS);
      auto dbTxins = getDbPtr(DB_SELECT::TXINS);
      auto dbHints = getDbPtr(DB_SELECT::TXHINTS);
      auto dbHashes = getDbPtr(DB_SELECT::KNOWNHASHES);
      closeDatabases();

      dbTxouts->eraseOnDisk();
      dbTxins->eraseOnDisk();
      dbHints->eraseOnDisk();
      dbHashes->eraseOnDisk();
   } else {
      auto db_subssh = getDbPtr(DB_SELECT::SUBSSH);
      auto db_subssh_meta = getDbPtr(DB_SELECT::SUBSSH_META);
      auto db_ssh = getDbPtr(DB_SELECT::SSH);
      auto db_spentness = getDbPtr(DB_SELECT::SPENTNESS);
      closeDatabases();

      db_subssh->eraseOnDisk();
      db_subssh_meta->eraseOnDisk();
      db_ssh->eraseOnDisk();
      db_spentness->eraseOnDisk();
   }
   openDatabases();
}

void LMDBBlockDatabase::destroyAndResetDatabases()
{
   // We want to make sure the database is restarted with the same parameters
   // it was called with originally
   {
      //save dbMap because closeDatabases clears it
      auto dbMap = dbMap_;
      auto hashTables = dbHashTables_;
      closeDatabases();
      for (auto& dbPair : dbMap) {
         dbPair.second->eraseOnDisk();
      }
      for (auto& table : hashTables) {
         for (auto& db : table.second) {
            db->eraseOnDisk();
         }
      }
   }

   // Reopen the databases with the exact same parameters as before
   // The close & destroy operations shouldn't have changed any of that.
   openDatabases();
}

/////////////////////////////////////////////////////////////////////////////
BinaryData LMDBBlockDatabase::getDBKeyForHash(BinaryDataRef txhash,
   uint8_t expectedDupId) const
{
   throw std::runtime_error("[LMDBBlockDatabase::getDBKeyForHash] fix me");
   #if 0
   if (txhash.getSize() < 4) {
      LOGWARN << "txhash is less than 4 bytes long";
      return {};
   }
   auto hash4 = txhash.getSliceRef(0, 4);

   auto txHints = beginTransaction(DB_SELECT::TXHINTS, LMDB::Mode::ReadOnly);
   BinaryRefReader brrHints = getValueRef(DB_SELECT::TXHINTS, DbPrefix::TXHINTS, hash4);

   uint32_t valSize = brrHints.getSize();
   if (valSize < 6) {
      return {};
   }
   uint32_t numHints = (uint32_t)brrHints.get_var_int();

   if (Config::DBSettings::getDbType() != ARMORY_DB_TYPE::Super) {
      uint32_t height;
      uint8_t  dup;
      uint16_t txIdx;
      for (uint32_t i = 0; i < numHints; i++) {
         BinaryDataRef hint = brrHints.get_BinaryDataRef(6);
         BinaryRefReader brrHint(hint);
         DBUtils::readBlkDataKeyNoPrefix(brrHint, height, dup, txIdx);

         auto txKey = DBUtils::getBlkDataKey(height, dup, txIdx);
         auto dbVal = getValueNoCopy(DB_SELECT::TXHINTS, txKey.getRef());
         if (dbVal.getSize() < 36) {
            continue;
         }
         auto txHashRef = dbVal.getSliceRef(4, 32);

         if (txHashRef != txhash) {
            continue;
         }
         return txKey.getSliceCopy(1, 6);
      }
   } else {
      BinaryData forkedMatch;
      bool offChainHints = false;
      for (uint32_t i = 0; i < numHints; i++) {
         BinaryDataRef hint = brrHints.get_BinaryDataRef(6);

         //check this key is on the main branch
         auto hintRef = hint.getSliceRef(0, 4);

         //check hash matches
         auto txhashfromdb = getTxHashForLdbKey(hint, nullptr);
         if (txhash != txhashfromdb) {
            continue;
         }
         return hint;
      }

      if (forkedMatch.empty()) {
         if (brrHints.getSizeRemaining() != 0) {
            LOGWARN << " bytes remaining for this hint";
         }
         if (offChainHints) {
            LOGWARN << " had off chain hits";
         }
      }
      return forkedMatch;
   }
   return {};
   #endif
}

////////////////////////////////////////////////////////////////////////////////
bool LMDBBlockDatabase::fillStoredSubHistory(StoredScriptHistory& ssh,
   unsigned start, unsigned end) const
{
   if (Config::DBSettings::getDbType() == ARMORY_DB_TYPE::Super) {
      return fillStoredSubHistory_Super(ssh, start, end);
   } else {
      auto subsshtx = beginTransaction(DB_SELECT::SUBSSH, LMDB::Mode::ReadOnly);
      auto subsshIter = subsshtx->getIterator();

      BinaryWriter dbkey_withHgtX;
      dbkey_withHgtX.put_uint8_t((uint8_t)DbPrefix::SCRIPT);
      dbkey_withHgtX.put_BinaryData(ssh.uniqueKey);

      if (start != 0) {
         dbkey_withHgtX.put_BinaryData(DBUtils::heightAndDupToHgtx(start, 0));
      }

      if (!subsshIter.seekTo(dbkey_withHgtX.getDataRef())) {
         return false;
      }

      // Now start iterating over the sub histories
      std::map<BinaryData, StoredSubHistory>::iterator iter;
      size_t numTxioRead = 0;
      do {
         size_t _sz = subsshIter.getKeyRef().getSize();
         BinaryDataRef keyNoPrefix = subsshIter.getKeyRef().getSliceRef(1, _sz - 1);
         if (!keyNoPrefix.startsWith(ssh.uniqueKey)) {
            break;
         }
         std::pair<BinaryData, StoredSubHistory> keyValPair;
         keyValPair.first = keyNoPrefix.getSliceCopy(_sz - 5, 4);
         keyValPair.second.unserializeDBKey(subsshIter.getKeyRef());

         //iter is at the right ssh, make sure hgtX <= endBlock
         if (keyValPair.second.height > end) {
            break;
         }
         //skip invalid dupIDs
         keyValPair.second.unserializeDBValue(subsshIter.getValueReader());
         iter = ssh.subHistMap.emplace(keyValPair).first;
         numTxioRead += iter->second.txioMap.size();
      } while (subsshIter.advanceAndRead(DbPrefix::SCRIPT));
      return true;
   }
}

////////////////////////////////////////////////////////////////////////////////
unsigned LMDBBlockDatabase::getShardIdForHeight(unsigned height) const
{
   auto hiMap = heightToBatchId_.get();
   if (hiMap->empty()) {
      return UINT32_MAX;
   }
   auto height_iter = hiMap->lower_bound(height);
   if (height_iter == hiMap->end()) {
      return hiMap->rbegin()->second;
   }
   if (height_iter->first > height && height_iter != hiMap->begin()) {
      --height_iter;
   }
   return height_iter->second;
}

unsigned LMDBBlockDatabase::getNextShardIdForHeight(unsigned height) const
{
   auto hiMap = heightToBatchId_.get();
   auto height_iter = hiMap->upper_bound(height);
   if (height_iter == hiMap->end()) {
      return UINT32_MAX;
   }
   return height_iter->second;
}

bool LMDBBlockDatabase::fillStoredSubHistory_Super(
   StoredScriptHistory& ssh, unsigned start, unsigned end) const
{
   auto meta_tx = beginTransaction(DB_SELECT::SUBSSH_META, LMDB::Mode::ReadOnly);

   //convert height range to batch id range
   auto start_id = getShardIdForHeight(start);
   if (start_id == UINT32_MAX) {
      return true;
   }
   auto end_id = getNextShardIdForHeight(end);

   //prepare for subssh db parsing
   BinaryWriter bwKey(4 + ssh.uniqueKey.getSize());
   bwKey.put_uint32_t(0);
   bwKey.put_BinaryData(ssh.uniqueKey);

   auto keyRef = bwKey.getDataRef();
   auto ptr = (uint8_t*)keyRef.getPtr();

   //get subssh summary iterator positioned at <= start_id
   auto ssh_lower_bound = ssh.subsshSummary.lower_bound(start_id);
   if (ssh_lower_bound == ssh.subsshSummary.end()) {
      return true;
   }
   if (ssh_lower_bound->first > start_id &&
      ssh_lower_bound != ssh.subsshSummary.begin()) {
      --ssh_lower_bound;
   }

   //grab db iterator
   auto subsshtx = beginTransaction(DB_SELECT::SUBSSH, LMDB::Mode::ReadOnly);
   auto dbIter = subsshtx->getIterator();

   while (ssh_lower_bound != ssh.subsshSummary.end()) {
      //break if iterator is past end_id
      if (ssh_lower_bound->first > end_id) {
         break;
      }

      //grab meta entry for batch id
      BinaryWriter bw_meta(8);
      bw_meta.put_uint32_t(ssh_lower_bound->first, BE);
      bw_meta.put_uint32_t(0);
      LMDB::DataRef keyCAR{8, bw_meta.getDataRef().getPtr()};
      auto meta_value = subsshtx->get(keyCAR);
      if (meta_value.len == 0) {
         LOGWARN << "missing meta entry at batch id " << ssh_lower_bound->first;
         ++ssh_lower_bound;
         continue;
      }

      //grab height offsets
      BinaryRefReader meta_refreader{
         (const uint8_t*)meta_value.data, meta_value.len};
      auto height_offset = meta_refreader.get_uint32_t();
      auto spent_offset = meta_refreader.get_uint32_t();

      //set batch id in subssh key
      auto id_ptr = (uint8_t*)&ssh_lower_bound->first;
      ptr[0] = id_ptr[3];
      ptr[1] = id_ptr[2];
      ptr[2] = id_ptr[1];
      ptr[3] = id_ptr[0];

      //set iterator at subssh key
      if (!dbIter.seekToExact(keyRef)) {
         LOGWARN << "missing subssh expected batch id";
         ++ssh_lower_bound;
         continue;
      }

      ssh.decompressManySubssh(dbIter.getValueRef(),
         height_offset, spent_offset,
         start, end);
      ++ssh_lower_bound;
   }
   return true;
}

////////
bool LMDBBlockDatabase::getStoredScriptHistorySummary(StoredScriptHistory& ssh,
   BinaryDataRef scrAddr) const
{
   ssh.clear();
   auto tx = beginTransaction(DB_SELECT::SSH, LMDB::Mode::ReadOnly);
   auto ldbIter = tx->getIterator();
   bool has = false;

   if (ldbIter.seekToExact(DbPrefix::SCRIPT, scrAddr)) {
      ssh.unserializeDBKey(ldbIter.getKeyRef());
      ssh.unserializeDBValue(ldbIter.getValueRef());
      has = true;
   }
   return has;
}

bool LMDBBlockDatabase::getStoredScriptHistory(StoredScriptHistory& ssh,
   BinaryDataRef scrAddr, uint32_t startBlock, uint32_t endBlock) const
{
   ssh.uniqueKey = scrAddr;
   if (!fillStoredSubHistory(ssh, startBlock, endBlock)) {
      return false;
   }
   return true;
}

bool LMDBBlockDatabase::getStoredSubHistoryAtHgtX(StoredSubHistory& subssh,
   const BinaryDataRef scrAddrStr, const BinaryData& hgtX) const
{
   BinaryWriter bw(scrAddrStr.getSize() + hgtX.getSize());
   bw.put_BinaryData(scrAddrStr);
   bw.put_BinaryData(hgtX);
   return getStoredSubHistoryAtHgtX(subssh, bw.getDataRef());
}

bool LMDBBlockDatabase::getStoredSubHistoryAtHgtX(StoredSubHistory& subssh,
   const BinaryData& dbkey) const
{
   if (Config::DBSettings::getDbType() == ARMORY_DB_TYPE::Super) {
      LOGERR << "deprecated in supernode";
      throw std::runtime_error("deprecated in supernode");
   }

   auto tx = beginTransaction(DB_SELECT::SUBSSH, LMDB::Mode::ReadOnly);
   LMDB::DataRef keyRef{dbkey.getSize(), dbkey.getPtr()};
   auto valRef = tx->get(keyRef);
   BinaryDataRef value{(const uint8_t*)valRef.data, valRef.len};
   if (value.empty()) {
      return false;
   }

   subssh.hgtX = dbkey.getSliceRef(-4, 4);
   subssh.unserializeDBValue(value);
   return true;
}

void LMDBBlockDatabase::getStoredScriptHistoryByRawScript(
   StoredScriptHistory& ssh, BinaryDataRef script) const
{
   BinaryData uniqueKey = BtcUtils::getTxOutScrAddr(script);
   getStoredScriptHistory(ssh, uniqueKey);
}

/////////////////////////////////////////////////////////////////////////////
// TODO: We should also read the HeaderHgtList entries to get the blockchain
//       sorting that is saved in the DB.  But right now, I'm not sure what
//       that would get us since we are reading all the headers and doing
//       a fresh organize/sort anyway.
void LMDBBlockDatabase::readAllHeaders(
   const std::function<void(std::shared_ptr<BlockHeader>)>& callback)
{
   auto tx = beginTransaction(DB_SELECT::HEADERS, LMDB::Mode::ReadOnly);
   auto ldbIter = tx->getIterator();
   if (!ldbIter.seekToFirst()) {
      return;
   }

   do {
      ldbIter.resetReaders();
      auto keyRef = ldbIter.getKeyRef();
      if (keyRef.getSize() != 4) {
         LOGERR << "How did we get a header key that is not uint32?"
            << " (" << keyRef.getSize() << ")";
         continue;
      }

      //key is uniqueID
      uint32_t uniqueID;
      std::memcpy(&uniqueID, keyRef.getPtr(), 4);
      if (uniqueID == 0xFFFFFFFF) {
         //we've hit the SDBI entry, we are done
         return;
      }
      uniqueID = ntohl(uniqueID);

      //header data
      auto brrVal = ldbIter.getValueReader();
      auto regHead = std::make_shared<BlockHeader>(
         brrVal.get_BinaryDataRef(HEADER_SIZE));

      //metadata
      regHead->setBlockSize(brrVal.get_uint32_t());
      regHead->setNumTx(brrVal.get_uint32_t());
      regHead->setBlockFileOffset(brrVal.get_uint64_t());
      regHead->setBlockFileNum(brrVal.get_uint16_t());
      regHead->setUniqueID(uniqueID);
      regHead->setMerkleValid((bool)brrVal.get_uint8_t());

      callback(regHead);
   } while (ldbIter.advanceAndRead());
}

////////////////////////////////////////////////////////////////////////////////
// Puts bare header into HEADERS DB.  Use "putStoredHeader" to add to both
// (which actually calls this method as the first step)
//
// Returns the duplicateID of the header just inserted
void LMDBBlockDatabase::putBareHeader(const StoredHeader& sbh)
{
   if (!sbh.isInitialized()) {
      LOGERR << "Attempting to put uninitialized bare header into DB";
      return;
   }

   if (sbh.uniqueID == UINT32_MAX) {
      throw LmdbWrapperException("Attempted to put a header with no ID");
   }

   uint32_t keyBE = htonl(sbh.uniqueID);
   LMDB::DataRef keyRef{4, (const char*)&keyBE};

   BinaryWriter bwData;
   sbh.serializeDBValue(bwData);
   LMDB::DataRef valRef{bwData.getSize(), bwData.getDataRef().getPtr()};

   auto tx = beginTransaction(DB_SELECT::HEADERS, LMDB::Mode::ReadWrite);
   tx->insert(keyRef, valRef);
}

////////////////////////////////////////////////////////////////////////////////
void LMDBBlockDatabase::putStoredZC(StoredTx& stx, const BinaryData& zcKey)
{
   BinaryWriter bwKey{zcKey.getSize() + 1};
   bwKey.put_uint8_t((uint8_t)DbPrefix::ZCDATA);
   bwKey.put_BinaryData(zcKey);
   LMDB::DataRef keyRef{bwKey.getSize(), bwKey.getDataRef().getPtr()};

   // Now add the base Tx entry in the BLKDATA DB.
   BinaryWriter bwVal;
   stx.serializeDBValue(bwVal, Config::DBSettings::getDbType());
   bwVal.put_uint32_t(stx.unixTime);
   LMDB::DataRef valRef{bwVal.getSize(), bwVal.getDataRef().getPtr()};

   auto tx = beginTransaction(DB_SELECT::ZERO_CONF, LMDB::Mode::ReadWrite);
   tx->insert(keyRef, valRef);

   // Make sure all the parameters of the TxOut are set right
   for (auto& stxoPair : stx.stxoMap) {
      stxoPair.second.txVersion = READ_UINT32_LE(stx.dataCopy.getPtr());
      stxoPair.second.txIndex = stx.txIndex;
      stxoPair.second.txOutIndex = stxoPair.first;
      BinaryData zcStxoKey(zcKey);
      zcStxoKey.append(WRITE_UINT16_BE(stxoPair.second.txOutIndex));
      putStoredZcTxOut(stxoPair.second, zcStxoKey);
   }
}

////////
Tx LMDBBlockDatabase::getFullTxCopy(
   uint16_t txIndex, std::shared_ptr<BlockHeader> bhPtr) const
{
   if (bhPtr == nullptr) {
      throw LmdbWrapperException("null bhPtr");
   }

   if (txIndex >= bhPtr->getNumTx()) {
      throw std::range_error("txid > numTx");
   }

   //open block file
   auto path = FileUtils::getBlkFilename(
      Config::Pathing::blkFilePath(), bhPtr->getBlockFileNum());
   auto fileMap = FileUtils::FileMap(path, false);
   try {
      std::vector<uint64_t> xoredData;
      std::shared_ptr<BlockData> block;
      if (!Config::DBSettings::isXored()) {
         block = BlockData::deserialize(
            fileMap.ptr() + bhPtr->getOffset(),
            bhPtr->getBlockSize(), bhPtr,
            BlockData::CheckHashes::NoChecks);
      } else {
         /*
         XOR chunks are 8 bytes aligned. Block data is packed tight,
         therefor the start of a block is not 8 aligned.

         Copy 8 bytes aligned data around the block, xor it, then read
         from the block start offset (ignore the bytes preceding the block
         that were carried over for alignement purposes)
         */
         size_t prepad = bhPtr->getOffset() % 8;
         xoredData.resize((prepad + bhPtr->getBlockSize() + 7) / 8);
         std::memcpy((uint8_t*)&xoredData[0],
            fileMap.ptr() + bhPtr->getOffset() - prepad,
            bhPtr->getBlockSize() + prepad);

         auto xorkey = Config::DBSettings::getXorKey();
         for (auto& chunk : xoredData) {
            chunk ^= xorkey;
         }

         block = BlockData::deserialize(
            (const uint8_t*)&xoredData[0] + prepad,
            bhPtr->getBlockSize(), bhPtr,
            BlockData::CheckHashes::NoChecks);
      }

      const auto& bctx = block->getTxns()[txIndex];
      BinaryRefReader brr(bctx->data_, bctx->size_);
      Tx tx{brr};
      tx.setTxHeight(bhPtr->getBlockHeight());
      tx.setDupId(0);
      tx.setTxIndex(txIndex);
      return tx;
   } catch (const BtcUtils::BlockDeserializingException&) {
      throw LmdbWrapperException("failed to grab tx");
   }
}

////////////////////////////////////////////////////////////////////////////////
BinaryData LMDBBlockDatabase::getTxHashForLdbKey(
   BinaryDataRef ldbKey6B, std::shared_ptr<BlockHeader> bhPtr) const
{
   if (!ldbKey6B.startsWith(DBUtils::ZCPrefix)) {
      if (Config::DBSettings::getDbType() != ARMORY_DB_TYPE::Super) {
         auto tx = beginTransaction(DB_SELECT::TXHINTS, LMDB::Mode::ReadOnly);
         BinaryData keyFull(ldbKey6B.getSize() + 1);
         keyFull[0] = (uint8_t)DbPrefix::TXDATA;
         ldbKey6B.copyTo(keyFull.getPtr() + 1, ldbKey6B.getSize());
         LMDB::DataRef keyRef{keyFull.getSize(), keyFull.getPtr()};

         auto txData = tx->get(keyRef);
         if (txData.len >= 36) {
            return BinaryData{(const uint8_t*)txData.data + 4, 32};
         }
      } else {
         //convert height to id
         unsigned height;
         uint8_t dupId;
         uint16_t txid;
         BinaryRefReader brr(ldbKey6B);
         DBUtils::readBlkDataKeyNoPrefix(brr, height, dupId, txid);
         auto tx = getFullTxCopy(txid, bhPtr);
         return tx.getThisHash();
      }
   }
   return {};
}

////////
bool LMDBBlockDatabase::getStoredHeader(
   StoredHeader& sbh, std::shared_ptr<BlockHeader> bh, bool withTx) const
{
   try {
      //open block file
      auto path = FileUtils::getBlkFilename(
         Config::Pathing::blkFilePath(), bh->getBlockFileNum());
      auto fileMap = FileUtils::FileMap(path, false);
      BinaryRefReader brr(fileMap.ptr() + bh->getOffset(), bh->getBlockSize());

      if (withTx) {
         sbh.unserializeFullBlock(brr, false, false);
      } else {
         sbh.unserializeSimple(brr);
      }
   } catch (...) {
      return false;
   }
   return true;
}

////////////////////////////////////////////////////////////////////////////////
bool LMDBBlockDatabase::getStoredZC(StoredTx& stx, BinaryDataRef zcKey) const
{
   //only by zcKey
   BinaryData zcDbKey;

   if (zcKey.getSize() == 6) {
      zcDbKey = BinaryData(7);
      uint8_t* ptr = zcDbKey.getPtr();
      ptr[0] = (uint8_t)DbPrefix::ZCDATA;
      memcpy(ptr + 1, zcKey.getPtr(), 6);
   } else {
      zcDbKey = zcKey;
   }

   auto tx = beginTransaction(DB_SELECT::ZERO_CONF, LMDB::Mode::ReadWrite);
   auto ldbIter = tx->getIterator();
   if (!ldbIter.seekToExact(zcDbKey)) {
      LOGERR << "ZERO_CONF DB does not have the requested ZC tx";
      LOGERR << "(" << zcKey.toHexStr() << ")";
      return false;
   }

   size_t nbytes = 0;
   do {
      // Stop if key doesn't start with [PREFIX | ZCkey | TXIDX]
      if (!ldbIter.checkKeyStartsWith(zcDbKey)) {
         break;
      }

      // Read the prefix, height and dup 
      uint16_t txOutIdx;
      BinaryRefReader txKey = ldbIter.getKeyReader();

      // Now actually process the iter value
      if (txKey.getSize() == 7) {
         // Get everything else from the iter value
         stx.unserializeDBValue(ldbIter.getValueRef());
         nbytes += stx.dataCopy.getSize();
      } else if(txKey.getSize() == 9) {
         txOutIdx = READ_UINT16_BE(ldbIter.getKeyRef().getSliceRef(7, 2));
         StoredTxOut & stxo = stx.stxoMap[txOutIdx];
         stxo.unserializeDBValue(ldbIter.getValueRef());
         stxo.parentHash = stx.thisHash;
         stxo.txVersion  = stx.version;
         stxo.txOutIndex = txOutIdx;
         nbytes += stxo.dataCopy.getSize();
      } else {
         LOGERR << "Unexpected BLKDATA entry while iterating";
         return false;
      }
   } while (ldbIter.advanceAndRead(DbPrefix::ZCDATA));

   stx.numBytes = stx.haveAllTxOut() ? nbytes : UINT32_MAX;
   return true;
}

void LMDBBlockDatabase::putStoredZcTxOut(const StoredTxOut& stxo,
   const BinaryData& zcKey)
{
   BinaryWriter bwKey{zcKey.getSize() + 1};
   bwKey.put_uint8_t((uint8_t)DbPrefix::ZCDATA);
   bwKey.put_BinaryData(zcKey);
   LMDB::DataRef keyRef{bwKey.getSize(), bwKey.getDataRef().getPtr()};

   BinaryWriter bwVal;
   stxo.serializeDBValue(bwVal);
   LMDB::DataRef valRef{bwVal.getSize(), bwVal.getDataRef().getPtr()};

   auto tx = beginTransaction(DB_SELECT::ZERO_CONF, LMDB::Mode::ReadWrite);
   tx->insert(keyRef, valRef);
}

////////
bool LMDBBlockDatabase::getStoredTxOut(StoredTxOut&,
   const BinaryData&, uint16_t) const
{
   throw std::runtime_error("[LMDBBlockDatabase::getStoredTxOut] deprecated");
   #if 0
   if (getDbType() != ARMORY_DB_TYPE::Super) {
      throw LmdbWrapperException("supernode only call");
   }

   auto txKey = getDBKeyForHash(txHash);
   if (txKey.empty()) {
      return false;
   }

   unsigned id;
   uint8_t dup;
   uint16_t txIdx;
   BinaryRefReader brrKey(txKey);
   DBUtils::readBlkDataKeyNoPrefix(brrKey, id, dup, txIdx);

   //grab header
   BinaryWriter bw;
   bw.put_BinaryData(txKey);
   bw.put_uint16_t(txoutid, BE);

   //grab tx
   auto stxo_tx = beginTransaction(DB_SELECT::STXO, LMDB::Mode::ReadOnly);
   auto data = getValueNoCopy(DB_SELECT::STXO, bw.getDataRef());
   if (data.empty()) {
      LOGWARN << "no txout for key: " << txKey.toHexStr();
      return false;
   }

   //convert to stxo
   stxo.unserializeDBValue(data);
   stxo.parentHash = txHash;
   stxo.txIndex = txIdx;
   stxo.txOutIndex = txoutid;
   stxo.isCoinbase = (txIdx == 0);
   return true;
   #endif
}

bool LMDBBlockDatabase::getStoredTxOut(StoredTxOut&,
   const BinaryData&) const
{
   throw std::runtime_error("[LMDBBlockDatabase::getStoredTxOut] deprecated");
   #if 0
   if (dbKey.getSize() != 8) {
      LOGERR << "Tried to get StoredTxOut, but the provided key is not of the "
         "proper size. Expect size is 8, this key is: " << dbKey.getSize();
      return false;
   }

   unsigned id;
   uint8_t dup;
   uint16_t txIdx, txoutid;

   BinaryRefReader txout_key(dbKey);
   DBUtils::readBlkDataKeyNoPrefix(txout_key, id, dup, txIdx, txoutid);

   if (getDbType() != ARMORY_DB_TYPE::Super) {
      //Let's look in the db first. Stxos are fetched mostly to spend coins,
      //so there is a high chance we wont need to pull the stxo from the raw
      //block, since fullnode keeps track of all relevant stxos
      auto tx = beginTransaction(DB_SELECT::STXO, LMDB::Mode::ReadOnly);
      BinaryRefReader brr = getValueReader(DB_SELECT::STXO, DbPrefix::TXDATA, dbKey);

      if (!brr.empty()) {
         stxo.blockHeight = id;
         stxo.txIndex = txIdx;
         stxo.txOutIndex = txoutid;
         stxo.unserializeDBValue(brr);
         return true;
      }
   } else {
      throw std::runtime_error("invalid call for supernode db");
   }
   return false;
   #endif
}

bool LMDBBlockDatabase::getStoredTxOut(StoredTxOut&,
   std::shared_ptr<BlockHeader>, uint16_t, uint16_t) const
{
   throw std::runtime_error("[LMDBBlockDatabase::getStoredTxOut] deprecated");
   #if 0
   if (getDbType() != ARMORY_DB_TYPE::Super) {
      throw std::runtime_error("supernode only");
   }

   auto txKey = DBUtils::getBlkDataKeyNoPrefix(
      header->getUniqueID(), 0xFF, txId, txOutId);

   auto stxo_tx = beginTransaction(DB_SELECT::STXO, LMDB::Mode::ReadOnly);
   auto data = getValueNoCopy(DB_SELECT::STXO, txKey);
   if (data.empty()) {
      LOGWARN << "no txout for key: " << txKey.toHexStr();
         return false;
      }

   stxo.unserializeDBValue(data);
   stxo.blockHeight = header->getBlockHeight();
   stxo.txIndex = txId;
   stxo.txOutIndex = txOutId;
   stxo.isCoinbase = txId == 0;

   //get spentness
   auto spentness_tx = beginTransaction(
      DB_SELECT::SPENTNESS, LMDB::Mode::ReadOnly);
   auto spentnessVal = getValueNoCopy(
      DB_SELECT::SPENTNESS, stxo.getSpentnessKey());
   if (!spentnessVal.empty()) {
      stxo.spentByTxInKey = spentnessVal;
      stxo.spentness = TXOUT_SPENT;
   } else {
      stxo.spentness = TXOUT_UNSPENT;
   }
   return true;
   #endif
}

bool LMDBBlockDatabase::getStoredTxOut(StoredTxOut&,
   uint32_t, uint8_t, uint16_t,
   uint16_t) const
{
   throw std::runtime_error("[LMDBBlockDatabase::getStoredTxOut] deprecated");
   #if 0
   auto blkKey = DBUtils::getBlkDataKeyNoPrefix(
      blockHeight, dupID, txIndex, txOutIndex);
   return getStoredTxOut(stxo, blkKey);
   #endif
}

bool LMDBBlockDatabase::getStoredTxOut(StoredTxOut&,
   uint32_t, uint16_t, uint16_t) const
{
   throw std::runtime_error("[LMDBBlockDatabase::getStoredTxOut] deprecated");
   #if 0
   return getStoredTxOut(stxo, blockHeight, 0, txIndex, txOutIndex);
   #endif
}

////////
void LMDBBlockDatabase::getSpentness(StoredTxOut& stxo)
{
   if (Config::DBSettings::getDbType() != ARMORY_DB_TYPE::Super) {
      throw LmdbWrapperException("need to implement this for full node");
   }

   auto key = stxo.getSpentnessKey();
   LMDB::DataRef keyRef{key.getSize(), key.getPtr()};

   auto spentness_tx = beginTransaction(DB_SELECT::SPENTNESS, LMDB::Mode::ReadOnly);
   auto val = spentness_tx->get(keyRef);
   BinaryDataRef spentnessVal{(const uint8_t*)val.data, val.len};
   if (!spentnessVal.empty()) {
      stxo.spentByTxInKey = spentnessVal;
      stxo.spentness = SPENTNESS::SPENT;
   } else {
      stxo.spentness = SPENTNESS::UNSPENT;
   }
}

////////////////////////////////////////////////////////////////////////////////
bool LMDBBlockDatabase::getStoredTxHints(StoredTxHints& sths,
   BinaryDataRef hashPrefix) const
{
   throw std::runtime_error(
      "[LMDBBlockDatabase::getStoredTxHints] this is cooked, redesign me");
   #if 0
   if(hashPrefix.getSize() < 4) {
      LOGERR << "Cannot get hints without at least 4-byte prefix";
      return false;
   }
   BinaryDataRef prefix4 = hashPrefix.getSliceRef(0,4);
   sths.txHashPrefix = prefix4.copy();

   auto bdr = getValueRef(DB_SELECT::TXHINTS, DbPrefix::TXHINTS, prefix4);
   if (!bdr.empty()) {
      sths.unserializeDBValue(bdr);
      return true;
   } else {
      sths.dbKeyList.resize(0);
      sths.preferredDBKey.resize(0);
      return false;
   }
   #endif
}

////////////////////////////////////////////////////////////////////////////////
TxRef LMDBBlockDatabase::getTxRef(BinaryDataRef txHash)
{
   auto key = getDBKeyForHash(txHash);
   if (key.empty()) {
      throw std::runtime_error("no tx for this hash");
   }
   return TxRef{key.getRef()};
}

////////////////////////////////////////////////////////////////////////////////
TxRef LMDBBlockDatabase::getTxRef(BinaryData hgtx, uint16_t txIndex)
{
   BinaryWriter bw;
   bw.put_BinaryData(hgtx);
   bw.put_uint16_t(txIndex, BE);
   return TxRef(bw.getDataRef());
}

////////////////////////////////////////////////////////////////////////////////
TxRef LMDBBlockDatabase::getTxRef(uint32_t hgt, uint8_t dup, uint16_t txIndex)
{
   BinaryWriter bw;
   bw.put_BinaryData(DBUtils::heightAndDupToHgtx(hgt, dup));
   bw.put_uint16_t(txIndex, BE);
   return TxRef(bw.getDataRef());
}

////////////////////////////////////////////////////////////////////////////////
// This is used only for debugging and testing with small database sizes.
// For intance, the reorg unit test only has a couple blocks, a couple 
// addresses and a dozen transactions.  We can easily predict and construct
// the output of this function or analyze the output by eye.
KVLIST LMDBBlockDatabase::getAllDatabaseEntries(DB_SELECT db)
{
   if (!databasesAreOpen()) {
      return KVLIST();
   }
   KVLIST outList;
   outList.reserve(100);

   auto tx = beginTransaction(db, LMDB::Mode::ReadOnly);
   auto ldbIter = tx->getIterator();
   ldbIter.seekToFirst();
   for (ldbIter.seekToFirst(); ldbIter.isValid(); ldbIter.advanceAndRead()) {
      size_t last = outList.size();
      outList.push_back(std::pair<BinaryData, BinaryData>());
      outList[last].first  = ldbIter.getKey();
      outList[last].second = ldbIter.getValue();
   }
   return outList;
}

////////////////////////////////////////////////////////////////////////////////
void LMDBBlockDatabase::printAllDatabaseEntries(DB_SELECT db)
{
   std::cout << "Printing DB entries... (DB=" << (int)db << ")" << std::endl;
   KVLIST dbList = getAllDatabaseEntries(db);
   if (dbList.empty()) {
      std::cout << "   <no entries in db>" << std::endl;
      return;
   }

   for (uint32_t i = 0; i < dbList.size(); i++) {
      std::cout << "   \"" << dbList[i].first.toHexStr() << "\"  ";
      std::cout << "   \"" << dbList[i].second.toHexStr() << "\"  " << std::endl;
   }
}

////////////////////////////////////////////////////////////////////////////////
std::map<uint32_t, uint32_t> LMDBBlockDatabase::getSSHSummary(
   BinaryDataRef scrAddrStr)
{
   std::map<uint32_t, uint32_t> SSHsummary;
   auto tx = beginTransaction(DB_SELECT::SSH, LMDB::Mode::ReadOnly);
   auto ldbIter = tx->getIterator();
   if (!ldbIter.seekToExact(DbPrefix::SCRIPT, scrAddrStr)) {
      return SSHsummary;
   }

   StoredScriptHistory ssh;
   BinaryDataRef sshKey = ldbIter.getKeyRef();
   ssh.unserializeDBKey(sshKey, true);
   ssh.unserializeDBValue(ldbIter.getValueReader());
   return ssh.subsshSummary;
}

/////////////////////////////////////////////////////////////////////////////
void LMDBBlockDatabase::resetSSHdb_Super()
{
   //delete checkpoint and ssh db
   {
      auto db_ssh = getDbPtr(DB_SELECT::SSH);
      closeDatabases();
      db_ssh->eraseOnDisk();
   }
   openDatabases();
}

/////////////////////////////////////////////////////////////////////////////
TxFilterPoolWriter LMDBBlockDatabase::getFilterPoolWriter(
   uint32_t fileNum) const
{
   auto key = DBUtils::getFilterPoolKey(fileNum);
   LMDB::DataRef keyRef{key.getSize(), key.getPtr()};
   auto tx = beginTransaction(DB_SELECT::TXFILTERS, LMDB::Mode::ReadOnly);
   auto val = tx->get(keyRef);
   try {
      BinaryDataRef valBdr{(const uint8_t*)val.data, val.len};
      return {valBdr};
   } catch (const TxFilterException&) {
      //return empty pool if data is missing
      return {};
   }
}

/////////////////////////////////////////////////////////////////////////////
BinaryDataRef LMDBBlockDatabase::getFilterPoolDataRef(uint32_t fileNum) const
{
   auto key = DBUtils::getFilterPoolKey(fileNum);
   LMDB::DataRef keyRef{key.getSize(), key.getPtr()};
   auto tx = beginTransaction(DB_SELECT::TXFILTERS, LMDB::Mode::ReadOnly);
   auto val = tx->get(keyRef);
   return {(const uint8_t*)val.data, val.len};
}

/////////////////////////////////////////////////////////////////////////////
void LMDBBlockDatabase::putFilterPoolForFileNum(
   uint32_t fileNum, const TxFilterPoolWriter& pool)
{
   auto key = DBUtils::getFilterPoolKey(fileNum);
   LMDB::DataRef keyRef{key.getSize(), key.getPtr()};


   BinaryWriter bw;
   pool.serialize(bw);
   LMDB::DataRef valRef{bw.getSize(), bw.getDataRef().getPtr()};

   //update on disk
   auto tx = beginTransaction(DB_SELECT::TXFILTERS, LMDB::Mode::ReadWrite);
   tx->insert(keyRef, valRef);
}

/////////////////////////////////////////////////////////////////////////////
void LMDBBlockDatabase::putMissingHashes(
   const std::set<BinaryData>& hashSet, uint32_t id)
{
   auto missingHashesKey = DBUtils::getMissingHashesKey(id);
   LMDB::DataRef keyRef{
      missingHashesKey.getSize(), missingHashesKey.getPtr()};

   BinaryWriter bw;
   bw.put_uint32_t(hashSet.size());
   for (const auto& hash : hashSet) {
      bw.put_BinaryData(hash);
   }
   LMDB::DataRef valRef{bw.getSize(), bw.getDataRef().getPtr()};

   auto tx = beginTransaction(DB_SELECT::TXFILTERS, LMDB::Mode::ReadWrite);
   tx->insert(keyRef, valRef);
}

std::set<BinaryData> LMDBBlockDatabase::getMissingHashes(uint32_t id) const
{
   auto missingHashesKey = DBUtils::getMissingHashesKey(id);
   LMDB::DataRef keyRef{
      missingHashesKey.getSize(), missingHashesKey.getPtr()};
   auto tx = beginTransaction(DB_SELECT::TXFILTERS, LMDB::Mode::ReadOnly);
   auto value = tx->get(keyRef);

   BinaryRefReader brr((const uint8_t*)value.data, value.len);
   if (brr.getSizeRemaining() < 4) {
      throw LmdbWrapperException("invalid missing hashes entry");
   }
   std::set<BinaryData> missingHashesSet;

   auto len = brr.get_uint32_t();
   if (value.len != len * 32 + 4) {
      throw LmdbWrapperException("missing hashes entry size mismatch");
   }
   for (uint32_t i = 0; i < len; i++) {
      missingHashesSet.emplace(std::move(brr.get_BinaryData(32)));
   }
   return missingHashesSet;
}

////////////////////////////////////////////////////////////////////////////////
void LMDBBlockDatabase::putStoredDBInfo(DB_SELECT db,
   const StoredDBInfo& sdbi, uint16_t id)
{
   auto dbPtr = getDbPtr(db);
   dbPtr->putStoredDBInfo(sdbi, id);
}

StoredDBInfo LMDBBlockDatabase::getStoredDBInfo(DB_SELECT db, uint16_t id)
{
   auto dbPtr = getDbPtr(db);
   return dbPtr->getStoredDBInfo(id);
}

////////////////////////////////////////////////////////////////////////////////
void LMDBBlockDatabase::openDB(DB_SELECT db)
{
   auto dbPtr = getDbPtr(db);
   dbPtr->open();
}

void LMDBBlockDatabase::closeDB(DB_SELECT db)
{
   auto dbPtr = getDbPtr(db);
   dbPtr->close();
}

////////////////////////////////////////////////////////////////////////////////
void LMDBBlockDatabase::loadHeightToIdMap()
{
   auto tx = beginTransaction(DB_SELECT::SUBSSH_META, LMDB::Mode::ReadOnly);
   auto dbIter = tx->getIterator();

   std::map<unsigned, unsigned> heightToIdMap;
   BinaryWriter bw_key(8);
   bw_key.put_uint64_t(0);
   if (!dbIter.seekToExact(bw_key.getDataRef())) {
      return;
   }

   do {
      auto brr_value = dbIter.getValueReader();
      auto height = brr_value.get_uint32_t();

      auto brr_key = dbIter.getKeyReader();
      auto ctr = brr_key.get_uint32_t(BE);

      heightToIdMap.emplace(height, ctr);
      ++ctr;
   } while (dbIter.advanceAndRead());
   heightToBatchId_.update(std::move(heightToIdMap));
}

////////////////////////////////////////////////////////////////////////////////
bool LMDBBlockDatabase::getOrSetFlaggedBlockFile(uint32_t fileNum)
{
   BinaryWriter bw_key(5);
   bw_key.put_uint8_t((uint8_t)DbPrefix::FLAGGED_BLOCKFILES);
   bw_key.put_uint32_t(fileNum, BE);

   auto tx = beginTransaction(DB_SELECT::HEADERS, LMDB::Mode::ReadWrite);
   auto dbIter = tx->getIterator();
   if (!dbIter.seekToExact(bw_key.getDataRef())) {
      //missing this file num, add it
      LMDB::DataRef keyRef{bw_key.getSize(), bw_key.getDataRef().getPtr()};
      tx->insert(keyRef, LMDB::DataRef{0, (const char*)nullptr});
      return true;
   } else {
      //nothing to set, return false
      return false;
   }
}

////////
std::vector<uint32_t> LMDBBlockDatabase::getFlaggedFileNums() const
{
   auto tx = beginTransaction(DB_SELECT::HEADERS, LMDB::Mode::ReadOnly);
   auto dbIter = tx->getIterator();
   if (!dbIter.seekToStartsWith(DbPrefix::FLAGGED_BLOCKFILES)) {
      return {};
   }

   std::vector<uint32_t> result;
   do {
      if (!dbIter.verifyPrefix(DbPrefix::FLAGGED_BLOCKFILES)) {
         break;
      }

      auto keyReader = dbIter.getKeyReader();
      result.emplace_back(keyReader.get_uint32_t(BE));
   } while (dbIter.advanceAndRead());
   return result;
}

void LMDBBlockDatabase::clearFlaggedFileNums()
{
   auto tx = beginTransaction(DB_SELECT::HEADERS, LMDB::Mode::ReadWrite);
   auto dbIter = tx->getIterator();
   if (!dbIter.seekToStartsWith(DbPrefix::FLAGGED_BLOCKFILES)) {
      return;
   }

   std::set<BinaryData> keysToDelete;
   do {
      if (!dbIter.verifyPrefix(DbPrefix::FLAGGED_BLOCKFILES)) {
         break;
      }
      keysToDelete.emplace(dbIter.getKey());
   } while (dbIter.advanceAndRead());

   for (const auto& key : keysToDelete) {
      tx->erase(LMDB::DataRef{key.getSize(), key.getPtr()});
   }
}

////////
void LMDBBlockDatabase::updateHeightToIdMap(std::map<unsigned, unsigned>& idmap)
{
   heightToBatchId_.update(std::move(idmap));
}

////////
std::map<uint64_t, TxOutData> LMDBBlockDatabase::getTxOutDataForScrAddrKey(
   uint32_t scrAddrId) const
{
   auto tx = beginTransaction(DB_SELECT::TXOUTS, LMDB::Mode::ReadWrite);
   auto dbIter = tx->getIterator();
   BinaryDataRef scrAddrIdRef{(const uint8_t*)&scrAddrId, sizeof(uint32_t)};
   if (!dbIter.seekToStartsWith(scrAddrIdRef)) {
      return {};
   }

   std::map<uint64_t, TxOutData> result;
   do {
      auto keyReader = dbIter.getKeyReader();
      if (keyReader.getSize() != 8) {
         continue;
      }
      //key is [32 bits scrAddrId | 32 bits blockID (BE)]
      uint64_t scrAddrKey = keyReader.get_uint64_t();
      uint32_t id = uint32_t(scrAddrKey);
      if (id != scrAddrId) {
         break;
      }

      //get blockID
      uint32_t blockID = DBUtils::getBlockIDFromScrAddrKey(scrAddrKey);

      //deser txoutdata bodies
      auto valReader = dbIter.getValueReader();
      while (valReader.getSizeRemaining() >= 12) {
         uint64_t amount = valReader.get_uint64_t();
         uint16_t txId = valReader.get_uint16_t();
         uint16_t txOutId = valReader.get_uint16_t();
         uint64_t txOutKey = DBUtils::constructTxIOKey(blockID, txId, txOutId);

         result.emplace(txOutKey, TxOutData{
            amount, blockID, txId, txOutId});
      }
   } while (dbIter.advanceAndRead());
   return result;
}

std::unordered_map<uint64_t, uint64_t>
LMDBBlockDatabase::getTxInDataForTxOutData(
   const std::map<uint64_t, TxOutData>& txOutData) const
{
   auto tx = beginTransaction(DB_SELECT::TXINS, LMDB::Mode::ReadWrite);
   auto dbPtr = getDbPtr(DB_SELECT::TXINS);

   std::unordered_map<uint64_t, uint64_t> result;
   for (const auto& txoutPair : txOutData) {
      LMDB::DataRef txOutKeyRef{
         sizeof(uint64_t), (const char*)&txoutPair.first};
      auto valueRef = tx->get(txOutKeyRef);
      if (valueRef.len != 8) {
         continue;
      }
      auto emplaceResult = result.emplace(txoutPair.first, 0UL).first;
      std::memcpy(&emplaceResult->second, valueRef.data, 8);
   }
   return result;
}

////////////////////////////////////////////////////////////////////////////////
// DatabaseContainer
DatabaseContainer::DatabaseContainer(
   const std::filesystem::path& basedir,
   const std::string& name, size_t mapSize) :
   baseDir_{basedir}, name_{name}, db_(mapSize)
{}

DatabaseContainer::~DatabaseContainer()
{
   close();
}

////////
std::string DatabaseContainer::getDbName(DB_SELECT db)
{
   switch (db)
   {
   case DB_SELECT::HEADERS:
      return "headers";

   case DB_SELECT::SCRADDR:
      return "scraddr";

   case DB_SELECT::SSH:
      return "ssh";

   case DB_SELECT::SUBSSH:
      return "subssh";

   case DB_SELECT::SUBSSH_META:
      return "subssh_meta";

   case DB_SELECT::TXOUTS:
      return "txouts";

   case DB_SELECT::TXINS:
      return "txins";

   case DB_SELECT::KNOWNHASHES:
      return "known_hashes";

   case DB_SELECT::TXHINTS:
      return "txhints";

   case DB_SELECT::TXFILTERS:
      return "txfilters";

   case DB_SELECT::ZERO_CONF:
      return "zeroconf";

   case DB_SELECT::SPENTNESS:
      return "spentness";

   default:
      throw LmdbWrapperException("unknown db");
   }
}

void DatabaseContainer::close()
{
   db_.close();
}

void DatabaseContainer::open()
{
   db_.open(baseDir_ / name_, name_);
}

////////
void DatabaseContainer::eraseOnDisk()
{
   close();
   std::filesystem::path dbPath = baseDir_ / name_;
   std::filesystem::remove(dbPath);

   dbPath.append("-lock");
   std::filesystem::remove(dbPath);
}

void DatabaseContainer::putStoredDBInfo(
   const StoredDBInfo& sdbi, uint16_t id)
{
   if (!sdbi.isInitialized()) {
      throw LmdbWrapperException("tried to write uninitiliazed sdbi");
   }
   auto key = StoredDBInfo::getDBKey(id);
   LMDB::DataRef keyRef{key.getSize(), key.getPtr()};

   BinaryWriter bw;
   sdbi.serializeDBValue(bw);
   LMDB::DataRef valRef{bw.getSize(), bw.getDataRef().getPtr()};

   auto tx = db_.beginTransaction(LMDB::Mode::ReadWrite);
   tx.insert(keyRef, valRef);
}

StoredDBInfo DatabaseContainer::getStoredDBInfo(uint16_t id)
{
   auto tx = db_.beginTransaction(LMDB::Mode::ReadOnly);
   auto key = StoredDBInfo::getDBKey(id);
   auto val = tx.get(LMDB::DataRef{key.getSize(), key.getPtr()});
   BinaryRefReader brr((const uint8_t*)val.data, val.len);

   if (brr.empty()) {
      throw LmdbWrapperException("no sdbi at this key");
   }
   StoredDBInfo sdbi;
   sdbi.unserializeDBValue(brr);
   return sdbi;
}

////////
std::unique_ptr<DBTransaction> DatabaseContainer::beginTransaction(
   LMDB::Mode mode) const
{
   return std::make_unique<DBTransaction>(
      std::move(db_.beginTransaction(mode)));
}

////////////////////////////////////////////////////////////////////////////////
// DBPair
DBPair::DBPair(size_t mapsize) :
   mapSize_(mapsize)
{}

////////
void DBPair::open(const std::filesystem::path& path, const std::string& dbName)
{
   if (isOpen()) {
      return;
   }
   unsigned flags = MDB_NOSYNC | MDB_NOTLS;

   env_.open(path, flags);
   env_.setMapSize(mapSize_);
   db_.open(&env_, dbName);
}

void DBPair::close()
{
   if (!isOpen()) {
      return;
   }
   db_.close();
   env_.close();
}

bool DBPair::isOpen() const
{
   return env_.isOpen() && db_.isOpen();
}

////////
LMDB::Transaction DBPair::beginTransaction(LMDB::Mode mode)
{
   return LMDB::Transaction{&env_, db_.dbi(), mode};
}

////////////////////////////////////////////////////////////////////////////////
// ShardFilter
ShardFilter::~ShardFilter()
{}

BinaryData ShardFilter::getDbKey()
{
   return WRITE_UINT32_BE(SHARD_FILTER_DBKEY);
}

std::unique_ptr<ShardFilter> ShardFilter::deserialize(BinaryDataRef dataRef)
{
   BinaryRefReader brr(dataRef);
   auto type = brr.get_uint8_t();
   switch (type)
   {
      case ShardFilterType_ScrAddr:
         return ShardFilter_ScrAddr::deserialize(dataRef);

      case ShardFilterType_Spentness:
         return ShardFilter_Spentness::deserialize(dataRef);

      default:
         throw FilterException("unexpected shard filter type");
   }
}

////////////////////////////////////////////////////////////////////////////////
// ShardFilter_ScrAddr
ShardFilter_ScrAddr::ShardFilter_ScrAddr(unsigned step) :
   step_(step)
{
#ifndef UNIT_TESTS
   //x < -exp(step * 1.6 / 50k) / (1 - exp(step * 1.6 / 50k))
   auto eVal = expf(step * 1.6f / 50000.0f);
   thresholdId_ = unsigned(-eVal / (1.0f - eVal));

   //height = (ln(id) / 1.6 + 4) * 50k
   thresholdValue_ = unsigned((logf(thresholdId_) / 1.6f + 4.0f) * 50000.0f);
#else
   thresholdId_ = 0;
   thresholdValue_ = 0;
#endif
}

////////
BinaryData ShardFilter_ScrAddr::serialize() const
{
   BinaryWriter bw;
   bw.put_uint8_t(ShardFilterType_ScrAddr);
   bw.put_uint32_t(step_);

   return bw.getData();
}

std::unique_ptr<ShardFilter> ShardFilter_ScrAddr::deserialize(
   BinaryDataRef dataRef)
{
   BinaryRefReader brr(dataRef);
   auto type = brr.get_uint8_t();
   if (type != (uint8_t)ShardFilterType_ScrAddr) {
      throw FilterException("shard filter type mismatch");
   }
   auto step = brr.get_uint32_t();
   return std::make_unique<ShardFilter_ScrAddr>(step);
}

////////
unsigned ShardFilter_ScrAddr::keyToId(BinaryDataRef keyRef) const
{
   auto size = keyRef.getSize();
   if (size < 4) {
      throw FilterException("key is too short for scrAddr shard filter");
   }
   BinaryRefReader brr(keyRef);
   brr.advance(size - 4);
   auto height = DBUtils::hgtxToHeight(brr.get_BinaryDataRef(4));

   if (height >= thresholdValue_) {
      auto diff = height - thresholdValue_;
      return thresholdId_ + (diff / step_);
   } else {
      //id = exp((height/50k - 4)*1.6)
      auto val = (float(height) / 50000.0f - 4.0f) * 1.6f;
      return expl(val);
   }
}

unsigned ShardFilter_ScrAddr::getHeightForId(unsigned id) const
{
   if (id == 0) {
      return 0;
   } else if (id <= thresholdId_) {
      return unsigned((logf(id) / 1.6f + 4.0f) * 50000.0f);
   } else {
      return thresholdValue_ + (id - thresholdId_) * step_;
   }
}

////////////////////////////////////////////////////////////////////////////////
// ShardFilter_Spentness
ShardFilter_Spentness::ShardFilter_Spentness(unsigned step) :
   step_(step)
{
#ifndef UNIT_TESTS
   //x < -exp(step / 50k) / (1 - exp(step / 50k))
   auto eVal = expf(step / 50000.0f);
   thresholdId_ = unsigned(-eVal / (1.0f - eVal));

   //height = (ln(id) + 4) * 50k
   thresholdValue_ = unsigned((logf(thresholdId_) + 4.0f) * 50000.0f);
#else 
   thresholdId_ = 0;
   thresholdValue_ = 0;
#endif
}

////////
BinaryData ShardFilter_Spentness::serialize() const
{
   BinaryWriter bw;
   bw.put_uint8_t(ShardFilterType_Spentness);
   bw.put_uint32_t(step_);
   return bw.getData();
}

std::unique_ptr<ShardFilter> ShardFilter_Spentness::deserialize(
   BinaryDataRef dataRef)
{
   BinaryRefReader brr(dataRef);
   auto type = brr.get_uint8_t();
   if (type != (uint8_t)ShardFilterType_Spentness) {
      throw FilterException("shard filter type mismatch");
   }
   auto step = brr.get_uint32_t();
   return std::make_unique<ShardFilter_Spentness>(step);
}

////////
unsigned ShardFilter_Spentness::keyToId(BinaryDataRef keyRef) const
{
   auto size = keyRef.getSize();
   if (size < 4) {
      throw FilterException("key is too short for scrAddr shard filter");
   }
   BinaryRefReader brr(keyRef);
   auto height = DBUtils::hgtxToHeight(brr.get_BinaryDataRef(4));

   if (height >= thresholdValue_) {
      auto diff = height - thresholdValue_;
      return thresholdId_ + (diff / step_);
   } else {
      //id = exp((height/50k - 4))
      auto val = (float(height) / 50000.0f - 4.0f);
      return expl(val);
   }
}

unsigned ShardFilter_Spentness::getHeightForId(unsigned id) const
{
   if (id == 0) {
      return 0;
   } else if (id <= thresholdId_) {
      return unsigned((logf(id) + 4.0f) * 50000.0f);
   } else {
      return thresholdValue_ + (id - thresholdId_) * step_;
   }
}
