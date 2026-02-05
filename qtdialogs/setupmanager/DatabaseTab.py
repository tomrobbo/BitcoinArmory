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

import qtdialogs.qtdefines as qtdefines

# Database scenario constants
SCENARIO_DB_LOCAL = "Automate ArmoryDB"
SCENARIO_DB_REMOTE = "Remote Database"
SCENARIO_DB_NONE = "Offline"

# Remote connection sub-modes
REMOTE_MODE_PEER = "Connect to Saved Peer"
REMOTE_MODE_IP = "Connect by IP (ad-hoc)"

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
      return '(unknown)'

   def isOwn(self):
      """Check if this is the 'own' peer (user's public key)."""
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

      # Public Key (required)
      keyLabel = QtWidgets.QLabel(self.tr("Public Key (hex):"))
      self.keyEdit = QtWidgets.QLineEdit()
      self.keyEdit.setPlaceholderText("66 hex chars (compressed secp256k1)")
      formLayout.addWidget(keyLabel, 0, 0)
      formLayout.addWidget(self.keyEdit, 0, 1)

      keyInfo = QtWidgets.QLabel(
         self.tr("Get this from the remote ArmoryDB startup output."))
      keyInfo.setStyleSheet("color: gray; font-style: italic; font-size: 9pt;")
      formLayout.addWidget(keyInfo, 1, 1)

      # Address (required) - must be ip:port format
      addressLabel = QtWidgets.QLabel(self.tr("Address (ip:port):"))
      self.addressEdit = QtWidgets.QLineEdit()
      self.addressEdit.setPlaceholderText(
         f"e.g. 192.168.1.100:{ARMORYDB_DEFAULT_PORT}")
      formLayout.addWidget(addressLabel, 2, 0)
      formLayout.addWidget(self.addressEdit, 2, 1)

      addrInfo = QtWidgets.QLabel(
         self.tr("IP address and port only. Domains not supported yet."))
      addrInfo.setStyleSheet("color: gray; font-style: italic; font-size: 9pt;")
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
            self.tr('Public key must be 66 hex characters (33 bytes).'))
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
      """Return the entered address (ip:port)."""
      return self.address

