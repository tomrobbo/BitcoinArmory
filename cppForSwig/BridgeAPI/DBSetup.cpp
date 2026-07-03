////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2025-2026, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include <cstdlib>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <random>
#include <cstring>
#include <charconv>

#ifndef _WIN32
   #include <sys/wait.h>
   #include "spawn.h"
#endif

#include "DBSetup.h"
#include <Utils/ArmoryConfig.h>
#include <Utils/FileUtils.h>
#include <Utils/Cryptography.h>
#include <Utils/JSON_codec.h>

#include <Wallets/IOHeader.h>
#include <Wallets/AuthorizedPeers.h>
#include <AsyncClient.h>
#include <Node/nodeRPC.h>

using namespace Armory;
using namespace Armory::Bridge;

using namespace std::string_view_literals;
using namespace std::chrono_literals;

namespace {
#ifdef _WIN32
   std::vector<std::string_view> satoshiDirCandidates{
      "Local/Bitcoin"sv,
      "Roaming/Bitcoin"sv
   };
   std::vector<std::string_view> satoshiBinCandidates{
      //figure out default windows install location
   };

   size_t getFileSize(HANDLE fHandle)
   {
      auto size = GetFileSize(fHandle, NULL);
      if (size == INVALID_FILE_SIZE) {
         throw std::runtime_error("failed to grab file size");
      }
      return size_t(size);
   }

#else
   std::vector<std::string_view> satoshiDirCandidates{
      ".bitcoin"sv
   };
   std::vector<std::string_view> satoshiBinCandidates{
      "/usr/bin"sv,
      "/usr/local/bin"sv,
      "/opt/bin"sv
   };

   size_t getFileSize(int fd)
   {
      struct stat buf;
      if (fstat(fd, &buf) != 0) {
         throw std::runtime_error(
            "fstat failed with error: " + std::string{strerror(errno)});
      }
      return buf.st_size;
   }
#endif

   std::pair<std::string, std::string> getIpAndPortFromPeerName(
      const std::string& peerName)
   {
      //TODO: flesh this out
      std::stringstream ss(peerName);
      std::pair<std::string, std::string> output;

      //ip
      std::getline(ss, output.first, ':');

      //port
      if (ss.good()) {
         std::getline(ss, output.second);
      } else {
         output.second = Config::NetworkSettings::dbPort();
      }
      return output;
   }

   ////////
   constexpr std::string_view blocksDir = "blocks"sv;
   constexpr std::string_view bitcoind = "bitcoind"sv;
   constexpr std::string_view xorFile = "xor.dat"sv;
   constexpr std::string_view bitcoinConfFile = "bitcoin.conf"sv;
   constexpr std::string_view cookieFile = ".cookie"sv;
   constexpr std::string_view prunedKey = "prune"sv;
   constexpr std::string_view rpcLogKey = "rpcuser"sv;
   constexpr std::string_view rpcPassKey = "rpcpassword"sv;
   constexpr std::string_view coreVersionHeader = "Bitcoin Core daemon version "sv;

   std::filesystem::path getSatoshiDatadir(const std::filesystem::path& path)
   {
      if (path.stem() != blocksDir) {
         return path;
      }
      return path.parent_path();
   }

   std::filesystem::path getBlocksDir(const std::filesystem::path& path)
   {
      if (path.stem() == blocksDir) {
         return path;
      }
      return path / blocksDir;
   }

   std::pair<bool, size_t> validateBlocksDir(const std::filesystem::path& dataDir)
   {
      auto blocksDir = getBlocksDir(dataDir);
      if (!FileUtils::isDir(blocksDir, 2)) {
         return { false, SIZE_MAX };
      }

      //look for first blkXXXXX.dat file
      std::filesystem::path firstBlkFile{};
      size_t totalChainSize = 0;
      for (const auto& entry : std::filesystem::directory_iterator{blocksDir}) {
         if (!entry.is_regular_file()) {
            continue;
         }

         auto blkFilePath = entry.path();
         auto filesize = FileUtils::getFileSize(blkFilePath);
         if (filesize == SIZE_MAX) {
            continue;
         }
         if (filesize >= 8 && firstBlkFile.empty()) {
            firstBlkFile = blkFilePath;
         }
         totalChainSize += filesize;
      }

      if (firstBlkFile.empty()) {
         return { false, SIZE_MAX };
      }

      //look for xor key
      auto xorPath = blocksDir / xorFile;
      uint64_t xorKey = 0;
      if (FileUtils::pathExists(xorPath, 2)) {
         auto fileCopy = FileUtils::FileCopy(xorPath);
         if (fileCopy.size() == 8) {
            std::memcpy(&xorKey, fileCopy.ptr(), 8);
         }
      }

      //check magic byte in first file
      uint64_t firstChunk;
      std::ifstream blkFileStream{firstBlkFile, std::ios::binary | std::ios::in};
      blkFileStream.read(reinterpret_cast<char*>(&firstChunk), sizeof(uint64_t));
      if (xorKey != 0) {
         firstChunk ^= xorKey;
      }

      auto magicBytes = Config::BitcoinSettings::getMagicBytes();
      auto isMatch = std::memcmp(
         reinterpret_cast<char*>(&firstChunk),
         magicBytes.getPtr(),
         magicBytes.getSize()
      ) == 0;
      return { isMatch, isMatch ? totalChainSize : SIZE_MAX };
   }

