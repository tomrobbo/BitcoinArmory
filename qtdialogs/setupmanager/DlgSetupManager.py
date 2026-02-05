################################################################################
#                                                                              #
#  Copyright (C) 2025, goatpig                                                 #
#  Distributed under the MIT license                                           #
#  See LICENSE-MIT or https://opensource.org/licenses/MIT                      #
#                                                                              #
################################################################################

from qtpy import QtCore, QtWidgets
from armoryengine.ArmoryUtils import CLI_OPTIONS, LOGINFO, LOGERROR
from armoryengine.Settings import TheSettings
from armoryengine.CppBridge import TheBridge, PeersDbCallback, ServerKeyCallback

from qtdialogs.ArmoryDialog import ArmoryDialog
import qtdialogs.qtdefines as qtdefines

from qtdialogs.setupmanager.WalletTab import WalletTab
from qtdialogs.setupmanager.CoreTab import CoreTab
from qtdialogs.setupmanager.DatabaseTab import (
   DatabaseTab, SCENARIO_DB_LOCAL, SCENARIO_DB_REMOTE, SCENARIO_DB_NONE,
   REMOTE_MODE_PEER, REMOTE_MODE_IP
)

# Dialog-specific constants (only used by this dialog)
MINIMUM_DIALOG_WIDTH = 500
MINIMUM_DIALOG_HEIGHT = 640

