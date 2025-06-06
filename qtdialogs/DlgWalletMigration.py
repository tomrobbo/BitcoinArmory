##############################################################################
#                                                                            #
#  Copyright (C) 2025, goatpig                                               #
#  Distributed under the MIT license                                         #
#  See LICENSE-MIT or https://opensource.org/licenses/MIT                    #
#                                                                            #
##############################################################################

from armoryengine.CppBridge import ServerPush
from armoryengine.ArmoryUtils import LOGWARN
from ui.QtExecuteSignal import TheSignalExecution

from qtdialogs.qtdefines import QtWidgets, QtCore, QRichLabel, HLINE
from qtdialogs.ArmoryDialog import ArmoryDialog
from qtdialogs.DlgUnlockWallet import DlgUnlockWallet
from qtdialogs.DlgChangePassphrase import DlgChangePassphrase

# --- Constants for UI layout ---
MARGIN_LEFT = 14
MARGIN_TOP = 16
MARGIN_RIGHT = 14
MARGIN_BOTTOM = 16
SPACING_TOP = 4
SPACING_TITLE_TO_SUBTITLE = 4
SPACING_SUBTITLE_TO_LINE = 12
SPACING_LINE_TO_FORM = 50
SPACING_FORM_TO_LINE2 = 60
SPACING_AFTER_LINE2 = 16
PASS_FIELD_HEIGHT = 24
PASS_FIELD_WIDTH = 300
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
   return lbl

