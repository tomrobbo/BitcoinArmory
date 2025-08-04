////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2025, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include <cstdlib>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>

#include "BlockchainDbClient.h"
#include "Wallets/Manager.h"
#include "Wallets/Notifications.h"

#include "../Wallets/IOHeader.h"
#include "../AsyncClient.h"

#include "spawn.h"
#include <random>

using namespace Armory::Bridge;
using namespace std::string_view_literals;
using namespace std::chrono_literals;

namespace {
   size_t getFileSize(int fd)
   {
      struct stat buf;
      if (fstat(fd, &buf) != 0) {
         throw std::runtime_error(
            "fstat failed with error: " + std::string{strerror(errno)});
      }
      return buf.st_size;
   }
}

int Armory::Bridge::autoDbPid = -1;

////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<Armory::Wallets::AuthorizedPeers> Armory::Bridge::spawnDb()
{
   /*
   Use posix_spawn, which wraps around fork() & execve() to spawn an instance
   of ArmoryDB, with tailored CLI args, environment and ephemeral AEAD 2-way
   handshake.

   Keys are prepared following these steps:
      1. CppBridge creates an ephemeral key store and adds its public key to
         the tailored envvar served to ArmoryDB via execve/posix_spawn.
      2. CppBridge creates a random key file in the datadir and locks it.
         The open file descriptor is sent to ArmoryDB via envvars as well.
      3. ArmoryDB detects automation via the --ephemeral CLI arg.
         It creates an ephemeral key store, reads the caller pubkey from
         envvars, adds it to the store and sets it as the store's master key.
      4. ArmoryDB grabs the key file fd from envvars and writes its pubkey
         there. This should work by virtue of opened file descriptors sharing
         rules for fork() evexve(), which posix_spawn() inherits.
      5. CppBridge detects changes to the key file, grabs the pubkey and
         injects it into its own store. The lock is released, the file closed
         and removed.
   */

   //sanity check
   if (autoDbPid != -1) {
      throw std::runtime_error("already have an instance of ArmoryDB");
   }

   //get full path to armorydb
   const std::filesystem::path armoryDbPath{
      Armory::Config::Pathing::runningDir() / "ArmoryDB" };
   if (!FileUtils::fileExists(armoryDbPath, 0)) {
      throw std::runtime_error("invalid db binary path: " + armoryDbPath.string());
   }

   //1. setup ephemeral authPeers
   auto peers = std::make_shared<Armory::Wallets::AuthorizedPeers>();
   const auto& pubkey = peers->getOwnPublicKey();
   BinaryDataRef keyRef{pubkey.pubkey, 33};
   std::string keyStr{ "CALLER_PUBKEY=" + keyRef.toHexStr() };

   //generate random db port & set it
   uint32_t port = (rand() % 10000) + 50000;
   auto portStr = std::to_string(port);
   std::string dbPortStr{ "--armorydb-port=" + portStr };
   Armory::Config::NetworkSettings::setDbPort(portStr);

   //db paths
   std::string dbDir{ "--dbdir=" + Armory::Config::Pathing::dbDir().string() };
   std::string dataDir{ "--datadir=" + Armory::Config::getDataDir().string() };

   //btc network
   std::string network;
   switch (Armory::Config::BitcoinSettings::getMode())
   {
      case Armory::Config::NETWORK_MODE_TESTNET:
         network = std::string{"--testnet"};
         break;

      case Armory::Config::NETWORK_MODE_REGTEST:
         network = std::string{"--regtest"};
         break;

      default:
         network = std::string{"--mainnet"};
   }

   //core settings
   std::string satoshiDir{
      "--satoshi-datadir=" + Armory::Config::Pathing::blkFilePath().string() };
   std::string satoshiPort{
      "--satoshi-port=" + Armory::Config::NetworkSettings::btcPort() };
   std::string rpcPort{
      "--satoshirpc-port=" + Armory::Config::NetworkSettings::rpcPort() };

   //setup argv
   auto dbPathStr = armoryDbPath.string();
   char* argv[] = {
      //first arg has to be binary's path
      dbPathStr.data(),
      //ephemeral mode, custom port to listen to
      (char*)"--ephemeral"sv.data(), dbPortStr.data(),
      //datadir, dbdir
      dataDir.data(), dbDir.data(),
      //network & core settings
      network.data(), satoshiDir.data(), satoshiPort.data(), rpcPort.data(),
      (char*)nullptr
   };

   //2. randomize a file name
   std::filesystem::path keyFilePath{ Armory::Config::getDataDir() /
      std::string{ "keyFile_" + BtcUtils::fortuna_.generateRandom(7).toHexStr() }};

   //open file and lock it
   auto fd = open(keyFilePath.c_str(), O_CREAT | O_EXCL | O_RSYNC | O_RDWR);
   if (fd == -1) {
      throw std::runtime_error("failed to create autodb key file");
   }
   if (flock(fd, LOCK_EX) != 0) {
      throw std::runtime_error(
         "failed to lock key file with error: " + std::string{strerror(errno)});
   }
   if (getFileSize(fd) != 0) {
      throw std::runtime_error("autodb key file isnt fresh");
   }
   std::string keyFileFd{ "KEYFILE_FD=" + std::to_string(fd) };

   //setup envp
   char* envp[] = {
      keyStr.data(), keyFileFd.data(),
      (char*)nullptr
   };

   //spawn the db
   int result = posix_spawn(&autoDbPid, argv[0], nullptr, nullptr, argv, envp);
   if (result != 0 || autoDbPid == -1) {
      throw std::runtime_error(
         "failed to spawn armorydb with error: " + std::string{strerror(errno)});
   }

   //5. wait for db to set pubkey in shared file
   unsigned count = 0;
   while (true) {
      if (count >= 100) {
         throw std::runtime_error("autodb handshake timeout");
      }

      if (getFileSize(fd) != 33) {
         //key file hasnt changed, keep polling
         std::this_thread::sleep_for(100ms);
         ++count;
         continue;
      }

      //grab db pubkey from shared file
      SecureBinaryData serverPubkey(33);
      lseek(fd, 0, SEEK_SET);
      if (read(fd, serverPubkey.getPtr(), 33) != 33) {
         throw std::runtime_error("failed to read pubkey from key file");
      }

      //add db key to custom store
      std::string addr{"127.0.0.1:" + portStr};
      peers->addPeer(serverPubkey, addr);
      break;
   }

   //clean up the file
   if (flock(fd, LOCK_UN) != 0) {
      throw std::runtime_error("failed to unlock key file");
   }
   if (close(fd) != 0) {
      throw std::runtime_error("failed to close key file");
   }
   if (!std::filesystem::remove(keyFilePath)) {
      throw std::runtime_error("key file did not exists!");
      //fs::remove returns false if there was nothing to remove.
      //it will throw on failure.
   }

   //return ephemeral key store
   return peers;
}