################################################################################
class PeerManagerDialog(QtWidgets.QDialog):
   """
   Dialog for managing and selecting remote database peers.

   Peers are stored in the bridge's peers database.
   This dialog provides UI for viewing, adding, and selecting peers.
   """
   def __init__(self, parent, peersDbLoaded=False):
      super().__init__(parent)
      self.peersDbLoaded = peersDbLoaded
      self.peers = []
      self.ownPublicKey = ''
      self.selectedPeerName = None
      self.setWindowTitle(self.tr('Manage Peers'))
      self.setMinimumWidth(550)
      self.setMinimumHeight(400)
      self.initUI()
      if self.peersDbLoaded:
         self.loadPeersFromBridge()

   def initUI(self):
      """Initialize the peer manager dialog UI."""
      layout = QtWidgets.QVBoxLayout(self)
      layout.setSpacing(12)

      # Own public key display
      ownKeyFrame = QtWidgets.QGroupBox(self.tr('Your Public Key'))
      ownKeyLayout = QtWidgets.QVBoxLayout(ownKeyFrame)
      ownKeyLayout.setContentsMargins(12, 12, 12, 12)

      self.ownKeyLabel = QtWidgets.QLabel(self.tr('(Not loaded)'))
      self.ownKeyLabel.setWordWrap(True)
      self.ownKeyLabel.setTextInteractionFlags(
         QtCore.Qt.TextSelectableByMouse | QtCore.Qt.TextSelectableByKeyboard)
      self.ownKeyLabel.setStyleSheet(
         "font-family: monospace; font-size: 9pt; padding: 4px; "
         "background-color: #f8f8f8; border: 1px solid #ddd;")
      ownKeyLayout.addWidget(self.ownKeyLabel)

      ownKeyInfo = QtWidgets.QLabel(
         self.tr("Share this key with peers who want to connect to you."))
      ownKeyInfo.setStyleSheet("color: gray; font-style: italic;")
      ownKeyLayout.addWidget(ownKeyInfo)

      layout.addWidget(ownKeyFrame)

      # Peer list
      listLabel = QtWidgets.QLabel(
         self.tr("Saved Peers (double-click to select):"))
      layout.addWidget(listLabel)

      self.peerList = QtWidgets.QListWidget()
      self.peerList.setSelectionMode(QtWidgets.QAbstractItemView.SingleSelection)
      self.peerList.itemDoubleClicked.connect(self.selectAndClose)
      layout.addWidget(self.peerList)

      # Action buttons
      actionLayout = QtWidgets.QHBoxLayout()

      self.refreshButton = QtWidgets.QPushButton(self.tr("Refresh"))
      self.refreshButton.clicked.connect(self.loadPeersFromBridge)
      actionLayout.addWidget(self.refreshButton)

      self.addButton = QtWidgets.QPushButton(self.tr("Add Peer..."))
      self.addButton.clicked.connect(self.addPeer)
      actionLayout.addWidget(self.addButton)

      actionLayout.addStretch()

      useButton = QtWidgets.QPushButton(self.tr("Use Selected"))
      useButton.clicked.connect(self.useSelectedPeer)
      actionLayout.addWidget(useButton)

      layout.addLayout(actionLayout)

      # Status label
      self.statusLabel = QtWidgets.QLabel('')
      self.statusLabel.setStyleSheet("color: gray;")
      layout.addWidget(self.statusLabel)

      # Dialog buttons
      buttonBox = QtWidgets.QDialogButtonBox(
         QtWidgets.QDialogButtonBox.Ok | QtWidgets.QDialogButtonBox.Cancel)
      buttonBox.accepted.connect(self.accept)
      buttonBox.rejected.connect(self.reject)
      layout.addWidget(buttonBox)

      if not self.peersDbLoaded:
         self.addButton.setEnabled(False)
         self.statusLabel.setText(
            self.tr('Peers database not loaded. Load it first to manage peers.'))

   def loadPeersFromBridge(self):
      """Load peers from bridge listPeers API."""
      self.peers = []
      self.ownPublicKey = ''
      bridgePeers = TheBridge.dbSetup.listPeers()
      for bp in bridgePeers:
         peer = PeerData.fromBridgePeer(bp)
         if peer.isOwn():
            self.ownPublicKey = peer.publicKey
         else:
            self.peers.append(peer)
      self.refreshDisplay()

   def refreshDisplay(self):
      """Refresh the UI with current peer data."""
      if self.ownPublicKey:
         self.ownKeyLabel.setText(self.ownPublicKey)
      else:
         self.ownKeyLabel.setText(self.tr('(Not available)'))

      self.peerList.clear()
      for peer in self.peers:
         displayText = peer.displayName()
         if len(peer.names) > 1:
            displayText += f" (+{len(peer.names) - 1} more)"
         item = QtWidgets.QListWidgetItem(displayText)
         item.setData(QtCore.Qt.UserRole, peer)
         item.setToolTip(f"Key: {peer.publicKey[:32]}...")
         self.peerList.addItem(item)

      if self.peers:
         self.statusLabel.setText(
            self.tr('{} peer(s) available').format(len(self.peers)))
      else:
         self.statusLabel.setText(self.tr('No peers saved yet.'))

   def addPeer(self):
      """Open dialog to add a new peer via bridge."""
      dialog = AddPeerDialog(self)
      if dialog.exec_() == QtWidgets.QDialog.Accepted:
         publicKey = dialog.getPublicKey()
         address = dialog.getAddress()
         result = TheBridge.dbSetup.addPeer(publicKey, [address])
         if result.success:
            LOGINFO(f"Added peer: {address}")
            self.loadPeersFromBridge()
         else:
            QtWidgets.QMessageBox.warning(
               self, self.tr('Add Peer Failed'),
               self.tr('Failed to add peer: {}').format(result.error))

   def useSelectedPeer(self):
      """Set selected peer and close dialog."""
      selected = self.peerList.currentItem()
      if selected:
         peer = selected.data(QtCore.Qt.UserRole)
         if peer.names:
            self.selectedPeerName = peer.names[0]
            self.accept()

   def selectAndClose(self, item):
      """Handle double-click to select peer and close."""
      peer = item.data(QtCore.Qt.UserRole)
      if peer.names:
         self.selectedPeerName = peer.names[0]
         self.accept()

   def getSelectedPeerName(self):
      """Return the selected peer name (ip:port), if any."""
      return self.selectedPeerName

   def getOwnPublicKey(self):
      """Return the user's own public key."""
      return self.ownPublicKey

