################################################################################
#                                                                              #
#  Copyright (C) 2025, goatpig                                                 #
#  Distributed under the MIT license                                           #
#  See LICENSE-MIT or https://opensource.org/licenses/MIT                      #
#                                                                              #
################################################################################

import os

from qtpy import QtCore, QtWidgets
from armoryengine.ArmoryUtils import ARMORY_DB_DIR, ARMORY_HOME_DIR, \
   BTC_HOME_DIR, ARMORYDB_DEFAULT_PORT, LOGINFO
from armoryengine.Settings import TheSettings
from armoryengine.CppBridge import TheBridge
from armorycolors import htmlColor

import qtdialogs.qtdefines as qtdefines

# Database scenario constants
SCENARIO_DB_LOCAL = "Automate ArmoryDB"
SCENARIO_REMOTE_IP = "Connect to IP"
SCENARIO_REMOTE_PEER = "Connect to Peer"
SCENARIO_DB_NONE = "Offline"

def isRemoteScenario(scenario):
   return scenario in (SCENARIO_REMOTE_IP, SCENARIO_REMOTE_PEER)

# Validation limits
MAX_RAM_USAGE = 256      # Max RAM in 128MB increments (~32GB)
MAX_THREAD_COUNT = 64    # Max threads for DB operations

################################################################################
class PeerData:
   """
   Data class for remote database peer information.

   Matches bridge Peer structure:
   - publicKey: hex string (66 chars compressed secp256k1)
   - names: list of ip:port strings
   """
   def __init__(self, publicKey='', names=None):
      self.publicKey = publicKey
      self.names = names if names else []

   @classmethod
   def fromBridgePeer(cls, bridgePeer):
      """Create PeerData from bridge listPeers response."""
      return cls(
         publicKey=bridgePeer.publicKey,
         names=list(bridgePeer.names)
      )

   def displayName(self):
      """Return display name: first name or truncated public key."""
      if self.names:
         return self.names[0]
      if self.publicKey:
         return self.publicKey[:16] + '...'
      return 'unknown'

   def isOwn(self):
      """Check if this is the own peer, i.e. user's public key."""
      return 'own' in self.names

################################################################################
class AddPeerDialog(QtWidgets.QDialog):
   """Dialog for adding a new peer to the peers database."""
   def __init__(self, parent):
      super().__init__(parent)
      self.publicKey = ''
      self.address = ''
      self.setWindowTitle(self.tr('Add Peer'))
      self.setMinimumWidth(450)
      self.initUI()

   def initUI(self):
      """Initialize the add peer dialog UI."""
      layout = QtWidgets.QVBoxLayout(self)
      layout.setSpacing(12)

      formLayout = QtWidgets.QGridLayout()
      formLayout.setSpacing(8)

      # Public Key
      keyLabel = QtWidgets.QLabel(self.tr("Public Key, hex:"))
      self.keyEdit = QtWidgets.QLineEdit()
      self.keyEdit.setPlaceholderText("66 hex chars, compressed secp256k1")
      formLayout.addWidget(keyLabel, 0, 0)
      formLayout.addWidget(self.keyEdit, 0, 1)

      keyInfo = QtWidgets.QLabel(
         self.tr("Get this from the remote ArmoryDB startup output."))
      keyInfo.setStyleSheet(
         f"color: {htmlColor('DisableFG')}; "
         "font-style: italic; font-size: 9pt;")
      formLayout.addWidget(keyInfo, 1, 1)

      # Address - must be ip:port format
      addressLabel = QtWidgets.QLabel(self.tr("Address, ip:port:"))
      self.addressEdit = QtWidgets.QLineEdit()
      self.addressEdit.setPlaceholderText(
         f"e.g. 192.168.1.100:{ARMORYDB_DEFAULT_PORT}")
      formLayout.addWidget(addressLabel, 2, 0)
      formLayout.addWidget(self.addressEdit, 2, 1)

      addrInfo = QtWidgets.QLabel(
         self.tr("IP address and port only. Domains not supported yet."))
      addrInfo.setStyleSheet(
         f"color: {htmlColor('DisableFG')}; "
         "font-style: italic; font-size: 9pt;")
      formLayout.addWidget(addrInfo, 3, 1)

      formLayout.setColumnStretch(1, 1)
      layout.addLayout(formLayout)

      buttonBox = QtWidgets.QDialogButtonBox(
         QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel)
      buttonBox.accepted.connect(self.validateAndAccept)
      buttonBox.rejected.connect(self.reject)
      layout.addWidget(buttonBox)

   def validateAndAccept(self):
      """Validate input and accept if valid."""
      publicKey = self.keyEdit.text().strip()
      if not publicKey:
         QtWidgets.QMessageBox.warning(
            self, self.tr('Missing Public Key'),
            self.tr('Public key is required to add a peer.'))
         return

      validHexChars = '0123456789abcdefABCDEF'
      if len(publicKey) != 66 or not all(c in validHexChars for c in publicKey):
         QtWidgets.QMessageBox.warning(
            self, self.tr('Invalid Public Key'),
            self.tr('Public key must be 66 hex characters, 33 bytes.'))
         return

      address = self.addressEdit.text().strip()
      if not address or ':' not in address:
         QtWidgets.QMessageBox.warning(
            self, self.tr('Invalid Address'),
            self.tr('Please enter address as ip:port.'))
         return

      self.publicKey = publicKey.lower()
      self.address = address
      self.accept()

   def getPublicKey(self):
      """Return the entered public key."""
      return self.publicKey

   def getAddress(self):
      """Return the entered address as ip:port."""
      return self.address