################################################################################
class DlgSetupManager(ArmoryDialog):
   """
   Setup Manager Dialog for Armory configuration.

   Orchestrates the three settings tabs:
   - Wallet settings and migration
   - Bitcoin Core configuration
   - Database settings (local/remote/none)
   """
   def __init__(self, parent=None, main=None):
      """Initialize the setup manager dialog."""
      super().__init__(parent, main)
      self.tabWidget = None
      self.walletTab = None
      self.coreTab = None
      self.databaseTab = None
      self.acceptButton = None
      self.cancelButton = None
      self.bottomFrame = None

      # Connection state tracking
      # When user clicks "Test Connection" and succeeds, we have a live connection
      self.connectionTested = False
      self.connectionSuccess = False

      # Callback state tracking for async operations
      self.peersDbCallback = None
      self.serverKeyCallback = None
      self.pendingConnectionResult = None  # For async connectToIp

      self.setupDialogProperties()
      self.initTabs()
      self.setupMainLayout()
      self.loadSettings()
      self.connectSignals()

   def setupDialogProperties(self):
      """Configure basic dialog properties and styling."""
      self.setMinimumWidth(MINIMUM_DIALOG_WIDTH)
      self.setMinimumHeight(MINIMUM_DIALOG_HEIGHT)
      self.setWindowTitle(self.tr('Armory Setup Manager'))
      self.setWindowFlags(QtCore.Qt.Window)
      self.setModal(True)
      self.setSizePolicy(QtWidgets.QSizePolicy.Preferred,
         QtWidgets.QSizePolicy.Preferred)
      qtdefines.applyDialogBaseStyle(self)

   def initTabs(self):
      """Initialize all tab widgets."""
      self.tabWidget = QtWidgets.QTabWidget()
      self.tabWidget.setContentsMargins(14, 6, 14, 8)
      self.walletTab = WalletTab(self, self.main)
      self.coreTab = CoreTab(self, self.main)
      self.databaseTab = DatabaseTab(self, self.main)
      self.tabWidget.addTab(self.walletTab, self.tr('Wallet Settings'))
      self.tabWidget.addTab(self.coreTab, self.tr('Core Settings'))
      self.tabWidget.addTab(self.databaseTab, self.tr('Database Settings'))
      if CLI_OPTIONS.offline:
         self.tabWidget.setTabEnabled(1, False)
         self.tabWidget.setTabEnabled(2, False)
         self.tabWidget.setTabText(1, self.tr('Core Settings (Offline)'))
         self.tabWidget.setTabText(2, self.tr('Database Settings (Offline)'))

   def setupMainLayout(self):
      """Set up the main layout with tabs and buttons."""
      mainLayout = QtWidgets.QVBoxLayout()
      mainLayout.setSpacing(8)
      mainLayout.setContentsMargins(0, 0, 0, 0)
      mainLayout.addWidget(self.tabWidget)
      self.createBottomButtonFrame()
      mainLayout.addWidget(self.bottomFrame)
      self.setLayout(mainLayout)

   def createBottomButtonFrame(self):
      """Create the bottom frame containing Accept/Cancel buttons."""
      self.bottomFrame = QtWidgets.QFrame()
      bottomLayout = QtWidgets.QHBoxLayout(self.bottomFrame)
      bottomLayout.setContentsMargins(14, 6, 14, 8)
      bottomLayout.setSpacing(8)
      buttonBox = QtWidgets.QDialogButtonBox()
      self.acceptButton = buttonBox.addButton(self.tr('Accept'),
         QtWidgets.QDialogButtonBox.AcceptRole)
      self.cancelButton = buttonBox.addButton(self.tr('Cancel'),
         QtWidgets.QDialogButtonBox.RejectRole)
      self.acceptButton.setFixedWidth(100)
      self.cancelButton.setFixedWidth(100)
      bottomLayout.addStretch(1)
      bottomLayout.addWidget(buttonBox)

   def connectSignals(self):
      """Connect all signals to their handlers."""
      self.acceptButton.clicked.connect(self.accept)
      self.cancelButton.clicked.connect(self.reject)

      # Connect database scenario changes to peers DB loading
      self.databaseTab.databaseScenarioCombo.currentIndexChanged.connect(
         self.onDbScenarioChanged)

      # Connect test connection request from DatabaseTab
      self.databaseTab.testConnectionRequested.connect(
         self.onTestConnectionRequested)

   def accept(self):
      """
      Validate settings, ensure valid connection, save, and accept.

      Connection validation rules (per maintainer requirements):
      - Offline mode: always allowed
      - Connection already tested and succeeded: use existing live connection
      - No test but valid settings: attempt connection now
      - No connection possible: force user to Database tab
      """
      if not self.validateAllSettings():
         return

      # Get current scenario
      dbSettings = self.databaseTab.collectSettings()
      scenario = dbSettings['scenario']

      # Offline mode is always valid - no connection needed
      if scenario == SCENARIO_DB_NONE:
         LOGINFO("Offline mode - no connection validation needed")
         self._saveAndAccept()
         return

      # If connection was tested and succeeded, we have a live connection
      if self.connectionTested and self.connectionSuccess:
         LOGINFO("Using existing tested connection")
         self._saveAndAccept()
         return

      # No explicit test - try to connect using current settings
      LOGINFO("No tested connection - attempting connection on accept")
      params = self.getDbConnectionParams()
      success, error = self.initiateDbConnection(params)

      if success:
         LOGINFO("Connection established on accept")
         self._saveAndAccept()
         return

      # Connection failed - force user to Database tab
      errorMsg = error if error else self.tr('Unknown error')
      QtWidgets.QMessageBox.warning(
         self,
         self.tr('Connection Required'),
         self.tr('Cannot save settings without a valid database connection.\n\n'
                 'Error: {}\n\n'
                 'Please configure a valid connection or select Offline mode.'
                 ).format(errorMsg)
      )
      self.tabWidget.setCurrentWidget(self.databaseTab)
      # Don't call super().accept() - dialog stays open

   def _saveAndAccept(self):
      """Save settings and accept the dialog."""
      try:
         self.saveSettings()
      except (OSError, IOError, PermissionError) as e:
         QtWidgets.QMessageBox.critical(
            self,
            self.tr('Settings Error'),
            self.tr('Failed to save settings: {}').format(str(e))
         )
         return
      super().accept()

   def showEvent(self, event):
      """Handle show event to ensure the dialog is properly displayed."""
      super().showEvent(event)
      flags = self.windowState()
      flags = flags & ~QtCore.Qt.WindowMinimized
      flags = flags | QtCore.Qt.WindowActive
      self.setWindowState(flags)
      self.raise_()

   def onBridgeReady(self):
      """Called when bridge is ready - load wallet list."""
      self.walletTab.loadWalletList()

   def registerWidgetActivateTime(self, widget):
      """Stub for entropy collection - no-op during setup."""
      pass

   def loadSettings(self):
      """Load settings for all tabs."""
      self.walletTab.loadSettings()
      self.coreTab.loadSettings()
      self.databaseTab.loadSettings()

      # If starting in remote mode, load peers database
      dbScenario = self.databaseTab.databaseScenarioCombo.currentText()
      if dbScenario == SCENARIO_DB_REMOTE:
         self.loadPeersDatabase()

   def onDbScenarioChanged(self, index):
      """Handle database scenario changes - load peers DB if switching to remote."""
      scenario = self.databaseTab.databaseScenarioCombo.itemText(index)
      if scenario == SCENARIO_DB_REMOTE:
         # Only load if not already loaded
         if not self.databaseTab.peersDbLoaded:
            reply = QtWidgets.QMessageBox.question(
               self,
               self.tr('Load Peers Database?'),
               self.tr('Remote mode requires the peers database.\n\n'
                  'Load it now to manage saved peers?'),
               QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No,
               QtWidgets.QMessageBox.Yes
            )
            if reply == QtWidgets.QMessageBox.Yes:
               self.loadPeersDatabase()

      # Reset connection state when scenario changes
      self.connectionTested = False
      self.connectionSuccess = False

   def onTestConnectionRequested(self):
      """
      Handle test connection request from DatabaseTab.

      Attempts to connect using current settings. A successful test establishes
      a live connection - no need to reconnect on dialog accept.
      """
      LOGINFO("Test connection requested")

      # Validate settings first
      if not self.databaseTab.validate():
         return

      # Get connection params and attempt connection
      params = self.getDbConnectionParams()
      scenario = params['scenario']

      if scenario == SCENARIO_DB_NONE:
         QtWidgets.QMessageBox.information(
            self,
            self.tr('Offline Mode'),
            self.tr('Offline mode selected - no connection to test.')
         )
         return

      # Mark as tested (will update success based on result)
      self.connectionTested = True
      self.connectionSuccess = False

      # Disable button during test
      self.databaseTab.testConnectionButton.setEnabled(False)
      self.databaseTab.testConnectionButton.setText(self.tr("Testing..."))

      try:
         success, error = self.initiateDbConnection(params)

         if success:
            self.connectionSuccess = True
            QtWidgets.QMessageBox.information(
               self,
               self.tr('Connection Successful'),
               self.tr('Successfully connected to ArmoryDB.')
            )
         else:
            errorMsg = error if error else self.tr('Unknown error')
            QtWidgets.QMessageBox.warning(
               self,
               self.tr('Connection Failed'),
               self.tr('Failed to connect to database:\n\n{}').format(errorMsg)
            )
      except Exception as e:
         LOGERROR(f"Connection test exception: {e}")
         QtWidgets.QMessageBox.critical(
            self,
            self.tr('Connection Error'),
            self.tr('Error during connection test:\n\n{}').format(str(e))
         )
      finally:
         # Re-enable button
         self.databaseTab.testConnectionButton.setEnabled(True)
         self.databaseTab.testConnectionButton.setText(self.tr("Test Connection"))

   def validateAllSettings(self):
      """Run validators on all tabs. Returns True if all valid."""
      if not self.coreTab.validateCorePath():
         return False
      if not self.walletTab.validate():
         return False
      if not self.coreTab.validate():
         return False
      if not self.databaseTab.validateDbPath():
         return False
      if not self.databaseTab.validate():
         return False
      return True

   def _setSettingIfChanged(self, name, value):
      """Only write setting to file if the value has actually changed."""
      try:
         currentValue = TheSettings.get(name)
         if currentValue != value:
            TheSettings.set(name, value)
      except KeyError:
         TheSettings.set(name, value)

   def saveSettings(self):
      """Save settings from all tabs."""
      walletSettings = self.walletTab.collectSettings()
      coreSettings = self.coreTab.collectSettings()
      dbSettings = self.databaseTab.collectSettings()

      # Paths
      self._setSettingIfChanged('CoreDataDir', coreSettings['corePath'])
      self._setSettingIfChanged('ArmoryDataDir', walletSettings['armoryPath'])
      self._setSettingIfChanged('DBDir', dbSettings['dbPath'])
      self._setSettingIfChanged('SatoshiDatadir', coreSettings['corePath'])

      # Core settings
      self._setSettingIfChanged('ManageSatoshi', coreSettings['manageSatoshi'])
      self._setSettingIfChanged('NetworkMode', coreSettings['networkMode'])
      if coreSettings['p2pPort']:
         self._setSettingIfChanged('BitcoinP2PPort', coreSettings['p2pPort'])
      if coreSettings['rpcPort']:
         self._setSettingIfChanged('BitcoinRPCPort', coreSettings['rpcPort'])

      # Database settings
      dbScenario = dbSettings['scenario']
      self._setSettingIfChanged('DBScenario', dbScenario)

      if dbScenario == SCENARIO_DB_LOCAL:
         dbTypeVal = 'DB_SUPER' if dbSettings['typeDisp'] == 'Supernode' \
            else 'DB_FULL'
         self._setSettingIfChanged('DBType', dbTypeVal)
         if dbSettings['ram']:
            self._setSettingIfChanged('RAMUsage', int(dbSettings['ram']))
         if dbSettings['threads']:
            self._setSettingIfChanged('ThreadCount', int(dbSettings['threads']))
      elif dbScenario == SCENARIO_DB_REMOTE:
         # Save remote mode preference
         self._setSettingIfChanged('RemoteMode', dbSettings['remoteMode'])
         self._setSettingIfChanged('HandshakeMode', dbSettings['handshakeMode'])

         # Save IP mode settings (for persistence even if using peer mode)
         if dbSettings['ipAddr']:
            self._setSettingIfChanged('RemoteDBHost', dbSettings['ipAddr'])
         if dbSettings['ipPort']:
            self._setSettingIfChanged('RemoteDBPort', dbSettings['ipPort'])

         # Note: Peer selection is stored in bridge's peers DB, not in settings
         # The selected peerName is used directly for connection, not persisted here

      if self.main:
         self.main.setSatoshiPaths()

   def getDbConnectionParams(self):
      """
      Get database connection parameters from current settings.

      Returns dict with keys:
      - scenario: SCENARIO_DB_LOCAL, SCENARIO_DB_REMOTE, or SCENARIO_DB_NONE
      - For LOCAL: satoshiPath, dbPath
      - For REMOTE: remoteMode, peerName OR (ipAddr, ipPort), oneWayAuth
      """
      dbSettings = self.databaseTab.collectSettings()
      coreSettings = self.coreTab.collectSettings()
      scenario = dbSettings['scenario']

      params = {'scenario': scenario}

      if scenario == SCENARIO_DB_LOCAL:
         params['satoshiPath'] = coreSettings['corePath']
         params['dbPath'] = dbSettings['dbPath']

      elif scenario == SCENARIO_DB_REMOTE:
         params['remoteMode'] = dbSettings['remoteMode']
         # Bridge uses oneWayAuth (True = 1-way, False = 2-way)
         # UI handshakeMode: 0 = 1-way, 1 = 2-way
         params['oneWayAuth'] = dbSettings['handshakeMode'] == 0

         if dbSettings['remoteMode'] == REMOTE_MODE_PEER:
            params['peerName'] = dbSettings['peerName']
         else:
            params['ipAddr'] = dbSettings['ipAddr']
            params['ipPort'] = dbSettings['ipPort'] if dbSettings['ipPort'] else '9001'

      return params

   def initiateDbConnection(self, params=None):
      """
      Initiate database connection based on settings.

      Args:
         params: Connection parameters dict (from getDbConnectionParams).
                 If None, will collect from current UI state.

      Returns:
         tuple: (success: bool, error: str or None)
      """
      if params is None:
         params = self.getDbConnectionParams()

      scenario = params['scenario']
      LOGINFO(f"Initiating DB connection: scenario={scenario}")

      if scenario == SCENARIO_DB_NONE:
         LOGINFO("Offline mode - no database connection")
         return (True, None)

      elif scenario == SCENARIO_DB_LOCAL:
         return self._connectLocalDb(params)

      elif scenario == SCENARIO_DB_REMOTE:
         return self._connectRemoteDb(params)

      return (False, "Unknown scenario")

   def _connectLocalDb(self, params):
      """Initiate local (automated) database connection."""
      try:
         satoshiPath = params.get('satoshiPath', '')
         dbPath = params.get('dbPath', '')

         LOGINFO(f"Calling automateDb: satoshiPath={satoshiPath}, dbPath={dbPath}")

         result = TheBridge.dbSetup.automateDb(
            satoshiPath=satoshiPath,
            dbDir=dbPath
         )

         if result.success:
            LOGINFO("automateDb succeeded")
            return (True, None)
         else:
            LOGERROR(f"automateDb failed: {result.error}")
            return (False, result.error)

      except Exception as e:
         LOGERROR(f"automateDb exception: {e}")
         return (False, str(e))

   def _connectRemoteDb(self, params):
      """Initiate remote database connection."""
      remoteMode = params['remoteMode']

      if remoteMode == REMOTE_MODE_PEER:
         return self._connectToPeer(params)
      else:
         return self._connectToIp(params)

   def _connectToPeer(self, params):
      """Connect to a saved peer from the peers database."""
      peerName = params.get('peerName', '')
      oneWayAuth = params.get('oneWayAuth', False)

      if not peerName:
         return (False, "No peer selected")

      try:
         LOGINFO(f"Calling connectToPeer: name={peerName}, oneWayAuth={oneWayAuth}")

         result = TheBridge.dbSetup.connectToPeer(
            peerName=peerName,
            oneWayAuth=oneWayAuth
         )

         if result.success:
            LOGINFO("connectToPeer succeeded")
            return (True, None)
         else:
            LOGERROR(f"connectToPeer failed: {result.error}")
            return (False, result.error)

      except Exception as e:
         LOGERROR(f"connectToPeer exception: {e}")
         return (False, str(e))

   def _connectToIp(self, params):
      """
      Connect to a remote database by IP address (ad-hoc, 1-way auth).

      This is an async operation:
      1. connectToIp is called
      2. Server presents its public key via callback
      3. User must approve/reject the key
      4. Connection completes based on user choice
      """
      ipAddr = params.get('ipAddr', '')
      ipPort = params.get('ipPort', '9001')

      if not ipAddr:
         return (False, "No IP address specified")

      try:
         LOGINFO(f"Calling connectToIp: {ipAddr}:{ipPort}")

         callbackId = f"connectToIp_{ipAddr}_{ipPort}"

         # Reset pending result
         self.pendingConnectionResult = None

         # Create callback handler for server key presentation
         self.serverKeyCallback = ServerKeyCallback(
            callbackId=callbackId,
            onPresentPubkey=self._onServerKeyPresented
         )

         # Initiate connection - async, result comes via callback
         TheBridge.dbSetup.connectToIp(
            ip=ipAddr,
            port=ipPort,
            callbackId=callbackId
         )

         LOGINFO("connectToIp initiated - waiting for server key")

         # For test connection flow, we need to wait for the callback
         # Use a QEventLoop with timeout to wait for async result
         loop = QtCore.QEventLoop()
         timer = QtCore.QTimer()
         timer.setSingleShot(True)
         timer.timeout.connect(loop.quit)

         # Check periodically if we got a result
         def checkResult():
            if self.pendingConnectionResult is not None:
               loop.quit()

         checkTimer = QtCore.QTimer()
         checkTimer.timeout.connect(checkResult)
         checkTimer.start(100)  # Check every 100ms

         timer.start(30000)  # 30 second timeout
         loop.exec_()

         checkTimer.stop()
         timer.stop()

         if self.pendingConnectionResult is None:
            return (False, "Connection timeout - no response from server")

         return self.pendingConnectionResult

      except Exception as e:
         LOGERROR(f"connectToIp exception: {e}")
         return (False, str(e))

   def _onServerKeyPresented(self, callback, serverPubkey):
      """
      Handle server public key presentation.

      Shows the key to user for approval. User must accept/reject.
      """
      LOGINFO(f"Server key presented: {serverPubkey[:20]}...")

      # Format key for display (break into chunks)
      formattedKey = '\n'.join([
         serverPubkey[i:i+16] for i in range(0, len(serverPubkey), 16)
      ])

      msg = self.tr(
         "The server is presenting its public key for verification.\n\n"
         "Server Public Key:\n{}\n\n"
         "Do you want to accept this connection?\n\n"
         "Note: Only accept if you trust this server."
      ).format(formattedKey)

      reply = QtWidgets.QMessageBox.question(
         self,
         self.tr('Verify Server Key'),
         msg,
         QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No,
         QtWidgets.QMessageBox.No
      )

      if reply == QtWidgets.QMessageBox.Yes:
         LOGINFO("User accepted server key")
         callback.replyAck(accept=True)
         self.pendingConnectionResult = (True, None)
      else:
         LOGINFO("User rejected server key")
         callback.replyAck(accept=False)
         self.pendingConnectionResult = (False, "Server key rejected by user")

   def loadPeersDatabase(self):
      """
      Load the peers database via bridge.

      This must be called before managing or selecting peers.
      Handles callbacks for:
      - unlockRequest: Prompt user for passphrase to unlock existing DB
      - setPassphrase: Prompt user to set passphrase for new DB
      """
      try:
         LOGINFO("Loading peers database...")

         callbackId = "loadPeersDb_setup"

         # Create callback handler for async responses
         self.peersDbCallback = PeersDbCallback(
            callbackId=callbackId,
            onUnlockRequest=self._onPeersDbUnlockRequest,
            onSetPassphrase=self._onPeersDbSetPassphrase,
            onSuccess=self._onPeersDbSuccess
         )

         # Initiate load - may return immediately if unencrypted,
         # or trigger callbacks if passphrase needed
         TheBridge.dbSetup.loadPeersDb(callbackId)

         # Note: For encrypted DBs, success comes via callback after passphrase
         # For now, we wait for the callback to update state
         LOGINFO("loadPeersDb initiated")
         return True

      except Exception as e:
         LOGERROR(f"loadPeersDb exception: {e}")
         QtWidgets.QMessageBox.warning(
            self,
            self.tr('Peers Database Error'),
            self.tr('Error loading peers database: {}').format(str(e))
         )
         return False

   def _onPeersDbUnlockRequest(self, callback, unlockRequest):
      """Handle peers DB unlock request - prompt for passphrase."""
      LOGINFO("Peers DB unlock request received")

      passphrase, ok = QtWidgets.QInputDialog.getText(
         self,
         self.tr('Unlock Peers Database'),
         self.tr('Enter passphrase to unlock the peers database:'),
         QtWidgets.QLineEdit.Password
      )

      if ok and passphrase:
         callback.replyUnlock(passphrase, success=True)
      else:
         callback.replyUnlock('', success=False)
         QtWidgets.QMessageBox.warning(
            self,
            self.tr('Peers Database'),
            self.tr('Peers database unlock cancelled.')
         )

   def _onPeersDbSetPassphrase(self, callback, passphraseRequest):
      """Handle peers DB new passphrase request - prompt for new passphrase."""
      LOGINFO("Peers DB set passphrase request received")

      # First passphrase entry
      passphrase, ok = QtWidgets.QInputDialog.getText(
         self,
         self.tr('Create Peers Database'),
         self.tr('Set a passphrase for the new peers database:'),
         QtWidgets.QLineEdit.Password
      )

      if not ok or not passphrase:
         callback.replySetPassphrase('', success=False)
         QtWidgets.QMessageBox.warning(
            self,
            self.tr('Peers Database'),
            self.tr('Peers database creation cancelled.')
         )
         return

      # Confirm passphrase
      confirm, ok = QtWidgets.QInputDialog.getText(
         self,
         self.tr('Confirm Passphrase'),
         self.tr('Confirm the passphrase:'),
         QtWidgets.QLineEdit.Password
      )

      if not ok or confirm != passphrase:
         callback.replySetPassphrase('', success=False)
         QtWidgets.QMessageBox.warning(
            self,
            self.tr('Passphrase Mismatch'),
            self.tr('Passphrases do not match. Creation cancelled.')
         )
         return

      # Send the passphrase with default KDF settings
      callback.replySetPassphrase(passphrase, kdfTargetMs=250, kdfTargetMB=32)

   def _onPeersDbSuccess(self, protoPacket):
      """Handle peers DB load success."""
      LOGINFO("Peers database loaded successfully")
      self.databaseTab.setPeersDbLoaded(True)
