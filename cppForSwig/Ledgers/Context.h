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
      class Context
      {
      private:
         std::map<uint32_t, uint32_t> timestamps_;
         mutable std::map<BinaryData, Tx> txMap_;
         std::shared_ptr<const ZeroConf::MempoolSnapshot> ss_;

      public:
         Context(
            std::map<uint32_t, uint32_t>&,
            std::map<BinaryData, Tx>&,
            std::shared_ptr<const ZeroConf::MempoolSnapshot>
         );

         uint32_t getTimestampForBlockHeight(uint32_t) const;
         const BinaryData& getTxHash(BinaryDataRef) const;
         size_t getTxOutCount(BinaryDataRef) const;
         const Tx& getTx(BinaryDataRef) const;
      };

      Context prepareContext(
         const std::map<BinaryData, TxIOPair>&,
         const Blockchain&, LMDBBlockDatabase*,
         std::shared_ptr<const ZeroConf::MempoolSnapshot>
      );
   }
}