   std::string getVersionStringFromOutput(const std::string& output)
   {
      if (output.size() < coreVersionHeader.size()) {
         throw std::runtime_error("invalid node executable output size");
      }
      std::string_view header{output.c_str(), coreVersionHeader.size()};
      if (header != coreVersionHeader) {
         throw std::runtime_error("node executable output header mismatch");
      }

      //assume next token is version string
      std::string_view body{
         output.c_str() + coreVersionHeader.size(),
         output.size() - coreVersionHeader.size()
      };
      auto end = body.find(' ');
      if (end == std::string_view::npos) {
         throw std::runtime_error("failed to tokenize node output");
      }
      return std::string{ body.substr(0, end) };
   }

   std::pair<bool, std::string> validateSatoshiBinary(
      const std::filesystem::path& binPath,
      const std::filesystem::path& dataDir)
   {
      /*
      Run `bitcoin --version`, grab stdout.
      Parse it for the version string.

      posix_spawn version, runs into permission issues, revisit later
      */
      if (!FileUtils::pathExists(binPath, 8)) {
         return { false, std::format(
            "{} is not an executable", binPath.string()) };
      }

      /***
      bitcoind will fail to detect the datadir when ran from fork/execv.
      We need to feed it the fully qualified datadir path.
      bitcoind wants to create the wallets folder regardless of start
      condition so pass it the correct datadir if we know it, or a
      temp folder.
      ***/
      std::filesystem::path targetDir = dataDir;
      if (dataDir.empty()) {
         auto randomFragemnt = Cryptography::PRNG::fortuna.generateRandom(4);
         targetDir = std::filesystem::temp_directory_path() / randomFragemnt.toHexStr();
         std::filesystem::create_directory(targetDir);
      }

      if (!targetDir.is_absolute()) {
         //is this a fully qualified path?
         targetDir = std::filesystem::absolute(dataDir);
      }
      auto dataDirStr = std::format("--datadir={}", targetDir.string());

      //bitcoind --version
      auto binPathStr = binPath.string();
      char* argv[] = {
         //first arg has to be binary's path
         binPathStr.data(),
         //version arg
         dataDirStr.data(),
         (char*)"--version"sv.data(),
         (char*)"--disablewallet"sv.data(),
         //mandatory terminator
         (char*)nullptr
      };

      //setup pipe to feed to child
      int stdout_pipe[2];
      if (pipe(stdout_pipe) != 0) {
         return { false, "failed to setup pipes" };
      }

      //tell posix_spawn to substitute child's fd 1 (stdout) with our pipe
      posix_spawn_file_actions_t ps_actions;
      posix_spawn_file_actions_init(&ps_actions);
      posix_spawn_file_actions_adddup2(&ps_actions, stdout_pipe[1], 1);

      std::string stdout_str;
      int pid = -1;
      int result = posix_spawn(&pid, argv[0], &ps_actions, nullptr, argv, nullptr);
      if (result == 0 && pid != -1) {
         //read the pipe, with 2sec timeout
         struct pollfd pfd{ stdout_pipe[0], POLLIN };
         while (poll(&pfd, 1, 2000) > 0) {
            stdout_str.resize(1024);
            auto bytesread = read(stdout_pipe[0], stdout_str.data(), 1023);
            if (bytesread > 0) {
               stdout_str.resize(bytesread);
            } else {
               stdout_str.clear();
            }
         }
      }

      //wait on pid
      int waitpid_status;
      waitpid(pid, &waitpid_status, 0);

      //cleanup pipes and posix_spawn data
      close(stdout_pipe[0]);
      close(stdout_pipe[1]);
      posix_spawn_file_actions_destroy(&ps_actions);

      if (dataDir.empty()) {
         std::filesystem::remove_all(targetDir);
      }

      try {
         return { true, getVersionStringFromOutput(stdout_str) };
      } catch (const std::exception& e) {
         return { false, e.what() };
      }
   }

