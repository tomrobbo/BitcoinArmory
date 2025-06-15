##############################################################################
#                                                                            #
#  Copyright (C) 2025, goatpig                                               #
#  Distributed under the MIT license                                         #
#  See LICENSE-MIT or https://opensource.org/licenses/MIT                    #
#                                                                            #
##############################################################################

from armoryengine.CppBridge import ServerPush
from ui.QtExecuteSignal import TheSignalExecution
from ui.Wizards import SetPassphrasePage, VerifyPassphrasePage, WalletProgressPage

from qtdialogs.qtdefines import QtWidgets, QtCore, QRichLabel, HLINE
from qtdialogs.ArmoryDialog import ArmoryDialog
from qtdialogs.DlgUnlockWallet import DlgUnlockWallet
from qtdialogs.DlgChangePassphrase import DlgChangePassphrase
from armorycolors import htmlColor

# --- Constants for UI layout ---
MARGIN_LEFT = 14
MARGIN_TOP = 6
MARGIN_RIGHT = 14
MARGIN_BOTTOM = 8
SPACING_TOP = 2
SPACING_TITLE_TO_SUBTITLE = 2
SPACING_SUBTITLE_TO_LINE = 6
SPACING_LINE_TO_FORM = 14
SPACING_FORM_TO_LINE2 = 14
SPACING_AFTER_LINE2 = 16
PASS_FIELD_HEIGHT = 24
PASS_FIELD_WIDTH = 24
GRID_H_SPACING = 10
GRID_V_SPACING = 6

################################################################################
def styledLabel(text):
   lbl = QtWidgets.QLabel(text)
   lbl.setAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
   lbl.setStyleSheet('font-size: 9pt; font-weight: 500;')
   lbl.setWordWrap(False)
   lbl.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
      QtWidgets.QSizePolicy.Minimum)
   return lbl

####
def styledValue(text):
   val = QtWidgets.QLabel(str(text))
   val.setAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
   val.setStyleSheet('font-size: 9pt;')
   val.setWordWrap(True)
   val.setSizePolicy(QtWidgets.QSizePolicy.Preferred,
      QtWidgets.QSizePolicy.Minimum)
   return val

####
def createHeadingLabel(text):
   lbl = QRichLabel(text)
   lbl.setAlignment(QtCore.Qt.AlignHCenter | QtCore.Qt.AlignTop)
   lbl.setContentsMargins(0, 0, 0, 0)
   lbl.setWordWrap(False)
   return lbl

####
def createSubtextLabel(text):
   lbl = QRichLabel(text)
   lbl.setAlignment(QtCore.Qt.AlignHCenter | QtCore.Qt.AlignTop)
   lbl.setContentsMargins(0, 0, 0, 0)
   lbl.setWordWrap(False)
   return lbl

################################################################################
class DlgUnlockMigratingWallet(DlgUnlockWallet):
   '''
   Wraps around DlgUnlockWallet to handle the unlock
   of legacy wallets at migration
   '''
   def __init__(self, parent, main):
      DlgUnlockWallet.__init__(self,
         wltID=parent._walletData.walletId,
         parent=parent, main=main,
         unlockMsg="Unlock Wallet To Migrate")

   def reply(self, passphrase):
      # Store the passphrase in the parent immediately after unlock
      self.parent._unlockedPassphrase = passphrase
      packet = self.parent.getNewPacket()
      packet.unlockRequest = passphrase
      packet.success = True
      self.parent.reply()
      self.done(0)  # Close the unlock dialog immediately after unlock

