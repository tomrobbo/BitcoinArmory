////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2019-2025, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <memory>
#include <string>
#include <set>
#include <map>
#include <filesystem>

#include <Utils/SecureBinaryData.h>
#include <btc/ecc_key.h>

#define PEERS_WALLET_PASSWORD "password"
#define PEERS_WALLET_BIP32_ACCOUNT 0xFF005618

struct AuthPeersLambdas;

namespace Armory
{
   namespace Wallets
   {
      class AssetWallet;

      namespace IO
      {
         struct ReadOnlyFileParams;
         struct CreateFileParams;
      };

      //////////////////////////////////////////////////////////////////////////
      class PeerFileMissing
      {
      public:
         PeerFileMissing(void)
         {}
      };

      ////////
      class AuthorizedPeersException : public std::runtime_error
      {
      public:
         AuthorizedPeersException(const std::string& err) :
            std::runtime_error(err)
         {}
      };

      ////////
      class PeerKey
      {
      private:
         const bool oneWayAuth_;
         const bool isServer_;
         const SecureBinaryData pubkey_;

      public:
         PeerKey(const SecureBinaryData&, bool, bool);

         const SecureBinaryData& getKey(void) const;
         bool isServer(void) const;
         std::string toHumanReadable(void) const;
         static PeerKey fromHumanReadable(const std::string&);
      };

      //////////////////////////////////////////////////////////////////////////
      class AuthorizedPeers
      {
      private:
         std::map<std::string, btc_pubkey> nameToKeyMap_;
         std::set<SecureBinaryData> keySet_;
         std::map<BinaryData, SecureBinaryData> privateKeys_;

         //for wallet management
         std::map<SecureBinaryData, std::set<unsigned>> keyToAssetIndexMap_;
         std::shared_ptr<AssetWallet> wallet_ = nullptr;

         //<pubkey, sig>
         std::pair<SecureBinaryData, SecureBinaryData> rootSignature_;

         //<pubkey, <description, asset id>>
         std::map<SecureBinaryData,
            std::pair<std::string, unsigned>> peerRootKeys_;

         //public key of master ACL; a client that completes a 2-way
         //AEAD handshake with this key will receive master credentials
         SecureBinaryData masterKey_;

      private:
         AuthorizedPeers(std::shared_ptr<AssetWallet>);
         AuthorizedPeers(SecureBinaryData&);

         void setOwnPrivateKey(SecureBinaryData&);
         void loadWallet(const IO::ReadOnlyFileParams&);
         void initFromWallet(void);
         void erasePeerRootKey(const SecureBinaryData&);

      public:
         AuthorizedPeers(const IO::ReadOnlyFileParams&);
         AuthorizedPeers(void);

         const std::map<std::string, btc_pubkey>& getPeerNameMap(void) const;
         const std::set<SecureBinaryData>& getPublicKeySet(void) const;
         const SecureBinaryData& getPrivateKey(const BinaryDataRef&) const;
         std::shared_ptr<AuthorizedPeers> getNarrowSet(const std::string&) const;

         /* addPeer:
         input:
         - pubkey as SecureBinaryData/btc_pubkey/PeerKey; secp256k1 un/compressed
           public key
         - vector<std::string> of names; at least 1
         */
         void addPeer(const PeerKey&, const std::vector<std::string>&);
         void addPeer(const btc_pubkey&, const std::vector<std::string>&);
         void addPeer(const SecureBinaryData&, const std::vector<std::string>&);

         void addRootSignature(
            const SecureBinaryData&, const SecureBinaryData&);
         void addPeerRootKey(const SecureBinaryData&, std::string);

         //
         void eraseName(const std::string&);
         void erasePeer(const PeerKey&);
         void eraseKey(const SecureBinaryData&);
         void eraseKey(const btc_pubkey&);

         const btc_pubkey& getOwnPublicKey(void) const;
         bool setMasterKey(const btc_pubkey&);
         bool setMasterKey(const SecureBinaryData&);
         void eraseMasterKey(void);
         bool isMasterKey(const btc_pubkey&) const;
         bool isMasterKey(const SecureBinaryData&) const;

         //takes path to peers db, passphrase lambdas are handled internally
         static void changeControlPassphrase(const std::filesystem::path&);
         static AuthPeersLambdas getAuthPeersLambdas(
            std::shared_ptr<AuthorizedPeers>);
         static std::shared_ptr<AuthorizedPeers> createWallet(
            const IO::CreateFileParams&);
      };
   } //namespace Wallets
} //namespace Armory
