##############################################################################
#                                                                            #
#  Copyright (C) 2025, goatpig                                               #
#  Distributed under the MIT license                                         #
#  See LICENSE-MIT or https://opensource.org/licenses/MIT                    #
#                                                                            #
##############################################################################


### This dialog handles the UI for migrating legacy wallets to the new format. ###

from qtdialogs.qtdefines import QtWidgets, QtCore, QRichLabel, HLINE
from qtdialogs.ArmoryDialog import ArmoryDialog

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

def styledLabel(text):
    lbl = QtWidgets.QLabel(text)
    lbl.setAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
    lbl.setStyleSheet('font-size: 9pt; font-weight: 500;')
    lbl.setWordWrap(False)
    lbl.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
       QtWidgets.QSizePolicy.Minimum)
    return lbl

def styledValue(text):
    val = QtWidgets.QLabel(str(text))
    val.setAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignVCenter)
    val.setStyleSheet('font-size: 9pt;')
    val.setWordWrap(True)
    val.setSizePolicy(QtWidgets.QSizePolicy.Preferred,
       QtWidgets.QSizePolicy.Minimum)
    return val

def createHeadingLabel(text):
    lbl = QRichLabel(text)
    lbl.setAlignment(QtCore.Qt.AlignHCenter | QtCore.Qt.AlignTop)
    lbl.setContentsMargins(0, 0, 0, 0)
    return lbl

def createSubtextLabel(text):
    lbl = QRichLabel(text)
    lbl.setAlignment(QtCore.Qt.AlignHCenter | QtCore.Qt.AlignTop)
    lbl.setContentsMargins(0, 0, 0, 0)
    return lbl