////
bool Armory::Bridge::isDbRunning()
{
   if (autoDbPid == -1) {
      return false;
   }

   siginfo_t processInfo;
   memset(&processInfo, 0, sizeof(processInfo));
   if (waitid(P_PID, (pid_t)autoDbPid, &processInfo, WEXITED | WNOHANG) != 0) {
      return false;
   }

   if (processInfo.si_pid == 0) {
      return true;
   }
   if (processInfo.si_code == CLD_EXITED || processInfo.si_code == CLD_KILLED) {
      autoDbPid = -1;
      return false;
   }
   return true;
}

////////////////////////////////////////////////////////////////////////////////
BdvPtr Armory::Bridge::setupClientConnection(
   std::shared_ptr<Armory::Wallets::AuthorizedPeers> peers,
   const std::function<void(BinaryData&)>& writeFunc,
   std::shared_ptr<WalletManager> wltManager)
{
   //setup bdv obj
   auto cbPtr = wltManager->setupBdvCallback(writeFunc);
   BdvPtr result = AsyncClient::BlockDataViewer::getNewBDV(
      Armory::Config::NetworkSettings::dbIP(),
      Armory::Config::NetworkSettings::dbPort(),
      peers, Armory::Config::NetworkSettings::oneWayAuth(),
      cbPtr
   );

   //TODO: set gui prompt to accept server pubkeys
   result->setCheckServerKeyPromptLambda(
      [](const BinaryData&, const std::string&)->bool
      { return true; }
   );

   //connect to db
   if (!result->connectToRemote()) {
      return nullptr;
   }
   result->registerWithDB(
      Armory::Config::BitcoinSettings::getMagicBytes().toHexStr());

   //notify setup is done
   cbPtr->notifySetupDone();
   wltManager->setBdvPtr(result);
   return result;
}