   /////////////////////////////////////////////////////////////////////////////
   class ShutdownCallback : public RemoteCallback
   {
   private:
      std::unique_ptr<std::promise<bool>> promPtr_;
      std::future<bool> fut_;

   public:
      ShutdownCallback()
      {
         promPtr_ = std::make_unique<std::promise<bool>>();
         fut_ = promPtr_->get_future();
      }

      //virtuals
      void run(BdmNotification) override
      {}

      void progress(BDMPhase,
         const std::vector<std::string>&,
         float, unsigned, unsigned
      ) override
      {}

      void disconnected() override
      {
         promPtr_->set_value(true);
      }

      void waitOnDisconnect()
      {
         fut_.get();
      }
   };
}

////////////////////////////////////////////////////////////////////////////////
BdvPtr Armory::Bridge::setupClientConnection(
   std::shared_ptr<Wallets::AuthorizedPeers> peers,
   const std::string& ip, const std::string& port, bool oneWayAuth,
   const std::function<bool(const BinaryData&)>& presentPubKeyFunc,
   std::shared_ptr<RemoteCallback> cbPtr)
{
   LOGINFO << "connecting to ArmoryDB by IP";

   //sanity check
   if (peers == nullptr) {
      throw std::runtime_error("null peers db");
   }

   //setup bdv obj
   BdvPtr bdvPtr = AsyncClient::BlockDataViewer::getNewBDV(
      ip, port,
      peers, oneWayAuth,
      cbPtr
   );

   if (presentPubKeyFunc) {
      bdvPtr->setCheckServerKeyPromptLambda(presentPubKeyFunc);
   }

   //connect to db
   if (!bdvPtr->connectToRemote()) {
      return nullptr;
   }
   bdvPtr->registerWithDB(
      Config::BitcoinSettings::getMagicBytes().toHexStr());

   //notify setup is done
   return bdvPtr;
}