################################################################################
class DlgWalletMigration(ArmoryDialog):
   """
   Dialog for migrating a legacy wallet to the new format.
   Handles all UI and logic for both legacy and regular wallets.
   """
   def __init__(self, parent, main, wallet_data):
      super(DlgWalletMigration, self).__init__(parent, main)
      self.legacyWalletPath = wallet_data.get('legacyWalletPath')
      self.newWallet = None
      self.passEdit = None
      # Define all attributes here
      self.wtype = wallet_data.get('type', 'Unknown')
      self.version = wallet_data.get('version', 'Unknown')
      self.watchingOnly = wallet_data.get('watching_only', False)
      self.encrypted = wallet_data.get('encrypted', False)
      self.walletLabel = wallet_data.get('label', '(none)')
      self.walletDescription = wallet_data.get('description', '(none)')
      self.addressCount = len(wallet_data.get('addresses', []))
      self.useCount = wallet_data.get('use_count', 0)
      self.walletId = wallet_data.get('wallet_id', '(none)')
      self.walletEncrypted = self.encrypted
      self.walletPassphrase = None
      self.subtextFontSize = 10
      self.listFontSize = 9
      self.btnCancel = QtWidgets.QPushButton(self.tr('Cancel'))
      self.btnCancel.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
         QtWidgets.QSizePolicy.Minimum)
      self.buttonSize = self.btnCancel.sizeHint()
      page1 = self.setupPage1(
         self.walletId, self.version, self.addressCount, self.useCount
      )
      self.page1MinSize = page1.sizeHint()
      self.valueColWidth = self._valueColWidth
      self.setupPage2()
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
      if hasattr(self, 'btnNext'):
         self.btnNext.clicked.connect(self.gotoPage2)
      if hasattr(self, 'btnBack'):
         self.btnBack.clicked.connect(self.gotoPage1)
      if hasattr(self, 'btnMigrate'):
         self.btnMigrate.clicked.connect(self.migrateWallet)
      if hasattr(self, 'btnFinish'):
         self.btnFinish.clicked.connect(self.accept)
      if hasattr(self, 'btnCancel'):
         self.btnCancel.clicked.connect(self.reject)
      if hasattr(self, 'passEdit'):
         self.passEdit.textChanged.connect(self.updateFinishButton)

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

   def setupPage1(self, walletId, version, addressCount, useCount):
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
      formWidget, gridContainer = self._createWalletDetailsForm(
         walletId, version, addressCount, useCount
      )
      btnFrame1 = self._createPage1Buttons()
      vbox1 = QtWidgets.QVBoxLayout()
      vbox1.setContentsMargins(MARGIN_LEFT, MARGIN_TOP, MARGIN_RIGHT, MARGIN_BOTTOM)
      vbox1.setSpacing(SPACING_TOP)
      vbox1.addWidget(lblHeading)
      vbox1.addSpacing(SPACING_TITLE_TO_SUBTITLE)
      vbox1.addWidget(lblSubtext)
      vbox1.addSpacing(SPACING_SUBTITLE_TO_LINE)
      hline1 = QtWidgets.QHBoxLayout()
      hline1.setContentsMargins(MARGIN_LEFT, 0, MARGIN_RIGHT, 0)
      hline1.addWidget(HLINE())
      vbox1.addLayout(hline1)
      vbox1.addSpacing(SPACING_LINE_TO_FORM)
      vbox1.addLayout(gridContainer)
      vbox1.addSpacing(SPACING_FORM_TO_LINE2)
      hline2 = QtWidgets.QHBoxLayout()
      hline2.setContentsMargins(MARGIN_LEFT, 0, MARGIN_RIGHT, 0)
      hline2.addWidget(HLINE())
      vbox1.addLayout(hline2)
      btnRowContainer = QtWidgets.QHBoxLayout()
      btnRowContainer.setContentsMargins(MARGIN_LEFT, 0, MARGIN_RIGHT, 0)
      btnRowContainer.addLayout(btnFrame1)
      vbox1.addLayout(btnRowContainer)
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

   def _createWalletDetailsForm(self, walletId, version, addressCount, useCount):
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
      row = 0
      label = styledLabel(self.tr('Wallet ID:'))
      value = styledValue(walletId)
      grid.addWidget(label, row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(value, row, 1, QtCore.Qt.AlignLeft)
      row += 1
      label = styledLabel(self.tr('Wallet Type:'))
      value = styledValue(self.wtype)
      grid.addWidget(label, row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(value, row, 1, QtCore.Qt.AlignLeft)
      row += 1
      label = styledLabel(self.tr('Version:'))
      value = styledValue(version)
      grid.addWidget(label, row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(value, row, 1, QtCore.Qt.AlignLeft)
      row += 1
      if not self.encrypted and self.watchingOnly:
         label = styledLabel(self.tr('Watching-only:'))
         value = styledValue(self.tr('Yes'))
         grid.addWidget(label, row, 0, QtCore.Qt.AlignLeft)
         grid.addWidget(value, row, 1, QtCore.Qt.AlignLeft)
         row += 1
      if not self.watchingOnly:
         label = styledLabel(self.tr('Encrypted:'))
         value = styledValue(
            self.tr('Yes') if self.encrypted else self.tr('No'))
         grid.addWidget(label, row, 0, QtCore.Qt.AlignLeft)
         grid.addWidget(value, row, 1, QtCore.Qt.AlignLeft)
         row += 1
      self.chkUnlockAtMigration = None
      if self.encrypted and not self.watchingOnly:
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
      label = styledLabel(self.tr('Label:'))
      value = styledValue(self.walletLabel)
      grid.addWidget(label, row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(value, row, 1, QtCore.Qt.AlignLeft)
      row += 1
      label = styledLabel(self.tr('Description:'))
      value = styledValue(self.walletDescription)
      grid.addWidget(label, row, 0, QtCore.Qt.AlignLeft)
      grid.addWidget(value, row, 1, QtCore.Qt.AlignLeft)
      row += 1
      addr_use_label = styledLabel(self.tr('Address & use count:'))
      addr_use_value = styledValue(f'{addressCount}, {useCount}')
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
      if self.wtype == 'legacy':
         if self.encrypted:
            btnFrame1.addWidget(self.btnCancel)
            btnFrame1.addWidget(self.btnNext)
         else:
            btnFrame1.addWidget(self.btnCancel)
            btnFrame1.addWidget(self.btnMigrate)
      elif self.encrypted and not self.watchingOnly:
         btnFrame1.addWidget(self.btnCancel)
         btnFrame1.addWidget(self.btnNext)
      else:
         btnFrame1.addWidget(self.btnCancel)
         btnFrame1.addWidget(self.btnFinish)
      return btnFrame1

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
      if self.wtype == 'legacy' and self.encrypted:
         self.btnMigrate2 = QtWidgets.QPushButton(self.tr('Migrate'))
         self.btnMigrate2.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
            QtWidgets.QSizePolicy.Minimum)
         self.btnMigrate2.setMinimumSize(self.buttonSize)
         self.btnMigrate2.setMaximumSize(self.buttonSize)
         btnFrame2.addWidget(self.btnMigrate2)
      elif self.encrypted and not self.watchingOnly:
         self.btnFinish2 = QtWidgets.QPushButton(self.tr('Finish'))
         self.btnFinish2.setSizePolicy(QtWidgets.QSizePolicy.Minimum,
            QtWidgets.QSizePolicy.Minimum)
         self.btnFinish2.setMinimumSize(self.buttonSize)
         self.btnFinish2.setMaximumSize(self.buttonSize)
         self.btnFinish2.setEnabled(False)
         btnFrame2.addWidget(self.btnFinish2)
      return btnFrame2 