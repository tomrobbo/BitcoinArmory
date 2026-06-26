##############################################################################
#                                                                            #
# Copyright (C) 2011-2015, Armory Technologies, Inc.                         #
# Distributed under the GNU Affero General Public License (AGPL v3)          #
# See LICENSE or http://www.gnu.org/licenses/agpl.html                       #
#                                                                            #
# Copyright (C) 2016-2022, goatpig                                           #
#  Distributed under the MIT license                                         #
#  See LICENSE-MIT or https://opensource.org/licenses/MIT                    #
#                                                                            #
##############################################################################

from html import escape

from qtpy import QtCore, QtWidgets

from qtdialogs.ArmoryDialog import ArmoryDialog
from qtdialogs.MsgBoxWithDNAA import MsgBoxWithDNAA
from qtdialogs.qtdefines import (
   GETFONT, MSGBOX, STRETCH, STYLE_SUNKEN, USERMODE,
   VERTICAL, HORIZONTAL, QRichLabel, makeLayoutFrame, tightSizeNChar,
)
from armoryengine.AddressUtils import encodePrivKeyBase58
from armoryengine.ArmoryUtils import (
   RightNow, binary_to_hex, getNameForAddrType, hash160,
   unixTimeToFormatStr, LOGERROR,
)
from armoryengine.Settings import TheSettings


