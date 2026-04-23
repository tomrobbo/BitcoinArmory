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

#include <list>
#include <vector>
#include <filesystem>

#include <Utils/BinaryData.h>
#include <Utils/ThreadSafeClasses.h>
#include "lmdbpp.h"

#define META_SHARD_ID               0xFFFFFFFF
#define SHARD_COUNTER_KEY           0xA76B6C00
#define SHARD_TOPHASH_ID            0xFFAAAA

#define SHARD_FILTER_DBKEY          0xAC28337D

#ifndef UNIT_TESTS
#define SHARD_FILTER_SCRADDR_STEP   1500
#define SHARD_FILTER_SPENTNESS_STEP 5000
#else
#define SHARD_FILTER_SCRADDR_STEP   2
#define SHARD_FILTER_SPENTNESS_STEP 2
#endif

class TxFilterPoolWriter;
struct StoredDBInfo;
struct StoredSubHistory;
class UnspentTxOut;
struct TxOutData;

enum class DB_SELECT : int;
enum class ARMORY_DB_TYPE : int;

////
struct FilterException : public std::runtime_error
{
   FilterException(const std::string&);
};

struct LmdbWrapperException : public std::runtime_error
{
   LmdbWrapperException(const std::string&);
};

////////////////////////////////////////////////////////////////////////////////
//
// Create & manage a bunch of different databases
//
////////////////////////////////////////////////////////////////////////////////

#define KVLIST std::vector<std::pair<BinaryData,BinaryData> > 

class Tx;
class TxIn;
class TxOut;
class TxRef;

struct StoredHeader;
struct StoredTx;
struct StoredTxOut;
struct StoredScriptHistory;
struct StoredTxHints;
struct StoredHeadHgtList;

enum class DbPrefix : uint8_t;

enum ShardFilterType
{
   ShardFilterType_ScrAddr = 0,
   ShardFilterType_Spentness
};

namespace Armory
{
   class BlockHeader;
}

////////////////////////////////////////////////////////////////////////////////
class DBIterator
{
private:
   LMDB::Iterator iter_;

public:
   DBIterator(LMDB::Iterator&&);
   ~DBIterator(void);

   bool isNull(void) const;
   bool isValid(void) const;
   bool isValid(DbPrefix);

   bool readIterData(void);
   bool retreat(void);
   bool advance(void);

   bool advance(DbPrefix);
   bool advanceAndRead(void);
   bool advanceAndRead(DbPrefix);

   BinaryData       getKey(void) const;
   BinaryData       getValue(void) const;
   BinaryDataRef    getKeyRef(void) const;
   BinaryDataRef    getValueRef(void) const;
   BinaryRefReader& getKeyReader(void) const;
   BinaryRefReader& getValueReader(void) const;

   // All the seekTo* methods do the exact same thing, the variant simply 
   // determines the meaning of the return true/false value.
   bool seekTo(BinaryDataRef);
   bool seekTo(DbPrefix, BinaryDataRef);
   bool seekToExact(BinaryDataRef);
   bool seekToExact(DbPrefix, BinaryDataRef);
   bool seekToStartsWith(BinaryDataRef);
   bool seekToStartsWith(DbPrefix);
   bool seekToStartsWith(DbPrefix, BinaryDataRef);
   bool seekToBefore(BinaryDataRef);
   bool seekToBefore(DbPrefix);
   bool seekToBefore(DbPrefix, BinaryDataRef);
   bool seekToFirst(void);
   bool seekToLast(void) ;

   // Return true if the iterator is currently on valid data, with key match
   bool checkKeyExact(BinaryDataRef);
   bool checkKeyExact(DbPrefix, BinaryDataRef);
   bool checkKeyStartsWith(BinaryDataRef);
   bool checkKeyStartsWith(DbPrefix, BinaryDataRef);

   bool verifyPrefix(DbPrefix, bool=true);
   void resetReaders(void);

protected:
   mutable BinaryDataRef    currKey_;
   mutable BinaryDataRef    currValue_;
   mutable BinaryRefReader  currKeyReader_;
   mutable BinaryRefReader  currValueReader_;

   bool isDirty_;
};

////////////////////////////////////////////////////////////////////////////////
class DBTransaction
{
private:
   LMDB::Transaction dbtx_;

public:
   DBTransaction(LMDB::Transaction&&);
   ~DBTransaction(void);

   void insert(const LMDB::DataRef&, const LMDB::DataRef&);
   void erase(const LMDB::DataRef&);
   LMDB::DataRef get(const LMDB::DataRef&) const;
   DBIterator getIterator(void) const;
};

