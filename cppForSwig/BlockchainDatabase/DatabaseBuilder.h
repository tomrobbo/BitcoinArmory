////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2016-2026, goatpig.                                         //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include <deque>

#include "BlockDataMap.h"
#include "bdmenums.h"
#include "Progress.h"

class LMDBBlockDatabase;
class BlockDataManager;
class ScrAddrFilter;
class UnresolvedHashException {};

typedef std::function<void(BDMPhase, double, unsigned, unsigned)> ProgressCallback;

namespace Armory
{
   class Blockchain;
   class ReorganizationState;
   class BlockHeader;

   namespace Database
   {
      class Builder
      {
      private:
         std::shared_ptr<BlockFiles> blockFiles_;
         std::shared_ptr<Blockchain> blockchain_;
         LMDBBlockDatabase* db_;
         std::shared_ptr<ScrAddrFilter> scrAddrFilter_;

         const ProgressCallback progress_;

         unsigned checkedTransactions_ = 0;
         const bool forceRescanSSH_;

      private:
         void loadBlockHeadersFromDB(const ProgressCallback&);
         std::set<uint32_t> addBlocksToDB(
            const BlockDataLoader::BlockDataCopy&);
         void parseBlockFile(
            const BlockDataLoader::BlockDataCopy&,
            const std::function<bool(const uint8_t*, size_t, size_t)>&
         );

         BlockOffset parseForNewHeaders(const ProgressCallback&);
         void parseForNewBlocks(const BlockOffset&, const ProgressCallback&);

         BinaryData initTransactionHistory(int32_t);
         BinaryData scanHistory(int32_t, bool, bool);
         void undoHistory(ReorganizationState&);

         void resetHistory(void);
         void verifyTransactions(void);
         void commitAllTxHints(const std::vector<std::shared_ptr<BlockData>>&);
         void commitAllStxos(const std::vector<std::shared_ptr<BlockData>>&);
         void cycleDatabases(void);

      public:
         Builder(std::shared_ptr<BlockFiles>,
            BlockDataManager&,
            const ProgressCallback&, bool);

         bool init(void);
         ReorganizationState update(void);

         void verifyChain(void);
         unsigned getCheckedTxCount(void) const { return checkedTransactions_; }

         //void verifyTxFilters(void);
         void checkTxHintsIntegrity(void);
      };
   } // namespace BlockchainData
} // namespace Armory