################################################################################
class DlgShowKeyList(ArmoryDialog):
   """
   Lists every key in the wallet. Public metadata is fetched from CppBridge
   through exportPublicKeys without unlocking the wallet. Private keys are
   fetched only when the user explicitly enables a private-key column, via
   exportPrivateKeys and the unlock callback flow.
   """
   def __init__(self, wlt, parent=None, main=None):
      super(DlgShowKeyList, self).__init__(parent, main)

      self.wlt = wlt
      self.watchingOnly = bool(getattr(self.wlt, 'watchingOnly', False))

      # exported keys, populated asynchronously from the bridge. Each entry is
      # a dict: assetId, privKey, pubKey (bytes), address (str), index (int),
      # addrType (int).
      self._entries = []
      self.havePriv = False
      self._unlockHandler = None
      self._privKeysLoaded = False
      self._privateExportInProgress = False

      self.strDescrReg = (self.tr(
         'The textbox below shows all keys that are part of this wallet, '
         'which includes both permanent keys and imported keys.  If you '
         'simply want to backup your wallet and you have no imported keys '
         'then all data below is reproducible from a plain paper backup.'))
      self.strDescrWarn = (self.tr(
         '<br><br>'
         '<font color="red">Warning:</font> The text box below contains '
         'the plaintext (unencrypted) private keys for each of '
         'the addresses in this wallet.  This information can be used '
         'to spend the money associated with those addresses, so please '
         'protect it like you protect the rest of your wallet. '))

      self.lblDescr = QRichLabel('')
      self.lblDescr.setAlignment(QtCore.Qt.AlignLeft | QtCore.Qt.AlignTop)

      txtFont = GETFONT('Fixed', 8)
      self.txtBox = QtWidgets.QTextEdit()
      self.txtBox.setReadOnly(True)
      self.txtBox.setFont(txtFont)
      w, h = tightSizeNChar(txtFont, 110)
      self.txtBox.setMinimumWidth(w)
      self.txtBox.setMaximumWidth(w)
      self.txtBox.setMinimumHeight(int(h * 3.2))
      self.txtBox.setSizePolicy(
         QtWidgets.QSizePolicy.Fixed, QtWidgets.QSizePolicy.Preferred)

      # Column selection checkboxes
      self.chkList = {}
      self.chkList['AddrStr']    = QtWidgets.QCheckBox(self.tr('Address String'))
      self.chkList['AddrType']   = QtWidgets.QCheckBox(self.tr('Address Type'))
      self.chkList['PubKeyHash'] = QtWidgets.QCheckBox(self.tr('Hash160'))
      self.chkList['PrivHexBE']  = QtWidgets.QCheckBox(self.tr('Private Key (Plain Hex)'))
      self.chkList['PrivB58']    = QtWidgets.QCheckBox(self.tr('Private Key (Plain Base58)'))
      self.chkList['PubKey']     = QtWidgets.QCheckBox(self.tr('Public Key (BE)'))
      self.chkList['ChainIndex'] = QtWidgets.QCheckBox(self.tr('Chain Index'))

      self.chkList['AddrStr'   ].setChecked(True)
      self.chkList['AddrType'  ].setChecked(True)
      self.chkList['PubKeyHash'].setChecked(False)
      self.chkList['PrivB58'   ].setChecked(False)
      self.chkList['PrivHexBE' ].setChecked(False)
      self.chkList['PubKey'    ].setChecked(True)
      self.chkList['ChainIndex'].setChecked(False)

      namelist = ['AddrStr', 'AddrType', 'PubKeyHash', 'PrivB58',
                  'PrivHexBE', 'PubKey', 'ChainIndex']

      for name in self.chkList.keys():
         if name in ('PrivB58', 'PrivHexBE'):
            self.chkList[name].toggled.connect(self._onPrivColumnToggled)
         else:
            self.chkList[name].toggled.connect(self.rewriteList)

      self.chkOmitSpaces = QtWidgets.QCheckBox(self.tr('Omit spaces in key data'))
      self.chkOmitSpaces.toggled.connect(self.rewriteList)

      std = (self.main.usermode == USERMODE.Standard)
      adv = (self.main.usermode == USERMODE.Advanced)
      if std:
         self.chkList['PubKeyHash'].setVisible(False)
         self.chkList['ChainIndex'].setVisible(False)
      elif adv:
         self.chkList['PubKeyHash'].setVisible(False)
         self.chkList['ChainIndex'].setVisible(False)

      chkBoxList = [self.chkList[n] for n in namelist]
      frmChks = makeLayoutFrame(VERTICAL, chkBoxList, STYLE_SUNKEN)

      btnGoBack   = QtWidgets.QPushButton(self.tr('<<< Go Back'))
      btnSaveFile = QtWidgets.QPushButton(self.tr('Save to File...'))
      btnCopyClip = QtWidgets.QPushButton(self.tr('Copy to Clipboard'))
      self.lblCopied = QRichLabel('')

      btnGoBack.clicked.connect(self.accept)
      btnSaveFile.clicked.connect(self.saveToFile)
      btnCopyClip.clicked.connect(self.copyToClipboard)
      frmGoBack = makeLayoutFrame(HORIZONTAL, [
         btnGoBack,
         STRETCH,
         self.chkOmitSpaces,
         STRETCH,
         self.lblCopied,
         btnCopyClip,
         btnSaveFile
      ])

      frmDescr = makeLayoutFrame(HORIZONTAL, [self.lblDescr], STYLE_SUNKEN)

      dlgLayout = QtWidgets.QGridLayout()
      dlgLayout.addWidget(frmDescr,    0, 0, 1, 1)
      dlgLayout.addWidget(frmChks,     0, 1, 1, 1)
      dlgLayout.addWidget(self.txtBox, 1, 0, 1, 2)
      dlgLayout.addWidget(frmGoBack,   2, 0, 1, 2)
      dlgLayout.setRowStretch(0, 0)
      dlgLayout.setRowStretch(1, 1)
      dlgLayout.setRowStretch(2, 0)

      self.setLayout(dlgLayout)
      self.rewriteList()
      self.setWindowTitle(self.tr('All Wallet Keys'))

      if self.watchingOnly:
         self.txtBox.setText(self.tr(
            'This is a watch-only wallet: it holds no private keys.'))
      else:
         self._startPublicExport()

   #############################################################################
   def _parseExportItems(self, items):
      entries = []
      for item in items:
         entries.append({
            'assetId': bytes(item.assetId),
            'privKey': bytes(item.privKey),
            'pubKey':  bytes(item.publicKey),
            'address': item.addressString,
            'index':   item.index,
            'addrType': item.addrType,
         })
      return entries

   #############################################################################
   def _startPublicExport(self):
      def onKeysReceived(reply):
         if reply.success:
            self._entries = self._parseExportItems(reply.wallet.exportKeys)
            self.executeMethod(self._onPublicKeysReady)
         else:
            err = getattr(reply, 'error', None) or self.tr('Export failed.')
            LOGERROR('exportPublicKeys failed: %s', err)
            self.executeMethod(self._onPublicExportFailed, str(err))

      self.wlt.exportPublicKeys(onKeysReceived)

   #############################################################################
   def _startPrivateExport(self):
      from qtdialogs.DlgUnlockWallet import UnlockWalletHandler
      self._unlockHandler = UnlockWalletHandler(
         self.wlt.walletId, self.tr('Export Key List'), self)

      def onKeysReceived(reply):
         if reply.success:
            self._mergePrivateKeys(reply.wallet.exportKeys)
            self._privKeysLoaded = True
            self._privateExportInProgress = False
            self.executeMethod(self._onPrivateKeysReady)
         else:
            err = getattr(reply, 'error', None) or self.tr('Export failed.')
            LOGERROR('exportPrivateKeys failed: %s', err)
            self.executeMethod(self._onPrivateExportFailed, str(err))

      self.wlt.exportPrivateKeys(onKeysReceived, self._unlockHandler)

   #############################################################################
   def _mergePrivateKeys(self, items):
      privByAsset = {}
      for item in items:
         assetId = bytes(item.assetId)
         privByAsset[assetId] = bytes(item.privKey)

      if len(privByAsset) != len(self._entries):
         LOGERROR('private export returned %d keys, expected %d',
            len(privByAsset), len(self._entries))

      for entry in self._entries:
         privKey = privByAsset.get(entry['assetId'])
         if privKey is not None:
            entry['privKey'] = privKey

   #############################################################################
   def _onPublicExportFailed(self, errMsg):
      self.txtBox.setText(
         self.tr('Could not export public key data from this wallet.') +
         '\n\n' + errMsg)

   #############################################################################
   def _onPrivateExportFailed(self, errMsg):
      self._privateExportInProgress = False
      for name in ('PrivB58', 'PrivHexBE'):
         chk = self.chkList[name]
         chk.blockSignals(True)
         chk.setChecked(False)
         chk.blockSignals(False)
      self.rewriteList()
      self.lblDescr.setText(
         self.strDescrReg +
         '<br><br><font color="red">' +
         self.tr('Private key export was not completed:') +
         '</font> ' + escape(errMsg))

   #############################################################################
   def _onPublicKeysReady(self):
      self.rewriteList()

   #############################################################################
   def _onPrivateKeysReady(self):
      for name in ('PrivB58', 'PrivHexBE'):
         self.chkList[name].blockSignals(True)
         self.chkList[name].setChecked(True)
         self.chkList[name].blockSignals(False)
      self.rewriteList()

   #############################################################################
   def _onPrivColumnToggled(self, checked):
      if not checked:
         self.rewriteList()
         return

      if self._privKeysLoaded or self._privateExportInProgress:
         self.rewriteList()
         return

      sender = self.sender()
      sender.blockSignals(True)
      sender.setChecked(False)
      sender.blockSignals(False)
      self._privateExportInProgress = True
      self._startPrivateExport()

   #############################################################################
   def rewriteList(self, *args):
      """
      Write out the full key list from the cached export entries.
      """
      whitespace = '' if self.chkOmitSpaces.isChecked() else ' '

      def fmtBin(b, nB=4):
         h = binary_to_hex(b)
         return whitespace.join([h[i:i + nB] for i in range(0, len(h), nB)])

      L = []
      L.append('Created:       ' + unixTimeToFormatStr(
         RightNow(), self.main.getPreferredDateFormat()))
      L.append('Wallet ID:     ' + self.wlt.walletId)
      L.append('Wallet Name:   ' + self.wlt.labelName)
      addrTypes = self.wlt.getAddressTypes()
      if addrTypes:
         typeNames = [getNameForAddrType(t) for t in addrTypes]
         L.append('Eligible types:' + ' ' + ', '.join(typeNames))
      defaultType = self.wlt.getDefaultAddressType()
      if defaultType:
         L.append('Default type:  ' + getNameForAddrType(defaultType))
      L.append('')

      self.havePriv = False
      for entry in self._entries:
         privKey = entry['privKey']
         pubKey  = entry['pubKey']
         addrType = entry.get('addrType', 0)

         if self.chkList['AddrStr'].isChecked():
            L.append((entry['address'] or self.tr('(no address)')))
         # addrType 0 means metadata was unavailable, not AddressEntryType::Default
         if self.chkList['AddrType'].isChecked() and addrType:
            L.append('   AddrType  : ' + getNameForAddrType(addrType))
         if self.chkList['PubKeyHash'].isChecked() and pubKey:
            L.append('   Hash160   : ' + fmtBin(hash160(pubKey)))
         if self.chkList['PrivB58'].isChecked() and privKey:
            pB58 = encodePrivKeyBase58(privKey)
            pB58Stretch = whitespace.join(
               [pB58[i:i + 6] for i in range(0, len(pB58), 6)])
            L.append('   PrivBase58: ' + pB58Stretch)
            self.havePriv = True
         if self.chkList['PrivHexBE'].isChecked() and privKey:
            L.append('   PrivHexBE : ' + fmtBin(privKey))
            self.havePriv = True
         if self.chkList['PubKey'].isChecked() and pubKey:
            L.append('   PublicKey : ' + fmtBin(pubKey))
         if self.chkList['ChainIndex'].isChecked():
            L.append('   ChainIndex: ' + str(entry['index']))

      self.txtBox.setText('\n'.join(L))
      if self.havePriv:
         self.lblDescr.setText(self.strDescrReg + self.strDescrWarn)
      else:
         self.lblDescr.setText(self.strDescrReg)

   #############################################################################
   def saveToFile(self):
      if self.havePriv:
         if not TheSettings.getSettingOrSetDefault('DNAA_WarnPrintKeys', False):
            result = MsgBoxWithDNAA(self, self.main, MSGBOX.Warning,
               title=self.tr('Plaintext Private Keys'),
               msg=self.tr('<font color="red"><b>REMEMBER:</b></font> The data you '
               'are about to save contains private keys.  Please make sure '
               'that only trusted persons will have access to this file. '
               '<br><br>Are you sure you want to continue?'),
               dnaaMsg=None, wCancel=True)
            if not result[0]:
               return
            TheSettings.set('DNAA_WarnPrintKeys', result[1])

      wltID = self.wlt.walletId
      fn = self.main.getFileSave(title=self.tr('Save Key List'),
                                 ffilter=[self.tr('Text Files (*.txt)')],
                                 defaultFilename=('keylist_%s_.txt' % wltID))
      if len(fn) > 0:
         with open(fn, 'w') as fileobj:
            fileobj.write(str(self.txtBox.toPlainText()))

   #############################################################################
   def copyToClipboard(self):
      clipb = QtWidgets.QApplication.clipboard()
      clipb.clear()
      clipb.setText(str(self.txtBox.toPlainText()))
      self.lblCopied.setText(self.tr('<i>Copied!</i>'))

   #############################################################################
   def cleanup(self):
      # Drop the plaintext private keys held in the entry cache.
      self._entries = []
      self._privateExportInProgress = False

   #############################################################################
   def accept(self):
      self.cleanup()
      super(DlgShowKeyList, self).accept()

   #############################################################################
   def reject(self):
      self.cleanup()
      super(DlgShowKeyList, self).reject()