////////////////////////////////////////////////////////////////////////////////
class DBPair
{
private:
   LMDB::Env env_;
   LMDB::DB db_;
   size_t mapSize_;

public:
   DBPair(size_t);

   LMDB::Transaction beginTransaction(LMDB::Mode);
   void open(const std::filesystem::path&, const std::string&);
   void close(void);
   bool isOpen(void) const;
};

////////////////////////////////////////////////////////////////////////////////
class DatabaseContainer
{
private:
   const std::filesystem::path baseDir_;
   const std::string name_;
   mutable DBPair db_;

public:
   //tor
   DatabaseContainer(const std::filesystem::path&, const std::string&, size_t);
   ~DatabaseContainer(void);

   //static
   static std::string getDbName(DB_SELECT);

   //virtual
   void open(void);
   void close(void);
   void eraseOnDisk(void);

   std::unique_ptr<DBTransaction> beginTransaction(LMDB::Mode) const;

   StoredDBInfo getStoredDBInfo(uint16_t);
   void putStoredDBInfo(const StoredDBInfo&, uint16_t);
};

////////////////////////////////////////////////////////////////////////////////
struct ShardFilter
{
   virtual ~ShardFilter(void) = 0;
   virtual unsigned keyToId(BinaryDataRef) const = 0;
   virtual unsigned getHeightForId(unsigned) const = 0;
   virtual BinaryData serialize(void) const = 0;

   static std::unique_ptr<ShardFilter> deserialize(BinaryDataRef);
   static BinaryData getDbKey(void);
};

////////
struct ShardFilter_ScrAddr : public ShardFilter
{
   const unsigned step_;
   unsigned thresholdId_;
   unsigned thresholdValue_;

   ShardFilter_ScrAddr(unsigned);

   unsigned keyToId(BinaryDataRef) const override;
   unsigned getHeightForId(unsigned) const override;
   BinaryData serialize(void) const override;

   static std::unique_ptr<ShardFilter> deserialize(BinaryDataRef);
};

////////
struct ShardFilter_Spentness : public ShardFilter
{
   const unsigned step_;
   unsigned thresholdId_;
   unsigned thresholdValue_;

   ShardFilter_Spentness(unsigned);

   unsigned keyToId(BinaryDataRef) const override;
   unsigned getHeightForId(unsigned) const override;
   BinaryData serialize(void) const override;

   static std::unique_ptr<ShardFilter> deserialize(BinaryDataRef);
};

////////////////////////////////////////////////////////////////////////////////
class LMDBBlockDatabase
{
   friend class ShardedSshParser;
   friend class BlockchainScanner_Super;

private:
   const std::filesystem::path dbDir_;

   std::shared_ptr<DatabaseContainer> getDbPtr(DB_SELECT) const;
   std::shared_ptr<DatabaseContainer> getHashTablePtr(DB_SELECT, uint8_t) const;

public:
   LMDBBlockDatabase(const std::filesystem::path&);
   ~LMDBBlockDatabase(void);

   /////////////////////////////////////////////////////////////////////////////
   void openDatabases(void);
   bool databasesAreOpen(void) const;

   void closeDatabases(void);
   void cycleDatabase(DB_SELECT);
   void destroyAndResetDatabases(void);
   void resetHistoryDatabases(void);

   /////////////////////////////////////////////////////////////////////////////
   std::unique_ptr<DBTransaction> beginTransaction(
      DB_SELECT, LMDB::Mode) const;
   std::unique_ptr<DBTransaction> beginHashTableTx(
      DB_SELECT, uint8_t, LMDB::Mode) const;

   /////////////////////////////////////////////////////////////////////////////
   BinaryData getDBKeyForHash(BinaryDataRef, uint8_t = UINT8_MAX) const;
   void readAllHeaders(
      const std::function<void(std::shared_ptr<Armory::BlockHeader>)>&);

   /////////////////////////////////////////////////////////////////////////////
   // Interface to translate Stored* objects to/from persistent DB storage
   /////////////////////////////////////////////////////////////////////////////
   StoredDBInfo getStoredDBInfo(DB_SELECT, uint16_t);
   void putStoredDBInfo(DB_SELECT, StoredDBInfo const&, uint16_t);

   /////////////////////////////////////////////////////////////////////////////
   // BareHeaders are those in the HEADERS DB with no blockdta associated
   void putBareHeader(const StoredHeader&);
   bool getStoredHeader(StoredHeader&,
      std::shared_ptr<Armory::BlockHeader>, bool=true) const;

   /////////////////////////////////////////////////////////////////////////////
   // StoredTx Accessors
   void putStoredZC(StoredTx&, const BinaryData&);
   bool getStoredZC(StoredTx&, BinaryDataRef) const;

