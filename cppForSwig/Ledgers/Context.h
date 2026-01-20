////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2025, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <map>
#include <memory>
#include <stdint.h>

class BinaryData;
class BinaryDataRef;
class Blockchain;
class TxIOPair;
class LMDBBlockDatabase;
class Tx;

namespace Armory
{
   namespace ZeroConf
   {
      class MempoolSnapshot;
   }

   namespace Ledgers
   {
      struct DBCache
      {
         std::map<BinaryData, Tx> txMap;
         std::map<uint32_t, uint32_t> timestamps;
      };

      class Context
      {
      private:
         const std::map<uint32_t, uint32_t> timestamps_;

         //TODO: setup with transparent bdr/bd comparator
         const std::map<BinaryData, Tx> txMap_;
         const std::map<BinaryData, std::map<uint32_t, BinaryData>> txioKeyToScrAddr_;

      public:
         Context(
            std::map<uint32_t, uint32_t>,
            std::map<BinaryData, Tx>&,
            std::map<BinaryData, std::map<uint32_t, BinaryData>>&
         );

         uint32_t getTimestampForBlockHeight(uint32_t) const;
         const BinaryData& getTxHash(BinaryDataRef) const;
         size_t getTxOutCount(BinaryDataRef) const;
         const Tx& getTx(BinaryDataRef) const;
         const BinaryData& getScrAddrForTxOut(const TxIOPair&) const;
      };

      Context prepareContext(
         const std::map<BinaryData, TxIOPair>&,
         const Blockchain&, LMDBBlockDatabase*,
         std::shared_ptr<const ZeroConf::MempoolSnapshot>
      );

      Context prepareContext(
         const std::map<BinaryData, TxIOPair>&,
         std::shared_ptr<const DBCache>,
         std::shared_ptr<const ZeroConf::MempoolSnapshot>
      );
   }
}