################################################################################
class DatabaseTab(QtWidgets.QWidget):
   """
   Database settings tab for Setup Manager.

   Manages:
   - Database directory selection
   - Database scenario (automate/remote/offline)
   - ArmoryDB configuration (type, RAM, threads)
   - Remote database configuration:
     - Connect to saved peer (2-way auth via peers DB)
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
      self.remoteModeCombo = None
      self.peerFrame = None
      self.ipFrame = None

      # Remote Peer sub-mode widgets
      self.peerCombo = None
      self.managePeersButton = None
      self.handshakeModeCombo = None

      # Remote IP sub-mode widgets
      self.ipEdit = None
      self.portEdit = None
      self.ownKeyLabel = None

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

      # Database scenario section
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
         SCENARIO_DB_LOCAL, SCENARIO_DB_REMOTE, SCENARIO_DB_NONE])
      self.databaseScenarioCombo.currentIndexChanged.connect(
         self.handleScenarioChange)

      # Local mode widgets
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

      # Remote mode widgets
      self.remoteModeCombo = QtWidgets.QComboBox()
      self.remoteModeCombo.setFixedWidth(220)
      self.remoteModeCombo.addItems([REMOTE_MODE_PEER, REMOTE_MODE_IP])
      self.remoteModeCombo.currentIndexChanged.connect(
         self.handleRemoteModeChange)

      # Remote Peer sub-mode widgets
      self.peerCombo = QtWidgets.QComboBox()
      self.peerCombo.setMinimumWidth(200)

      self.managePeersButton = QtWidgets.QPushButton(self.tr("Manage..."))
      self.managePeersButton.setFixedWidth(80)
      self.managePeersButton.clicked.connect(self.openPeerManager)

      self.handshakeModeCombo = QtWidgets.QComboBox()
      self.handshakeModeCombo.setFixedWidth(200)
      self.handshakeModeCombo.addItems(["1-way (client only)", "2-way (mutual)"])
      self.handshakeModeCombo.setToolTip(
         self.tr("1-way: client verifies server\n2-way: mutual authentication"))

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
      self.ownKeyLabel = QtWidgets.QLabel(self.tr('(Load peers DB to view)'))
      self.ownKeyLabel.setWordWrap(True)
      self.ownKeyLabel.setTextInteractionFlags(
         QtCore.Qt.TextSelectableByMouse | QtCore.Qt.TextSelectableByKeyboard)
      self.ownKeyLabel.setStyleSheet(
         "font-family: monospace; font-size: 8pt; padding: 4px; "
         "background-color: #f8f8f8; border: 1px solid #ddd;")

      # CLI command display
      self.cliCommandLabel = QtWidgets.QLabel()
      self.cliCommandLabel.setWordWrap(False)
      self.cliCommandLabel.setStyleSheet(
         "background-color: #f0f0f0; border: 1px solid #ccc; padding: 8px; "
         "font-family: monospace; font-size: 10pt; color: #333;")
      self.cliCommandLabel.setText(
         "CLI Command: (will be generated based on settings)")

      # Test connection button
      self.testConnectionButton = QtWidgets.QPushButton(
         self.tr("Test Connection"))
      self.testConnectionButton.setFixedWidth(140)
      self.testConnectionButton.setToolTip(
         self.tr("Test connection to database with current settings.\n"
                 "A successful test establishes a live connection."))
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
      ramLabel = QtWidgets.QLabel(self.tr("RAM Usage (MB)"))
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
      """Create the remote database configuration frame with sub-modes."""
      self.remoteFrame = QtWidgets.QGroupBox(self.tr('Remote Connection'))
      remoteLayout = QtWidgets.QVBoxLayout(self.remoteFrame)
      remoteLayout.setContentsMargins(12, 12, 12, 12)
      remoteLayout.setSpacing(8)

      # Connection mode selector
      modeLayout = QtWidgets.QHBoxLayout()
      modeLabel = QtWidgets.QLabel(self.tr("Connection Mode:"))
      modeLayout.addWidget(modeLabel)
      modeLayout.addWidget(self.remoteModeCombo)
      modeLayout.addStretch()
      remoteLayout.addLayout(modeLayout)

      # Sub-frame for saved peer connection
      self.peerFrame = QtWidgets.QFrame()
      peerFrameLayout = QtWidgets.QVBoxLayout(self.peerFrame)
      peerFrameLayout.setContentsMargins(0, 8, 0, 0)
      peerFrameLayout.setSpacing(8)

      peerGrid = QtWidgets.QGridLayout()
      peerGrid.setSpacing(8)

      peerLabel = QtWidgets.QLabel(self.tr("Saved Peer:"))
      peerSelectLayout = QtWidgets.QHBoxLayout()
      peerSelectLayout.setSpacing(8)
      peerSelectLayout.addWidget(self.peerCombo, 1)
      peerSelectLayout.addWidget(self.managePeersButton)

      handshakeLabel = QtWidgets.QLabel(self.tr("Auth Mode:"))

      peerGrid.addWidget(peerLabel, 0, 0)
      peerGrid.addLayout(peerSelectLayout, 0, 1)
      peerGrid.addWidget(handshakeLabel, 1, 0)
      peerGrid.addWidget(self.handshakeModeCombo, 1, 1)
      peerGrid.setColumnStretch(1, 1)
      peerFrameLayout.addLayout(peerGrid)

      peerInfo = QtWidgets.QLabel(
         self.tr("Uses peers database for authenticated connections."))
      peerInfo.setStyleSheet("color: gray; font-style: italic;")
      peerFrameLayout.addWidget(peerInfo)

      remoteLayout.addWidget(self.peerFrame)

      # Sub-frame for direct IP connection
      self.ipFrame = QtWidgets.QFrame()
      ipFrameLayout = QtWidgets.QVBoxLayout(self.ipFrame)
      ipFrameLayout.setContentsMargins(0, 8, 0, 0)
      ipFrameLayout.setSpacing(8)

      ipGrid = QtWidgets.QGridLayout()
      ipGrid.setSpacing(8)

      ipLabel = QtWidgets.QLabel(self.tr("IP Address:"))
      portLabel = QtWidgets.QLabel(self.tr("Port:"))

      ipGrid.addWidget(ipLabel, 0, 0)
      ipGrid.addWidget(self.ipEdit, 0, 1)
      ipGrid.addWidget(portLabel, 0, 2)
      ipGrid.addWidget(self.portEdit, 0, 3)
      ipGrid.setColumnStretch(1, 1)
      ipFrameLayout.addLayout(ipGrid)

      ipInfo = QtWidgets.QLabel(
         self.tr("1-way auth only. Server key will be shown for approval."))
      ipInfo.setStyleSheet("color: gray; font-style: italic;")
      ipFrameLayout.addWidget(ipInfo)

      remoteLayout.addWidget(self.ipFrame)

      # Own public key section (for sharing with peers)
      ownKeyFrame = QtWidgets.QFrame()
      ownKeyLayout = QtWidgets.QVBoxLayout(ownKeyFrame)
      ownKeyLayout.setContentsMargins(0, 12, 0, 0)
      ownKeyLayout.setSpacing(4)

      ownKeyHeader = QtWidgets.QLabel(self.tr("Your Public Key (share with peers):"))
      ownKeyLayout.addWidget(ownKeyHeader)
      ownKeyLayout.addWidget(self.ownKeyLabel)

      remoteLayout.addWidget(ownKeyFrame)

      # Initial sub-frame visibility
      self.ipFrame.hide()

   def updateCliCommandDisplay(self):
      """Update the CLI command display based on current database settings.

      Note: This is informational only. Actual connection is done via bridge API.
      """
      dbScenario = self.databaseScenarioCombo.currentText()
      if dbScenario == SCENARIO_DB_LOCAL:
         dbType = self.databaseTypeCombo.currentText()
         ramUsage = self.ramUsageEdit.text() or '50'
         threadCount = self.threadCountEdit.text() or '4'
         dbDir = self.databaseDirEdit.text() or ARMORY_DB_DIR
         dataDir = ARMORY_HOME_DIR or '/path/to/armory'
         if BTC_HOME_DIR:
            satoshiDir = os.path.join(BTC_HOME_DIR, 'blocks')
         else:
            satoshiDir = '/path/to/bitcoin/blocks'
         dbTypeArg = 'DB_SUPER' if dbType == 'Supernode' else 'DB_FULL'
         cmdParts = [
            '# Local ArmoryDB (bridge: automateDb)',
            'ArmoryDB',
            '--ephemeral',
            f'--db-type={dbTypeArg}',
            f'--ram-usage={ramUsage}',
            f'--thread-count={threadCount}',
            f'--datadir="{dataDir}"',
            f'--dbdir="{dbDir}"',
            f'--satoshi-datadir="{satoshiDir}"'
         ]
      elif dbScenario == SCENARIO_DB_REMOTE:
         remoteMode = self.remoteModeCombo.currentText()
         if remoteMode == REMOTE_MODE_PEER:
            if self.peerCombo.currentData():
               peerName = self.peerCombo.currentText()
            else:
               peerName = '(none)'
            authMode = '2-way' if self.handshakeModeCombo.currentIndex() == 1 \
               else '1-way'
            cmdParts = [
               '# Remote via saved peer (bridge: connectToPeer)',
               f'Peer: {peerName}',
               f'Auth: {authMode}'
            ]
         else:
            ipAddr = self.ipEdit.text().strip() or '(not set)'
            portText = self.portEdit.text().strip() or str(ARMORYDB_DEFAULT_PORT)
            cmdParts = [
               '# Remote via IP (bridge: connectToIp)',
               f'IP: {ipAddr}',
               f'Port: {portText}',
               'Auth: 1-way only'
            ]
      else:
         cmdParts = ['# Offline mode', '(No database connection)']
      commandLine = '\n  '.join(cmdParts)
      self.cliCommandLabel.setText(f"Settings:\n  {commandLine}")

   def handleRemoteModeChange(self, index):
      """Handle remote connection sub-mode changes."""
      currentMode = self.remoteModeCombo.currentText()
      if currentMode == REMOTE_MODE_PEER:
         self.peerFrame.show()
         self.ipFrame.hide()
      else:
         self.peerFrame.hide()
         self.ipFrame.show()
      self.updateCliCommandDisplay()

   def handleScenarioChange(self, index):
      """Handle changes to the database scenario selection."""
      dbScenario = self.databaseScenarioCombo.itemText(index)
      isLocal = dbScenario == SCENARIO_DB_LOCAL
      isRemote = dbScenario == SCENARIO_DB_REMOTE
      isOffline = dbScenario == SCENARIO_DB_NONE
      self.dirFrame.setVisible(not isRemote)
      self.localDatabaseFrame.setVisible(isLocal)
      self.remoteFrame.setVisible(isRemote)
      self.cliFrame.setVisible(not isRemote)
      # Test connection only available for local and remote scenarios
      self.testFrame.setVisible(not isOffline)
      self.updateCliCommandDisplay()

   def onTestConnectionClicked(self):
      """Handle Test Connection button click.

      Emits testConnectionRequested signal for parent to handle the actual
      connection attempt. The parent (DlgSetupManager) will call the
      appropriate bridge API based on current settings.
      """
      LOGINFO("Test Connection requested from DatabaseTab")
      self.testConnectionRequested.emit()

   def refreshPeerCombo(self):
      """Refresh peer combo box from bridge."""
      self.peerCombo.clear()
      self.cachedPeers = []
      self.ownPublicKey = ''

      if not self.peersDbLoaded:
         self.peerCombo.addItem(self.tr('(Load peers DB first)'))
         self.peerCombo.setEnabled(False)
         return

      bridgePeers = TheBridge.dbSetup.listPeers()
      for bp in bridgePeers:
         peer = PeerData.fromBridgePeer(bp)
         if peer.isOwn():
            self.ownPublicKey = peer.publicKey
            self.ownKeyLabel.setText(peer.publicKey)
         else:
            self.cachedPeers.append(peer)
            # Add each name (ip:port) as a combo item
            for name in peer.names:
               displayText = name
               self.peerCombo.addItem(displayText, peer)

      if self.peerCombo.count() == 0:
         self.peerCombo.addItem(self.tr('(No peers saved)'))
         self.peerCombo.setEnabled(False)
      else:
         self.peerCombo.setEnabled(True)

   def openPeerManager(self):
      """Open the peer manager dialog."""
      dialog = PeerManagerDialog(self, peersDbLoaded=self.peersDbLoaded)
      if dialog.exec_() == QtWidgets.QDialog.Accepted:
         selectedPeerName = dialog.getSelectedPeerName()
         if selectedPeerName:
            self.refreshPeerCombo()
            # Select the chosen peer in combo
            idx = self.peerCombo.findText(selectedPeerName)
            if idx >= 0:
               self.peerCombo.setCurrentIndex(idx)
         # Update own public key display
         ownKey = dialog.getOwnPublicKey()
         if ownKey:
            self.ownPublicKey = ownKey
            self.ownKeyLabel.setText(ownKey)

   def setPeersDbLoaded(self, loaded):
      """Update peers database loaded state and refresh UI accordingly."""
      self.peersDbLoaded = loaded
      self.refreshPeerCombo()
      self.managePeersButton.setEnabled(loaded)
      if loaded:
         self.ownKeyLabel.setText(
            self.ownPublicKey if self.ownPublicKey else self.tr('(Available)'))
      else:
         self.ownKeyLabel.setText(self.tr('(Load peers DB to view)'))

   def loadSettings(self):
      """Load database tab settings from configuration."""
      self.databaseDirEdit.setText(os.path.normpath(ARMORY_DB_DIR))

      # Database configuration
      dbScenario = TheSettings.getSettingOrSetDefault(
         'DBScenario', 'Automate ArmoryDB')
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
               self.tr("(Database already bootstrapped - cannot change mode)"))
            self.dbBootstrapLabel.setStyleSheet(
               "color: orange; font-style: italic;")
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
      elif dbScenario == SCENARIO_DB_REMOTE:
         # Load remote sub-mode preference
         remoteMode = TheSettings.getSettingOrSetDefault(
            'RemoteMode', REMOTE_MODE_PEER)
         self.remoteModeCombo.setCurrentText(remoteMode)
         self.handleRemoteModeChange(self.remoteModeCombo.currentIndex())

         # Load handshake mode setting (for peer mode)
         handshakeMode = TheSettings.getSettingOrSetDefault('HandshakeMode', 0)
         self.handshakeModeCombo.setCurrentIndex(handshakeMode)

         # Load IP connection settings (for IP mode)
         savedHost = TheSettings.get('RemoteDBHost')
         savedPort = TheSettings.get('RemoteDBPort')
         if savedHost:
            self.ipEdit.setText(savedHost)
         if savedPort:
            self.portEdit.setText(str(savedPort))

         # Note: Peers are loaded from bridge via refreshPeerCombo()
         # when peersDbLoaded is set to True

   def collectSettings(self):
      """Return current database config from UI as a dict."""
      dbScenario = self.databaseScenarioCombo.currentText()
      isLocal = dbScenario == SCENARIO_DB_LOCAL
      isRemote = dbScenario == SCENARIO_DB_REMOTE

      # Remote connection details
      remoteMode = self.remoteModeCombo.currentText() if isRemote else ''
      peerName = ''
      ipAddr = ''
      ipPort = ''
      handshakeMode = 0

      if isRemote:
         if remoteMode == REMOTE_MODE_PEER:
            # Peer mode: get selected peer name (ip:port) from combo
            if self.peerCombo.currentData() is not None:
               peerName = self.peerCombo.currentText()
            handshakeMode = self.handshakeModeCombo.currentIndex()
         else:
            # IP mode: get host/port from edit fields
            ipAddr = self.ipEdit.text().strip()
            portText = self.portEdit.text().strip()
            ipPort = portText if portText else str(ARMORYDB_DEFAULT_PORT)

      return {
         'dbPath': str(self.databaseDirEdit.text()),
         'scenario': str(dbScenario),
         # Local mode settings
         'typeDisp': str(self.databaseTypeCombo.currentText()) if isLocal else '',
         'ram': str(self.ramUsageEdit.text()) if isLocal else '',
         'threads': str(self.threadCountEdit.text()) if isLocal else '',
         # Remote mode settings
         'remoteMode': remoteMode,
         'peerName': peerName,
         'ipAddr': ipAddr,
         'ipPort': ipPort,
         'handshakeMode': handshakeMode,
      }

   def validate(self):
      """Validate database tab settings. Returns True if valid."""
      dbScenario = self.databaseScenarioCombo.currentText()
      if dbScenario == SCENARIO_DB_LOCAL:
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
               msg = self.tr('Thread count must be an integer between 1 and {}.')
               QtWidgets.QMessageBox.warning(
                  self, self.tr('Invalid Thread Count'),
                  msg.format(MAX_THREAD_COUNT))
               return False
      elif dbScenario == SCENARIO_DB_REMOTE:
         remoteMode = self.remoteModeCombo.currentText()
         if remoteMode == REMOTE_MODE_PEER:
            # Peer mode: must have a selected peer
            if self.peerCombo.currentData() is None:
               QtWidgets.QMessageBox.warning(
                  self,
                  self.tr('No Peer Selected'),
                  self.tr('Please select a saved peer or switch to IP mode.'))
               return False
         else:
            # IP mode: validate IP and port
            ipAddr = self.ipEdit.text().strip()
            if not ipAddr:
               QtWidgets.QMessageBox.warning(
                  self,
                  self.tr('Missing IP Address'),
                  self.tr('Please enter the remote database IP address.'))
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
                     self.tr('Port must be an integer between 1 and 65535.'))
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
