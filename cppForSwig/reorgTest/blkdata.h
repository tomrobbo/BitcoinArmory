////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2011-2015, Armory Technologies, Inc.                        //
//  Distributed under the GNU Affero General Public License (AGPL v3)         //
//  See LICENSE-ATI or http://www.gnu.org/licenses/agpl.html                  //
//                                                                            //
//                                                                            //
//  Copyright (C) 2016-2025, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#pragma once

namespace TestChain
{
const BinaryData addrA = BinaryData::CreateFromHex("62e907b15cbf27d5425399ebf6f0fb50ebb88f18");
const BinaryData scrAddrA = HASH160PREFIX + addrA;
//const BinaryData privKeyAddrA = BinaryData::CreateFromHex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
const BinaryData addrB = BinaryData::CreateFromHex("ee26c56fc1d942be8d7a24b2a1001dd894693980");
const BinaryData scrAddrB = HASH160PREFIX + addrB;
const BinaryData privKeyAddrB = BinaryData::CreateFromHex("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
const BinaryData addrC = BinaryData::CreateFromHex("cb2abde8bccacc32e893df3a054b9ef7f227a4ce");
const BinaryData scrAddrC = HASH160PREFIX + addrC;
const BinaryData privKeyAddrC = BinaryData::CreateFromHex("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
const BinaryData addrD = BinaryData::CreateFromHex("c522664fb0e55cdc5c0cea73b4aad97ec8343232");
const BinaryData scrAddrD = HASH160PREFIX + addrD;
const BinaryData privKeyAddrD = BinaryData::CreateFromHex("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
const BinaryData addrE = BinaryData::CreateFromHex("47b8ad0b1d6803260ce428d9e09e2cd99fd3b359");
const BinaryData scrAddrE = HASH160PREFIX + addrE;
const BinaryData privKeyAddrE = BinaryData::CreateFromHex("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
const BinaryData addrF = BinaryData::CreateFromHex("d63b766cd342e6f0f7390dd454065e4bbea26b1b");
const BinaryData scrAddrF = HASH160PREFIX + addrF;
const BinaryData privKeyAddrF = BinaryData::CreateFromHex("efefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefef");

// LB1 = AddrB + AddrC
// LB2 = AddrD + AddrE
const std::string lb1B58ID = "DuR9CQX6";
const BinaryData lb1ScrAddr = BinaryData::CreateFromHex("fe0102cb2abde8bccacc32e893df3a054b9ef7f227a4ceee26c56fc1d942be8d7a24b2a1001dd894693980");
const BinaryData lb1ScrAddrP2SH = BinaryData::CreateFromHex("05ae3c7b8b4b00ae7b3d8702d0a23bcda2ac541e34");
const std::string lb2B58ID = "eqWJZTcT";
const BinaryData lb2ScrAddr = BinaryData::CreateFromHex("fe020247b8ad0b1d6803260ce428d9e09e2cd99fd3b359c522664fb0e55cdc5c0cea73b4aad97ec8343232");
const BinaryData lb2ScrAddrP2SH = BinaryData::CreateFromHex("05de46f84ba728121eb3bdb1c30140cd6c0c8b7e25");

const BinaryData blkHash0 = BinaryData::CreateFromHex("6fe28c0ab6f1b372c1a6a246ae63f74f931e8365e15a089c68d6190000000000");
const BinaryData blkHash1 = BinaryData::CreateFromHex("fefba729db13850ba6ecc29a7dd41d80fb4bf7e063d5e093f48f0c5d00000000");
const BinaryData blkHash2 = BinaryData::CreateFromHex("3641abc766dcc85643af42ad0999c2370ef1a5e521547fa4b55f5735449021ee");
const BinaryData blkHash3 = BinaryData::CreateFromHex("23c0558cccfdbce453814beb4c5b0f6fa0625e1c0895df55ed810fbabfe04c65");
const BinaryData blkHash4 = BinaryData::CreateFromHex("7f640833c553582959a144f2738c8683a601ab67a80f3b040cc92d5200000000");
const BinaryData blkHash5 = BinaryData::CreateFromHex("e4b8313bec211633992fde8b9bb9727f56cb0413e2108ceab280fc7e00000000");
const BinaryData blkHash4A = BinaryData::CreateFromHex("fb97af2c706d9410d90651d171f8e728d228fbfe7a14b58d475cb55800000000");
const BinaryData blkHash5A = BinaryData::CreateFromHex("3a6a8a8a1b0a64d28c07b680e3a079530c5997894aafd31684b5f83500000000");

const unsigned int zcTxSize = 258;
const std::string zcTxHash256 = "b6b6f145742a9072fd85f96772e63a00eb4101709aa34ec5dd59e8fc904191a7";
const unsigned int lbZCTxSize = 378;
}

/***
Block #0, 6fe28c0ab6f1b372c1a6a246ae63f74f931e8365e15a089c68d6190000000000
   Prev: 0000000000000000000000000000000000000000000000000000000000000000
   Txs: 1

   * Tx [0:0], 3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a
      inputs: 1, outputs: 1

      + Coinbase
         amount: 50

      - TxOut [0:0-0]
         dest: scrAddrA
         amount: 50


Block #1, fefba729db13850ba6ecc29a7dd41d80fb4bf7e063d5e093f48f0c5d00000000
   Prev: 6fe28c0ab6f1b372c1a6a246ae63f74f931e8365e15a089c68d6190000000000
   Txs: 1

   * Tx [1:0], ec6ee10ddb21cc8b21524f890e8dd8aed0b7e020032eed9e167a566c8659f35c
      inputs: 1, outputs: 1

      + Coinbase
         amount: 50

      - TxOut [1:0-0]
         dest: scrAddrB
         amount: 50


Block #2, 3641abc766dcc85643af42ad0999c2370ef1a5e521547fa4b55f5735449021ee
   Prev: fefba729db13850ba6ecc29a7dd41d80fb4bf7e063d5e093f48f0c5d00000000
   Txs: 3

   * Tx [2:0], 63a471e560a4a4d3b956173c84175c39eece33dbae2c7987f1edf3bda1fb420c
      inputs: 1, outputs: 1

      + Coinbase
         amount: 50

      - TxOut [2:0-0]
         dest: scrAddrB
         amount: 50

   * Tx [2:1], b46699df740c38435c0430e2595c0a61733658ab923121d8a2edeac3895ae541
      inputs: 1, outputs: 3

      + TxIn #0
         Outpoint: [1:0-0]
         amount: 50
         addr: scrAddrB

      - TxOut [2:1-0]
         dest: 3HaHxKtpHmj2ADdJfjSsiadKv8kdrdA1bz
         amount: 15
      - TxOut [2:1-1]
         dest: 37SVrXUCEvteC4jj8uBFCHyJFsPo4x2b98DxLpn41mCCYWYLKE47sAia2fZPRybVK
         amount: 10
      - TxOut [2:1-2]
         dest: scrAddrB
         amount: 25

   * Tx [2:2], 9ca7f69284d0e8b0ea5ad7915b50d65817fb5c592fdfc351e473b1291690c76b
      inputs: 1, outputs: 2

      + TxIn #0
         Outpoint: [2:1-2]
         amount: 25
         addr: scrAddrB

      - TxOut [2:2-0]
         dest: scrAddrF
         amount: 20
      - TxOut [2:2-1]
         dest: scrAddrB
         amount: 5


Block #3, 23c0558cccfdbce453814beb4c5b0f6fa0625e1c0895df55ed810fbabfe04c65
   Prev: 3641abc766dcc85643af42ad0999c2370ef1a5e521547fa4b55f5735449021ee
   Txs: 6

   * Tx [3:0], feac40d88e7931cfb01fd4aee8a0fae1481c2df357e0b9c7f6db33379d8bbc6f
      inputs: 1, outputs: 1

      + Coinbase
         amount: 50

      - TxOut [3:0-0]
         dest: scrAddrC
         amount: 50

   * Tx [3:1], d3f38cb8a7e9294db20b67bd99e64bfb6320cfe286f7f77170b33e685a2827a3
      inputs: 1, outputs: 2

      + TxIn #0
         Outpoint: [2:2-0]
         amount: 20
         addr: scrAddrF

      - TxOut [3:1-0]
         dest: scrAddrD
         amount: 5
      - TxOut [3:1-1]
         dest: scrAddrF
         amount: 15

   * Tx [3:2], 42551f6cb5674bdad8850a4a423101e42e17559568080f7dcd3d2c9480ae53ea
      inputs: 1, outputs: 2

      + TxIn #0
         Outpoint: [2:1-0]
         amount: 15
         addr: 3HaHxKtpHmj2ADdJfjSsiadKv8kdrdA1bz

      - TxOut [3:2-0]
         dest: scrAddrE
         amount: 5
      - TxOut [3:2-1]
         dest: 37SPXt1EegMHw91FX1waQXUjyygC7LxPNk2ijn5tZsR2cjsJ38hyYg6aCeVzFKxkV
         amount: 10

   * Tx [3:3], 91c1d6c4eb074ce3570641e615a0ab7f6ca81f91186492b79e4118086ab047b6
      inputs: 1, outputs: 2

      + TxIn #0
         Outpoint: [2:1-1]
         amount: 10
         addr: 37SVrXUCEvteC4jj8uBFCHyJFsPo4x2b98DxLpn41mCCYWYLKE47sAia2fZPRybVK

      - TxOut [3:3-0]
         dest: scrAddrC
         amount: 5
      - TxOut [3:3-1]
         dest: 3MxJxCsbBtposzYCvQxKDvKicte2sdfwWt
         amount: 5

   * Tx [3:4], 36d958de246b7e422376803e4eb26cac9c82606a315a523f0b5c9f8c07a44677
      inputs: 2, outputs: 2

      + TxIn #0
         Outpoint: [2:0-0]
         amount: 50
         addr: scrAddrB
      + TxIn #1
         Outpoint: [2:2-1]
         amount: 5
         addr: scrAddrB

      - TxOut [3:4-0]
         dest: scrAddrE
         amount: 25
      - TxOut [3:4-1]
         dest: scrAddrB
         amount: 30

   * Tx [3:5], 9ec8177ca0a4f7aa21ec88a324f236a4d1dce6c610812a90e16febef4603a438
      inputs: 1, outputs: 2

      + TxIn #0
         Outpoint: [3:1-1]
         amount: 15
         addr: scrAddrF

      - TxOut [3:5-0]
         dest: 37SVrXUCEvteC4jj8uBFCHyJFsPo4x2b98DxLpn41mCCYWYLKE47sAia2fZPRybVK
         amount: 10
      - TxOut [3:5-1]
         dest: scrAddrF
         amount: 5


Block #4, 7f640833c553582959a144f2738c8683a601ab67a80f3b040cc92d5200000000
   Prev: 23c0558cccfdbce453814beb4c5b0f6fa0625e1c0895df55ed810fbabfe04c65
   Txs: 4

   * Tx [4:0], d5fcc32ccb7e4c01049df1458c628253298f78c74a7cdd8df8f40125fbbf4f42
      inputs: 1, outputs: 1

      + Coinbase
         amount: 50

      - TxOut [4:0-0]
         dest: scrAddrD
         amount: 50

   * Tx [4:1], 2564950008ae20c57bd2d5ffb2ceca67a33e003dc69f0d5b7c31c03a16e78071
      inputs: 1, outputs: 2

      + TxIn #0
         Outpoint: [3:2-1]
         amount: 10
         addr: 37SPXt1EegMHw91FX1waQXUjyygC7LxPNk2ijn5tZsR2cjsJ38hyYg6aCeVzFKxkV

      - TxOut [4:1-0]
         dest: scrAddrF
         amount: 5
      - TxOut [4:1-1]
         dest: 37SPXt1EegMHw91FX1waQXUjyygC7LxPNk2ijn5tZsR2cjsJ38hyYg6aCeVzFKxkV
         amount: 5

   * Tx [4:2], 1fb865e8cf2e430cca3a80adf8375bea5fddb0080e326b8df668373dfe599c07
      inputs: 1, outputs: 1

      + TxIn #0
         Outpoint: [3:3-1]
         amount: 5
         addr: 3MxJxCsbBtposzYCvQxKDvKicte2sdfwWt

      - TxOut [4:2-0]
         dest: scrAddrD
         amount: 5

   * Tx [4:3], 6705262a37294c4e99890418668cfb97738b51b253ac86fcbee599a19fe15cea
      inputs: 2, outputs: 3

      + TxIn #0
         Outpoint: [3:0-0]
         amount: 50
         addr: scrAddrC
      + TxIn #1
         Outpoint: [3:3-0]
         amount: 5
         addr: scrAddrC

      - TxOut [4:3-0]
         dest: 3HaHxKtpHmj2ADdJfjSsiadKv8kdrdA1bz
         amount: 25
      - TxOut [4:3-1]
         dest: 37SVrXUCEvteC4jj8uBFCHyJFsPo4x2b98DxLpn41mCCYWYLKE47sAia2fZPRybVK
         amount: 20
      - TxOut [4:3-2]
         dest: scrAddrC
         amount: 10


Block #5, e4b8313bec211633992fde8b9bb9727f56cb0413e2108ceab280fc7e00000000
   Prev: 7f640833c553582959a144f2738c8683a601ab67a80f3b040cc92d5200000000
   Txs: 3

   * Tx [5:0], dc62ffa88e123ee99aa38ac5e73a65994e9332fbc0c554ebde7885900f5c06cb
      inputs: 1, outputs: 1

      + Coinbase
         amount: 50

      - TxOut [5:0-0]
         dest: scrAddrB
         amount: 50

   * Tx [5:1], b6b6f145742a9072fd85f96772e63a00eb4101709aa34ec5dd59e8fc904191a7
      inputs: 1, outputs: 2

      + TxIn #0
         Outpoint: [3:4-1]
         amount: 30
         addr: scrAddrB

      - TxOut [5:1-0]
         dest: scrAddrC
         amount: 10
      - TxOut [5:1-1]
         dest: scrAddrB
         amount: 20

   * Tx [5:2], 55501098859122d73e3e360d90574b468b8299f578738ce293789a7eeeb678b4
      inputs: 1, outputs: 1

      + TxIn #0
         Outpoint: [4:1-0]
         amount: 5
         addr: scrAddrF

      - TxOut [5:2-0]
         dest: scrAddrD
         amount: 5
***/