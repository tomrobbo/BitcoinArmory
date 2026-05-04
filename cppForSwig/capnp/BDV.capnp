@0x833fa1ae387540de;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("Armory::Codec::BDV");
using Types = import "Types.capnp";

##### statics #####
struct StaticRequest {
   magicWord               @0 : Text;

   union {
      unset                @1 : Void;

      register             @2 : Void;
      unregister           @3 : Void;
      shutdown             @4 : Void;
      shutdownNode         @5 : Void;
      getNodeStatus        @6 : Void;
      getFeeSchedule       @7 : Text;
      getTopBlockHeight    @8 : Void;
      getHeadersByHeight   @9 : List(UInt32);
      getHeadersById       @10: List(UInt32);
      broadcast            @11: List(Data);
      rpcBroadcast         @12: Data;
   }
}

struct StaticReply {
   union {
      unset                @0 : Void;

      getNodeStatus        @1 : Types.NodeStatus;
      getFeeSchedule       @2 : List(Types.FeeSchedule);
      getTopBlockHeight    @3 : UInt32;
      getHeadersByHeight   @4 : List(Types.Header);
      getHeadersById       @5 : List(Types.Header);
   }
}

##### bdv #####
struct BdvRequest {
   enum WalletType {
      unset    @0;
      wallet   @1;
      lockbox  @2;
   }

   struct RegisterWalletRequest {
      walletId                   @0 : Types.WalletId;
      isNew                      @1 : Bool;
      addresses                  @2 : List(Address);
      walletType                 @3 : WalletType;
   }

   struct OutpointRequest {
      struct Body {
         txHash      @0 : Types.Hash;
         outpointIds @1 : List(UInt16);
      }

      withZc         @0 : Bool;
      outpoints      @1 : List(Body);
   }

   struct AddressOutputsRequest {
      addresses      @0 : List(Address);
      heightCutoff   @1 : UInt32;
      zcCutoff       @2 : UInt32;
   }

   union {
      unset                      @0 : Void;

      registerWallet             @1 : RegisterWalletRequest;
      unregisterWallet           @2 : Types.WalletId;
      goOnline                   @3 : Void;

      getTxsByHash               @4 : List(Types.Hash);
      getTxsByKey                @5 : List(Types.TxKey);
      getOutputsForOutpoints     @6 : OutpointRequest;
      getOutputsForAddress       @7 : AddressOutputsRequest;
      getCombinedBalances        @8 : Void;
      getTxios                   @9 : UInt32;
   }
}

struct BdvReply {
   struct AddressOutputReply {
      struct AddressOutputs {
         addr        @0 : Address;
         outputs     @1 : List(Types.Output);
      }

      heightCutoff   @0 : UInt32;
      zcCutoff       @1 : UInt32;
      addresses      @2 : List(AddressOutputs);
   }

   union {
      unset                      @0 : Void;

      registerWallet             @1 : Void;
      unregisterWallet           @2 : Void;
      goOnline                   @3 : Void;

      getTxsByHash               @4 : List(Types.Tx);
      getTxsByKey                @5 : List(Types.Tx);
      getOutputsForOutpoints     @6 : List(Types.Output);
      getOutputsForAddress       @7 : AddressOutputReply;
      getCombinedBalances        @8 : List(Types.CombinedBalanceAndCount);
      getTxios                   @9 : List(Types.TxioPair);
   }
}

##### wallets #####
struct TxoutRequest {
   targetValue @0 : UInt64;
   zc          @1 : Bool;
   rbf         @2 : Bool;
}

struct Address {
   prefix   @0 : UInt8;
   body     @1 : Data;
}

struct WalletRequest {
   walletId                @0 : Types.WalletId;

   union {
      unset                @1 : Void;

      getBalanceAndCount   @2 : UInt32;
      getOutputs           @3 : TxoutRequest;
      setConfTarget        @4 : UInt32;
      unregisterAddresses  @5 : List(Address);
   }
}

struct WalletReply {
   union {
      unset                @0 : Void;

      getBalanceAndCount   @1 : Types.BalanceAndCount;
      getOutputs           @2 : List(Types.Output);
      setConfTarget        @3 : Void;
      unregisterAddresses  @4 : Void;
   }
}

##### addresses #####
struct AddressRequest {
   address                 @0 : Address;

   union {
      unset                @1 : Void;

      getBalanceAndCount   @2 : Void;
      getOutputs           @3 : TxoutRequest;
   }
}

struct AddressReply {
   union {
      unset                @0 : Void;

      getBalanceAndCount   @1 : Types.BalanceAndCount;
      getOutputs           @2 : List(Types.Output);
   }
}

##### main request/reply #####
struct Request {
   msgId       @0 : UInt64;

   union {
      static   @1 : StaticRequest;
      bdv      @2 : BdvRequest;
      wallet   @3 : WalletRequest;
      address  @4 : AddressRequest;
   }
}

struct Reply {
   msgId       @0 : UInt64;
   success     @1 : Bool;
   error       @2 : Text;

   union {
      static   @3 : StaticReply;
      bdv      @4 : BdvReply;
      wallet   @5 : WalletReply;
      address  @6 : AddressReply;
   }
}

##### notifications #####
struct Notification {

   struct ServerError {
      code              @0 : Int32;
      errStr            @1 : Text;
      errData           @2 : Data;
   }

   struct Refresh {
      type              @0 : UInt32;
      ids               @1 : List(Text);
   }

   requestId @0 : Text;
   union {
      terminate         @1 : Void;
      continuePolling   @2 : Void;
      ready             @3 : Types.NewBlockNotif;
      newBlock          @4 : Types.NewBlockNotif;
      zc                @5 : List(Types.TxioPair);
      invalidatedZc     @6 : List(Data);
      refresh           @7 : Refresh;
      nodeStatus        @8 : Types.NodeStatus;
      progress          @9 : Types.ScanProgress;
      error             @10: ServerError;
   }
}

struct Notifications {
   notifs @0 : List(Notification);
}