################################################################################
class DatabaseTab(QtWidgets.QWidget):
   """
   Database settings tab for Setup Manager.

   Manages:
   - Database directory selection
   - Database scenario (automate/remote/offline)
   - ArmoryDB configuration (type, RAM, threads)
   - Remote database configuration:
     - Connect to known peer (2-way auth via peers DB)
     - Connect by IP (1-way auth, ad-hoc)
   - CLI command display (informational)

   Signals:
      testConnectionRequested: Emitted when user clicks Test Connection button.
         Parent (DlgSetupManager) handles the actual connection attempt.
   """
   # Signal to request connection test from parent dialog
   testConnectionRequested = QtCore.Signal()

   def __init__(self, parent, main=None):
      super().__init__(parent)
      self.main = main
      # Common widgets
      self.databaseDirEdit = None
      self.databaseScenarioCombo = None
      self.dirFrame = None
      self.cliFrame = None
      self.cliCommandLabel = None
      self.dbBootstrapLabel = None

      # Local (automate) mode widgets
      self.localDatabaseFrame = None
      self.databaseTypeCombo = None
      self.ramUsageEdit = None
      self.threadCountEdit = None

      # Remote mode widgets
      self.remoteFrame = None
      self.peerFrame = None
      self.ipFrame = None

      # Remote Peer sub-mode widgets
      self.peerList = None
      self.loadPeersDbButton = None
      self.addPeerButton = None
      self.handshakeModeCombo = None
      self.peersDbStatusLabel = None

      # Remote IP sub-mode widgets
      self.ipEdit = None
      self.portEdit = None
      self.ownKeyEdit = None

      # Test connection widgets
      self.testFrame = None
      self.testConnectionButton = None

      # State tracking
      self.peersDbLoaded = False
      self.cachedPeers = []
      self.ownPublicKey = ''

      self.initUI()

   def initUI(self):
      """Initialize the database settings tab UI."""
      mainLayout = QtWidgets.QVBoxLayout()
      mainLayout.setContentsMargins(14, 6, 14, 8)
      mainLayout.setSpacing(8)

      title = qtdefines.createTabTitle(self.tr('Database Settings'))
      mainLayout.addWidget(title)

      self.initWidgets()

      # Database scenario section (always on top)
      scenarioFrame = QtWidgets.QGroupBox(self.tr('Database Scenario'))
      scenarioLayout = QtWidgets.QVBoxLayout(scenarioFrame)
      scenarioLayout.setContentsMargins(12, 12, 12, 12)
      scenarioLayout.setSpacing(8)

      scenarioGrid = QtWidgets.QGridLayout()
      scenarioGrid.setSpacing(8)

      dbScenarioLabel = QtWidgets.QLabel(self.tr("Mode"))

      scenarioGrid.addWidget(dbScenarioLabel, 0, 0)
      scenarioGrid.addWidget(self.databaseScenarioCombo, 0, 1)
      scenarioGrid.setColumnStretch(1, 1)

      scenarioLayout.addLayout(scenarioGrid)
      mainLayout.addWidget(scenarioFrame)

      self.dirFrame = QtWidgets.QGroupBox(self.tr('Database Directory'))
      dirFrameLayout = QtWidgets.QVBoxLayout(self.dirFrame)
      dirFrameLayout.setContentsMargins(12, 12, 12, 12)
      dirFrameLayout.setSpacing(8)

      dirGrid = QtWidgets.QGridLayout()
      dirGrid.setSpacing(8)

      dbDirLabel = QtWidgets.QLabel(self.tr("Location"))
      dirInputLayout, _ = qtdefines.createDirectoryInputLayout(
         self, self.databaseDirEdit, self.tr('Select Database Directory'))

      dirGrid.addWidget(dbDirLabel, 0, 0)
      dirGrid.addLayout(dirInputLayout, 0, 1)
      dirGrid.setColumnStretch(1, 1)

      dirFrameLayout.addLayout(dirGrid)
      mainLayout.addWidget(self.dirFrame)

      self.createLocalDatabaseFrame()
      mainLayout.addWidget(self.localDatabaseFrame)

      self.createRemoteFrame()
      mainLayout.addWidget(self.remoteFrame)

      self.cliFrame = QtWidgets.QGroupBox(self.tr('Generated Command Line'))
      cliLayout = QtWidgets.QVBoxLayout(self.cliFrame)
      cliLayout.setContentsMargins(12, 12, 12, 12)
      cliLayout.addWidget(self.cliCommandLabel)
      mainLayout.addWidget(self.cliFrame)

      # Test connection frame (shown for local and remote, hidden for offline)
      # Centered alignment using qtdefines spacing
      self.testFrame = QtWidgets.QFrame()
      testFrameLayout = QtWidgets.QHBoxLayout(self.testFrame)
      testFrameLayout.setSpacing(qtdefines.UI_BUTTON_SPACING)
      testFrameLayout.setContentsMargins(0, 12, 0, qtdefines.UI_BUTTON_SPACING)
      testFrameLayout.addStretch(1)
      testFrameLayout.addWidget(self.testConnectionButton)
      testFrameLayout.addStretch(1)
      mainLayout.addWidget(self.testFrame)

      mainLayout.addStretch()

      self.remoteFrame.hide()
      self.updateCliCommandDisplay()

      self.setLayout(mainLayout)

   def initWidgets(self):
      """Initialize database tab widgets as instance variables."""
      # Common widgets
      self.databaseDirEdit = QtWidgets.QLineEdit()
      self.databaseDirEdit.setMinimumWidth(400)
      self.databaseDirEdit.textChanged.connect(self.updateCliCommandDisplay)

      self.databaseScenarioCombo = QtWidgets.QComboBox()
      self.databaseScenarioCombo.setFixedWidth(200)
      self.databaseScenarioCombo.addItems([
         SCENARIO_DB_LOCAL, SCENARIO_REMOTE_IP,
         SCENARIO_REMOTE_PEER, SCENARIO_DB_NONE])
      self.databaseScenarioCombo.currentIndexChanged.connect(
         self.handleScenarioChange)

      self.databaseTypeCombo = QtWidgets.QComboBox()
      self.databaseTypeCombo.setFixedWidth(200)
      self.databaseTypeCombo.addItems(["Full Database", "Supernode"])
      self.databaseTypeCombo.currentTextChanged.connect(
         self.updateCliCommandDisplay)

      self.ramUsageEdit = QtWidgets.QLineEdit()
      self.ramUsageEdit.setFixedWidth(100)
      self.ramUsageEdit.textChanged.connect(self.updateCliCommandDisplay)

      self.threadCountEdit = QtWidgets.QLineEdit()
      self.threadCountEdit.setFixedWidth(100)
      self.threadCountEdit.textChanged.connect(self.updateCliCommandDisplay)

      # Remote Peer sub-mode widgets
      self.peerList = QtWidgets.QListWidget()
      self.peerList.setMinimumWidth(200)
      self.peerList.setMinimumHeight(80)
      self.peerList.setMaximumHeight(140)
      self.peerList.setSelectionMode(
         QtWidgets.QAbstractItemView.SingleSelection)
      self.peerList.currentItemChanged.connect(
         self.onPeerSelectionChanged)
      hint = QtWidgets.QListWidgetItem(
         self.tr("Load peers database to see saved peers"))
      hint.setFlags(QtCore.Qt.NoItemFlags)
      self.peerList.addItem(hint)

      self.peersDbStatusLabel = QtWidgets.QLabel(
         self.tr('Load peers database:'))

      self.loadPeersDbButton = QtWidgets.QPushButton(
         self.tr("Load DB"))
      self.loadPeersDbButton.setFixedWidth(80)

      self.addPeerButton = QtWidgets.QPushButton(self.tr("Add Peer..."))
      self.addPeerButton.setFixedWidth(100)
      self.addPeerButton.clicked.connect(self.addPeerFromTab)
      self.addPeerButton.setEnabled(False)

      self.handshakeModeCombo = QtWidgets.QComboBox()
      self.handshakeModeCombo.setFixedWidth(200)
      self.handshakeModeCombo.addItems(
         ["1-way, client only", "2-way, mutual"])
      self.handshakeModeCombo.setToolTip(
         self.tr("1-way: client verifies server\n"
            "2-way: mutual authentication"))

      # Remote IP sub-mode widgets
      self.ipEdit = QtWidgets.QLineEdit()
      self.ipEdit.setMinimumWidth(150)
      self.ipEdit.setPlaceholderText("e.g. 192.168.1.100")
      self.ipEdit.textChanged.connect(self.updateCliCommandDisplay)

      self.portEdit = QtWidgets.QLineEdit()
      self.portEdit.setFixedWidth(80)
      self.portEdit.setPlaceholderText(str(ARMORYDB_DEFAULT_PORT))
      self.portEdit.textChanged.connect(self.updateCliCommandDisplay)

      # Own public key display for sharing
      self.ownKeyEdit = QtWidgets.QLineEdit()
      self.ownKeyEdit.setReadOnly(True)
      self.ownKeyEdit.setPlaceholderText(
         self.tr('Load peers DB to view'))

      # CLI command display
      self.cliCommandLabel = QtWidgets.QLabel()
      self.cliCommandLabel.setWordWrap(False)
      self.cliCommandLabel.setStyleSheet(
         f"background-color: {htmlColor('SlightBkgdDark')}; "
         f"border: 1px solid {htmlColor('Mid')}; "
         "padding: 8px; font-family: monospace; "
         f"font-size: 10pt; color: {htmlColor('Foreground')};")
      self.cliCommandLabel.setText(
         "CLI command will be generated based on settings")

      # Test connection button
      self.testConnectionButton = QtWidgets.QPushButton(
         self.tr("Connect"))
      self.testConnectionButton.setFixedWidth(140)
      self.testConnectionButton.setToolTip(
         self.tr("Connect to the database with current settings"))
      self.testConnectionButton.clicked.connect(self.onTestConnectionClicked)

   def createLocalDatabaseFrame(self):
      """Create the ArmoryDB configuration frame."""
      self.localDatabaseFrame = QtWidgets.QGroupBox(
         self.tr('ArmoryDB Configuration'))
      localDbLayout = QtWidgets.QVBoxLayout(self.localDatabaseFrame)
      localDbLayout.setContentsMargins(12, 12, 12, 12)
      localDbLayout.setSpacing(8)

      localDbGrid = QtWidgets.QGridLayout()
      localDbGrid.setSpacing(8)

      dbTypeLabel = QtWidgets.QLabel(self.tr("Database Type"))
      ramLabel = QtWidgets.QLabel(self.tr("RAM Usage, MB"))
      threadLabel = QtWidgets.QLabel(self.tr("Thread Count"))

      localDbGrid.addWidget(dbTypeLabel, 0, 0)
      localDbGrid.addWidget(self.databaseTypeCombo, 0, 1)
      localDbGrid.addWidget(ramLabel, 1, 0)
      localDbGrid.addWidget(self.ramUsageEdit, 1, 1)
      localDbGrid.addWidget(threadLabel, 2, 0)
      localDbGrid.addWidget(self.threadCountEdit, 2, 1)
      localDbGrid.setColumnStretch(1, 1)

      localDbLayout.addLayout(localDbGrid)

   def createRemoteFrame(self):
      """Create the remote database configuration frame."""
      self.remoteFrame = QtWidgets.QGroupBox(
         self.tr('Remote Connection'))
      remoteLayout = QtWidgets.QVBoxLayout(self.remoteFrame)
      remoteLayout.setContentsMargins(12, 12, 12, 12)
      remoteLayout.setSpacing(8)

      # Peer sub-frame
      self.peerFrame = QtWidgets.QFrame()
      peerLayout = QtWidgets.QVBoxLayout(self.peerFrame)
      peerLayout.setContentsMargins(0, 0, 0, 0)
      peerLayout.setSpacing(8)

      dbStatusLayout = QtWidgets.QHBoxLayout()
      dbStatusLayout.setSpacing(8)
      dbStatusLayout.addWidget(self.peersDbStatusLabel)
      dbStatusLayout.addWidget(self.loadPeersDbButton)
      dbStatusLayout.addStretch()
      peerLayout.addLayout(dbStatusLayout)

      peerListLabel = QtWidgets.QLabel(
         self.tr("Known Peers:"))
      peerLayout.addWidget(peerListLabel)
      peerLayout.addWidget(self.peerList)

      peerButtonLayout = QtWidgets.QHBoxLayout()
      peerButtonLayout.setSpacing(8)
      peerButtonLayout.addWidget(self.addPeerButton)
      peerButtonLayout.addStretch()
      handshakeLabel = QtWidgets.QLabel(self.tr("Auth:"))
      peerButtonLayout.addWidget(handshakeLabel)
      peerButtonLayout.addWidget(self.handshakeModeCombo)
      peerLayout.addLayout(peerButtonLayout)

      remoteLayout.addWidget(self.peerFrame)

      # IP sub-frame
      self.ipFrame = QtWidgets.QFrame()
      ipLayout = QtWidgets.QVBoxLayout(self.ipFrame)
      ipLayout.setContentsMargins(0, 0, 0, 0)
      ipLayout.setSpacing(8)

      ipGrid = QtWidgets.QGridLayout()
      ipGrid.setSpacing(8)
      ipLabel = QtWidgets.QLabel(self.tr("IP Address:"))
      portLabel = QtWidgets.QLabel(self.tr("Port:"))
      ipGrid.addWidget(ipLabel, 0, 0)
      ipGrid.addWidget(self.ipEdit, 0, 1)
      ipGrid.addWidget(portLabel, 0, 2)
      ipGrid.addWidget(self.portEdit, 0, 3)
      ipGrid.setColumnStretch(1, 1)
      ipLayout.addLayout(ipGrid)

      remoteLayout.addWidget(self.ipFrame)

      # Own public key (for sharing with peers)
      ownKeyLayout = QtWidgets.QVBoxLayout()
      ownKeyLayout.setContentsMargins(0, 12, 0, 0)
      ownKeyLayout.setSpacing(4)
      ownKeyHeader = QtWidgets.QLabel(
         self.tr("Your Public Key:"))
      ownKeyHeader.setToolTip(
         self.tr("Share with peers for mutual auth"))
      ownKeyLayout.addWidget(ownKeyHeader)
      ownKeyLayout.addWidget(self.ownKeyEdit)
      remoteLayout.addLayout(ownKeyLayout)

   def updateCliCommandDisplay(self):
      """Update the CLI command display based on current database settings.

      Informational only. Actual connection is via bridge API.
      """
      scenario = self.databaseScenarioCombo.currentText()
      if scenario == SCENARIO_DB_LOCAL:
         dbType = self.databaseTypeCombo.currentText()
         ramUsage = self.ramUsageEdit.text() or '50'
         threadCount = self.threadCountEdit.text() or '4'
         dbDir = self.databaseDirEdit.text() or ARMORY_DB_DIR
         dataDir = ARMORY_HOME_DIR or '/path/to/armory'
         if BTC_HOME_DIR:
            satoshiDir = os.path.join(
               BTC_HOME_DIR, 'blocks')
         else:
            satoshiDir = '/path/to/bitcoin/blocks'
         dbTypeArg = 'DB_SUPER' \
            if dbType == 'Supernode' else 'DB_FULL'
         cmdParts = [
            '# Local ArmoryDB (bridge: automateDb)',
            'ArmoryDB', '--ephemeral',
            f'--db-type={dbTypeArg}',
            f'--ram-usage={ramUsage}',
            f'--thread-count={threadCount}',
            f'--datadir="{dataDir}"',
            f'--dbdir="{dbDir}"',
            f'--satoshi-datadir="{satoshiDir}"'
         ]
      elif scenario == SCENARIO_REMOTE_PEER:
         selected = self.peerList.currentItem()
         peerName = selected.text() if selected \
            else 'none'
         authMode = '2-way' \
            if self.handshakeModeCombo.currentIndex() == 1 \
            else '1-way'
         cmdParts = [
            '# Remote via peer (bridge: connectToPeer)',
            f'Peer: {peerName}',
            f'Auth: {authMode}'
         ]
      elif scenario == SCENARIO_REMOTE_IP:
         ipAddr = self.ipEdit.text().strip() \
            or 'not set'
         portText = self.portEdit.text().strip() \
            or str(ARMORYDB_DEFAULT_PORT)
         cmdParts = [
            '# Remote via IP (bridge: connectToIp)',
            f'IP: {ipAddr}',
            f'Port: {portText}',
            'Auth: 1-way only'
         ]
      else:
         cmdParts = [
            '# Offline mode',
            'No database connection']
      commandLine = '\n  '.join(cmdParts)
      self.cliCommandLabel.setText(f"Settings:\n  {commandLine}")

   def handleScenarioChange(self, index):
      """Handle changes to the database scenario selection."""
      scenario = self.databaseScenarioCombo.itemText(index)
      isLocal = scenario == SCENARIO_DB_LOCAL
      isRemote = isRemoteScenario(scenario)
      isPeer = scenario == SCENARIO_REMOTE_PEER
      isIp = scenario == SCENARIO_REMOTE_IP
      isOffline = scenario == SCENARIO_DB_NONE

      self.dirFrame.setVisible(not isRemote)
      self.localDatabaseFrame.setVisible(isLocal)
      self.remoteFrame.setVisible(isRemote)
      self.peerFrame.setVisible(isPeer)
      self.ipFrame.setVisible(isIp)
      self.cliFrame.setVisible(not isRemote)
      self.testFrame.setVisible(not isOffline)

      if isPeer:
         selected = self.peerList.currentItem()
         if selected and selected.data(QtCore.Qt.UserRole):
            self.testConnectionButton.setText(
               self.tr("Connect: {}").format(
                  selected.text()))
         else:
            self.testConnectionButton.setText(
               self.tr("Connect"))
      else:
         self.testConnectionButton.setText(
            self.tr("Connect"))

      self.updateCliCommandDisplay()

   def onPeerSelectionChanged(self, current, previous):
      """Update connect button label with selected peer name."""
      if current and current.data(QtCore.Qt.UserRole):
         label = self.tr("Connect: {}").format(
            current.text())
         self.testConnectionButton.setText(label)
      else:
         self.testConnectionButton.setText(
            self.tr("Connect"))

   def onTestConnectionClicked(self):
      """Emit testConnectionRequested for parent to handle."""
      LOGINFO("Connect requested from DatabaseTab")
      self.testConnectionRequested.emit()

   def refreshPeerList(self):
      """Refresh peer list widget from bridge."""
      self.peerList.clear()
      self.cachedPeers = []
      self.ownPublicKey = ''

      if not self.peersDbLoaded:
         self._showPeerListHint(
            self.tr("Load peers database to see saved peers"))
         return

      bridgePeers = TheBridge.dbSetup.listPeers()
      for bp in bridgePeers:
         peer = PeerData.fromBridgePeer(bp)
         if peer.isOwn():
            self.ownPublicKey = peer.publicKey
            self.ownKeyEdit.setText(peer.publicKey)
         else:
            self.cachedPeers.append(peer)
            for name in peer.names:
               item = QtWidgets.QListWidgetItem(name)
               item.setData(QtCore.Qt.UserRole, peer)
               item.setToolTip(
                  f"Key: {peer.publicKey[:32]}...")
               self.peerList.addItem(item)

      if not self.cachedPeers:
         self._showPeerListHint(
            self.tr("No peers yet, use Add Peer to add one"))

   def _showPeerListHint(self, text):
      """Show a disabled hint item in the peer list."""
      hint = QtWidgets.QListWidgetItem(text)
      hint.setFlags(QtCore.Qt.NoItemFlags)
      self.peerList.addItem(hint)

   def addPeerFromTab(self):
      """Add a peer directly from the database tab."""
      if not self.peersDbLoaded:
         QtWidgets.QMessageBox.warning(
            self,
            self.tr('Peers DB Required'),
            self.tr('Load the peers database first to add peers.'))
         return
      dialog = AddPeerDialog(self)
      if dialog.exec_() == QtWidgets.QDialog.Accepted:
         publicKey = dialog.getPublicKey()
         address = dialog.getAddress()
         result = TheBridge.dbSetup.addPeer(publicKey, [address])
         if result.success:
            LOGINFO(f"Added peer: {address}")
            self.refreshPeerList()
         else:
            QtWidgets.QMessageBox.warning(
               self, self.tr('Add Peer Failed'),
               self.tr('Failed to add peer: {}').format(
                  result.error))

   def setPeersDbLoaded(self, loaded):
      """Update peers database loaded state and refresh UI."""
      self.peersDbLoaded = loaded
      self.refreshPeerList()
      self.addPeerButton.setEnabled(loaded)
      if loaded:
         self.peersDbStatusLabel.setText(
            self.tr('Peers database:'))
         self.loadPeersDbButton.setText(
            self.tr("Reload"))
         self.ownKeyEdit.setText(
            self.ownPublicKey if self.ownPublicKey
            else '')
         self.ownKeyEdit.setPlaceholderText(
            self.tr('Available after reload'))
      else:
         self.peersDbStatusLabel.setText(
            self.tr('Load peers database:'))
         self.loadPeersDbButton.setText(
            self.tr("Load DB"))
         self.ownKeyEdit.clear()
         self.ownKeyEdit.setPlaceholderText(
            self.tr('Load peers DB to view'))

   def setDbSettingsLocked(self, locked):
      """Lock or unlock DB settings after a successful connection.

      C++ bridge bug: bdvPtr_ is not reset after cleanupDb,
      so reconnection is not possible on the same session.
      Lock settings to prevent the user from changing them
      after a successful test connection.
      """
      self.databaseScenarioCombo.setEnabled(not locked)
      self.databaseDirEdit.setEnabled(not locked)
      self.databaseTypeCombo.setEnabled(not locked)
      self.ramUsageEdit.setEnabled(not locked)
      self.threadCountEdit.setEnabled(not locked)
      self.ipEdit.setEnabled(not locked)
      self.portEdit.setEnabled(not locked)
      self.testConnectionButton.setEnabled(not locked)
      if locked:
         self.testConnectionButton.setText(
            self.tr("Connected"))

   def loadSettings(self):
      """Load database tab settings from configuration."""
      self.databaseDirEdit.setText(os.path.normpath(ARMORY_DB_DIR))

      dbScenario = TheSettings.getSettingOrSetDefault(
         'DBScenario', SCENARIO_DB_LOCAL)
      # Migrate old scenario names
      if dbScenario == "Remote Database":
         oldMode = TheSettings.getSettingOrSetDefault(
            'RemoteMode', '')
         if 'Peer' in oldMode:
            dbScenario = SCENARIO_REMOTE_PEER
         else:
            dbScenario = SCENARIO_REMOTE_IP
      elif dbScenario in ("Remote (by Peer)", "Remote, by Peer"):
         dbScenario = SCENARIO_REMOTE_PEER
      elif dbScenario in ("Remote (by IP)", "Remote, by IP"):
         dbScenario = SCENARIO_REMOTE_IP
      self.databaseScenarioCombo.setCurrentText(dbScenario)

      # Check if database has already been bootstrapped
      dbIsBootstrapped = False
      if os.path.exists(ARMORY_DB_DIR):
         dbFiles = os.listdir(ARMORY_DB_DIR)
         dbIsBootstrapped = any(f.endswith('.db') or f.endswith('.ldb')
            for f in dbFiles)

      if dbIsBootstrapped:
         self.databaseScenarioCombo.setEnabled(False)
         if self.dbBootstrapLabel is None:
            self.dbBootstrapLabel = QtWidgets.QLabel(
               self.tr("Database already bootstrapped, cannot change mode"))
            self.dbBootstrapLabel.setStyleSheet(
               f"color: {htmlColor('TextWarn')}; "
               "font-style: italic;")
            layout = self.databaseScenarioCombo.parent().layout()
            if layout:
               layout.addWidget(self.dbBootstrapLabel)

      self.handleScenarioChange(self.databaseScenarioCombo.currentIndex())

      # Scenario-specific settings
      dbScenario = self.databaseScenarioCombo.currentText()
      if dbScenario == SCENARIO_DB_LOCAL:
         dbTypeSetting = TheSettings.getSettingOrSetDefault('DBType', 'DB_FULL')
         if dbTypeSetting == 'DB_SUPER':
            self.databaseTypeCombo.setCurrentText('Supernode')
         else:
            self.databaseTypeCombo.setCurrentText('Full Database')
         self.ramUsageEdit.setText(
            str(TheSettings.getSettingOrSetDefault('RAMUsage', 50)))
         self.threadCountEdit.setText(
            str(TheSettings.getSettingOrSetDefault('ThreadCount', 4)))
      elif isRemoteScenario(dbScenario):
         handshakeMode = TheSettings.getSettingOrSetDefault(
            'HandshakeMode', 0)
         self.handshakeModeCombo.setCurrentIndex(
            handshakeMode)
         savedHost = TheSettings.get('RemoteDBHost')
         savedPort = TheSettings.get('RemoteDBPort')
         if savedHost:
            self.ipEdit.setText(savedHost)
         if savedPort:
            self.portEdit.setText(str(savedPort))

   def collectSettings(self):
      """Return current database config from UI as a dict."""
      scenario = self.databaseScenarioCombo.currentText()
      isLocal = scenario == SCENARIO_DB_LOCAL
      isPeer = scenario == SCENARIO_REMOTE_PEER
      isIp = scenario == SCENARIO_REMOTE_IP

      peerName = ''
      ipAddr = ''
      ipPort = ''
      handshakeMode = 0

      if isPeer:
         selected = self.peerList.currentItem()
         if selected and selected.data(QtCore.Qt.UserRole):
            peerName = selected.text()
         handshakeMode = \
            self.handshakeModeCombo.currentIndex()
      elif isIp:
         ipAddr = self.ipEdit.text().strip()
         portText = self.portEdit.text().strip()
         ipPort = portText if portText \
            else str(ARMORYDB_DEFAULT_PORT)

      return {
         'dbPath': str(self.databaseDirEdit.text()),
         'scenario': str(scenario),
         'typeDisp': str(
            self.databaseTypeCombo.currentText()
            ) if isLocal else '',
         'ram': str(
            self.ramUsageEdit.text()) if isLocal else '',
         'threads': str(
            self.threadCountEdit.text()
            ) if isLocal else '',
         'peerName': peerName,
         'ipAddr': ipAddr,
         'ipPort': ipPort,
         'handshakeMode': handshakeMode,
      }

   def validate(self):
      """Validate database tab settings."""
      scenario = self.databaseScenarioCombo.currentText()
      if scenario == SCENARIO_DB_LOCAL:
         ramText = str(self.ramUsageEdit.text()).strip()
         threadText = str(self.threadCountEdit.text()).strip()
         if ramText:
            try:
               ram = int(ramText)
               if ram < 1 or ram > MAX_RAM_USAGE:
                  raise ValueError()
            except ValueError:
               msg = self.tr('RAM usage must be an integer between 1 and {}.')
               QtWidgets.QMessageBox.warning(self, self.tr('Invalid RAM Usage'),
                  msg.format(MAX_RAM_USAGE))
               return False
         if threadText:
            try:
               threads = int(threadText)
               if threads < 1 or threads > MAX_THREAD_COUNT:
                  raise ValueError()
            except ValueError:
               msg = self.tr('Thread count must be between '
                  '1 and {}.')
               QtWidgets.QMessageBox.warning(
                  self, self.tr('Invalid Thread Count'),
                  msg.format(MAX_THREAD_COUNT))
               return False
      elif scenario == SCENARIO_REMOTE_PEER:
         selected = self.peerList.currentItem()
         if not selected \
               or not selected.data(QtCore.Qt.UserRole):
            QtWidgets.QMessageBox.warning(
               self,
               self.tr('No Peer Selected'),
               self.tr('Select a known peer or use '
                  'Connect to IP mode.'))
            return False
      elif scenario == SCENARIO_REMOTE_IP:
         ipAddr = self.ipEdit.text().strip()
         if not ipAddr:
            QtWidgets.QMessageBox.warning(
               self,
               self.tr('Missing IP Address'),
               self.tr('Enter the remote database IP.'))
            return False
         portText = self.portEdit.text().strip()
         if portText:
            try:
               port = int(portText)
               if port < 1 or port > 65535:
                  raise ValueError()
            except ValueError:
               QtWidgets.QMessageBox.warning(
                  self,
                  self.tr('Invalid Port'),
                  self.tr('Port must be 1-65535.'))
               return False
      return True

   def validateDbPath(self):
      """
      Validate database directory exists, create if needed.
      Returns True if valid/created, False on error.
      """
      dbPath = str(self.databaseDirEdit.text())
      if not os.path.exists(dbPath):
         try:
            os.makedirs(dbPath)
         except (OSError, PermissionError) as e:
            QtWidgets.QMessageBox.critical(
               self,
               self.tr('Error'),
               self.tr('Cannot create database directory: {}').format(str(e))
            )
            return False
      return True