   /////////////////////////////////////////////////////////////////////////////
   // StoredTxOut Accessors
   void putStoredZcTxOut(const StoredTxOut&, const BinaryData&);

   bool getStoredTxOut(StoredTxOut&,
      uint32_t,
      uint8_t,
      uint16_t,
      uint16_t) const;

   bool getStoredTxOut(StoredTxOut&,
      uint32_t,
      uint16_t,
      uint16_t) const;

   bool getStoredTxOut(
      StoredTxOut&, const BinaryData&) const;
   bool getStoredTxOut(
      StoredTxOut&, const BinaryData&, uint16_t) const;
   bool getStoredTxOut(
      StoredTxOut&, const std::shared_ptr<Armory::BlockHeader>, uint16_t, uint16_t) const;
   void getSpentness(StoredTxOut&);

   /////////////////////////////////////////////////////////////////////////////
   // StoredScriptHistory Accessors
   std::map<uint32_t, uint32_t> getSSHSummary(BinaryDataRef);
   bool getStoredScriptHistory(StoredScriptHistory&,
      BinaryDataRef,
      uint32_t = 0,
      uint32_t = UINT32_MAX) const;

   bool getStoredSubHistoryAtHgtX(StoredSubHistory&,
      const BinaryDataRef, const BinaryData&) const;
   bool getStoredSubHistoryAtHgtX(StoredSubHistory&,
      const BinaryData&) const;
   bool getStoredScriptHistorySummary(StoredScriptHistory&,
      BinaryDataRef) const;
   void getStoredScriptHistoryByRawScript(
      StoredScriptHistory&,
      BinaryDataRef) const;
   
   bool fillStoredSubHistory(StoredScriptHistory&, unsigned, unsigned) const;
   bool fillStoredSubHistory_Super(StoredScriptHistory&, unsigned, unsigned) const;

   /////////////////////////////////////////////////////////////////////////////
   // tx hints
   bool getStoredTxHints(StoredTxHints&, BinaryDataRef) const;

   /////////////////////////////////////////////////////////////////////////////
   // Tx stuff
   TxRef getTxRef(BinaryDataRef);
   TxRef getTxRef(BinaryData, uint16_t);
   TxRef getTxRef(uint32_t, uint8_t, uint16_t);
   Tx getFullTxCopy(uint16_t, std::shared_ptr<Armory::BlockHeader>) const;
   BinaryData getTxHashForLdbKey(BinaryDataRef,
      std::shared_ptr<Armory::BlockHeader>) const;

   /////////////////////////////////////////////////////////////////////////////
   // TxOut/In stuff
   std::map<uint64_t, TxOutData> getTxOutDataForScrAddrKey(uint32_t) const;
   std::unordered_map<uint64_t, uint64_t> getTxInDataForTxOutData(
      const std::map<uint64_t, TxOutData>&) const;

   /////////////////////////////////////////////////////////////////////////////
   KVLIST getAllDatabaseEntries(DB_SELECT);
   void   printAllDatabaseEntries(DB_SELECT);

   const std::filesystem::path& baseDir(void) const;

   void closeDB(DB_SELECT);
   void openDB(DB_SELECT);
   void resetSSHdb_Super(void);

   ////
   TxFilterPoolWriter getFilterPoolWriter(uint32_t) const;
   BinaryDataRef getFilterPoolDataRef(uint32_t) const;
   void putFilterPoolForFileNum(uint32_t, const TxFilterPoolWriter& pool);

   void putMissingHashes(const std::set<BinaryData>&, uint32_t);
   std::set<BinaryData> getMissingHashes(uint32_t) const;

   ////
   void updateHeightToIdMap(std::map<unsigned, unsigned>&);
   void loadHeightToIdMap();
   unsigned getShardIdForHeight(unsigned) const;
   unsigned getNextShardIdForHeight(unsigned) const;

   //// block files flagged for reparsing
   bool getOrSetFlaggedBlockFile(uint32_t);
   std::vector<uint32_t> getFlaggedFileNums(void) const;
   void clearFlaggedFileNums(void);

public:
   std::map<DB_SELECT, std::shared_ptr<DatabaseContainer>> dbMap_;
   std::map<DB_SELECT, std::vector<std::shared_ptr<DatabaseContainer>>> dbHashTables_;

private:
   bool     dbIsOpen_;
   uint32_t ldbBlockSize_;

   // In this case, a address is any TxOut script, which is usually
   // just a 25-byte script.  But this generically captures all types
   // of addresses including pubkey-only, P2SH
   Armory::Threading::TransactionalMap<unsigned, unsigned> heightToBatchId_;
};