####
def createSubtextLabel(text):
   lbl = QRichLabel(text)
   lbl.setAlignment(QtCore.Qt.AlignHCenter | QtCore.Qt.AlignTop)
   lbl.setContentsMargins(0, 0, 0, 0)
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
      self._passphrase = None

   def reply(self, passphrase):
      if self.parent.reusePassprhase:
         self._passphrase = passphrase
      packet = self.parent.getNewPacket()
      packet.unlockRequest = passphrase
      packet.success = True
      self.parent.reply()

   def getPassphrase(self):
      if not self._passphrase:
         raise Exception("do not have passphrase for migrated wallet!")
      return self._passphrase

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
      self.passEdit = None
      self.dlgUnlock = None
      self.reusePassprhase = False

      self.btnCancel = QtWidgets.QPushButton(self.tr('Cancel'))
      self.btnCancel.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
         QtWidgets.QSizePolicy.Minimum)
      self.buttonSize = self.btnCancel.sizeHint()

      self.subtextFontSize = 10
      self.listFontSize = 9

      #page 1
      page1 = self.setupPage1()
      self.page1MinSize = page1.sizeHint()

      #page 2
      self.setupPage2()

      #layout
      self.stack = QtWidgets.QStackedLayout()
      self.stack.addWidget(page1)
      self.stack.addWidget(self.page2Widget)
      self.setLayout(self.stack)
      self.setWindowTitle(self.tr('Wallet Migration'))
      self.setSizePolicy(QtWidgets.QSizePolicy.Preferred,
         QtWidgets.QSizePolicy.Preferred)
      self.layout().setSizeConstraint(QtWidgets.QLayout.SetFixedSize)
      self.stack.setCurrentIndex(0)
      self.setFixedSize(self.sizeHint())

      # Connect signals directly
      self.btnNext.clicked.connect(self.proceedWithMigration)
      self.btnBack.clicked.connect(self.gotoPage1)
      self.btnMigrate.clicked.connect(self.migrateWallet)
      self.btnFinish.clicked.connect(self.accept)
      self.btnCancel.clicked.connect(self.reject)
      self.passEdit.textChanged.connect(self.updateFinishButton)

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
      """Sets up the first page (wallet details) of the migration dialog."""
      lblHeading = createHeadingLabel(
         self.tr('<span style="font-size:16pt;"><b>Wallet Details</b></span>')
      )
      lblSubtext = createSubtextLabel(
         self.tr(
            '<span style="font-size:10pt;">Review the details of the wallet you '
            'selected.</span>'
         )
      )
      formWidget, gridContainer = self._createWalletDetails()
      btnFrame1 = self._createPage1Buttons()

      #titles
      vbox1 = QtWidgets.QVBoxLayout()
      vbox1.setContentsMargins(MARGIN_LEFT, MARGIN_TOP, MARGIN_RIGHT, MARGIN_BOTTOM)
      vbox1.setSpacing(SPACING_TOP)
      vbox1.addWidget(lblHeading)
      vbox1.addSpacing(SPACING_TITLE_TO_SUBTITLE)
      vbox1.addWidget(lblSubtext)
      vbox1.addSpacing(SPACING_SUBTITLE_TO_LINE)

      #separator
      hline1 = QtWidgets.QHBoxLayout()
      hline1.setContentsMargins(MARGIN_LEFT, 0, MARGIN_RIGHT, 0)
      hline1.addWidget(HLINE())
      vbox1.addLayout(hline1)

      #body
      vbox1.addSpacing(SPACING_LINE_TO_FORM)
      vbox1.addLayout(gridContainer)
      vbox1.addSpacing(SPACING_FORM_TO_LINE2)

      #separator
      hline2 = QtWidgets.QHBoxLayout()
      hline2.setContentsMargins(MARGIN_LEFT, 0, MARGIN_RIGHT, 0)
      hline2.addWidget(HLINE())
      vbox1.addLayout(hline2)

      #buttons
      btnRowContainer = QtWidgets.QHBoxLayout()
      btnRowContainer.setContentsMargins(MARGIN_LEFT, 0, MARGIN_RIGHT, 0)
      btnRowContainer.addLayout(btnFrame1)
      vbox1.addLayout(btnRowContainer)

      #actual wizard page
      page1 = QtWidgets.QWidget()
      page1.setLayout(vbox1)
      labelTexts = [
         self.tr('Wallet ID:'), self.tr('Wallet Type:'), self.tr('Version:'),
         self.tr('Encrypted:'), self.tr('Watching-only:'),
         self.tr('Unlock wallet at migration:'), self.tr('Label:'),
         self.tr('Description:'), self.tr('Address & use count:')
      ]
      _, valueColWidth = self.setupGridAndMinWidth(
         formWidget.layout(), labelTexts)
      self._valueColWidth = valueColWidth
      return page1

   def _createWalletDetails(self):
      formWidget = QtWidgets.QWidget()
      formWidget.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
         QtWidgets.QSizePolicy.Preferred)
      grid = QtWidgets.QGridLayout(formWidget)
      grid.setContentsMargins(0, 0, 0, 0)
      grid.setHorizontalSpacing(GRID_H_SPACING)
      grid.setVerticalSpacing(GRID_V_SPACING)
      grid.setColumnStretch(0, 0)
      grid.setColumnStretch(1, 0)
      gridContainer = QtWidgets.QHBoxLayout()
      gridContainer.addStretch(1)
      gridContainer.addWidget(formWidget, 0, QtCore.Qt.AlignLeft)
      gridContainer.addStretch(1)

      #id, type & version
      row = 0
      label = styledLabel(self.tr('Wallet ID:'))
      value = styledValue(self._walletData.walletId)
      grid.addWidget(label, row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(value, row, 1, QtCore.Qt.AlignLeft)
      row += 1
      label = styledLabel(self.tr('Wallet Type:'))
      value = styledValue(self._walletData.which())
      grid.addWidget(label, row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(value, row, 1, QtCore.Qt.AlignLeft)
      row += 1
      label = styledLabel(self.tr('Version:'))
      value = styledValue(self._walletData.seedVersion)
      grid.addWidget(label, row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(value, row, 1, QtCore.Qt.AlignLeft)
      row += 1

      #encryption
      if self._walletData.watchingOnly:
         label = styledLabel(self.tr('Watching-only:'))
         value = styledValue(self.tr('Yes'))
         grid.addWidget(label, row, 0, QtCore.Qt.AlignLeft)
         grid.addWidget(value, row, 1, QtCore.Qt.AlignLeft)
         row += 1
      else:
         label = styledLabel(self.tr('Encrypted:'))
         value = styledValue(
            self.tr('Yes') if self._walletData.encrypted else self.tr('No'))
         grid.addWidget(label, row, 0, QtCore.Qt.AlignLeft)
         grid.addWidget(value, row, 1, QtCore.Qt.AlignLeft)
         row += 1
      self.chkUnlockAtMigration = None
      if self._walletData.encrypted:
         self.chkUnlockAtMigration = QtWidgets.QCheckBox()
         self.chkUnlockAtMigration.setChecked(True)
         self.chkUnlockAtMigration.setContentsMargins(0, 0, 0, 0)
         unlockLabel = styledLabel(
            self.tr('Unlock wallet at migration:'))
         self.chkUnlockAtMigration.setSizePolicy(
            QtWidgets.QSizePolicy.Minimum, QtWidgets.QSizePolicy.Minimum)
         grid.addWidget(unlockLabel, row, 0, QtCore.Qt.AlignLeft)
         grid.addWidget(self.chkUnlockAtMigration, row, 1, QtCore.Qt.AlignLeft)
         row += 1

      #label & description
      label = styledLabel(self.tr('Label:'))
      value = styledValue(self._walletData.label)
      grid.addWidget(label, row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(value, row, 1, QtCore.Qt.AlignLeft)
      row += 1
      label = styledLabel(self.tr('Description:'))
      value = styledValue(self._walletData.description)
      grid.addWidget(label, row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(value, row, 1, QtCore.Qt.AlignLeft)
      row += 1

      #addresses
      addr_use_label = styledLabel(self.tr('Address & use count:'))
      addr_use_value = styledValue(
         f'{self._walletData.addressCount}, {self._walletData.highestUsedIndex}')
      grid.addWidget(addr_use_label, row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(addr_use_value, row, 1, QtCore.Qt.AlignLeft)
      row += 1
      return formWidget, gridContainer

   def _createPage1Buttons(self):
      """Creates the button row for page 1 based on wallet type and encryption."""
      # Create all buttons here to ensure they exist before being added
      self.btnNext = QtWidgets.QPushButton(self.tr('Next'))
      self.btnNext.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
         QtWidgets.QSizePolicy.Minimum)
      self.btnNext.setMinimumSize(self.buttonSize)
      self.btnNext.setMaximumSize(self.buttonSize)

      self.btnMigrate = QtWidgets.QPushButton(self.tr('Migrate'))
      self.btnMigrate.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
         QtWidgets.QSizePolicy.Minimum)
      self.btnMigrate.setMinimumSize(self.buttonSize)
      self.btnMigrate.setMaximumSize(self.buttonSize)

      self.btnFinish = QtWidgets.QPushButton(self.tr('Finish'))
      self.btnFinish.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
         QtWidgets.QSizePolicy.Minimum)
      self.btnFinish.setMinimumSize(self.buttonSize)
      self.btnFinish.setMaximumSize(self.buttonSize)

      self.btnCancel = QtWidgets.QPushButton(self.tr('Cancel'))
      self.btnCancel.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
         QtWidgets.QSizePolicy.Minimum)
      self.btnCancel.setMinimumSize(self.buttonSize)
      self.btnCancel.setMaximumSize(self.buttonSize)

      btnFrame1 = QtWidgets.QHBoxLayout()
      btnFrame1.addStretch(1)
      if self._walletData.which() == 'legacy':
         if self._walletData.encrypted:
            btnFrame1.addWidget(self.btnCancel)
            btnFrame1.addWidget(self.btnNext)
         else:
            btnFrame1.addWidget(self.btnCancel)
            btnFrame1.addWidget(self.btnMigrate)
      elif self._walletData.encrypted and not self._walletData.watchingOnly:
         btnFrame1.addWidget(self.btnCancel)
         btnFrame1.addWidget(self.btnNext)
      else:
         btnFrame1.addWidget(self.btnCancel)
         btnFrame1.addWidget(self.btnFinish)
      return btnFrame1

   #############################################################################
   def processUnlockRequest(self, ids):
      #spawn unlock DlgUnlockWallet
      if not self.dlgUnlock:
         self.dlgUnlock = DlgUnlockMigratingWallet(
            parent=self, main=self.main)
      self.dlgUnlock.setIds(ids)

   ####
   def processSetNewPassphrase(self, isPriv):
      packet = self.getNewPacket()
      passPacket = packet.init("walletCreation")

      if isPriv:
         #private passphrase, do we reuse old one or get a fresh one?
         if not self.reusePassprhase:
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
         return

      elif payload.which() == 'unlockRequest':
         self.processUnlockRequest(payload.unlockRequest)
         return

      elif payload.which() == 'walletCreation':
         #requests for passwords
         notif = payload.walletCreation
         if notif.which() == 'setCtrlPass':
            self.processSetNewPassphrase(False)
         elif notif.which() == 'setPrivPass':
            self.processSetNewPassphrase(True)
         return

      elif payload.which() == 'walletProgress':
         print (f"wallet progress notif during restore: {payload.walletProgress}")
         return

   ####
   def parseProtoPacket(self, payload):
      print (f"process payload cb: {payload}")
      TheSignalExecution.executeMethod(self.processCallback, payload)

   ####
   def proceedWithMigration(self):
      '''
      Ask bridge the migrate the wallet, notifications will be sent
      to overloaded parseProtoPacket (from ServerPush parent class)
      '''
      def doneCallback(success, error):
         #move to final page
         if success:
            raise Exception("IMPLEMENT ME 2")
         else:
            #handle the failure
            LOGWARN(f"wallet migration failed with error: {error}")
            raise Exception("NOTIFY USER AND CLEAN ME UP")

      self.main.wallets.migrateWallet(
         self._walletPath,
         self.callbackId, doneCallback)

   ################################################################################
   def gotoPage2(self):
      """Switch to the passphrase entry page."""
      self.stack.setCurrentIndex(1)
      if self.passEdit:
         self.passEdit.setFocus()

   def gotoPage1(self):
      """Switch back to the wallet details page."""
      self.stack.setCurrentIndex(0)

   def updateFinishButton(self):
      """Enable/disable the finish button based on passphrase input."""
      if self.passEdit and hasattr(self, 'btnFinish2'):
         self.btnFinish2.setEnabled(bool(self.passEdit.text()))
         # Store passphrase when user enters it
         if self.passEdit.text():
            self.walletPassphrase = self.passEdit.text()

   def migrateWallet(self):
      """Perform the migration process (real backend integration required)."""
      try:
         passphrase = self.passEdit.text()
         confirm = getattr(self, 'confirmEdit', None)
         if confirm:
            confirm = confirm.text()
         if not passphrase or (confirm is not None and passphrase != confirm):
            QtWidgets.QMessageBox.warning(
               self, self.tr('Migration Cancelled'),
               self.tr('Passphrases do not match.'))
            return
         # Store passphrase for reuse if checkbox is checked
         if hasattr(self, 'chkReusePass') and self.chkReusePass.isChecked():
            self.walletPassphrase = passphrase
         # TODO: Integrate with real backend migration logic here
         # Example: self.newWallet = backend.migrate_wallet(...)
         QtWidgets.QMessageBox.information(
            self, self.tr('Migration Successful'),
            self.tr('Wallet migration completed successfully!'))
         self.accept()
      except Exception as e:
         QtWidgets.QMessageBox.critical(
            self, self.tr('Migration Failed'),
            self.tr('Failed to migrate wallet: %s') % str(e))
         return

   def onMigrationComplete(self):
      """Handle wallet migration completion and launch wallet creation wizard."""
      from ui.Wizards import WalletWizard
      parent = self.parent() if callable(self.parent) else None
      wizard = WalletWizard(parent, self.main)
      # Pre-fill wallet name and description
      wizard.walletCreationPage.pageFrame.editName.setText(self.walletLabel)
      wizard.walletCreationPage.pageFrame.editDescription.setText(
         self.walletDescription)
      # If passphrase is available, pre-fill and skip steps
      if self.walletPassphrase:
         wizard.setPassphrasePage.pageFrame.editPasswd1.setText(
            self.walletPassphrase)
         wizard.setPassphrasePage.pageFrame.editPasswd2.setText(
            self.walletPassphrase)
         wizard.verifyPassphrasePage.pageFrame.edtPasswd3.setText(
            self.walletPassphrase)
         wizard.reuse_passphrase = True  # Set the flag to enable skipping
      wizard.exec_()

   def accept(self):
      """Handle dialog acceptance and pass data to wallet creation wizard."""
      # Check passphrase reuse directly here
      if (hasattr(self, 'chkReusePass') and 
         self.chkReusePass.isChecked() and 
         self.passEdit):
         self.walletPassphrase = self.passEdit.text()
      self.onMigrationComplete()
      super(DlgWalletMigration, self).accept()

   def setupPage2(self):
      """Sets up the second page (passphrase entry) of the migration dialog."""
      self.page2Widget = QtWidgets.QWidget()
      vbox2 = QtWidgets.QVBoxLayout()
      vbox2.setContentsMargins(MARGIN_LEFT, MARGIN_TOP, MARGIN_RIGHT, MARGIN_BOTTOM)
      vbox2.addSpacing(SPACING_TOP)
      lblHeading2 = createHeadingLabel(
         self.tr('<span style="font-size:16pt;"><b>Enter Passphrase</b></span>'))
      vbox2.addWidget(lblHeading2)
      vbox2.addSpacing(SPACING_TITLE_TO_SUBTITLE)
      lblSubtext2 = createSubtextLabel(
         self.tr('<span style="font-size:10pt;">Enter your passphrase to continue.</span>'))
      subtitleRow = QtWidgets.QHBoxLayout()
      subtitleRow.setContentsMargins(40, 0, 40, 0)
      subtitleRow.addWidget(lblSubtext2)
      vbox2.addLayout(subtitleRow)
      vbox2.addSpacing(SPACING_SUBTITLE_TO_LINE)
      hline1_2 = QtWidgets.QHBoxLayout()
      hline1_2.setContentsMargins(MARGIN_LEFT, 0, MARGIN_RIGHT, 0)
      hline1_2.addWidget(HLINE())
      vbox2.addLayout(hline1_2)
      vbox2.addSpacing(70)
      vbox2.addSpacing(12)
      formContainer = self._createPassphraseForm()
      vbox2.addWidget(formContainer)
      vbox2.addStretch(1)
      vbox2.addSpacing(80)
      hline2_2 = QtWidgets.QHBoxLayout()
      hline2_2.setContentsMargins(MARGIN_LEFT, 0, MARGIN_RIGHT, 0)
      hline2_2.addWidget(HLINE())
      vbox2.addLayout(hline2_2)
      btnRowContainer2 = QtWidgets.QHBoxLayout()
      btnRowContainer2.setContentsMargins(MARGIN_LEFT, 0, MARGIN_RIGHT, 0)
      btnFrame2 = self._createPage2Buttons()
      btnRowContainer2.addLayout(btnFrame2)
      vbox2.addLayout(btnRowContainer2)
      self.page2Widget.setLayout(vbox2)
      self.page2Widget.setMinimumSize(self.page1MinSize)
      self.setMinimumSize(self.page1MinSize)

   def _createPassphraseForm(self):
      formWidget2 = QtWidgets.QWidget()
      formWidget2.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
         QtWidgets.QSizePolicy.Preferred)
      formWidget2.setContentsMargins(40, 0, 40, 0)
      grid2 = QtWidgets.QGridLayout(formWidget2)
      grid2.setContentsMargins(0, 0, 0, 0)
      grid2.setHorizontalSpacing(GRID_H_SPACING)
      grid2.setVerticalSpacing(GRID_V_SPACING)
      grid2.setColumnStretch(0, 1)
      row2 = 0
      passRow = QtWidgets.QHBoxLayout()
      passLabel = styledLabel(self.tr('Passphrase:'))
      self.passEdit = QtWidgets.QLineEdit()
      self.passEdit.setEchoMode(QtWidgets.QLineEdit.Password)
      self.passEdit.setMinimumHeight(PASS_FIELD_HEIGHT)
      self.passEdit.setMaximumHeight(PASS_FIELD_HEIGHT)
      self.passEdit.setSizePolicy(QtWidgets.QSizePolicy.Expanding,
         QtWidgets.QSizePolicy.Minimum)
      passRow.addWidget(passLabel)
      passRow.addSpacing(1)
      passRow.addWidget(self.passEdit, 1)
      grid2.addLayout(passRow, row2, 0)
      row2 += 1
      reuseRow = QtWidgets.QHBoxLayout()
      reuseLabel = styledLabel(
         self.tr('Reuse passphrase for the new wallet:'))
      self.chkReusePass = QtWidgets.QCheckBox()
      self.chkReusePass.setChecked(True)
      self.chkReusePass.setSizePolicy(QtWidgets.QSizePolicy.Fixed,
         QtWidgets.QSizePolicy.Fixed)
      reuseRow.addWidget(reuseLabel)
      reuseRow.addStretch(1)
      reuseRow.addWidget(self.chkReusePass)
      grid2.addLayout(reuseRow, row2, 0)
      row2 += 1
      self.passEdit.textChanged.connect(self.updateFinishButton)
      formWidget2.setMinimumWidth(self.page1MinSize.width())

      formContainer = QtWidgets.QWidget()
      formContainerLayout = QtWidgets.QVBoxLayout(formContainer)
      formContainerLayout.setContentsMargins(0, 0, 0, 0)
      formContainerLayout.addStretch(1)
      formContainerLayout.addWidget(formWidget2, 0, QtCore.Qt.AlignHCenter)
      formContainerLayout.addStretch(1)
      return formContainer

   def _createPage2Buttons(self):
      """Creates the button row for page 2 based on wallet type and encryption."""
      btnFrame2 = QtWidgets.QHBoxLayout()
      btnFrame2.addStretch(1)
      self.btnBack = QtWidgets.QPushButton(self.tr('Back'))
      self.btnBack.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
         QtWidgets.QSizePolicy.Minimum)
      self.btnBack.setMinimumSize(self.buttonSize)
      self.btnBack.setMaximumSize(self.buttonSize)

      btnFrame2.addWidget(self.btnBack)
      if self._walletData.which() == 'legacy' and self._walletData.encrypted:
         self.btnMigrate2 = QtWidgets.QPushButton(self.tr('Migrate'))
         self.btnMigrate2.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
            QtWidgets.QSizePolicy.Minimum)
         self.btnMigrate2.setMinimumSize(self.buttonSize)
         self.btnMigrate2.setMaximumSize(self.buttonSize)
         btnFrame2.addWidget(self.btnMigrate2)
      elif self._walletData.encrypted and not self._walletData.watchingOnly:
         self.btnFinish2 = QtWidgets.QPushButton(self.tr('Finish'))
         self.btnFinish2.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
            QtWidgets.QSizePolicy.Minimum)
         self.btnFinish2.setMinimumSize(self.buttonSize)
         self.btnFinish2.setMaximumSize(self.buttonSize)
         self.btnFinish2.setEnabled(False)
         btnFrame2.addWidget(self.btnFinish2)
      return btnFrame2