################################################################################
class DlgWalletMigration(ArmoryDialog, ServerPush):
   """
   Dialog for migrating a legacy wallet to the new format.
   """
   def __init__(self, parent, main, wltPath, walletData):
      ServerPush.__init__(self)
      ArmoryDialog.__init__(self, parent, main)
      self._walletPath = wltPath
      self._walletData = walletData
      self.dlgUnlock = None
      self.reusePassphrase = False
      self._doneBtn = None
      self._resultWidget = None
      self._resultLabel = None
      self._migration_complete = False
      self._migration_failed = False

      self.btnCancel = QtWidgets.QPushButton(self.tr('Cancel'))
      self.btnCancel.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
         QtWidgets.QSizePolicy.Minimum)
      self.buttonSize = self.btnCancel.sizeHint()

      self.subtextFontSize = 10
      self.listFontSize = 9

      #page 1
      self.page1 = self.setupPage1()

      # Instantiate wizard pages for migration
      self.setPassphrasePage = SetPassphrasePage(self)
      self.verifyPassphrasePage = VerifyPassphrasePage(self)
      self.walletProgressPage = WalletProgressPage(self)

      # Create and add Done button to the progress page
      self._doneBtn = QtWidgets.QPushButton(self.tr('Done'))
      self._doneBtn.setEnabled(False)  # Initially disabled
      self._doneBtn.clicked.connect(self.close)
      btnLayout = QtWidgets.QHBoxLayout()
      btnLayout.addStretch(1)
      btnLayout.addWidget(self._doneBtn)
      # Store the button layout for later use
      self._doneBtnLayout = btnLayout
      self.walletProgressPage.pageFrame.layout().addLayout(btnLayout)

      # Add NEXT button to setPassphrasePage
      nextBtn = QtWidgets.QPushButton(self.tr('Next'))
      nextBtn.setEnabled(False)
      nextBtn.clicked.connect(self.nextFromSetPassphrase)
      set_layout = self.setPassphrasePage.pageFrame.layout()
      set_layout.addWidget(nextBtn, alignment=QtCore.Qt.AlignRight)

      # Wire up passphrase validation to enable/disable Next button
      def enableNextBtn():
         pw1 = self.setPassphrasePage.pageFrame.editPasswd1.text()
         pw2 = self.setPassphrasePage.pageFrame.editPasswd2.text()
         def isASCII(s):
            try:
               s.encode('ascii')
               return True
            except Exception:
               return False
         nextBtn.setEnabled(bool(pw1) and pw1 == pw2 and len(
            pw1) >= 5 and isASCII(pw1) and isASCII(pw2))
      self.setPassphrasePage.pageFrame.passphraseCallback = enableNextBtn
      # Call once to set initial state
      enableNextBtn()

      # Add DONE button to VerifyPassphrasePage
      doneBtn = QtWidgets.QPushButton(self.tr('Done'))
      doneBtn.clicked.connect(self.nextFromVerifyPassphrase)
      verify_layout = self.verifyPassphrasePage.pageFrame.layout()
      row = verify_layout.rowCount()
      verify_layout.addWidget(doneBtn, row, 1, alignment=QtCore.Qt.AlignRight)
      
      #layout
      self.stack = QtWidgets.QStackedLayout()
      self.stack.addWidget(self.page1)
      self.stack.addWidget(self.setPassphrasePage)
      self.stack.addWidget(self.verifyPassphrasePage)
      self.stack.addWidget(self.walletProgressPage)
      self.setLayout(self.stack)
      self.setWindowTitle(self.tr('Wallet Migration'))
      self.setSizePolicy(QtWidgets.QSizePolicy.Preferred,
         QtWidgets.QSizePolicy.Preferred)
      self.layout().setSizeConstraint(QtWidgets.QLayout.SetFixedSize)
      self.stack.setCurrentIndex(0)
      self.setFixedSize(self.sizeHint())

      # Connect signals directly
      self.btnNext.clicked.connect(self.proceedWithMigration)
      self.btnCancel.clicked.connect(self.reject)

      # Track passphrase for reuse
      self._unlockedPassphrase = None
      self._newPassphrase = None

      self._wired = False

   #############################################################################
   def setupGridAndMinWidth(self, grid, labelTexts):
      """Sets minimum width for value columns in the grid."""
      test_label = QtWidgets.QLabel(max(labelTexts, key=len))
      test_label.setStyleSheet(
         f'font-size: {self.listFontSize}pt; font-weight: 500;')
      test_label.setWordWrap(False)
      test_label.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
         QtWidgets.QSizePolicy.Minimum)
      test_label.adjustSize()
      leftColWidth = test_label.sizeHint().width()
      for i in range(grid.rowCount()):
         item = grid.itemAtPosition(i, 1)
         if item is not None:
            widget = item.widget()
            if widget is not None:
               widget.setMinimumWidth(leftColWidth)
      valueColWidth = 0
      for i in range(grid.rowCount()):
         item = grid.itemAtPosition(i, 1)
         if item is not None:
            widget = item.widget()
            if widget is not None:
               valueColWidth = max(valueColWidth,
                  widget.sizeHint().width())
      return leftColWidth, valueColWidth

   def setupPage1(self):
      """Sets up the first page (wallet details) of the migration dialog with modern, compact visuals."""
      # Create main container widget
      page1 = QtWidgets.QWidget()
      layout = QtWidgets.QVBoxLayout()
      layout.setContentsMargins(14, 6, 14, 8)
      layout.setSpacing(8)

      # Header
      title = createHeadingLabel(self.tr
         ('<span style="font-size:16pt;"><b>Wallet Details</b></span>'))
      layout.addWidget(title, alignment=QtCore.Qt.AlignHCenter)
      subtitle = createSubtextLabel(self.tr
         ('<span style="font-size:10pt;">Review the details of the wallet you selected</span>'))
      layout.addWidget(subtitle, alignment=QtCore.Qt.AlignHCenter)
      layout.addSpacing(2)

      # Top horizontal line
      hline1 = QtWidgets.QHBoxLayout()
      hline1.setContentsMargins(14, 6, 14, 0)
      hline1.addWidget(HLINE())
      layout.addLayout(hline1)
      layout.addSpacing(6)

      # Wallet details group box
      groupBox = QtWidgets.QGroupBox(self.tr("Wallet Information"))
      vbox = QtWidgets.QVBoxLayout()
      vbox.setContentsMargins(12, 12, 12, 12)
      vbox.setSpacing(8)
      grid = QtWidgets.QGridLayout()
      grid.setContentsMargins(0, 0, 0, 0)
      grid.setHorizontalSpacing(GRID_H_SPACING)
      grid.setVerticalSpacing(GRID_V_SPACING)
      grid.setColumnStretch(0, 0)
      grid.setColumnStretch(1, 0)
      row = 0
      grid.addWidget(styledLabel(self.tr('Wallet ID:')), row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(styledValue(self._walletData.walletId), row, 1, QtCore.Qt.AlignLeft)
      row += 1
      grid.addWidget(styledLabel(self.tr('Wallet Type:')), row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(styledValue(self._walletData.which()), row, 1, QtCore.Qt.AlignLeft)
      row += 1
      grid.addWidget(styledLabel(self.tr('Version:')), row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(styledValue(self._walletData.seedVersion), row, 1, QtCore.Qt.AlignLeft)
      row += 1
      if self._walletData.watchingOnly:
         grid.addWidget(styledLabel(self.tr('Watching-only:')), row, 0, QtCore.Qt.AlignLeft)
         grid.addWidget(styledValue(self.tr('Yes')), row, 1, QtCore.Qt.AlignLeft)
         row += 1
      else:
         grid.addWidget(styledLabel(self.tr('Encrypted:')), row, 0, QtCore.Qt.AlignLeft)
         grid.addWidget(styledValue(self.tr('Yes') 
            if self._walletData.encrypted else self.tr('No')), row, 1, QtCore.Qt.AlignLeft)
         row += 1
      grid.addWidget(styledLabel(self.tr('Label:')), row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(styledValue(self._walletData.label), row, 1, QtCore.Qt.AlignLeft)
      row += 1
      grid.addWidget(styledLabel(self.tr('Description:')), row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(styledValue(self._walletData.description), row, 1, QtCore.Qt.AlignLeft)
      row += 1
      grid.addWidget(styledLabel(self.tr('Address & use count:')), row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(styledValue(
         f'{self._walletData.addressCount}, {self._walletData.highestUsedIndex}'
         ), row, 1, QtCore.Qt.AlignLeft)
      row += 1
      vbox.addLayout(grid)
      groupBox.setLayout(vbox)
      layout.addWidget(groupBox, alignment=QtCore.Qt.AlignHCenter)

      # Bottom horizontal line
      hline2 = QtWidgets.QHBoxLayout()
      hline2.setContentsMargins(14, 6, 14, 0)
      hline2.addWidget(HLINE())
      layout.addLayout(hline2)
      layout.addSpacing(6)

      # Button row
      btnFrame1 = self._createPage1Buttons()
      layout.addLayout(btnFrame1)

      page1.setLayout(layout)
      return page1

   def _createPage1Buttons(self):
      """Creates the button row for page 1 based on wallet type and encryption."""
      # Create all buttons here to ensure they exist before being added
      self.btnNext = QtWidgets.QPushButton(self.tr('Next'))
      self.btnCancel = QtWidgets.QPushButton(self.tr('Cancel'))

      btnFrame1 = QtWidgets.QHBoxLayout()
      btnFrame1.addStretch(1)
      btnFrame1.addWidget(self.btnCancel)
      btnFrame1.addWidget(self.btnNext)
      return btnFrame1

   #############################################################################
   def processUnlockRequest(self, ids):
      # Ignore unlock requests if migration is already complete
      if self._migration_complete:
         return
      if not self.dlgUnlock:
         self.dlgUnlock = DlgUnlockMigratingWallet(
            parent=self, main=self.main)
         self.dlgUnlock.finished.connect(self._onUnlockDialogClosed)
      self.dlgUnlock.setIds(ids)

   def _onUnlockDialogClosed(self, result):
      # Ensure dlgUnlock is reset after dialog is closed
      self.dlgUnlock = None

   ####
   def processSetNewPassphrase(self, isPriv):
      packet = self.getNewPacket()
      passPacket = packet.init("walletCreation")

      if isPriv:
         #private passphrase, do we reuse old one or get a fresh one?
         if not self.reusePassphrase:
            dlgPasswd = DlgChangePassphrase(self, self.main)
            if dlgPasswd.exec_():
               packet.success = True

               #passphrase
               passPacket.passphrase = str(dlgPasswd.edtPasswd1.text())

               #unlock target in milliseconds
               privKdfTargetMs = int(dlgPasswd.advancedOptionsTab.getKdfSec() * 1000)
               if privKdfTargetMs <= 0:
                  privKdfTargetMs = 2000
               passPacket.kdfTargetMs = privKdfTargetMs

               #memory target in MB
               privKdfTargetMem = int(dlgPasswd.advancedOptionsTab.getKdfBytes() / (1024**2))
               if privKdfTargetMem <= 0:
                  privKdfTargetMem = 128
               passPacket.kdfTargetMB = privKdfTargetMem
            else:
               QtWidgets.QMessageBox.critical(self, self.tr('Cannot Encrypt'), \
                  self.tr('You requested your migrated wallet be encrypted, but no '
                  'valid passphrase was supplied. Aborting wallet recovery.'), \
                  QtWidgets.QMessageBox.Ok)
               packet.success = False
               self.reject()
         else:
            #user wants to reuse old passphrase, feed that instead
            packet.success = True
            passPacket.kdfTargetMs = 2000
            passPacket.kdfTargetMB = 128

            #set the passphrase provided to self.dlgUnlock and clean it up
            passPacket.passphrase = self.dlgUnlock.getPassphrase()
            del self.dlgUnlock
      else:
         #control passphrase, return empty for now
         packet.success = True
         passPacket.passphrase = ""
         passPacket.kdfTargetMs = 250
         passPacket.kdfTargetMB = 0
      self.reply()

   ####
   def processCallback(self, payload):
      if payload.which() == 'cleanup':
         self._migration_complete = True
         if self.dlgUnlock:
            self.dlgUnlock.close()
            self.dlgUnlock = None
         self.stack.setCurrentWidget(self.walletProgressPage)
         self.walletProgressPage.pageFrame.progressBar.hide()
         
         # Create and show result label
         self._resultLabel = QtWidgets.QLabel("")
         self._resultLabel.setAlignment(QtCore.Qt.AlignHCenter)
         self._resultLabel.setStyleSheet(
            "font-size: 11pt; font-weight: bold; color: %s;" % (
               htmlColor('TextRed') if self._migration_failed else '#00FF00') +
            "background: transparent; border: none;")
         if self._migration_failed:
            self._resultLabel.setText(
               f"Migration of Wallet {self._walletData.walletId} was failed!")
            # Show backend error message below the FAILURE label
            errorMsg = getattr(payload, 'errorMessage', None)
            if errorMsg:
               self._errorLabel = QtWidgets.QLabel(errorMsg)
               self._errorLabel.setAlignment(QtCore.Qt.AlignHCenter)
               self._errorLabel.setStyleSheet(
                  "font-size: 8pt; color: %s; background: transparent;" +
                  "border: none;" % htmlColor('TextRed'))
               layout = self.walletProgressPage.pageFrame.layout()
               layout.insertWidget(
                  layout.count() - 1, self._resultLabel, alignment=QtCore.Qt.AlignHCenter)
               layout.insertWidget(
                  layout.count() - 1, self._errorLabel, alignment=QtCore.Qt.AlignHCenter)
            else:
               layout = self.walletProgressPage.pageFrame.layout()
               layout.insertWidget(
                  layout.count() - 1, self._resultLabel, alignment=QtCore.Qt.AlignHCenter)
         else:
            self._resultLabel.setText(
               f"Migration of Wallet {self._walletData.walletId} was successful!")
            layout = self.walletProgressPage.pageFrame.layout()
            layout.insertWidget(
               layout.count() - 1, self._resultLabel, alignment=QtCore.Qt.AlignHCenter)
         # Enable the Done button when process completes
         self._doneBtn.setEnabled(True)
         return

      elif payload.which() == 'unlockRequest':
         self.processUnlockRequest(payload.unlockRequest)
         return

      elif payload.which() == 'walletCreation':
         # No need to close unlock dialog or get passphrase here; already handled
         notif = payload.walletCreation
         if notif.which() == 'setCtrlPass':
            self.processSetNewPassphrase(False)
         elif notif.which() == 'setPrivPass':
            choice = self.promptPassphraseReuseChoice()
            if choice == 'reuse':
               # Use the passphrase already stored after unlock
               packet = self.getNewPacket()
               reply = packet.init('walletCreation')
               reply.passphrase = self._unlockedPassphrase
               reply.kdfTargetMs = 2000
               reply.kdfTargetMB = 128
               packet.success = True
               self.reply()
               self.stack.setCurrentWidget(self.walletProgressPage)
               self.resize(self.stack.currentWidget().sizeHint())
            elif choice == 'new':
               self.setPassphrasePage.pageFrame.editPasswd1.clear()
               self.setPassphrasePage.pageFrame.editPasswd2.clear()
               self.stack.setCurrentWidget(self.setPassphrasePage)
               self.resize(self.stack.currentWidget().sizeHint())
            else:
               QtWidgets.QMessageBox.critical(
                  self,
                  self.tr('Migration Failed'),
                  self.tr('Migration was cancelled or failed. Please try again.')
               )
               packet = self.getNewPacket()
               reply = packet.init('walletCreation')
               reply.success = False
               self.reply()
               self.reject()
         return

      elif payload.which() == 'walletProgress':
         # Show progress page and update as events arrive
         self.stack.setCurrentWidget(self.walletProgressPage)
         notif = payload.walletProgress
         if notif.which() == 'createFile':
            self.walletProgressPage.pageFrame.updateProgress(
               f"creating file: {notif.createFile}")
         elif notif.which() == 'initFile':
            self.walletProgressPage.pageFrame.updateProgress(
               f"setting up master record (id: {notif.initFile})")
         elif notif.which() == 'readFile':
            self.walletProgressPage.pageFrame.updateProgress("populating master record")
         elif notif.which() == 'createAccount':
            self.walletProgressPage.pageFrame.updateProgress(
               f"adding account: {notif.createAccount}")
         elif notif.which() == 'extendChain':
            chainProg = notif.extendChain
            self.walletProgressPage.pageFrame.updateProgress("extending address chain: "
               f"{chainProg.current}/{chainProg.total}")
         return

   ####
   def parseProtoPacket(self, payload):
      print (f"process payload cb: {payload}")
      TheSignalExecution.executeMethod(self.processCallback, payload)

   ####
   def proceedWithMigration(self):
      '''
      Ask bridge to migrate the wallet, notifications will be sent
      to overloaded parseProtoPacket (from ServerPush parent class)
      '''
      def doneCallback(success, error):
         if success:
            # Do not reload wallets or close the dialog here; wait for onMigrationComplete
            pass
         else:
            QtWidgets.QMessageBox.critical(self, self.tr('Migration Failed'), error)
            self.reject()

      self.main.wallets.migrateWallet(
         self._walletPath,
         self.callbackId, doneCallback)

   ################################################################################

   def accept(self):
      """Handle dialog acceptance and pass data to wallet creation wizard."""
      # Check passphrase reuse directly here
      if self.chkReusePass and self.chkReusePass.isChecked() and self.passEdit:
         self._walletPassphrase = self.passEdit.text()
         self._onMigrationComplete()
      super(DlgWalletMigration, self).accept()

   def promptPassphraseReuseChoice(self):
      """Show a modal dialog asking the user to reuse or create a new passphrase."""
      msgBox = QtWidgets.QMessageBox(self)
      msgBox.setWindowTitle(self.tr('Passphrase Options'))
      msgBox.setText(self.tr(
         'Would you like to reuse your old passphrase or create a new one for the migrated wallet?'))
      reuseBtn = msgBox.addButton(self.tr(
         'Reuse Old Passphrase'), QtWidgets.QMessageBox.AcceptRole)
      newBtn = msgBox.addButton(self.tr(
         'Create New Passphrase'), QtWidgets.QMessageBox.ActionRole)
      cancelBtn = msgBox.addButton(self.tr(
         'Cancel'), QtWidgets.QMessageBox.RejectRole)
      msgBox.setDefaultButton(reuseBtn)
      msgBox.exec_()
      if msgBox.clickedButton() == reuseBtn:
         return 'reuse'
      elif msgBox.clickedButton() == newBtn:
         return 'new'
      else:
         return 'cancel'

   # Handle completion of set/verify passphrase pages
   def nextFromSetPassphrase(self):
      # Called when setPassphrasePage is complete
      self._newPassphrase = self.setPassphrasePage.pageFrame.getPassphrase()
      self.verifyPassphrasePage.pageFrame.edtPasswd3.clear()
      self.stack.setCurrentWidget(self.verifyPassphrasePage)
      self.resize(self.stack.currentWidget().sizeHint())

   def nextFromVerifyPassphrase(self):
      # Called when verifyPassphrasePage is complete
      verifyPass = self.verifyPassphrasePage.pageFrame.edtPasswd3.text()
      if verifyPass != self._newPassphrase:
         QtWidgets.QMessageBox.critical(self, self.tr('Invalid Passphrase'),
            self.tr(
               'You entered your confirmation passphrase incorrectly!'), QtWidgets.QMessageBox.Ok)
         return
      # Send new passphrase to backend
      packet = self.getNewPacket()
      reply = packet.init('walletCreation')
      reply.passphrase = self._newPassphrase
      reply.kdfTargetMs = 2000
      reply.kdfTargetMB = 128
      packet.success = True
      self.reply()
      self.stack.setCurrentWidget(self.walletProgressPage)
      self.resize(self.stack.currentWidget().sizeHint())

   # Connect page completion signals
   def showEvent(self, event):
      super().showEvent(event)
      # Connect only once
      if not self._wired:
         self.setPassphrasePage.pageFrame.editPasswd2.returnPressed.connect(
            self.nextFromSetPassphrase)
         self.verifyPassphrasePage.pageFrame.edtPasswd3.returnPressed.connect(
            self.nextFromVerifyPassphrase)
         self._wired = True

   def handleUnlockCancelForWatchOnly(self, unlockDlg):
      reply = QtWidgets.QMessageBox.question(
         self, self.tr('Create Watching-Only Wallet?'),
         self.tr('Unlock was cancelled. Do you want to create a watching-only wallet instead?'),
         QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No
      )
      if reply == QtWidgets.QMessageBox.Yes:
         # Show progress page and trigger backend
         self.stack.setCurrentWidget(self.walletProgressPage)
         self.resize(self.stack.currentWidget().sizeHint())
         unlockDlg.reply("")
      else:
         self.abortMigration()
         self.reject()

   def abortMigration(self):
      packet = self.getNewPacket()
      reply = packet.init('cleanup')
      packet.success = False
      self.reply()

   def onMigrationComplete(self):
      self.accept()