BdvPtr Armory::Bridge::setupClientConnection(
   std::shared_ptr<Wallets::AuthorizedPeers> peers,
   const Wallets::PeerKey& peerObj,
   std::shared_ptr<RemoteCallback> cbPtr)
{
   LOGINFO << "connecting to ArmoryDB by peer";

   auto peerNames = peers->getPeerNameMap(peerObj.isOneWay());
   for (const auto& peerName : peerNames) {
      if (std::memcmp(peerObj.getKey().getPtr(),
         peerName.second.pubkey,
         BIP151PUBKEYSIZE) != 0) {
         continue;
      }

      auto ipAndPort = getIpAndPortFromPeerName(peerName.first);
      auto bdvPtr = setupClientConnection(peers,
         ipAndPort.first, ipAndPort.second,
         peerObj.isOneWay(), {},
         cbPtr
      );
      if (bdvPtr != nullptr) {
         return bdvPtr;
      }
   }
   return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
std::filesystem::path Node::Core::findDatadir()
{
   auto home = FileUtils::getUserHomePath();
   for (const auto& candidate : satoshiDirCandidates) {
      std::filesystem::path dataDir = home / candidate;

      //look for /blocks folder
      auto blocksDir = getBlocksDir(dataDir);
      if (validateBlocksDir(blocksDir).first == true) {
         return dataDir;
      }
   }
   throw std::runtime_error("could not find a valid satoshi datadir");
}

std::filesystem::path Node::Core::findBinary()
{
   //look for bitcoind
   for (const auto& candidate : satoshiDirCandidates) {
      auto binPath = std::filesystem::path{candidate} / bitcoind;
      if (validateSatoshiBinary(binPath, {}).first == true) {
         return binPath;
      }
   }
   throw std::runtime_error("could not find a valid satoshi binary");}

////////
Node::Core::DatadirState Node::Core::validateDatadir(
   const std::filesystem::path& datadir)
{
   auto satoshiDir = getSatoshiDatadir(datadir);
   auto blocksDirValidation = validateBlocksDir(satoshiDir);
   if (blocksDirValidation.first == false) {
      throw std::runtime_error(
         std::format("{} is not a valid satoshi datadir", satoshiDir.string()));
   }

   //inspect files in the folder
   bool hasCookie = false;
   std::vector<std::string> confLines;
   for (const auto& entry : std::filesystem::directory_iterator{satoshiDir}) {
      if (!entry.is_regular_file()) {
         continue;
      }

      auto fpath = entry.path();
      const auto& fName = fpath.filename();
      if (fName == bitcoinConfFile) {
         confLines = Config::SettingsUtils::getLines(fpath);
      } else if (fName == cookieFile) {
         hasCookie = true;
      }
   }

   bool isPruned = false;
   bool hasRpcLog = false;
   bool hasRpcPass = false;
   for (const auto& confLine : confLines) {
      auto keyval = Config::SettingsUtils::getKeyValFromLine(confLine, '=');
      if (keyval.first == prunedKey) {
         const auto& val = keyval.second;
         int result = INT32_MAX;
         auto charConvResult = std::from_chars(
            val.data(), val.data() + val.size(), result);
         if (charConvResult.ec == std::errc{} && result != 0) {
            isPruned = true;
         }
      } else if (keyval.first == rpcLogKey) {
         if (!keyval.second.empty()) {
            hasRpcLog = true;
         }
      } else if (keyval.second == rpcPassKey) {
         if (!keyval.second.empty()) {
            hasRpcPass = true;
         }
      }
   }

   //TODO: cookie detection has to be more subtle

   return DatadirState{
      satoshiDir,
      blocksDirValidation.second,
      isPruned
   };
}

Node::Core::BinaryState Node::Core::validateBinary(
   const std::filesystem::path& binPath)
{
   auto result = validateSatoshiBinary(binPath, {});
   if (result.first == false) {
      throw std::runtime_error(result.second);
   }
   return { binPath, result.second };
}

////////////////////////////////////////////////////////////////////////////////
// AutomationContext
AutomationContext::AutomationContext(
   const std::filesystem::path& satoshiDir,
   const std::filesystem::path& satoshiBin,
   const std::filesystem::path& dbDir,
   bool automateNode, bool automateDb) :
   satoshiDir_{satoshiDir}, satoshiBin_{satoshiBin}, dbDir_{dbDir},
   automateNode_{automateNode}, automateDb_{automateDb}
{}

uint32_t AutomationContext::getDbPort() const
{
   return dbPort_;
}

std::shared_ptr<Wallets::AuthorizedPeers> AutomationContext::getPeersDb() const
{
   return peers_;
}

////////
void AutomationContext::automateSatoshi()
{
   //sanity check
   if (autoSatoshiPid_ != -1) {
      throw std::runtime_error("already have an instance of Core");
   }

   /*TODO:
   Force rpc log/pass when automating core.
   rpcuser can be passed as a cli arg.
   rpcpassword can be passed via stdin using "-stdin -stdinrpcpass".
   stdin can be replaced by a pipe via posix_spawn
   */

   //randomize rpc log and pass
   rpcLogin_ = Cryptography::PRNG::fortuna.generateRandom(16).toHexStr();
   rpcPass_ = Cryptography::PRNG::fortuna.generateRandom(16).toHexStr();

   //setup argv
   auto binpathArg = satoshiBin_.string();
   auto datadirArg = std::format("--datadir={}", satoshiDir_.string());
   auto rpcUserArg = std::format("--rpcuser={}", rpcLogin_);
   char* argv[] = {
      //first arg has to be binary's path
      binpathArg.data(),
      //satoshi datadir
      datadirArg.data(),
      //rpc user
      rpcUserArg.data(),
      //extra spots, for testnet and stdin manipulation
      nullptr, nullptr,
      //null terminator
      nullptr
   };

   int lastIndex = 3;
   if (Config::BitcoinSettings::getMode() == Config::NETWORK_MODE_TESTNET) {
      argv[lastIndex++] = (char*)"--testnet"sv.data();
   }
   //enable reading rpc pass from stdin
   argv[lastIndex] = (char*)"--stdin --stdinrpcpass"sv.data();

   //hijack core's stdin
   int stdin_pipe[2];
   if (pipe(stdin_pipe) != 0) {
      throw std::runtime_error("failed to setup pipes");
   }

   //tell posix_spawn to substitute child's fd 2 (stdin) with our pipe
   posix_spawn_file_actions_t ps_actions;
   posix_spawn_file_actions_init(&ps_actions);
   posix_spawn_file_actions_adddup2(&ps_actions, stdin_pipe[0], 2);

   //spawn the node
   int result = posix_spawn(&autoSatoshiPid_, argv[0], &ps_actions, nullptr, argv, nullptr);
   if (result != 0 || autoSatoshiPid_ == -1) {
      throw std::runtime_error(std::format(
         "failed to spawn bitcoind with error: {}", strerror(errno)));
   }

   //feed it the rpc pass
   if (write(stdin_pipe[1], rpcPass_.data(), rpcPass_.size()) != rpcPass_.size()) {
      throw std::runtime_error("error while writing to Core's stdin");
   }

   //cleanup pipes and posix_spawn data
   close(stdin_pipe[0]);
   close(stdin_pipe[1]);
   posix_spawn_file_actions_destroy(&ps_actions);
}

bool AutomationContext::isSatoshiRunning()
{
   if (autoSatoshiPid_ == -1) {
      return false;
   }

   siginfo_t processInfo;
   memset(&processInfo, 0, sizeof(processInfo));
   if (waitid(P_PID, (pid_t)autoSatoshiPid_, &processInfo, WEXITED | WNOHANG) != 0) {
      return false;
   }

   if (processInfo.si_pid == 0) {
      return true;
   }
   if (processInfo.si_code == CLD_EXITED || processInfo.si_code == CLD_KILLED) {
      autoSatoshiPid_ = -1;
      return false;
   }
   return true;
}

void AutomationContext::cleanupSatoshi()
{
   /*
   If we started the core node, connect to it via RPC and ask it to shutdown
   */

   //TODO: use callback to notify of shutdown progression

   if (autoSatoshiPid_ == -1) {
      //not our node
      return;
   }

   try {
      //connect to RPC
      auto rpc = Node::Core::RPC::Client(false, rpcLogin_, rpcPass_);

      //request shutdown
      if (!rpc.shutdown()) {
         LOGERR << "rpc shutdown request was rejected by node";
      }

      //wait on pid
      int waitpid_status;
      waitpid(autoSatoshiPid_, &waitpid_status, 0);
   } catch (const JSON::Exception& e) {
      LOGERR << "rpc shutdown request failed with error: " << e.what();
   }
}

////////////////////////////////////////////////////////////////////////////////
#ifdef _WIN32
void* Armory::Bridge::autoDbHandle = INVALID_HANDLE_VALUE;

std::pair<std::shared_ptr<Wallets::AuthorizedPeers>, uint32_t>
Bridge::spawnDb(const std::filesystem::path& satoshiPath,
   const std::filesystem::path& dbDir)
{
   /*
   Use CreateProcess to spawn an instance of ArmoryDB, with tailored CLI args,
   environment and ephemeral AEAD 2-way handshake.

   Keys are prepared following these steps:
      1. CppBridge creates an ephemeral key store and adds its public key to
         the tailored envvar served to ArmoryDB via CreateProcess.
      2. CppBridge creates a random key file in the datadir and locks it.
         The handle is made inheritable and sent to ArmoryDB via envvars.
      3. ArmoryDB detects automation via the --ephemeral CLI arg.
         It creates an ephemeral key store, reads the caller pubkey from
         envvars, adds it to the store and sets it as the store's master key.
      4. ArmoryDB grabs the key file fd from envvars and writes its pubkey
         there. This should work by virtue of opened handle sharing
         rules set via CreateProcess and CreateFile.
      5. CppBridge detects changes to the key file, grabs the pubkey and
         injects it into its own store. The lock is released, the file closed
         and removed.
   */

   //sanity check
   if (autoDbHandle != INVALID_HANDLE_VALUE) {
      throw std::runtime_error("already have an instance of ArmoryDB");
   }

   const std::filesystem::path armoryDbPath{
      Config::Pathing::runningDir() / L"ArmoryDB.exe" };
   if (!FileUtils::pathExists(armoryDbPath, 0)) {
      throw std::runtime_error("invalid db binary path: " + armoryDbPath.string());
   }

   //1. setup ephemeral authPeers
   auto peers = std::make_shared<Wallets::AuthorizedPeers>();

   //generate random db port & set it
   uint32_t port = (rand() % 10000) + 50000;
   std::wstring dbPortStr{ L"--armorydb-port=" + std::to_wstring(port) };

   //db paths
   std::wstring dbDirWStr{ L"--dbdir=" + dbDir.wstring() };
   std::wstring dataDir{ L"--datadir=" + Config::getDataDir().wstring() };

   //btc network
   std::wstring network;
   switch (Config::BitcoinSettings::getMode())
   {
      case Config::NETWORK_MODE_TESTNET:
         network = std::wstring{L"--testnet"};
         break;

      case Config::NETWORK_MODE_REGTEST:
         network = std::wstring{L"--regtest"};
         break;

      default:
         network = std::wstring{L"--mainnet"};
   }

   //core settings
   std::wstring satoshiDir{
      L"--satoshi-datadir=" + satoshiPath.wstring() };
   std::wstring satoshiPort{
      L"--satoshi-port=" + Config::NetworkSettings::btcPortW() };

   std::wstring rpcPort{
      L"--satoshirpc-port=" + Config::NetworkSettings::rpcPortW() };

   //2. randomize a file name
   std::filesystem::path keyFilePath{ Config::getDataDir() /
      std::string{ "keyFile_" +
         Cryptography::PRNG::fortuna.generateRandom(7).toHexStr() }};

   //1. use CreateFile to generate a inheritable file handle
   SECURITY_DESCRIPTOR secDep;
   if (!InitializeSecurityDescriptor(&secDep, SECURITY_DESCRIPTOR_REVISION)) {
      throw std::runtime_error("failed to init keyFile security descriptor");
   }
   SECURITY_ATTRIBUTES secAtt;
   secAtt.nLength = sizeof(SECURITY_ATTRIBUTES);
   secAtt.lpSecurityDescriptor = &secDep;
   secAtt.bInheritHandle = true;

   auto fileHandle = CreateFileW(keyFilePath.c_str(),
      //child process (ArmoryDB) will inherit the handle for writing
      GENERIC_READ | GENERIC_WRITE,

      //no other process should be allowed to open the file while own it
      0,

      //handle should be inheritable
      &secAtt,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      NULL
   );
   if (fileHandle == INVALID_HANDLE_VALUE) {
      auto lastError = GetLastError();
      throw std::runtime_error("failed to create key file with error: " + lastError);
   }

   //2. use CreateProcess to spawn ArmoryDB, and have it inherit the file handle
   std::wstring commandLine{ armoryDbPath.wstring() + L" " +
      L"--ephemeral " + dbPortStr + L" " + dataDir + L" " + dbDirWStr + L" " +
      network + L" " + satoshiDir + L" " + satoshiPort + L" " + rpcPort
   };

   //mandatory, process handle is writting in pi after start
   STARTUPINFOW si;
   PROCESS_INFORMATION pi;
   ZeroMemory( &si, sizeof(si) );
   si.cb = sizeof(si);
   ZeroMemory( &pi, sizeof(pi) );

   /*
   On Windows we add envvars to the parent instead of creating custom ones.
   The child will inherit them.

   It seems that there is a set of undocumented envvars to provide for a
   binary to even run on Windows.
   */
   const auto& pubkey = peers->getOwnPublicKey();
   BinaryDataRef keyRef{pubkey.pubkey, 33};
   SetEnvironmentVariable("CALLER_PUBKEY", keyRef.toHexStr().c_str());
   SetEnvironmentVariable("KEYFILE_HANDLE", std::to_string((uint64_t)fileHandle).c_str());

   if (!CreateProcessW(NULL,
      commandLine.data(),
      NULL,
      NULL,
      true, //inherit parent handles where possible
      NORMAL_PRIORITY_CLASS,
      NULL, //no explicit envvars on windows, let child inherit parent's
      NULL,
      &si, &pi
   )) {
      auto lastError = GetLastError();
      throw std::runtime_error("failed to spawn ArmorDB with error: " + std::to_string(lastError));
   }
   autoDbHandle = pi.hProcess;
   CloseHandle(pi.hThread);

   //5. wait for db to set pubkey in shared file
   unsigned count = 0;
   while (true) {
      if (count >= 100) {
         throw std::runtime_error("autodb handshake timeout");
      }

      if (getFileSize(fileHandle) != 33) {
         //key file hasnt changed, keep polling
         std::this_thread::sleep_for(100ms);
         ++count;
         continue;
      }

      //grab db pubkey from shared file
      SecureBinaryData serverPubkey(33);
      DWORD sizeRead = 0;
      SetFilePointer(fileHandle, 0, NULL, FILE_BEGIN);
      if (!ReadFile(fileHandle, serverPubkey.getPtr(), 33, &sizeRead, NULL) || sizeRead != 33) {
         throw std::runtime_error("failed to read pubkey from key file");
      }

      //add db key to custom store
      std::string addr{"127.0.0.1:" + std::to_string(port)};
      peers->addPeer(serverPubkey, {addr}, {}, false);
      break;
   }

   //close & delete the file
   if (!CloseHandle(fileHandle)) {
      throw std::runtime_error("failed to close key file handle");
   }

   if (!std::filesystem::remove(keyFilePath)) {
      throw std::runtime_error("key file did not exists!");
      //fs::remove returns false if there was nothing to remove.
      //it will throw on failure.
   }

   //return ephemeral key store
   return { peers, port };
}
#else
void AutomationContext::automateDb()
{
   LOGINFO << "spawning ArmoryDB";

   /*
   Use posix_spawn, which wraps around fork() & execve() to spawn an instance
   of ArmoryDB, with tailored CLI args, environment and ephemeral AEAD 2-way
   handshake.

   Keys are prepared following these steps:
      1. CppBridge creates an ephemeral key store and adds its public key to
         the tailored envvar served to ArmoryDB via execve/posix_spawn.
      2. CppBridge spawn ArmoryDB, replacing stdout by a pipe.
      3. ArmoryDB detects automation via the --ephemeral CLI arg.
         It creates an ephemeral key store, reads the caller pubkey from
         envvars, adds it to the store and sets it as the store's master key.
      4. ArmoryDB writes its public key to stdout.
      5. CppBridge detects changes to the key file, grabs the pubkey and
         injects it into its own store. The pipe is cleaned up.
   */

   //sanity check
   if (autoDbPid_ != -1) {
      throw std::runtime_error("already have an instance of ArmoryDB");
   }

   //get full path to armorydb
   const std::filesystem::path armoryDbPath{
      Config::Pathing::runningDir() / "ArmoryDB" };
   if (!FileUtils::pathExists(armoryDbPath, 8)) {
      throw std::runtime_error("invalid db binary path: " + armoryDbPath.string());
   }

   //1. setup ephemeral authPeers
   peers_ = std::make_shared<Wallets::AuthorizedPeers>();
   const auto& pubkey = peers_->getOwnPublicKey();
   BinaryDataRef keyRef{pubkey.pubkey, 33};
   std::string keyStr{ "CALLER_PUBKEY=" + keyRef.toHexStr() };

   //generate random db port & set it
   dbPort_ = (rand() % 10000) + 50000;
   auto portStr = std::to_string(dbPort_);
   std::string dbPortStr{ "--armorydb-port=" + portStr };
   Armory::Config::NetworkSettings::setDbPort(portStr);

   //db paths
   std::string dbDirArg{ "--dbdir=" + dbDir_.string() };
   std::string dataDirArg{ "--datadir=" + Config::getDataDir().string() };

   //btc network
   std::string network;
   switch (Config::BitcoinSettings::getMode())
   {
      case Config::NETWORK_MODE_TESTNET:
         network = std::string{"--testnet"};
         break;

      case Config::NETWORK_MODE_REGTEST:
         network = std::string{"--regtest"};
         break;

      default:
         network = std::string{"--mainnet"};
   }

   //core settings
   std::string satoshiDir{
      "--satoshi-datadir=" + satoshiDir_.string() };
   std::string satoshiPort{
      "--satoshi-port=" + Config::NetworkSettings::btcPort() };
   std::string rpcPort{
      "--satoshirpc-port=" + Config::NetworkSettings::rpcPort() };

   //setup argv
   auto dbPathStr = armoryDbPath.string();
   char* argv[] = {
      //first arg has to be binary's path
      dbPathStr.data(),
      //ephemeral mode, custom port to listen to
      (char*)"--ephemeral"sv.data(), dbPortStr.data(),
      //datadir, dbdir
      dataDirArg.data(), dbDirArg.data(),
      //network & core settings
      network.data(), satoshiDir.data(), satoshiPort.data(), rpcPort.data(),
      (char*)nullptr
   };

   //2. replace ArmoryDB's stdout by a pipe
   int stdout_pipe[2];
   if (pipe(stdout_pipe) != 0) {
      throw std::runtime_error("failed to setup pipes");
   }

   //tell posix_spawn to substitute child's fd 1 (stdout) with our pipe
   posix_spawn_file_actions_t ps_actions;
   posix_spawn_file_actions_init(&ps_actions);
   posix_spawn_file_actions_adddup2(&ps_actions, stdout_pipe[1], 1);

   //setup envp
   auto rpcLogEnvArg = std::format("CORERPCLOG={}", rpcLogin_);
   auto rpcPassEnvArg = std::format("CORERPCPASS={}", rpcPass_);
   char* envp[] = {
      keyStr.data(),
      (char*)nullptr,
      (char*)nullptr,
      (char*)nullptr
   };

   if (automateNode_) {
      //feed custom rpc log/pass via envvars if we automate the node too
      envp[1] = rpcLogEnvArg.data();
      envp[2] = rpcPassEnvArg.data();
   }

   //spawn the db
   int result = posix_spawn(&autoDbPid_, argv[0], &ps_actions, nullptr, argv, envp);
   if (result != 0 || autoDbPid_ == -1) {
      throw std::runtime_error(
         "failed to spawn ArmoryDB with error: " + std::string{strerror(errno)});
   }

   //5. wait for db to push pubkey through stdout
   unsigned count = 0; bool gotKey = false;
   struct pollfd pfd{ stdout_pipe[0], POLLIN };
   while (poll(&pfd, 1, 500) > 0) {
      //grab db pubkey from pipe
      std::string stdOutStr; stdOutStr.resize(100);
      auto bytesread = read(stdout_pipe[0], stdOutStr.data(), 99);
      if (bytesread > 0) {
         stdOutStr.resize(bytesread);
         SecureBinaryData serverPubkey{READHEX(stdOutStr)};
         std::string addr{"127.0.0.1:" + portStr};
         try {
            peers_->addPeer(serverPubkey, {addr}, {}, false);
            gotKey = true;
            break;
         } catch (const Wallets::AuthorizedPeersException& e) {
            std::cout << e.what();
            continue;
         }
      }

      if (count++ >= 10) {
         throw std::runtime_error("autodb handshake timeout");
      }
   }

   if (!gotKey) {
      throw std::runtime_error("failed to grab public key from db");
   }

   //cleanup pipes and posix_spawn data
   close(stdout_pipe[0]);
   close(stdout_pipe[1]);
   posix_spawn_file_actions_destroy(&ps_actions);
}
#endif

////
bool AutomationContext::isDbRunning()
{
#ifdef _WIN32
   if (autoDbHandle == INVALID_HANDLE_VALUE) {
      return false;
   }

   if (WaitForSingleObject(autoDbHandle, 0) != WAIT_TIMEOUT) {
      //we need to close this handle after use
      CloseHandle(autoDbHandle);
      autoDbHandle = INVALID_HANDLE_VALUE;
      return false;
   }
#else
   if (autoDbPid_ == -1) {
      return false;
   }

   siginfo_t processInfo;
   memset(&processInfo, 0, sizeof(processInfo));
   if (waitid(P_PID, (pid_t)autoDbPid_, &processInfo, WEXITED | WNOHANG) != 0) {
      return false;
   }

   if (processInfo.si_pid == 0) {
      return true;
   }
   if (processInfo.si_code == CLD_EXITED || processInfo.si_code == CLD_KILLED) {
      autoDbPid_ = -1;
      return false;
   }
#endif
   return true;
}

////////
void AutomationContext::cleanupDb()
{
   if (autoDbPid_ == -1) {
      //not our db
      return;
   }

   //create bdv object
   auto callback = std::make_shared<ShutdownCallback>();
   auto port = std::to_string(dbPort_);
   auto bdvPtr = setupClientConnection(peers_,
      "127.0.0.1", port,
      false, nullptr, callback);
   if (bdvPtr == nullptr) {
      throw std::runtime_error("automatedDb connection failed");
   }

   //request db shutdown, wait on d/c notif
   bdvPtr->shutdown();
   callback->waitOnDisconnect();
   bdvPtr.reset();

   //wait on db shutdown
   while (isDbRunning()) {
      std::this_thread::sleep_for(100ms);
   }
}

////////////////////////////////////////////////////////////////////////////////
bool AutomationContext::run(const CallbackFunc& notifyStep)
{
   if (hasRun_) {
      throw std::runtime_error("context already in use");
   }
   hasRun_ = true;

   if (automateDb_) {
      //we always automate the db if we automate the node
      if (automateNode_) {
         notifyStep(AutomationStep::SpawnNode);
         automateSatoshi();
      }
      notifyStep(AutomationStep::SpawnDb);
      automateDb();
      return true;
   } else {
      return false;
   }
}

void AutomationContext::cleanup(const CallbackFunc& notifyStep)
{
   if (!hasRun_) {
      notifyStep(AutomationStep::Done);
      return;
   }
   hasRun_ = false;

   if (automateDb_) {
      notifyStep(AutomationStep::ShutdownDb);
      cleanupDb();
   }
   if (automateNode_) {
      notifyStep(AutomationStep::ShutdownNode);
      cleanupSatoshi();
   }
   notifyStep(AutomationStep::Done);
}
