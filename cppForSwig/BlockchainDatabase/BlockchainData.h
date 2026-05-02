////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2026, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <memory>
#include <string>

#include <Utils/Types.h>

class Tx;
class BinaryData;

namespace Armory
{
   class Blockchain;
   class Hash32;

   ////////
   struct BlockchainDataException : public std::runtime_error
   {
      BlockchainDataException(const std::string&);
   };

   ////////
   class BlockchainData
   {
   private:
      std::shared_ptr<Blockchain> blockchain_;

   public:
      BlockchainData(std::shared_ptr<Blockchain>);

      Tx getTx(const Types::TxKey&) const;
      Tx getTx(const Types::BlockId&, Types::TxId) const;

      Hash32 getTxHashForTxKey(const Types::TxKey&) const;
      BinaryData getRawBlockForId(Types::BlockId) const;
   };
}
